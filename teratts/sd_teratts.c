/*
 * sd_teratts - speech-dispatcher output module for TeraTTSv2 voices
 *
 * TeraTTSv2 (TeraSpace) is one model for Russian and English; a voice is a
 * style (styles/<name>/style_ttl.npy, style_dp.npy) of the model release
 * in TerattsModelDir.  styles/ru_f1 becomes the voice teratts-ru-f1,
 * styles/eng_m2_whisper teratts-en-m2-whisper.  Every voice reads both
 * languages: the text is read as English when the message's language is
 * English, else as Russian.  The Russian stress marks come from the Vosk
 * TTS dictionary (common/ru-stress.c); a "+" written before a vowel wins.
 *
 * The protocol, the threads, the text and the audio are tts-module's.
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

#include "ru-stress.h"
#include "teratts.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)
#define ERR(...) TTS_ERR (__VA_ARGS__)

/* longer text gets a wrong duration and is slow: cut it */
#define MAX_CHARS 300

/* configuration */
static gchar *cfg_model_dir = NULL;
static gchar *cfg_dictionary = NULL;
static int cfg_threads = 0;

static gchar *model_dir = NULL;            /* the one found */
static TeraTts *model = NULL;              /* set by the loader */
static char model_key[] = "teratts";       /* the one model of all voices */
static RuStress *stress = NULL;



static gboolean
teratts_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "TerattsModelDir") == 0)
    g_free (cfg_model_dir), cfg_model_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TerattsDictionary") == 0)
    g_free (cfg_dictionary), cfg_dictionary = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TerattsThreads") == 0)
    cfg_threads = atoi (value);
  else
    return FALSE;
  return TRUE;
}



/* ru_f1 -> teratts-ru-f1, ru, female; eng_m2_whisper -> teratts-en-m2-whisper */
static void
add_voice (const gchar *style)
{
  GError *error = NULL;
  TeraVoice *tv = teratts_voice_load (model_dir, style, &error);
  gchar **f, *rest, *name;
  const gchar *lang, *gender = NULL;

  if (tv == NULL)
    {
      DBG ("skipping %s: %s", style, error->message);
      g_error_free (error);
      return;
    }
  f = g_strsplit (style, "_", -1);
  lang = g_str_equal (f[0], "eng") ? "en" : f[0];
  if (f[1] != NULL && f[1][0] == 'f')
    gender = "female";
  else if (f[1] != NULL && f[1][0] == 'm')
    gender = "male";
  rest = g_strjoinv ("-", f + 1);
  name = *rest != '\0' ? g_strdup_printf ("teratts-%s-%s", lang, rest)
                       : g_strdup_printf ("teratts-%s", style);
  tts_voice_add (name, lang, gender, model_key, tv);
  g_free (name);
  g_free (rest);
  g_strfreev (f);
}



static gboolean
teratts_init (gchar **msg)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "teratts", "teratts-v2", NULL);
  GError *error = NULL;
  gchar **styles;

  model_dir = g_strdup (cfg_model_dir != NULL ? cfg_model_dir : "/usr/share/teratts/teratts-v2");
  if (!g_file_test (model_dir, G_FILE_TEST_IS_DIR) && g_file_test (user, G_FILE_TEST_IS_DIR))
    {
      g_free (model_dir);
      model_dir = g_strdup (user);
    }
  g_free (user);
  styles = teratts_list_voices (model_dir);
  /* Russian voices first: they are the defaults */
  for (int pass = 0; pass < 2; pass++)
    for (gchar **s = styles; s != NULL && *s != NULL; s++)
      if (g_str_has_prefix (*s, "ru_") == (pass == 0))
        add_voice (*s);
  g_strfreev (styles);
  if (tts_voices ()->len == 0)
    {
      *msg = g_strdup_printf ("no TeraTTS voices in %s", model_dir);
      return FALSE;
    }
  stress = ru_stress_new (cfg_dictionary != NULL ? cfg_dictionary : RU_STRESS_DEFAULT_DICTIONARIES,
                          &error);
  if (stress == NULL)
    {
      ERR ("%s, the model guesses the stress", error->message);
      g_error_free (error);
    }
  *msg = g_strdup_printf ("TeraTTS ready, %u voices", tts_voices ()->len);
  return TRUE;
}



static int
wanted_threads (void)
{
  long n = sysconf (_SC_NPROCESSORS_ONLN);

  if (cfg_threads > 0)
    return cfg_threads;
  /* teratts.py takes the physical cores; more were slower with Kokoro
   * on a 4-core / 8-thread Ryzen */
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}



static gchar *
stress_cb (const gchar *text, gpointer data)
{
  return ru_stress_apply (data, text, RU_STRESS_PLUS);
}



/* in the loader thread */
static gboolean
teratts_load (gpointer key, GError **error)
{
  int saved = tts_stderr_off ();
  TeraTts *t = teratts_new (model_dir, wanted_threads (), error);
  gchar *text;
  gsize n;

  tts_stderr_restore (saved);
  if (t == NULL)
    return FALSE;
  /* the first run allocates the buffers; pay it here, not on the first
   * message */
  text = teratts_prepare (t, "Привет.", "ru", NULL, NULL);
  g_free (teratts_synth (t, ((TtsVoice *) g_ptr_array_index (tts_voices (), 0))->data, text,
                         1.0f, 1, NULL, 0, &n, NULL));
  g_free (text);
  DBG ("loaded %s (%d threads)", model_dir, wanted_threads ());
  model = t;
  return TRUE;
}



static void
teratts_unload (gpointer key)
{
  teratts_free (model);
  model = NULL;
}



/* ------------------------------------------------------------------ speech */

static const gchar *
teratts_text_language (const TtsSettings *s, const TtsVoice *v)
{
  return s->language != NULL && g_ascii_strncasecmp (s->language, "en", 2) == 0 ? "en" : "ru";
}



/* where to cut a text that is too long: the last comma-like mark before the
 * middle, else the last space before it, else the middle */
static glong
cut_point (const gchar *text)
{
  glong n = g_utf8_strlen (text, -1), half = n / 2, space = -1, mark = -1, i = 0;

  for (const gchar *p = text; *p != '\0' && i < half; p = g_utf8_next_char (p), i++)
    {
      gunichar c = g_utf8_get_char (p);
      if (c == ' ')
        {
          space = i;
          if (i > 0)
            {
              gunichar prev = g_utf8_get_char (g_utf8_prev_char (p));
              if (prev == ',' || prev == ':' || prev == ';' || prev == 0x2014)
                mark = i;
            }
        }
    }
  if (mark > n / 4)
    return mark;
  return space > 0 ? space : MAX (half, 1);
}



/* in the worker */
static gboolean
speak_text (const gchar *text, const TtsPiece *p, GError **error)
{
  /* rate -100..100 -> half .. double speed */
  float scale = (float) pow (2.0, -p->settings->rate / 100.0);
  gint64 t0 = g_get_monotonic_time ();
  gchar *prepared;
  gsize n = 0;
  float *wav;

  if (g_utf8_strlen (text, -1) > MAX_CHARS)
    {
      const gchar *mid = g_utf8_offset_to_pointer (text, cut_point (text));
      gchar *first = g_strndup (text, mid - text);
      gchar *rest = g_strdup (mid);
      gboolean ok = speak_text (g_strstrip (first), p, error);

      if (ok && !tts_stopped () && *g_strstrip (rest) != '\0')
        ok = speak_text (rest, p, error);
      g_free (first);
      g_free (rest);
      return ok;
    }
  prepared = teratts_prepare (model, text, teratts_text_language (p->settings, p->voice),
                              stress != NULL ? stress_cb : NULL, stress);
  if (prepared == NULL)
    return TRUE;
  wav = teratts_synth (model, p->voice->data, prepared, scale, g_str_hash (text), NULL, 0, &n,
                       error);
  if (wav == NULL)
    {
      g_free (prepared);
      return FALSE;
    }
  if (tts_debug)
    {
      gchar *nfc = g_utf8_normalize (prepared, -1, G_NORMALIZE_NFC);
      DBG ("%.2f s of speech in %.2f s: %s", n / (double) TERATTS_SAMPLE_RATE,
           (g_get_monotonic_time () - t0) / 1e6, nfc);
      g_free (nfc);
    }
  tts_audio_float (wav, n, TERATTS_SAMPLE_RATE);
  g_free (wav);
  g_free (prepared);
  return TRUE;
}



static gboolean
teratts_speak (const TtsPiece *p, GError **error)
{
  return speak_text (p->text, p, error);
}



static void
teratts_close (void)
{
  ru_stress_free (stress);
  stress = NULL;
}



const TtsEngine tts_engine = {
  .name = "sd_teratts",
  .prefix = "Teratts",
  .max_models = 1,
  .idle_timeout = 300,
  .first_chunk = 40,
  .config = teratts_config,
  .init = teratts_init,
  .load = teratts_load,
  .unload = teratts_unload,
  .speak = teratts_speak,
  .text_language = teratts_text_language,
  .close = teratts_close,
};
