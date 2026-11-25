/*
 * tts-stress - the stress stage of the speech-dispatcher modules
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tts-stress.h"

#include "ru-homograph.h"

#include <stdio.h>
#include <string.h>

#define MAX_TEXT 65536

struct TtsStress
{
  TtsStressConfig cfg;
  RuStress *dict;
  gboolean dict_tried;          /* opened, or failed once and logged */
  TtsStressResolver resolver;
  gpointer resolver_data;
  RuHomograph *homograph;
};

void
tts_stress_config_init (TtsStressConfig *c, gboolean enabled, RuStressMark mark)
{
  c->enabled = enabled;
  c->dictionary = g_strdup (RU_STRESS_DEFAULT_DICTIONARIES);
  c->mark = mark;
  c->mark_set = FALSE;
  c->homographs = NULL;
  c->homographs_dir = g_build_filename (g_get_user_data_dir (), "linux-tts", "ruaccent", NULL);
  c->homographs_threads = 2;
  c->homographs_idle = 300;
}

void
tts_stress_config_clear (TtsStressConfig *c)
{
  g_clear_pointer (&c->dictionary, g_free);
  g_clear_pointer (&c->homographs, g_free);
  g_clear_pointer (&c->homographs_dir, g_free);
}

static gboolean
parse_int (const gchar *value, int lo, int hi, int *out)
{
  gint64 v;
  gchar *end;

  v = g_ascii_strtoll (value, &end, 10);
  if (end == value || *end != '\0' || v < lo || v > hi)
    return FALSE;
  *out = (int) v;
  return TRUE;
}

gboolean
tts_stress_config_parse (TtsStressConfig *c, const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "TextStress") == 0)
    c->enabled = g_ascii_strcasecmp (value, "1") == 0 || g_ascii_strcasecmp (value, "yes") == 0
                 || g_ascii_strcasecmp (value, "true") == 0 || g_ascii_strcasecmp (value, "on") == 0;
  else if (g_ascii_strcasecmp (key, "TextStressDictionary") == 0 && *value != '\0')
    g_free (c->dictionary), c->dictionary = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextStressMark") == 0)
    {
      if (g_ascii_strcasecmp (value, "plus") == 0 || g_strcmp0 (value, "+") == 0)
        c->mark = RU_STRESS_PLUS, c->mark_set = TRUE;
      else if (g_ascii_strcasecmp (value, "acute") == 0)
        c->mark = RU_STRESS_ACUTE, c->mark_set = TRUE;
      else if (g_ascii_strcasecmp (value, "none") == 0)
        c->mark = RU_STRESS_NONE, c->mark_set = TRUE;
      /* anything else keeps the module's default */
    }
  else if (g_ascii_strcasecmp (key, "TextStressHomographs") == 0)
    {
      g_clear_pointer (&c->homographs, g_free);
      if (g_ascii_strcasecmp (value, "rules") == 0 || g_ascii_strcasecmp (value, "tiny") == 0
          || g_ascii_strcasecmp (value, "turbo") == 0)
        c->homographs = g_ascii_strdown (value, -1);
      /* none and anything else: off */
    }
  else if (g_ascii_strcasecmp (key, "TextStressHomographsDir") == 0)
    {
      if (g_path_is_absolute (value))
        g_free (c->homographs_dir), c->homographs_dir = g_strdup (value);
    }
  else if (g_ascii_strcasecmp (key, "TextStressHomographsThreads") == 0)
    parse_int (value, 1, 16, &c->homographs_threads);
  else if (g_ascii_strcasecmp (key, "TextStressHomographsIdle") == 0)
    parse_int (value, 0, 86400, &c->homographs_idle);
  else
    return FALSE;
  return TRUE;
}

/* The models of the packages ruaccent-model-tiny/-turbo; the user's
 * (~/.local/share/linux-tts/ruaccent, from xfce4-speech-settings) win */
#define HOMOGRAPHS_SYSTEM_DIR "/usr/share/linux-tts/ruaccent"

static gboolean
has_model (const gchar *dir, const gchar *size)
{
  gchar   *path = g_build_filename (dir, size, "model.onnx", NULL);
  gboolean found = g_file_test (path, G_FILE_TEST_IS_REGULAR);

  g_free (path);
  return found;
}

/* @dir, or the system one when @dir is the default and has no model of
 * @size but the system has */
static gchar *
homographs_dir (const gchar *dir, const gchar *size)
{
  gchar   *user = g_build_filename (g_get_user_data_dir (), "linux-tts", "ruaccent", NULL);
  gboolean system = strcmp (dir, user) == 0 && strcmp (size, "rules") != 0
                    && !has_model (dir, size) && has_model (HOMOGRAPHS_SYSTEM_DIR, size);

  g_free (user);
  return g_strdup (system ? HOMOGRAPHS_SYSTEM_DIR : dir);
}

TtsStress *
tts_stress_new (const TtsStressConfig *c)
{
  TtsStress *s;

  if (c == NULL || !c->enabled)
    return NULL;
  s = g_new0 (TtsStress, 1);
  s->cfg = *c;
  s->cfg.dictionary = g_strdup (c->dictionary);
  s->cfg.homographs = g_strdup (c->homographs);
  s->cfg.homographs_dir = g_strdup (c->homographs_dir);
  if (c->homographs != NULL && c->homographs_dir != NULL)
    {
      gchar *dir = homographs_dir (c->homographs_dir, c->homographs);

      s->homograph = ru_homograph_new (dir, c->homographs, c->homographs_threads,
                                       c->homographs_idle);
      g_free (dir);
    }
  return s;
}

void
tts_stress_free (TtsStress *s)
{
  if (s == NULL)
    return;
  ru_stress_free (s->dict);
  ru_homograph_free (s->homograph);
  tts_stress_config_clear (&s->cfg);
  g_free (s);
}

void
tts_stress_set_resolver (TtsStress *s, TtsStressResolver resolver, gpointer data)
{
  if (s == NULL)
    return;
  s->resolver = resolver;
  s->resolver_data = data;
}

static gboolean
is_russian (const gchar *lang)
{
  return lang != NULL && g_ascii_strncasecmp (lang, "ru", 2) == 0
         && (lang[2] == '\0' || lang[2] == '-' || lang[2] == '_');
}

gchar *
tts_stress_marks (const gchar *text, const gchar *lang, RuStressMark mark)
{
  if (text == NULL || !is_russian (lang) || strlen (text) > MAX_TEXT
      || (strchr (text, '+') == NULL && strstr (text, "\xcc\x81") == NULL)
      || !g_utf8_validate (text, -1, NULL))
    return g_strdup (text);
  if (mark != RU_STRESS_PLUS && mark != RU_STRESS_ACUTE)
    mark = RU_STRESS_NONE;
  return ru_stress_marks (text, mark);
}

gchar *
tts_stress_apply (TtsStress *s, const gchar *text, const gchar *lang)
{
  return tts_stress_apply_mark (s, text, lang, s != NULL ? s->cfg.mark : RU_STRESS_NONE);
}

gchar *
tts_stress_apply_mark (TtsStress *s, const gchar *text, const gchar *lang,
                       RuStressMark voice_mark)
{
  gchar *marked, *resolved;
  RuStressMark mark;

  if (s == NULL)
    return tts_stress_marks (text, lang, voice_mark);
  if (text == NULL || !is_russian (lang) || strlen (text) > MAX_TEXT
      || !g_utf8_validate (text, -1, NULL))
    return g_strdup (text);
  if (!s->dict_tried)
    {
      GError *error = NULL;

      s->dict_tried = TRUE;
      s->dict = ru_stress_new (s->cfg.dictionary, &error);
      if (s->dict == NULL)
        {
          /* once: the stage then passes text through */
          g_printerr ("tts-stress: %s; no stress marks\n", error->message);
          g_error_free (error);
        }
    }
  if (s->dict == NULL)
    return tts_stress_marks (text, lang, s->cfg.mark_set ? s->cfg.mark : voice_mark);
  ru_homograph_set_stress (s->homograph, s->dict);
  mark = s->cfg.mark_set ? s->cfg.mark : voice_mark;
  if (mark != RU_STRESS_PLUS && mark != RU_STRESS_ACUTE)
    mark = RU_STRESS_NONE;
  marked = ru_stress_apply (s->dict, text, mark);
  if (s->resolver != NULL)
    resolved = s->resolver (text, marked, mark, s->resolver_data);
  else if (s->homograph != NULL)
    resolved = ru_homograph_resolve (s->homograph, text, marked, mark);
  else
    return marked;
  if (resolved == NULL || !g_utf8_validate (resolved, -1, NULL))
    {
      g_free (resolved);
      return marked;
    }
  g_free (marked);
  return resolved;
}
