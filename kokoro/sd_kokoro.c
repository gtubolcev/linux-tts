/*
 * sd_kokoro - speech-dispatcher output module for Kokoro voices
 *
 * The models are the directories in KokoroModelsDir (and
 * ~/.local/share/kokoro-tts) named kokoro-<lang>[-...] with config.json
 * (the "vocab"), model.onnx and voices/<name>.bin.  voices.txt lists the
 * voices as "<name> <male|female|-> [<onnx file>]" (a voice may need its own
 * fine-tuned model); without it every voices/<name>.bin is a voice of
 * model.onnx.  A voice is called kokoro-<lang>-<name>.  Russian has its own
 * text front end (ru-g2p.c: stress marks from the Vosk dictionary, espeak-ng
 * with an acute-aware ru_dict).  kokoro-mul is the model of Kokoro-82M v1.0
 * (hexgrad), whose voices tell their language by the first letter of the
 * name (af_heart: American English, female); its English, Spanish, French,
 * Hindi, Italian and Brazilian Portuguese voices get misaki's espeak-ng
 * front end (espeak-g2p.c), the Japanese and Chinese ones are skipped.
 *
 * One model is kept at a time.  The protocol, the threads, the text and the
 * audio are tts-module's.
 *
 * KokoroProvider "vulkan" (the link sd_kokoro-vulkan-rs and its
 * kokoro-vulkan-rs.conf) runs the model on the graphics card through the
 * onnx-vulkan-rs plugin of onnxruntime; without the plugin that synthesizer
 * does not start.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tts-module.h"

#include "kokoro.h"
#include "espeak-g2p.h"
#include "ru-g2p.h"
#include "ru-stress.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)
#define ERR(...) TTS_ERR (__VA_ARGS__)

typedef struct
{
  gchar *onnx;              /* path */
  gchar *config;            /* config.json */
  const gchar *warmup;      /* IPA for the first run */
  KokoroModel *km;          /* set by the loader */
} Model;

/* configuration */
static gchar *cfg_models_dir = NULL;
static gchar *cfg_dictionary = NULL;
static gchar *cfg_espeak_data = NULL;
static int cfg_threads = 0;
static gboolean cfg_vulkan = FALSE;       /* Provider "vulkan" */

/* VULKAN_EP_LIBRARY: the plugin next to the private libonnxruntime; meson
 * passes <libdir>/linux-tts/...  KOKORO_VULKAN_EP overrides it for tests. */
#ifndef VULKAN_EP_LIBRARY
#error "VULKAN_EP_LIBRARY comes from meson"
#endif

static const gchar *
vulkan_ep (void)
{
  const gchar *env = g_getenv ("KOKORO_VULKAN_EP");

  if (!cfg_vulkan)
    return NULL;
  return env != NULL && *env != '\0' ? env : VULKAN_EP_LIBRARY;
}

static GPtrArray *models = NULL;           /* Model */
static RuG2p *g2p = NULL;



static void
kokoro_setup (void)
{
  cfg_models_dir = g_strdup ("/usr/share/kokoro-tts");
  cfg_dictionary = g_strdup (RU_STRESS_DEFAULT_DICTIONARIES);
  cfg_espeak_data = g_strdup ("/usr/share/speech-dispatcher-kokoro");
}



static gboolean
kokoro_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "KokoroModelsDir") == 0)
    g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "KokoroDictionary") == 0)
    g_free (cfg_dictionary), cfg_dictionary = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "KokoroEspeakData") == 0)
    g_free (cfg_espeak_data), cfg_espeak_data = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "KokoroThreads") == 0)
    cfg_threads = atoi (value);
  else if (g_ascii_strcasecmp (key, "KokoroProvider") == 0)
    cfg_vulkan = g_ascii_strcasecmp (value, "vulkan") == 0;
  else
    return FALSE;
  return TRUE;
}



/* ------------------------------------------------------------------ models */

static Model *
model_get (const gchar *onnx, const gchar *config, const gchar *lang)
{
  Model *m;

  for (guint i = 0; i < models->len; i++)
    {
      m = g_ptr_array_index (models, i);
      if (strcmp (m->onnx, onnx) == 0)
        return m;
    }
  m = g_new0 (Model, 1);
  m->onnx = g_strdup (onnx);
  m->config = g_strdup (config);
  m->warmup = strcmp (lang, "ru") == 0 ? "prʲivʲˈet, kˈɑk dʲɪlˈɑ?" : "həlˈO, hˌW ɑɹ jˈu?";
  g_ptr_array_add (models, m);
  return m;
}



static gboolean
voice_exists (const gchar *name)
{
  GPtrArray *voices = tts_voices ();

  for (guint i = 0; i < voices->len; i++)
    if (strcmp (((TtsVoice *) g_ptr_array_index (voices, i))->name, name) == 0)
      return TRUE;
  return FALSE;
}



static void
add_voice (const gchar *dir, const gchar *lang, const gchar *name,
           const gchar *gender, const gchar *onnx_file)
{
  GError *error = NULL;
  gchar *low = g_ascii_strdown (lang, -1);
  gchar *full = g_strdup_printf ("kokoro-%s-%s", low, name);
  gchar *bin = g_strdup_printf ("%s.bin", name);
  gchar *style_path = g_build_filename (dir, "voices", bin, NULL);
  gchar *onnx = g_build_filename (dir, onnx_file != NULL ? onnx_file : "model.onnx", NULL);
  gchar *config = g_build_filename (dir, "config.json", NULL);
  gfloat *style = NULL;

  /* a user's voice hides a system one of the same name */
  if (voice_exists (full))
    goto out;
  if (!g_file_test (onnx, G_FILE_TEST_EXISTS))
    {
      DBG ("skipping %s: no %s", full, onnx);
      goto out;
    }
  style = kokoro_voice_load (style_path, &error);
  if (style == NULL)
    {
      DBG ("skipping %s: %s", full, error->message);
      g_error_free (error);
      goto out;
    }
  if (gender != NULL && strcmp (gender, "male") != 0 && strcmp (gender, "female") != 0)
    gender = NULL;
  tts_voice_add (full, lang, gender, model_get (onnx, config, lang), style);
out:
  g_free (low);
  g_free (full);
  g_free (bin);
  g_free (style_path);
  g_free (onnx);
  g_free (config);
}



/* The language of a voice of Kokoro-82M v1.0 by the first letter of its
 * name (misaki's lang_code), and its gender by the second; NULL for a
 * language without a front end here */
static const gchar *
mul_voice_lang (const gchar *name, const gchar **gender)
{
  static const gchar *const codes[][2] = {
    { "a", "en-US" }, { "b", "en-GB" }, { "e", "es" }, { "f", "fr" },
    { "h", "hi" }, { "i", "it" }, { "p", "pt-BR" },
  };

  if (strlen (name) < 3 || name[2] != '_')
    return NULL;
  if (*gender == NULL)
    *gender = name[1] == 'f' ? "female" : name[1] == 'm' ? "male" : NULL;
  for (guint i = 0; i < G_N_ELEMENTS (codes); i++)
    if (name[0] == codes[i][0][0])
      return kokoro_g2p_speaks (codes[i][1]) ? codes[i][1] : NULL;
  return NULL;
}

static void
add_dir_voice (const gchar *dir, const gchar *lang, const gchar *name,
               const gchar *gender, const gchar *onnx_file)
{
  if (strcmp (lang, "mul") == 0)
    {
      const gchar *voice_lang = mul_voice_lang (name, &gender);

      if (voice_lang == NULL)
        {
          DBG ("skipping %s of %s: no text front end for its language", name, dir);
          return;
        }
      lang = voice_lang;
    }
  add_voice (dir, lang, name, gender, onnx_file);
}

static void
add_model_dir (const gchar *dir, const gchar *base)
{
  gchar *lang, *list, *contents = NULL;
  const gchar *p = base + strlen ("kokoro-");

  lang = g_strndup (p, strcspn (p, "-_"));
  if (strcmp (lang, "ru") != 0 && strcmp (lang, "mul") != 0)
    {
      DBG ("skipping %s: no text front end for \"%s\"", dir, lang);
      g_free (lang);
      return;
    }
  list = g_build_filename (dir, "voices.txt", NULL);
  if (g_file_get_contents (list, &contents, NULL, NULL))
    {
      gchar **lines = g_strsplit (contents, "\n", -1);
      for (gchar **l = lines; *l != NULL; l++)
        {
          gchar **f;
          gint n;
          g_strstrip (*l);
          if (**l == '\0' || **l == '#')
            continue;
          if (!g_shell_parse_argv (*l, &n, &f, NULL))
            continue;
          add_dir_voice (dir, lang, f[0], n > 1 ? f[1] : NULL, n > 2 ? f[2] : NULL);
          g_strfreev (f);
        }
      g_strfreev (lines);
      g_free (contents);
    }
  else
    {
      gchar *vdir = g_build_filename (dir, "voices", NULL);
      GDir *d = g_dir_open (vdir, 0, NULL);
      GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
      const gchar *f;
      while (d != NULL && (f = g_dir_read_name (d)) != NULL)
        if (g_str_has_suffix (f, ".bin"))
          g_ptr_array_add (names, g_strndup (f, strlen (f) - 4));
      if (d != NULL)
        g_dir_close (d);
      g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
      for (guint i = 0; i < names->len; i++)
        add_dir_voice (dir, lang, g_ptr_array_index (names, i), NULL, NULL);
      g_ptr_array_free (names, TRUE);
      g_free (vdir);
    }
  g_free (list);
  g_free (lang);
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
    if (g_str_has_prefix (f, "kokoro-"))
      g_ptr_array_add (names, g_strdup (f));
  g_dir_close (d);
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      gchar *cfg = g_build_filename (p, "config.json", NULL);
      if (g_file_test (cfg, G_FILE_TEST_EXISTS))
        add_model_dir (p, g_ptr_array_index (names, i));
      g_free (cfg);
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static gboolean
kokoro_init (gchar **msg)
{
  GError *error = NULL;
  gchar *user;

  /* without the plugin the model would run on the CPU; this synthesizer
   * exists for the graphics card, so it stays out of the list */
  if (cfg_vulkan && !g_file_test (vulkan_ep (), G_FILE_TEST_EXISTS))
    {
      *msg = g_strdup_printf ("the Vulkan plugin %s is not installed", vulkan_ep ());
      return FALSE;
    }
  /* the voices of a language espeak-ng lacks are left out */
  if (!espeak_g2p_init (cfg_espeak_data, &error))
    {
      *msg = g_strdup (error->message);
      g_error_free (error);
      return FALSE;
    }
  user = g_build_filename (g_get_user_data_dir (), "kokoro-tts", NULL);
  models = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);
  if (tts_voices ()->len == 0)
    {
      *msg = g_strdup_printf ("no Kokoro voices in %s", cfg_models_dir);
      return FALSE;
    }
  /* the stress dictionary only for Russian voices */
  for (guint i = 0; i < tts_voices ()->len && g2p == NULL && error == NULL; i++)
    if (strcmp (((TtsVoice *) g_ptr_array_index (tts_voices (), i))->language, "ru") == 0)
      g2p = ru_g2p_new (*cfg_dictionary != '\0' ? cfg_dictionary : NULL, cfg_espeak_data, &error);
  if (error != NULL)
    {
      *msg = g_strdup (error->message);
      g_error_free (error);
      return FALSE;
    }
  *msg = g_strdup_printf ("Kokoro ready, %u voices%s", tts_voices ()->len,
                          cfg_vulkan ? ", on Vulkan" : "");
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



static const gfloat *
first_style_of (Model *m)
{
  GPtrArray *voices = tts_voices ();

  for (guint i = 0; i < voices->len; i++)
    {
      TtsVoice *v = g_ptr_array_index (voices, i);
      if (v->model == m)
        return v->data;
    }
  return NULL;
}



/* in the loader thread */
static gboolean
kokoro_load (gpointer model, GError **error)
{
  Model *m = model;
  int saved = tts_stderr_off ();
  gsize n;

  float *wav;

  m->km = kokoro_model_new (m->onnx, m->config, wanted_threads (), vulkan_ep (), error);
  tts_stderr_restore (saved);
  if (m->km == NULL)
    return FALSE;
  /* the first run allocates the buffers (on Vulkan it also builds the
   * pipelines, ~1.7 s); pay it here, not on the first message ("привет, как
   * дела?").  A model the plugin accepts but cannot run fails here. */
  wav = kokoro_synth (m->km, first_style_of (m), m->warmup, 1.0f, &n, error);
  if (wav == NULL)
    {
      kokoro_model_free (m->km);
      m->km = NULL;
      return FALSE;
    }
  g_free (wav);
  DBG ("loaded %s (%s, %d threads)", m->onnx, cfg_vulkan ? "vulkan" : "cpu", wanted_threads ());
  return TRUE;
}



static void
kokoro_unload (gpointer model)
{
  Model *m = model;

  kokoro_model_free (m->km);
  m->km = NULL;
}



/* ------------------------------------------------------------------ speech */



/* in the worker */
static gboolean
speak_text (const gchar *text, const TtsPiece *p, GError **error)
{
  Model *m = p->voice->model;
  /* the frame speeds the audio up further */
  float speed = (float) tts_model_speed (p->settings);
  gint64 t0 = g_get_monotonic_time ();
  const gchar *lang = p->voice->language;
  gchar *ipa = strcmp (lang, "ru") == 0 ? ru_g2p_phonemize (g2p, text) : kokoro_g2p (lang, text);
  glong len = g_utf8_strlen (ipa, -1);
  gsize n = 0;
  float *wav;

  if (len > KOKORO_MAX_PHONEMES && g_utf8_strlen (text, -1) > 1)
    {
      /* Kokoro takes at most 510 phonemes: say it in two halves */
      const gchar *mid = g_utf8_offset_to_pointer (text, tts_cut_point (text, g_utf8_strlen (text, -1) / 2));
      gchar *first = g_strndup (text, mid - text);
      gchar *rest = g_strdup (mid);
      gboolean ok;

      g_free (ipa);
      ok = speak_text (g_strstrip (first), p, error);
      if (ok && !tts_stopped () && *g_strstrip (rest) != '\0')
        ok = speak_text (rest, p, error);
      g_free (first);
      g_free (rest);
      return ok;
    }
  if (len == 0)
    {
      g_free (ipa);
      return TRUE;
    }
  if (len > KOKORO_MAX_PHONEMES)
    *g_utf8_offset_to_pointer (ipa, KOKORO_MAX_PHONEMES) = '\0';
  wav = kokoro_synth (m->km, p->voice->data, ipa, speed, &n, error);
  if (wav == NULL)
    {
      g_free (ipa);
      return FALSE;
    }
  DBG ("%.2f s of speech in %.2f s: %s [%s]", n / (double) KOKORO_SAMPLE_RATE,
       (g_get_monotonic_time () - t0) / 1e6, text, ipa);
  tts_audio_float (wav, n, KOKORO_SAMPLE_RATE);
  g_free (wav);
  g_free (ipa);
  return TRUE;
}



static gboolean
kokoro_speak (const TtsPiece *p, GError **error)
{
  return speak_text (p->text, p, error);
}



static void
kokoro_close (void)
{
  ru_g2p_free (g2p);
  g2p = NULL;
}



/* the provider and the Vulkan plugin, for the verdict of the test load */
static gchar *
kokoro_probe_salt (void)
{
  struct stat st;

  if (!cfg_vulkan)
    return g_strdup ("cpu");
  if (stat (vulkan_ep (), &st) != 0)
    return g_strdup ("vulkan none");
  return g_strdup_printf ("vulkan %lld %lld.%09ld", (long long) st.st_size,
                          (long long) st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
}

/* the model's files, for the test load in a child process */
static const gchar *
kokoro_model_path (gpointer model)
{
  return ((Model *) model)->onnx;
}



const TtsEngine tts_engine = {
  .name = "sd_kokoro",
  .prefix = "Kokoro",
  .max_models = 1,
  .idle_timeout = 300,
  .first_chunk = 40,
  .setup = kokoro_setup,
  .config = kokoro_config,
  .init = kokoro_init,
  .load = kokoro_load,
  .unload = kokoro_unload,
  .model_path = kokoro_model_path,
  .probe_salt = kokoro_probe_salt,
  .speak = kokoro_speak,
  .close = kokoro_close,
};
