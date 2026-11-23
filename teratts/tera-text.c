/*
 * tera-text - see tera-text.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tera-text.h"
#include "num-words.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define TERA_TEXT_ERROR (g_quark_from_static_string ("tera-text-error"))
#define TABLE_SIZE 65536

struct TeraText
{
  gint16 table[TABLE_SIZE];     /* code point -> id, -1: not in the model */
  GRegex *number_re;
  GRegex *groups_re;
  GRegex *digits_re;
};

static gboolean
table_load (TeraText *t, const gchar *path, GError **error)
{
  JsonParser *p = json_parser_new ();
  JsonNode *root;
  JsonArray *a;
  gboolean ok = FALSE;

  if (!json_parser_load_from_file (p, path, error))
    goto out;
  root = json_parser_get_root (p);
  if (!JSON_NODE_HOLDS_ARRAY (root)
      || json_array_get_length (a = json_node_get_array (root)) != TABLE_SIZE)
    {
      g_set_error (error, TERA_TEXT_ERROR, 1, "%s: not a table of %d ids", path, TABLE_SIZE);
      goto out;
    }
  for (guint i = 0; i < TABLE_SIZE; i++)
    t->table[i] = (gint16) json_array_get_int_element (a, i);
  ok = TRUE;
out:
  g_object_unref (p);
  return ok;
}


TeraText *
tera_text_new (const gchar *indexer, GError **error)
{
  TeraText *t = g_new0 (TeraText, 1);

  if (!table_load (t, indexer, error))
    {
      g_free (t);
      return NULL;
    }
  /* teratts.py TAGGED_NUMBER, but a full stop after the number may end
   * the sentence: "версия 3.14." */
  t->number_re = g_regex_new ("(?<![\\w.])[-\\x{2212}]?\\d+(?:[.,]\\d+)?(?!\\w|[.,]\\d)",
                              G_REGEX_OPTIMIZE, 0, NULL);
  /* digit groups: "21 000" (space, no-break space, narrow no-break space) */
  t->groups_re = g_regex_new ("(?<![\\d.,])\\d{1,3}(?:[ \\x{a0}\\x{202f}]\\d{3})+(?![\\d]|[.,]\\d)",
                              G_REGEX_OPTIMIZE, 0, NULL);
  /* what is left: "1.2.3", "5th" */
  t->digits_re = g_regex_new ("\\d+", 0, 0, NULL);
  return t;
}

void
tera_text_free (TeraText *t)
{
  if (t == NULL)
    return;
  if (t->number_re != NULL)
    g_regex_unref (t->number_re);
  if (t->groups_re != NULL)
    g_regex_unref (t->groups_re);
  if (t->digits_re != NULL)
    g_regex_unref (t->digits_re);
  g_free (t);
}

gint
tera_text_id (const TeraText *t, gunichar c)
{
  return c < TABLE_SIZE ? t->table[c] : -1;
}

/* ---------------------------------------------------------------- text */

static gboolean
in_table (TeraText *t, gunichar c)
{
  return c < TABLE_SIZE && t->table[c] >= 0;
}

/* every code point of c's NFKD form is in the table */
static gboolean
supported (TeraText *t, gunichar c)
{
  gchar buf[8];
  gchar *d;
  gboolean ok = TRUE;

  if (in_table (t, c))
    return TRUE;
  buf[g_unichar_to_utf8 (c, buf)] = '\0';
  d = g_utf8_normalize (buf, -1, G_NORMALIZE_NFKD);
  for (const gchar *p = d; ok && p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    ok = in_table (t, g_utf8_get_char (p));
  ok = ok && d != NULL && *d != '\0';
  g_free (d);
  return ok;
}

static gboolean
is_cyrillic (gunichar c)
{
  return c >= 0x0400 && c <= 0x04FF;
}

static gboolean
is_latin (gunichar c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
         || (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7);
}

static gboolean
is_ru_vowel (gunichar c)
{
  return c != 0 && g_utf8_strchr ("аеёиоуыэюяАЕЁИОУЫЭЮЯ", -1, c) != NULL;
}

/* Whitespace collapsed, a space after ,.!?;:… (not inside 3.5 or 3,5)
 * and between a digit and a letter, unsupported characters dropped
 * (digits kept for the number words), "+" kept only before a Russian
 * vowel (a stress mark; an acute after the vowel becomes one), "<" ">" dropped: the tags are added later. */
static gchar *
clean (TeraText *t, const gchar *text)
{
  gchar *nfc = g_utf8_normalize (text, -1, G_NORMALIZE_NFC);
  glong n;
  gunichar *u = nfc != NULL ? g_utf8_to_ucs4_fast (nfc, -1, &n) : NULL;
  GString *out = g_string_new (NULL);
  gboolean space = TRUE;        /* drop leading spaces */

  /* an acute after a Russian vowel ("замо́к") is the stress mark of
   * dictionaries: the model's "+" before the vowel instead */
  for (glong i = 1; u != NULL && i < n; i++)
    if (u[i] == 0x0301 && is_ru_vowel (u[i - 1]))
      {
        u[i] = u[i - 1];
        u[i - 1] = '+';
      }

  for (glong i = 0; u != NULL && i < n; i++)
    {
      gunichar c = u[i], next = i + 1 < n ? u[i + 1] : 0, prev = i > 0 ? u[i - 1] : 0;

      if (g_unichar_isspace (c))
        {
          if (!space)
            g_string_append_c (out, ' ');
          space = TRUE;
          continue;
        }
      /* a dash between words is a pause: "поехали — и ушёл" */
      if ((c == 0x2014 || c == 0x2013) && prev == ' ' && (next == ' ' || next == 0))
        {
          if (out->len >= 2 && g_unichar_isalnum (g_utf8_get_char (
                g_utf8_prev_char (out->str + out->len - 1))))
            {
              g_string_truncate (out, out->len - 1);
              g_string_append_c (out, ',');
              space = FALSE;
            }
          continue;
        }
      /* "&" is spelt out per language in add_run */
      if (c == '<' || c == '>' || (c == '+' && !is_ru_vowel (next))
          || (!g_unichar_isdigit (c) && c != '&' && !supported (t, c)))
        continue;
      g_string_append_unichar (out, c);
      space = FALSE;
      if (next != 0 && !g_unichar_isspace (next)
          && (c == 0x2026 || (c < 0x80 && strchr (",.!?;:", (int) c) != NULL))
          && !((c == '.' || c == ',') && g_unichar_isdigit (prev) && g_unichar_isdigit (next)))
        {
          g_string_append_c (out, ' ');
          space = TRUE;
        }
      else if (g_unichar_isdigit (c) && g_unichar_isalpha (next))
        {
          g_string_append_c (out, ' ');
          space = TRUE;
        }
    }
  while (out->len > 0 && out->str[out->len - 1] == ' ')
    g_string_truncate (out, out->len - 1);
  g_free (u);
  g_free (nfc);
  return g_string_free (out, FALSE);
}

/* "21 000" -> "21000" */
static gboolean
groups_cb (const GMatchInfo *m, GString *res, gpointer data)
{
  gchar *lit = g_match_info_fetch (m, 0);
  for (const gchar *p = lit; *p != '\0'; p = g_utf8_next_char (p))
    if (g_ascii_isdigit (*p))
      g_string_append_c (res, *p);
  g_free (lit);
  return FALSE;
}

static gboolean
number_cb (const GMatchInfo *m, GString *res, gpointer lang)
{
  gchar *lit = g_match_info_fetch (m, 0);
  gchar *w = num_words (lit, lang);
  g_string_append (res, w != NULL ? w : lit);   /* too long: the digits are dropped */
  g_free (w);
  g_free (lit);
  return FALSE;
}

/* one language run: numbers spelt out, digits left over dropped,
 * stress marks, the tags; TRUE if it has letters */
static gboolean
add_run (TeraText *t, GString *out, const gchar *run, const gchar *lang,
         TeraStressFunc stress, gpointer stress_data)
{
  gchar *w1 = g_regex_replace_eval (t->number_re, run, -1, 0, 0, number_cb,
                                    (gpointer) lang, NULL);
  gchar *words = g_regex_replace_eval (t->digits_re, w1, -1, 0, 0, number_cb,
                                       (gpointer) lang, NULL);
  GString *kept = g_string_new (NULL);
  gchar *marked;
  gboolean letters = FALSE;

  for (const gchar *p = words; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == '&')
        {
          if (kept->len > 0 && kept->str[kept->len - 1] != ' ')
            g_string_append_c (kept, ' ');
          g_string_append (kept, g_str_equal (lang, "ru") ? "и" : "and");
          if (p[1] != ' ' && p[1] != '\0')
            g_string_append_c (kept, ' ');
          continue;
        }
      if (!g_unichar_isdigit (c))
        g_string_append_unichar (kept, c);
      letters = letters || g_unichar_isalpha (c);
    }
  marked = stress != NULL && g_str_equal (lang, "ru") ? stress (kept->str, stress_data)
                                                      : g_strdup (kept->str);
  g_string_append_printf (out, "<%s>%s</%s>", lang, marked, lang);
  g_free (marked);
  g_string_free (kept, TRUE);
  g_free (words);
  g_free (w1);
  return letters;
}

gchar *
tera_text_prepare (TeraText *t, const gchar *text, const gchar *lang,
                   TeraStressFunc stress, gpointer stress_data)
{
  gchar *raw = clean (t, text);
  gchar *c = g_regex_replace_eval (t->groups_re, raw, -1, 0, 0, groups_cb, NULL, NULL);
  GString *out = g_string_new (NULL);
  const gchar *run_lang = NULL, *start = c, *p;
  gboolean letters = FALSE;
  gchar *nfkd;

  if (!g_str_equal (lang, "en"))
    lang = "ru";
  /* split into runs: a letter of the other script starts a new run;
   * spaces, punctuation and numbers stay with the run before them */
  for (p = c; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar ch = g_utf8_get_char (p);
      const gchar *l = is_cyrillic (ch) ? "ru" : is_latin (ch) ? "en" : NULL;
      if (l == NULL)
        continue;
      if (run_lang == NULL)
        run_lang = l;
      else if (l != run_lang)
        {
          gchar *r = g_strndup (start, p - start);
          letters |= add_run (t, out, r, run_lang, stress, stress_data);
          g_free (r);
          start = p;
          run_lang = l;
        }
    }
  if (*start != '\0')
    letters |= add_run (t, out, start, run_lang != NULL ? run_lang : lang, stress, stress_data);
  g_free (c);
  g_free (raw);
  if (!letters)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  nfkd = g_utf8_normalize (out->str, -1, G_NORMALIZE_NFKD);
  g_string_free (out, TRUE);
  return nfkd;
}

