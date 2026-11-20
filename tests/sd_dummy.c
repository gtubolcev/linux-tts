/*
 * sd_dummy - a fake engine on tts-module, for the protocol tests
 *
 * Voices: dummy-en-female-1, dummy-en-male-1 (model "en"), dummy-ru-female-1
 * (model "ru"), dummy-de-1 (model "bad", fails to load).  Loading takes
 * DummyLoadMs, a piece takes DummySynthMs and becomes 100 samples per
 * character of the value 0x7d0a (the bytes that need escaping).  A piece
 * with "FAIL" fails.  Every piece is logged to stderr as "dummy: say <text>".
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tts-module.h"

static int load_ms = 0, synth_ms = 0;
static char model_en[] = "en", model_ru[] = "ru", model_bad[] = "bad";
static int loaded_count = 0;

static gboolean
dummy_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "DummyLoadMs") == 0)
    load_ms = atoi (value);
  else if (g_ascii_strcasecmp (key, "DummySynthMs") == 0)
    synth_ms = atoi (value);
  else
    return FALSE;
  return TRUE;
}

static gboolean
dummy_init (gchar **msg)
{
#ifdef HAVE_SONIC
  fputs ("dummy: sonic\n", stderr);
#endif
  tts_voice_add ("dummy-en-female-1", "en", "female", model_en, NULL);
  tts_voice_add ("dummy-en-male-1", "en", "male", model_en, NULL);
  tts_voice_add ("dummy-ru-female-1", "ru", "female", model_ru, NULL);
  tts_voice_add ("dummy-de-1", "de", NULL, model_bad, NULL);
  return TRUE;
}

static gboolean
dummy_load (gpointer model, GError **error)
{
  /* not interruptible, like ONNX Runtime */
  g_usleep ((gulong) load_ms * 1000);
  if (model == model_bad)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "bad model");
      return FALSE;
    }
  fprintf (stderr, "dummy: loaded %s (%d)\n", (char *) model, ++loaded_count);
  return TRUE;
}

static void
dummy_unload (gpointer model)
{
  fprintf (stderr, "dummy: unloaded %s (%d)\n", (char *) model, --loaded_count);
}

static gboolean
dummy_speak (const TtsPiece *p, GError **error)
{
  /* as a model does: 100 samples a character at speed 1 */
  double speed = tts_model_speed (p->settings);
  glong n = (glong) (g_utf8_strlen (p->text, -1) * 100 / speed);
  gint16 *s;

  fprintf (stderr, "dummy: say %s|%s\n", p->voice->name, p->text);
  if (p->settings->rate != 0)
    fprintf (stderr, "dummy: speed %.2f\n", speed);
  if (strstr (p->text, "FAIL") != NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "asked to fail");
      return FALSE;
    }
  for (int t = 0; t < synth_ms && !tts_stopped (); t += 5)
    g_usleep (5000);
  s = g_new (gint16, n);
  for (glong i = 0; i < n; i++)
    s[i] = 0x7d0a;
  tts_audio_s16 (s, n, 16000);
  g_free (s);
  return TRUE;
}

const TtsEngine tts_engine = {
  .name = "sd_dummy",
  .prefix = "Dummy",
  .max_models = 1,
  .idle_timeout = 0,
  .first_chunk = 40,
  .config = dummy_config,
  .init = dummy_init,
  .load = dummy_load,
  .unload = dummy_unload,
  .speak = dummy_speak,
};
