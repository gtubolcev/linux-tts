/*
 * ru-stress - see ru-stress.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "ru-stress.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACUTE 0x0301
#define YO 0x0451               /* ё */
#define YO_UPPER 0x0401         /* Ё */

struct RuStress
{
  const char *dict;     /* mmap of the dictionary */
  gsize dict_size;
};

static gboolean
is_ru_letter (gunichar c)
{
  return (c >= 0x0410 && c <= 0x044F) || c == YO || c == YO_UPPER;
}

static gboolean
is_ru_vowel (gunichar c)
{
  return c != 0 && g_utf8_strchr ("аеёиоуыэюяАЕЁИОУЫЭЮЯ", -1, c) != NULL;
}

RuStress *
ru_stress_new (const gchar *dict, GError **error)
{
  RuStress *s;
  struct stat st;
  int fd = open (dict, O_RDONLY | O_CLOEXEC);

  if (fd < 0 || fstat (fd, &st) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "%s: %s", dict, g_strerror (errno));
      if (fd >= 0)
        close (fd);
      return NULL;
    }
  s = g_new0 (RuStress, 1);
  if (st.st_size > 0)
    {
      void *p = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED)
        {
          s->dict = p;
          s->dict_size = st.st_size;
        }
    }
  close (fd);
  return s;
}

void
ru_stress_free (RuStress *s)
{
  if (s == NULL)
    return;
  if (s->dict != NULL)
    munmap ((void *) s->dict, s->dict_size);
  g_free (s);
}

/* the phonemes of word (lower case), or NULL; binary search over lines */
static gchar *
dict_lookup (RuStress *s, const gchar *word)
{
  gsize lo = 0, hi = s->dict_size, wl = strlen (word);

  if (s->dict == NULL || wl == 0)
    return NULL;
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2, start = mid, tab;
      int c;

      /* the line that holds mid: [start, tab) is its word */
      while (start > lo && s->dict[start - 1] != '\n')
        start--;
      tab = start;
      while (tab < s->dict_size && s->dict[tab] != '\t' && s->dict[tab] != '\n')
        tab++;
      c = memcmp (s->dict + start, word, MIN (tab - start, wl));
      if (c == 0 && tab - start != wl)
        c = tab - start < wl ? -1 : 1;
      if (c == 0)
        {
          const char *p = s->dict + tab + 1;
          const char *nl = memchr (p, '\n', s->dict_size - (p - s->dict));
          return g_strndup (p, (nl != NULL ? nl : s->dict + s->dict_size) - p);
        }
      if (c < 0)
        {
          const char *nl = memchr (s->dict + tab, '\n', s->dict_size - tab);
          lo = nl != NULL ? (gsize) (nl - s->dict) + 1 : s->dict_size;
        }
      else
        hi = start;
    }
  return NULL;
}

/* For the vowels of word, in order: TRUE where the dictionary has the
 * stress, and whether that vowel is pronounced "o" (е read as ё).
 * FALSE when the word is unknown or its vowels do not match the phonemes. */
static gboolean
lookup_stress (RuStress *s, const gchar *lower, int n_vowels,
               gboolean *stressed, gboolean *as_o)
{
  gchar *ph = dict_lookup (s, lower);
  gchar **tok;
  int v = 0;
  gboolean ok = TRUE;

  /* "ё" is often written as "е"; the dictionary may know only one */
  if (ph == NULL && strstr (lower, "ё") != NULL)
    {
      gchar **parts = g_strsplit (lower, "ё", -1);
      gchar *e = g_strjoinv ("е", parts);
      g_strfreev (parts);
      ph = dict_lookup (s, e);
      g_free (e);
    }
  if (ph == NULL)
    return FALSE;
  tok = g_strsplit (ph, " ", -1);
  for (int k = 0; tok[k] != NULL && ok; k++)
    {
      gsize l = strlen (tok[k]);
      if (l < 2 || (tok[k][l - 1] != '0' && tok[k][l - 1] != '1'))
        continue;               /* a consonant */
      if (v == n_vowels)
        ok = FALSE;
      else
        {
          stressed[v] = tok[k][l - 1] == '1';
          as_o[v] = tok[k][0] == 'o';
          v++;
        }
    }
  g_strfreev (tok);
  g_free (ph);
  return ok && v == n_vowels;
}

static void
add_char (GString *out, gunichar c)
{
  g_string_append_unichar (out, c);
}

gchar *
ru_stress_apply (RuStress *s, const gchar *text, RuStressMark mark)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (text, -1, &n);
  GString *out = g_string_sized_new (strlen (text) + 32);

  for (glong i = 0; i < n;)
    {
      glong j = i;
      gboolean marked = FALSE;
      int nv = 0;

      if (!is_ru_letter (t[i]) && !(t[i] == '+' && i + 1 < n && is_ru_vowel (t[i + 1])))
        {
          add_char (out, t[i++]);
          continue;
        }
      /* one word: letters with the marks a user may have put in */
      while (j < n && (is_ru_letter (t[j]) || t[j] == ACUTE
                       || (t[j] == '+' && j + 1 < n && is_ru_vowel (t[j + 1]))))
        {
          marked = marked || t[j] == ACUTE || t[j] == '+';
          nv += is_ru_vowel (t[j]);
          j++;
        }
      if (!marked && nv >= 2)
        {
          gchar *word = g_ucs4_to_utf8 (t + i, j - i, NULL, NULL, NULL);
          gchar *lower = g_utf8_strdown (word, -1);
          gboolean *stressed = g_new0 (gboolean, nv), *as_o = g_new0 (gboolean, nv);

          if (lookup_stress (s, lower, nv, stressed, as_o))
            {
              int v = 0;
              for (glong k = i; k < j; k++)
                {
                  gunichar c = t[k];
                  gboolean st;

                  if (!is_ru_vowel (c))
                    {
                      add_char (out, c);
                      continue;
                    }
                  st = stressed[v];
                  if (st && as_o[v] && (c == 0x0435 || c == 0x0415))
                    c = c == 0x0435 ? YO : YO_UPPER;
                  v++;
                  if (st && mark == RU_STRESS_PLUS)
                    add_char (out, '+');
                  add_char (out, c);
                  /* ё needs no acute: it is always stressed */
                  if (st && mark == RU_STRESS_ACUTE && c != YO && c != YO_UPPER)
                    add_char (out, ACUTE);
                }
              i = j;
            }
          g_free (stressed);
          g_free (as_o);
          g_free (lower);
          g_free (word);
          if (i == j)
            continue;
        }
      for (; i < j; i++)
        add_char (out, t[i]);
    }
  g_free (t);
  return g_string_free (out, FALSE);
}
