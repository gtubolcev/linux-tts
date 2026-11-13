/*
 * num-words - see num-words.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "num-words.h"

#include <string.h>

#define MAX_INT_DIGITS 15
#define MAX_FRAC_DIGITS 6

/* ------------------------------------------------------------- Russian */

static const char *const ru_ones_m[] = {
  "", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять",
  "десять", "одиннадцать", "двенадцать", "тринадцать", "четырнадцать",
  "пятнадцать", "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать",
};
static const char *const ru_tens[] = {
  "", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят",
  "семьдесят", "восемьдесят", "девяносто",
};
static const char *const ru_hundreds[] = {
  "", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот",
  "семьсот", "восемьсот", "девятьсот",
};
/* scale words: [form for 1, for 2-4, for 5-20 and 0] */
static const char *const ru_scale[][3] = {
  { "", "", "" },
  { "тысяча", "тысячи", "тысяч" },
  { "миллион", "миллиона", "миллионов" },
  { "миллиард", "миллиарда", "миллиардов" },
  { "триллион", "триллиона", "триллионов" },
};

/* 0 for 1, 1 for 2-4, 2 otherwise (11-14 included) */
static int
ru_plural (guint64 n)
{
  if (n % 100 >= 11 && n % 100 <= 14)
    return 2;
  if (n % 10 == 1)
    return 0;
  if (n % 10 >= 2 && n % 10 <= 4)
    return 1;
  return 2;
}

static void
add_word (GString *s, const char *w)
{
  if (*w == '\0')
    return;
  if (s->len > 0 && s->str[s->len - 1] != ' ')
    g_string_append_c (s, ' ');
  g_string_append (s, w);
}

/* 1..999; feminine: одна, две */
static void
ru_triple (GString *s, guint n, gboolean feminine)
{
  add_word (s, ru_hundreds[n / 100]);
  n %= 100;
  if (n >= 20)
    {
      add_word (s, ru_tens[n / 10]);
      n %= 10;
    }
  if (feminine && n == 1)
    add_word (s, "одна");
  else if (feminine && n == 2)
    add_word (s, "две");
  else
    add_word (s, ru_ones_m[n]);
}

static void
ru_cardinal (GString *s, guint64 n, gboolean feminine)
{
  guint g[5];
  int top = 0;

  if (n == 0)
    {
      add_word (s, "ноль");
      return;
    }
  for (int i = 0; i < 5; i++, n /= 1000)
    if ((g[i] = n % 1000) != 0)
      top = i;
  for (int i = top; i >= 0; i--)
    {
      if (g[i] == 0)
        continue;
      /* тысяча is feminine: одна тысяча, две тысячи */
      ru_triple (s, g[i], i == 1 || (i == 0 && feminine));
      add_word (s, ru_scale[i][ru_plural (g[i])]);
    }
}

static const char *const ru_frac[][2] = {
  { "", "" },
  { "десятая", "десятых" },
  { "сотая", "сотых" },
  { "тысячная", "тысячных" },
  { "десятитысячная", "десятитысячных" },
  { "стотысячная", "стотысячных" },
  { "миллионная", "миллионных" },
};

/* ------------------------------------------------------------- English */

static const char *const en_ones[] = {
  "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
  "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
  "seventeen", "eighteen", "nineteen",
};
static const char *const en_tens[] = {
  "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};
static const char *const en_scale[] = {
  "", "thousand", "million", "billion", "trillion",
};

/* 1..99 */
static void
en_pair (GString *s, guint n)
{
  if (n < 20)
    add_word (s, en_ones[n]);
  else
    {
      add_word (s, en_tens[n / 10]);
      if (n % 10 != 0)
        {
          g_string_append_c (s, '-');
          g_string_append (s, en_ones[n % 10]);
        }
    }
}

/* num2words: "one thousand, two hundred and thirty-four",
 * "one thousand and one" */
static void
en_cardinal (GString *s, guint64 n)
{
  guint g[5];
  int top = 0;
  gboolean first = TRUE;

  if (n == 0)
    {
      add_word (s, "zero");
      return;
    }
  for (int i = 0; i < 5; i++, n /= 1000)
    if ((g[i] = n % 1000) != 0)
      top = i;
  for (int i = top; i >= 0; i--)
    {
      if (g[i] == 0)
        continue;
      if (!first)
        {
          if (i == 0 && g[0] < 100)
            add_word (s, "and");
          else
            g_string_append_c (s, ',');
        }
      first = FALSE;
      if (g[i] >= 100)
        {
          add_word (s, en_ones[g[i] / 100]);
          add_word (s, "hundred");
          if (g[i] % 100 != 0)
            add_word (s, "and");
        }
      if (g[i] % 100 != 0)
        en_pair (s, g[i] % 100);
      add_word (s, en_scale[i]);
    }
}

/* ----------------------------------------------------------------- API */

gchar *
num_words (const gchar *literal, const gchar *lang)
{
  const gchar *p = literal;
  gboolean neg = FALSE, ru = g_str_equal (lang, "ru");
  gchar int_part[MAX_INT_DIGITS + 1], frac[MAX_FRAC_DIGITS + 1];
  gsize ni = 0, nf = 0;
  GString *s;

  if (*p == '-')
    neg = TRUE, p++;
  else if (g_str_has_prefix (p, "\xe2\x88\x92"))   /* U+2212 minus */
    neg = TRUE, p += 3;
  for (; g_ascii_isdigit (*p); p++)
    {
      if (ni == MAX_INT_DIGITS)
        return NULL;
      int_part[ni++] = *p;
    }
  int_part[ni] = '\0';
  if (ni == 0)
    return NULL;
  if (*p == '.' || *p == ',')
    for (p++; g_ascii_isdigit (*p); p++)
      {
        if (nf == MAX_FRAC_DIGITS)
          return NULL;
        frac[nf++] = *p;
      }
  if (*p != '\0')
    return NULL;
  /* as Python's float() does: 3.10 is 3.1 */
  while (nf > 0 && frac[nf - 1] == '0')
    nf--;
  frac[nf] = '\0';

  s = g_string_new (NULL);
  if (neg)
    add_word (s, ru ? "минус" : "minus");
  if (ru)
    {
      guint64 i = g_ascii_strtoull (int_part, NULL, 10);
      if (nf == 0)
        ru_cardinal (s, i, FALSE);
      else
        {
          guint64 f = g_ascii_strtoull (frac, NULL, 10);
          ru_cardinal (s, i, TRUE);
          add_word (s, ru_plural (i) == 0 ? "целая" : "целых");
          ru_cardinal (s, f, TRUE);
          add_word (s, ru_frac[nf][ru_plural (f) == 0 ? 0 : 1]);
        }
    }
  else
    {
      en_cardinal (s, g_ascii_strtoull (int_part, NULL, 10));
      if (nf > 0)
        {
          add_word (s, "point");
          for (gsize k = 0; k < nf; k++)
            add_word (s, en_ones[frac[k] - '0']);
        }
    }
  return g_string_free (s, FALSE);
}
