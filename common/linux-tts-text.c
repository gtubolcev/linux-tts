/*
 * linux-tts-text: the text stages of the modules as a private library, see
 * linux-tts-text.h.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <glib.h>

#include "linux-tts-text.h"
#include "tts-config.h"
#include "tts-norm.h"
#include "tts-stress.h"

#define EXPORT __attribute__ ((visibility ("default")))

struct LinuxTtsText
{
  GMutex lock;          /* one message at a time */
  TtsNorm *norm;        /* NULL: normalization off */
  TtsStressConfig stress_cfg;
  TtsStress *stress;    /* made on the first Russian text; NULL: off */
};

static gchar *
unquote (gchar *s)
{
  gsize n;

  s = g_strstrip (s);
  n = strlen (s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
    {
      s[n - 1] = '\0';
      s++;
    }
  return s;
}

static void
read_config (const char *configfile, TtsNormConfig *norm, TtsStressConfig *stress)
{
  gchar *contents = NULL, **lines;

  if (configfile == NULL || !tts_config_read (configfile, &contents))
    return;
  lines = g_strsplit (contents, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *key = g_strstrip (*l), *value = key;

      if (*key == '\0' || *key == '#')
        continue;
      while (*value != '\0' && !g_ascii_isspace (*value))
        value++;
      if (*value != '\0')
        *value++ = '\0';
      value = unquote (value);
      if (!tts_norm_config_parse (norm, key, value))
        tts_stress_config_parse (stress, key, value);
    }
  g_strfreev (lines);
  g_free (contents);
}

EXPORT LinuxTtsText *
linux_tts_text_new (const char *configfile)
{
  LinuxTtsText *t = g_new0 (LinuxTtsText, 1);
  TtsNormConfig norm;

  g_mutex_init (&t->lock);
  tts_norm_config_init (&norm);
  tts_stress_config_init (&t->stress_cfg, TRUE, RU_STRESS_NONE);
  read_config (configfile, &norm, &t->stress_cfg);
  t->norm = tts_norm_new (&norm);
  tts_norm_config_clear (&norm);
  return t;
}

EXPORT void
linux_tts_text_free (LinuxTtsText *t)
{
  if (t == NULL)
    return;
  tts_norm_free (t->norm);
  tts_stress_free (t->stress);
  tts_stress_config_clear (&t->stress_cfg);
  g_mutex_clear (&t->lock);
  g_free (t);
}

EXPORT void
linux_tts_text_string_free (char *s)
{
  g_free (s);
}

/* one piece of plain text through the stages: a new string */
static gchar *
prepare_plain (LinuxTtsText *t, const gchar *text, const gchar *lang, RuStressMark mark)
{
  gchar *normed, *stressed;

  normed = t->norm != NULL ? tts_norm_text (t->norm, text, lang) : g_strdup (text);
  if (t->stress == NULL && t->stress_cfg.enabled)
    t->stress = tts_stress_new (&t->stress_cfg);
  if (t->stress == NULL)
    return normed;
  stressed = tts_stress_apply_mark (t->stress, normed, lang, mark);
  g_free (normed);
  return stressed;
}

static gboolean
blank (const gchar *s, gsize n)
{
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isspace (s[i]))
      return FALSE;
  return TRUE;
}

/* &amp; &lt; &gt; &quot; &apos; &#N; &#xN; -> characters; anything else
 * stays as it is */
static gchar *
unescape (const gchar *s, gsize n)
{
  GString *out = g_string_sized_new (n);
  static const struct { const char *name; char c; } ents[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
  };

  for (gsize i = 0; i < n; i++)
    {
      gboolean done = FALSE;

      if (s[i] != '&')
        {
          g_string_append_c (out, s[i]);
          continue;
        }
      for (guint e = 0; e < G_N_ELEMENTS (ents) && !done; e++)
        {
          gsize l = strlen (ents[e].name);

          if (i + 1 + l <= n && strncmp (s + i + 1, ents[e].name, l) == 0)
            {
              g_string_append_c (out, ents[e].c);
              i += l;
              done = TRUE;
            }
        }
      if (!done && i + 2 < n && s[i + 1] == '#')
        {
          gboolean hex = s[i + 2] == 'x' || s[i + 2] == 'X';
          gsize j = i + (hex ? 3 : 2);
          guint64 v = 0;
          gsize start = j;

          while (j < n && j - start < 8 && (hex ? g_ascii_isxdigit (s[j]) : g_ascii_isdigit (s[j])))
            v = v * (hex ? 16 : 10) + (hex ? g_ascii_xdigit_value (s[j]) : s[j] - '0'), j++;
          if (j > start && j < n && s[j] == ';' && v > 0 && v <= 0x10FFFF
              && g_unichar_validate ((gunichar) v))
            {
              g_string_append_unichar (out, (gunichar) v);
              i = j;
              done = TRUE;
            }
        }
      if (!done)
        g_string_append_c (out, '&');
    }
  return g_string_free (out, FALSE);
}

static void
append_escaped (GString *out, const gchar *s)
{
  for (; *s != '\0'; s++)
    switch (*s)
      {
      case '&': g_string_append (out, "&amp;"); break;
      case '<': g_string_append (out, "&lt;"); break;
      case '>': g_string_append (out, "&gt;"); break;
      default: g_string_append_c (out, *s);
      }
}

/* the text between the tags through the stages, the tags (also comments,
 * CDATA is not expected from speech-dispatcher) copied */
static gchar *
prepare_ssml (LinuxTtsText *t, const gchar *s, gsize n, const gchar *lang, RuStressMark mark)
{
  GString *out = g_string_sized_new (n + n / 2);
  gsize i = 0;

  while (i < n)
    {
      if (s[i] == '<')
        {
          gsize j = i + 1;
          char quote = 0;

          for (; j < n; j++)
            {
              if (quote != 0)
                {
                  if (s[j] == quote)
                    quote = 0;
                }
              else if (s[j] == '"' || s[j] == '\'')
                quote = s[j];
              else if (s[j] == '>')
                break;
            }
          if (j < n)
            j++;
          g_string_append_len (out, s + i, j - i);
          i = j;
        }
      else
        {
          gsize j = i;
          gchar *plain, *ready;

          while (j < n && s[j] != '<')
            j++;
          if (blank (s + i, j - i))
            g_string_append_len (out, s + i, j - i);
          else
            {
              gsize a = i, b = j;

              /* the normalizer trims; the spaces around a mark stay */
              while (g_ascii_isspace (s[a]))
                a++;
              while (g_ascii_isspace (s[b - 1]))
                b--;
              g_string_append_len (out, s + i, a - i);
              plain = unescape (s + a, b - a);
              ready = prepare_plain (t, plain, lang, mark);
              append_escaped (out, ready);
              g_string_append_len (out, s + b, j - b);
              g_free (ready);
              g_free (plain);
            }
          i = j;
        }
    }
  return g_string_free (out, FALSE);
}

EXPORT char *
linux_tts_text_prepare (LinuxTtsText *t, const char *text, long len, const char *lang, int ssml,
                        const char *mark)
{
  RuStressMark m = RU_STRESS_NONE;
  gsize n;
  gchar *copy, *out;

  if (t == NULL || text == NULL)
    return NULL;
  n = len < 0 ? strlen (text) : (gsize) len;
  if (n == 0 || !g_utf8_validate_len (text, n, NULL) || memchr (text, '\0', n) != NULL)
    return NULL;
  if (mark != NULL && (strcmp (mark, "+") == 0 || g_ascii_strcasecmp (mark, "plus") == 0))
    m = RU_STRESS_PLUS;
  else if (mark != NULL && g_ascii_strcasecmp (mark, "acute") == 0)
    m = RU_STRESS_ACUTE;

  g_mutex_lock (&t->lock);
  if (ssml)
    out = prepare_ssml (t, text, n, lang, m);
  else
    {
      copy = g_strndup (text, n);
      out = prepare_plain (t, copy, lang, m);
      g_free (copy);
    }
  g_mutex_unlock (&t->lock);
  if (out != NULL && strlen (out) == n && memcmp (out, text, n) == 0)
    {
      g_free (out);
      return NULL;
    }
  return out;
}
