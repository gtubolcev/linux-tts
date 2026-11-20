/*
 * sd_vosk - speech-dispatcher output module for Vosk TTS models (alphacep)
 *
 * The models are the directories in VoskModelsDir (and
 * ~/.local/share/vosk-tts) with a config.json of the "multistream_v1" type,
 * like vosk-model-tts-ru-0.9-multi.  The language comes from the directory
 * name (tts-ru- -> ru); each speaker of the model is a voice:
 * vosk-ru-female-1 for female_0.
 *
 * Loading a model takes long (ONNX Runtime prepares a big graph); one model
 * is kept at a time.  The protocol, the threads, the text and the audio are
 * tts-module's.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tts-module.h"

#include "vosk.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)

typedef struct
{
  int sid;
} Speaker;

/* configuration */
static gchar *cfg_models_dir = NULL;
static int cfg_threads = 0;
static gboolean cfg_bert = TRUE;

static GPtrArray *models = NULL;           /* VoskModel */



static gboolean
vosk_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "VoskModelsDir") == 0)
    g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "VoskThreads") == 0)
    cfg_threads = atoi (value);
  else if (g_ascii_strcasecmp (key, "VoskBert") == 0)
    cfg_bert = tts_parse_bool (value);
  else
    return FALSE;
  return TRUE;
}



/* ------------------------------------------------------------------ models */

static void
add_model (const gchar *dir)
{
  GError *error = NULL;
  gchar *base = g_path_get_basename (dir);
  VoskModel *m;
  GPtrArray *speakers;
  gboolean first_of_language = TRUE;

  /* a user's model hides a system one of the same name */
  for (guint i = 0; i < models->len; i++)
    {
      gchar *other = g_path_get_basename (vosk_model_dir (g_ptr_array_index (models, i)));
      gboolean same = strcmp (other, base) == 0;
      g_free (other);
      if (same)
        {
          g_free (base);
          return;
        }
    }

  m = vosk_model_open (dir, &error);
  if (m == NULL)
    {
      DBG ("skipping %s: %s", dir, error->message);
      g_error_free (error);
      g_free (base);
      return;
    }
  for (guint i = 0; i < models->len; i++)
    if (strcmp (vosk_model_language (g_ptr_array_index (models, i)), vosk_model_language (m)) == 0)
      first_of_language = FALSE;
  g_ptr_array_add (models, m);

  speakers = vosk_model_speakers (m);
  for (guint i = 0; i < speakers->len; i++)
    {
      VoskSpeaker *s = g_ptr_array_index (speakers, i);
      const gchar *us = strrchr (s->name, '_');
      const gchar *lang = vosk_model_language (m);
      const gchar *gender = NULL;
      gchar *stem, *name;
      Speaker *sp = g_new0 (Speaker, 1);

      sp->sid = s->id;
      /* female_0 -> female-1 */
      if (us != NULL && g_ascii_isdigit (us[1]))
        {
          gchar *g = g_strndup (s->name, us - s->name);
          if (strcmp (g, "female") == 0)
            gender = "female";
          else if (strcmp (g, "male") == 0)
            gender = "male";
          stem = g_strdup_printf ("%s-%d", g, atoi (us + 1) + 1);
          g_free (g);
        }
      else
        stem = g_strdup (s->name);
      /* the second model of a language gets its directory name in the voices */
      name = first_of_language
             ? g_strdup_printf ("vosk-%s-%s", lang, stem)
             : g_strdup_printf ("vosk-%s-%s-%s", lang, base, stem);
      tts_voice_add (name, lang, gender, m, sp);
      g_free (stem);
      g_free (name);
    }
  g_free (base);
}



static void
scan_models (const gchar *parent)
{
  GDir *d = g_dir_open (parent, 0, NULL);
  const gchar *f;
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

  if (d == NULL)
    return;
  while ((f = g_dir_read_name (d)) != NULL)
    g_ptr_array_add (names, g_strdup (f));
  g_dir_close (d);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      gchar *cfg = g_build_filename (p, "config.json", NULL);
      if (g_file_test (cfg, G_FILE_TEST_EXISTS))
        add_model (p);
      g_free (cfg);
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static gboolean
vosk_init (gchar **msg)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "vosk-tts", NULL);

  if (cfg_models_dir == NULL)
    cfg_models_dir = g_strdup ("/usr/share/vosk-tts");
  models = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);
  if (tts_voices ()->len == 0)
    {
      *msg = g_strdup_printf ("no Vosk TTS models in %s", cfg_models_dir);
      return FALSE;
    }
  return TRUE;
}



static int
wanted_threads (void)
{
  long n = sysconf (_SC_NPROCESSORS_ONLN);

  if (cfg_threads > 0)
    return cfg_threads;
  /* more threads were slower on a 4-core / 8-thread Ryzen */
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}



/* in the loader thread */
static gboolean
vosk_load (gpointer model, GError **error)
{
  VoskModel *m = model;
  gchar *cache = g_build_filename (g_get_user_cache_dir (), "sd_vosk", NULL);
  int saved = tts_stderr_off ();
  gboolean ok = vosk_model_load (m, wanted_threads (), cfg_bert, cache, error);

  tts_stderr_restore (saved);
  g_free (cache);
  if (ok)
    {
      /* the first run of the parallel executor is ~50 % slower (it
       * allocates every buffer); pay it here, not on the first message */
      gsize n;
      g_free (vosk_synth (m, g_str_equal (vosk_model_language (m), "ru")
                             ? "Привет, как дела?" : "Hello, how are you?",
                          0, 1.0f, &n, NULL, NULL));
      DBG ("loaded %s (%d threads%s)", vosk_model_dir (m), wanted_threads (),
           cfg_bert ? ", BERT" : "");
    }
  return ok;
}



static void
vosk_unload (gpointer model)
{
  vosk_model_unload (model);
}



/* in the worker */
static gboolean
vosk_speak (const TtsPiece *p, GError **error)
{
  VoskModel *m = p->voice->model;
  Speaker *sp = p->voice->data;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, p->settings->rate / 100.0);
  gsize n = 0;
  float *wav = vosk_synth (m, p->text, sp->sid, speed, &n, NULL, error);

  if (wav == NULL)
    return error == NULL || *error == NULL;
  tts_audio_float (wav, n, vosk_model_sample_rate (m));
  g_free (wav);
  return TRUE;
}



const TtsEngine tts_engine = {
  .name = "sd_vosk",
  .prefix = "Vosk",
  .max_models = 1,
  .idle_timeout = 300,
  .first_chunk = 40,
  .config = vosk_config,
  .init = vosk_init,
  .load = vosk_load,
  .unload = vosk_unload,
  .speak = vosk_speak,
};
