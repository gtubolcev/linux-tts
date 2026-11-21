/*
 * ru-g2p - Russian text to IPA for the Russian Kokoro, see ru-g2p.h
 *
 * Reference: ru_g2p.py of zaakirio/kokoro-ru (Apache-2.0), misaki's
 * EspeakG2P and phonemizer 3.6 (punctuation preserved, ties), whose
 * output this reproduces character for character.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ru-g2p.h"
#include "ru-stress.h"
#include "espeak-g2p.h"

#include <string.h>

#define ACUTE 0x0301

struct RuG2p
{
  RuStress *stress;     /* NULL: no dictionary, no stress marks */
};



/* ------------------------------------------------------------- helpers */

static gboolean
is_ru_letter (gunichar c)
{
  return (c >= 0x0430 && c <= 0x044F) || c == 0x0451
         || (c >= 0x0410 && c <= 0x042F) || c == 0x0401;
}

static gboolean
is_ru_vowel (gunichar c)
{
  return c != 0 && g_utf8_strchr ("аеёиоуыэюя", -1, c) != NULL;
}

/* replaces every occurrence of old in s (in place: frees s) */
static gchar *
replace_all (gchar *s, const gchar *old, const gchar *new)
{
  gchar **parts;
  gchar *r;

  if (strstr (s, old) == NULL)
    return s;
  parts = g_strsplit (s, old, -1);
  r = g_strjoinv (new, parts);
  g_strfreev (parts);
  g_free (s);
  return r;
}

static gboolean
in_list (const gchar *s, const char *const *list)
{
  for (; *list != NULL; list++)
    if (strcmp (s, *list) == 0)
      return TRUE;
  return FALSE;
}

/* UCS-4 string builder */
typedef GArray UStr;

static UStr *
ustr_new (void)
{
  return g_array_new (TRUE, TRUE, sizeof (gunichar));
}

static void
ustr_add (UStr *u, gunichar c)
{
  g_array_append_val (u, c);
}

static gchar *
ustr_free_utf8 (UStr *u)
{
  gchar *s = g_ucs4_to_utf8 ((gunichar *) u->data, u->len, NULL, NULL, NULL);
  g_array_free (u, TRUE);
  return s;
}

/* the word without the stress marks */
static gchar *
strip_acute (const gunichar *w, glong n)
{
  UStr *u = ustr_new ();
  for (glong i = 0; i < n; i++)
    if (w[i] != ACUTE)
      ustr_add (u, w[i]);
  return ustr_free_utf8 (u);
}

/* bare (utf-8) with ACUTE after its vowel_n-th vowel */
static UStr *
seat_acute (const gchar *bare, int vowel_n)
{
  UStr *u = ustr_new ();
  int n = 0;
  for (const gchar *p = bare; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      ustr_add (u, c);
      if (is_ru_vowel (c) && ++n == vowel_n)
        ustr_add (u, ACUTE);
    }
  return u;
}



/* ------------------------------------------------------------ espeak */

RuG2p *
ru_g2p_new (const gchar *dict, const gchar *espeak_path, GError **error)
{
  RuG2p *g;

  if (!espeak_g2p_init (espeak_path, error) || !espeak_g2p_check_voice ("ru", error))
    return NULL;
  g = g_new0 (RuG2p, 1);
  if (dict != NULL)
    {
      GError *e = NULL;
      /* without a dictionary espeak-ng guesses the stress */
      if ((g->stress = ru_stress_new (dict, &e)) == NULL)
        {
          g_printerr ("ru-g2p: %s; the stress will be guessed\n", e->message);
          g_error_free (e);
        }
    }
  return g;
}

void
ru_g2p_free (RuG2p *g)
{
  if (g == NULL)
    return;
  ru_stress_free (g->stress);
  g_free (g);
}

/* ---------------------------------------------------------- respelling */

static const char *const ogo_blacklist[] = {
  "много", "немного", "намного", "ненамного", "строго", "нестрого",
  "настрого", "дорого", "недорого", "полого", "убого", "лего", "диего",
  "ого", "огого", NULL
};

static const char *const respell_substr[][2] = {
  { "солнц", "сонц" },
  { "чувств", "чуств" },
  { "здравств", "здраств" },
  { "счастлив", "счаслив" },
  { "завистлив", "завислив" },
  { "участлив", "учаслив" },
  { "совестлив", "совеслив" },
};

static const char *const respell_word_tab[][2] = {
  { "дорого", "дорога" },
  { "недорого", "недорога" },
  { "настрого", "настрога" },
  { "конечно", "конешно" },
  { "скучно", "скушно" },
  { "скучный", "скушный" },
  { "нарочно", "нарошно" },
  { "яичница", "яишница" },
  { "скворечник", "скворешник" },
  { "девичник", "девишник" },
  { "двоечник", "двоешник" },
  { "троечник", "троешник" },
  { "прачечная", "прашечная" },
  { "горчичник", "горчишник" },
  { "пустячный", "пустяшный" },
};

static glong
find_acute (const gunichar *w, glong n)
{
  for (glong i = 0; i < n; i++)
    if (w[i] == ACUTE)
      return i;
  return -1;
}

static int
vowels_before (const gunichar *w, glong idx)
{
  int n = 0;
  for (glong i = 0; i < idx; i++)
    if (is_ru_vowel (w[i]))
      n++;
  return n;
}

/* _respell_word(); appends the result to out */
static void
respell_word (const gunichar *w0, glong n, UStr *out)
{
  gunichar *w = g_memdup2 (w0, n * sizeof (gunichar));
  gchar *bare = strip_acute (w, n);
  gboolean any = FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (respell_word_tab); i++)
    if (strcmp (bare, respell_word_tab[i][0]) == 0)
      {
        glong idx = find_acute (w, n);
        UStr *u;
        if (idx > 0)
          u = seat_acute (respell_word_tab[i][1], vowels_before (w, idx));
        else
          u = seat_acute (respell_word_tab[i][1], 0);
        g_array_append_vals (out, u->data, u->len);
        g_array_free (u, TRUE);
        goto done;
      }
  if (!in_list (bare, ogo_blacklist))
    {
      /* ([а-яё́]*?[ое]́?)го(́?)$ -> \1во\2 */
      glong e = n;
      if (e > 0 && w[e - 1] == ACUTE)
        e--;
      if (e >= 3 && w[e - 1] == 0x043E && w[e - 2] == 0x0433)
        {
          glong b = e - 3;
          if (b >= 0 && w[b] == ACUTE)
            b--;
          if (b >= 0 && (w[b] == 0x043E || w[b] == 0x0435))
            {
              w[e - 2] = 0x0432;
              g_free (bare);
              bare = strip_acute (w, n);
            }
        }
    }
  for (guint i = 0; i < G_N_ELEMENTS (respell_substr); i++)
    any = any || strstr (bare, respell_substr[i][0]) != NULL;
  if (any)
    {
      glong idx = find_acute (w, n);
      int vowel_n = idx > 0 ? vowels_before (w, idx) : 0;
      UStr *u;
      for (guint i = 0; i < G_N_ELEMENTS (respell_substr); i++)
        bare = replace_all (bare, respell_substr[i][0], respell_substr[i][1]);
      u = seat_acute (bare, vowel_n);
      g_array_append_vals (out, u->data, u->len);
      g_array_free (u, TRUE);
      goto done;
    }
  g_array_append_vals (out, w, n);
done:
  g_free (bare);
  g_free (w);
}

/* respell(): over the runs of [а-яё́] (the text is lower case) */
static gchar *
respell (const gchar *text)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (text, -1, &n);
  UStr *out = ustr_new ();

  for (glong i = 0; i < n;)
    {
      if (is_ru_letter (t[i]) || t[i] == ACUTE)
        {
          glong j = i;
          while (j < n && (is_ru_letter (t[j]) || t[j] == ACUTE))
            j++;
          respell_word (t + i, j - i, out);
          i = j;
        }
      else
        ustr_add (out, t[i++]);
    }
  g_free (t);
  return ustr_free_utf8 (out);
}



/* ------------------------------------------------------ vowel reduction */

static gboolean
uin (gunichar c, const gchar *set)
{
  return g_utf8_strchr (set, -1, c) != NULL;
}

static void
reduce_token (const gunichar *t, glong n, UStr *out)
{
  gunichar *c = g_memdup2 (t, n * sizeof (gunichar));
  GArray *vowels = g_array_new (FALSE, FALSE, sizeof (glong));
  gboolean *stressed = g_new0 (gboolean, n + 1);
  GArray *sord = g_array_new (FALSE, FALSE, sizeof (int));
  glong primary = -1;

  for (glong i = 0; i < n; i++)
    if (uin (c[i], "aɑoeiuyʌəɪɐɛɨ"))
      g_array_append_val (vowels, i);
  if (vowels->len == 0)
    {
      g_array_append_vals (out, c, n);
      goto done;
    }
  for (glong i = 0; i < n; i++)
    if (c[i] == 0x02C8)
      {
        primary = i;
        break;
      }
  for (glong i = 0; i < n; i++)
    {
      if (c[i] != 0x02C8 && c[i] != 0x02CC)
        continue;
      if (c[i] == 0x02CC && primary != -1 && i > primary)
        continue;
      for (guint k = 0; k < vowels->len; k++)
        if (g_array_index (vowels, glong, k) > i)
          {
            stressed[g_array_index (vowels, glong, k)] = TRUE;
            break;
          }
    }
  for (guint k = 0; k < vowels->len; k++)
    if (stressed[g_array_index (vowels, glong, k)])
      {
        int o = k;
        g_array_append_val (sord, o);
      }
  for (guint ord = 0; ord < vowels->len; ord++)
    {
      glong i = g_array_index (vowels, glong, ord);
      int next = -1;
      if (stressed[i] || !uin (c[i], "aɑoʌə"))
        continue;
      if (i > 0 && (c[i - 1] == 0x02B2 || c[i - 1] == 'j'))
        {
          c[i] = 0x026A;  /* ɪ */
          continue;
        }
      for (guint s = 0; s < sord->len; s++)
        if (g_array_index (sord, int, s) > (int) ord)
          {
            next = g_array_index (sord, int, s);
            break;
          }
      c[i] = next == (int) ord + 1 || i == 0 ? 0x0250 /* ɐ */ : 0x0259 /* ə */;
    }
  for (glong k = 0; k < n; k++)
    if (!(primary != -1 && c[k] == 0x02CC && k > primary))
      ustr_add (out, c[k]);
done:
  g_array_free (sord, TRUE);
  g_free (stressed);
  g_array_free (vowels, TRUE);
  g_free (c);
}

/* reduce_vowels (split on ' ') then lengthen_sch (ɕ not before ː) */
static gchar *
reduce_vowels (const gchar *ps)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (ps, -1, &n);
  UStr *r = ustr_new (), *out = ustr_new ();
  glong start = 0;

  for (glong i = 0; i <= n; i++)
    if (i == n || t[i] == ' ')
      {
        reduce_token (t + start, i - start, r);
        if (i < n)
          ustr_add (r, ' ');
        start = i + 1;
      }
  for (guint i = 0; i < r->len; i++)
    {
      gunichar c = g_array_index (r, gunichar, i);
      ustr_add (out, c);
      if (c == 0x0255 && (i + 1 >= r->len || g_array_index (r, gunichar, i + 1) != 0x02D0))
        ustr_add (out, 0x02D0);
    }
  g_array_free (r, TRUE);
  g_free (t);
  return ustr_free_utf8 (out);
}



/* ----------------------------------------------------------- pipeline */

gchar *
ru_g2p_accentuate (RuG2p *g, const gchar *text)
{
  gchar *stressed, *marked;
  glong n;
  gunichar *t;
  UStr *out;

  if (g->stress == NULL)
    return g_utf8_strdown (text, -1);
  out = ustr_new ();
  /* before lower case: a capital letter picks the reading of a name */
  stressed = ru_stress_apply (g->stress, text, RU_STRESS_ACUTE);
  marked = g_utf8_strdown (stressed, -1);
  g_free (stressed);
  /* a stress the user set with "+" before the vowel: the acute after it */
  t = g_utf8_to_ucs4_fast (marked, -1, &n);
  g_free (marked);
  for (glong i = 0; i < n; i++)
    if (t[i] == '+' && i + 1 < n && is_ru_vowel (t[i + 1]))
      {
        ustr_add (out, t[++i]);
        if (t[i] != 0x0451)
          ustr_add (out, ACUTE);
      }
    else
      ustr_add (out, t[i]);
  g_free (t);
  return ustr_free_utf8 (out);
}

static gchar *
brackets (const gchar *text)
{
  gchar *s = g_strdup (text);
  for (gchar *p = s; *p != '\0'; p++)
    if (*p == '[' || *p == '{')
      *p = '(';
    else if (*p == ']' || *p == '}')
      *p = ')';
  return s;
}

gchar *
ru_g2p_phonemize_marked (RuG2p *g, const gchar *marked)
{
  static const char *const e2m[][2] = {
    /* misaki EspeakG2P, sorted() by key */
    { "a^ɪ", "I" }, { "a^ʊ", "W" }, { "d^z", "ʣ" }, { "d^ʒ", "ʤ" },
    { "e^ɪ", "A" }, { "o^ʊ", "O" }, { "s^s", "S" }, { "t^s", "ʦ" },
    { "t^ʃ", "ʧ" }, { "ɔ^ɪ", "Y" }, { "ə^ʊ", "Q" },
  };
  static const char *const normalize[][2] = {
    { "u\"", "u" }, { "ɭ", "l" }, { "ɵ", "o" }, { "ʑ", "ʒ" }, { "ʐ", "ʒ" },
    { "ʧʲ", "ʧ" },
  };
  gchar *b = brackets (marked);
  gchar *low = g_utf8_strdown (b, -1);
  gchar *s = respell (low), *ps, *r;

  (void) g;
  g_free (b);
  g_free (low);
  /* misaki: angles to curly quotes, parentheses to angles */
  s = replace_all (s, "«", "“");
  s = replace_all (s, "»", "”");
  s = replace_all (s, "(", "«");
  s = replace_all (s, ")", "»");
  ps = espeak_g2p_phonemize ("ru", s);
  g_free (s);
  g_strstrip (ps);
  for (guint i = 0; i < G_N_ELEMENTS (e2m); i++)
    ps = replace_all (ps, e2m[i][0], e2m[i][1]);
  ps = replace_all (ps, "^", "");
  ps = replace_all (ps, "-", "");
  ps = replace_all (ps, "«", "(");
  ps = replace_all (ps, "»", ")");
  for (guint i = 0; i < G_N_ELEMENTS (normalize); i++)
    ps = replace_all (ps, normalize[i][0], normalize[i][1]);
  r = reduce_vowels (ps);
  g_free (ps);
  return r;
}

gchar *
ru_g2p_phonemize (RuG2p *g, const gchar *text)
{
  gchar *b = brackets (text);
  gchar *m = ru_g2p_accentuate (g, b);
  gchar *r = ru_g2p_phonemize_marked (g, m);
  g_free (b);
  g_free (m);
  return r;
}
