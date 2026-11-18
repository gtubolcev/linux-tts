/*
 * tts-stress - the stress stage of the speech-dispatcher modules
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "tts-stress.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_DICTIONARY "/usr/share/vosk-tts/vosk-model-tts-ru-0.9-multi/dictionary.sorted"
#define MAX_TEXT 65536

struct TtsStress
{
  TtsStressConfig cfg;
  RuStress *dict;
  gboolean dict_tried;          /* opened, or failed once and logged */
  TtsStressResolver resolver;
  gpointer resolver_data;
};

void
tts_stress_config_init (TtsStressConfig *c, gboolean enabled, RuStressMark mark)
{
  c->enabled = enabled;
  c->dictionary = g_strdup (DEFAULT_DICTIONARY);
  c->mark = mark;
}

void
tts_stress_config_clear (TtsStressConfig *c)
{
  g_clear_pointer (&c->dictionary, g_free);
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
        c->mark = RU_STRESS_PLUS;
      else if (g_ascii_strcasecmp (value, "acute") == 0)
        c->mark = RU_STRESS_ACUTE;
      /* anything else keeps the module's default */
    }
  else
    return FALSE;
  return TRUE;
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
  return s;
}

void
tts_stress_free (TtsStress *s)
{
  if (s == NULL)
    return;
  ru_stress_free (s->dict);
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
tts_stress_apply (TtsStress *s, const gchar *text, const gchar *lang)
{
  gchar *marked, *resolved;

  if (s == NULL || text == NULL || !is_russian (lang) || strlen (text) > MAX_TEXT
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
    return g_strdup (text);
  marked = ru_stress_apply (s->dict, text, s->cfg.mark);
  if (s->resolver == NULL)
    return marked;
  resolved = s->resolver (marked, s->cfg.mark, s->resolver_data);
  if (resolved == NULL || !g_utf8_validate (resolved, -1, NULL))
    {
      g_free (resolved);
      return marked;
    }
  g_free (marked);
  return resolved;
}
