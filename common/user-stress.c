/*
 * user-stress - see user-stress.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "user-stress.h"

#include <string.h>

#define ACUTE 0x0301
/* a marked word is looked for this many words ahead of the last one found:
 * the normalizer adds words (numbers, abbreviations) but keeps the order */
#define LOOKAHEAD 64
/* marked words a found one may jump over */
#define SKIP_AHEAD 3

typedef struct
{
  gchar *word;          /* casefolded, without marks */
  guint64 vowels;       /* bit k: the k-th vowel is stressed */
  gboolean acute;       /* marked with U+0301, else with "+" */
  guint same_before;    /* unmarked equal words since the last marked one */
} Marked;

struct UserStress
{
  GArray *words;        /* Marked, in text order */
};

static gboolean
is_letter (gunichar c)
{
  return (c >= 0x0410 && c <= 0x044F) || c == 0x0401 || c == 0x0451;
}

static gboolean
is_vowel (gunichar c)
{
  return c != 0 && g_utf8_strchr ("аеёиоуыэюяАЕЁИОУЫЭЮЯ", -1, c) != NULL;
}

/* "+" is a mark before a vowel at a word start or inside a word, not after a
 * digit or a Latin letter ("2+она" is a sum) */
static gboolean
is_plus_mark (const gunichar *t, glong i, glong n)
{
  return t[i] == '+' && i + 1 < n && is_vowel (t[i + 1])
         && (i == 0 || is_letter (t[i - 1])
             || !(g_unichar_isalnum (t[i - 1]) || t[i - 1] == '+' || t[i - 1] == ACUTE));
}

static void
marked_clear (gpointer p)
{
  g_free (((Marked *) p)->word);
}

UserStress *
user_stress_take (const gchar *text, gchar **plain)
{
  glong n;
  gunichar *t;
  GString *out;
  UserStress *u = NULL;
  GPtrArray *since;     /* unmarked words since the last marked one */

  *plain = NULL;
  if (text == NULL || (strstr (text, "\xcc\x81") == NULL && strchr (text, '+') == NULL))
    return NULL;
  since = g_ptr_array_new_with_free_func (g_free);
  t = g_utf8_to_ucs4_fast (text, -1, &n);
  out = g_string_sized_new (strlen (text));
  for (glong i = 0; i < n;)
    {
      GString *word;
      guint64 vowels = 0;
      gboolean acute = FALSE;
      int nv = 0;

      if (!is_letter (t[i]) && !is_plus_mark (t, i, n))
        {
          g_string_append_unichar (out, t[i++]);
          continue;
        }
      word = g_string_new (NULL);
      while (i < n && (is_letter (t[i]) || t[i] == ACUTE || is_plus_mark (t, i, n)))
        {
          if (t[i] == '+')
            {
              if (nv < 64)
                vowels |= G_GUINT64_CONSTANT (1) << nv;   /* the next vowel */
            }
          else if (t[i] == ACUTE)
            {
              /* after a vowel; a stray one is dropped */
              if (nv > 0 && nv <= 64 && is_vowel (t[i - 1]))
                {
                  vowels |= G_GUINT64_CONSTANT (1) << (nv - 1);
                  acute = TRUE;
                }
            }
          else
            {
              nv += is_vowel (t[i]);
              g_string_append_unichar (word, t[i]);
            }
          i++;
        }
      g_string_append_len (out, word->str, word->len);
      if (vowels != 0 && word->len > 0)
        {
          Marked m = { g_utf8_casefold (word->str, word->len), vowels, acute, 0 };

          for (guint k = 0; k < since->len; k++)
            m.same_before += strcmp (since->pdata[k], m.word) == 0;
          g_ptr_array_set_size (since, 0);

          if (u == NULL)
            {
              u = g_new0 (UserStress, 1);
              u->words = g_array_new (FALSE, FALSE, sizeof (Marked));
              g_array_set_clear_func (u->words, marked_clear);
            }
          g_array_append_val (u->words, m);
        }
      else if (word->len > 0)
        g_ptr_array_add (since, g_utf8_casefold (word->str, word->len));
      g_string_free (word, TRUE);
    }
  g_free (t);
  g_ptr_array_free (since, TRUE);
  if (u == NULL)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  *plain = g_string_free (out, FALSE);
  return u;
}

gchar *
user_stress_put (const UserStress *u, const gchar *text)
{
  glong n;
  gunichar *t;
  GString *out;
  guint next = 0;       /* the next marked word to place */
  guint skipped = 0;    /* words passed while looking for it */
  guint same = 0;       /* equal words passed since the last one placed */

  if (u == NULL || text == NULL)
    return g_strdup (text);
  t = g_utf8_to_ucs4_fast (text, -1, &n);
  out = g_string_sized_new (strlen (text) + 2 * u->words->len);
  for (glong i = 0; i < n;)
    {
      glong j = i;
      gchar *utf8, *fold;
      const Marked *m;

      if (!is_letter (t[i]))
        {
          g_string_append_unichar (out, t[i++]);
          continue;
        }
      while (j < n && is_letter (t[j]))
        j++;
      utf8 = g_ucs4_to_utf8 (t + i, j - i, NULL, NULL, NULL);
      fold = g_utf8_casefold (utf8, -1);
      /* a marked word the normalizer changed is given up after LOOKAHEAD
       * words, so that it does not hold back the marks after it */
      while (next < u->words->len && skipped >= LOOKAHEAD)
        {
          next++;
          skipped = 0;
          same = 0;
        }
      /* or one of the few after it, when the normalizer changed this one */
      m = NULL;
      for (guint k = next; k < u->words->len && k <= next + SKIP_AHEAD; k++)
        if (strcmp (fold, g_array_index (u->words, Marked, k).word) == 0)
          {
            /* "дом до́м": the second "дом" */
            if (k == next && same < g_array_index (u->words, Marked, k).same_before)
              {
                same++;
                break;
              }
            next = k;
            m = &g_array_index (u->words, Marked, k);
            break;
          }
      if (m != NULL)
        {
          int v = 0;

          for (glong k = i; k < j; k++)
            {
              gboolean st = is_vowel (t[k]) && v < 64 && (m->vowels >> v & 1);

              v += is_vowel (t[k]);
              if (st && !m->acute)
                g_string_append_c (out, '+');
              g_string_append_unichar (out, t[k]);
              if (st && m->acute)
                g_string_append_unichar (out, ACUTE);
            }
          next++;
          skipped = 0;
          same = 0;
        }
      else
        {
          g_string_append (out, utf8);
          skipped++;
        }
      g_free (fold);
      g_free (utf8);
      i = j;
    }
  g_free (t);
  return g_string_free (out, FALSE);
}

void
user_stress_free (UserStress *u)
{
  if (u == NULL)
    return;
  g_array_free (u->words, TRUE);
  g_free (u);
}
