/*
 * ru-stress - see ru-stress.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ru-stress.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACUTE 0x0301
#define YE 0x0435               /* е */
#define YE_UPPER 0x0415         /* Е */
#define YO 0x0451               /* ё */
#define YO_UPPER 0x0401         /* Ё */
#define MAX_DICTS 8
#define MAX_VARIANTS 16

typedef struct
{
  const char *map;
  gsize size;
  gboolean phonemes;    /* the Vosk format */
} Dict;

struct RuStress
{
  Dict dict[MAX_DICTS];
  int n_dicts;
};

/* what the text around a word says about it */
typedef enum
{
  WANT_ANY,             /* sentence start, all capitals: cannot tell */
  WANT_PROPER,          /* capital letter inside a sentence */
  WANT_COMMON,          /* small letter */
} Want;

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

static gboolean
dict_open (Dict *d, const gchar *path, GError **error)
{
  struct stat st;
  int fd;
  void *p;

  errno = 0;
  fd = open (path, O_RDONLY | O_CLOEXEC);

  if (fd < 0 || fstat (fd, &st) != 0 || st.st_size == 0)
    {
      int e = fd >= 0 && errno == 0 ? ENODATA : errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e),
                   "%s: %s", path, g_strerror (e));
      if (fd >= 0)
        close (fd);
      return FALSE;
    }
  p = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close (fd);
  if (p == MAP_FAILED)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "%s: %s", path, g_strerror (errno));
      return FALSE;
    }
  d->map = p;
  d->size = st.st_size;
  /* the stress format has Cyrillic after the tabs, Vosk only ASCII */
  d->phonemes = TRUE;
  for (gsize k = 0, n = MIN (d->size, 65536); k < n && d->phonemes; k++)
    if (d->map[k] == '\t')
      for (gsize m = k + 1; m < n && d->map[m] != '\n'; m++)
        if ((unsigned char) d->map[m] >= 0x80)
          {
            d->phonemes = FALSE;
            break;
          }
  return TRUE;
}

RuStress *
ru_stress_new (const gchar *dicts, GError **error)
{
  RuStress *s = g_new0 (RuStress, 1);
  gchar **paths = g_strsplit (dicts != NULL ? dicts : "", ":", -1);
  GString *why = g_string_new (NULL);

  for (int i = 0; paths[i] != NULL && s->n_dicts < MAX_DICTS; i++)
    {
      GError *e = NULL;

      if (*paths[i] == '\0')
        continue;
      if (dict_open (&s->dict[s->n_dicts], paths[i], &e))
        s->n_dicts++;
      else
        {
          g_string_append_printf (why, "%s%s", why->len > 0 ? "; " : "", e->message);
          g_error_free (e);
        }
    }
  g_strfreev (paths);
  if (s->n_dicts == 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "no stress dictionary: %s",
                   why->len > 0 ? why->str : "none given");
      g_string_free (why, TRUE);
      g_free (s);
      return NULL;
    }
  g_string_free (why, TRUE);
  return s;
}

void
ru_stress_free (RuStress *s)
{
  if (s == NULL)
    return;
  for (int i = 0; i < s->n_dicts; i++)
    munmap ((void *) s->dict[i].map, s->dict[i].size);
  g_free (s);
}

/* the value for key, or NULL; binary search over lines */
static gchar *
dict_lookup (const Dict *d, const gchar *key)
{
  gsize lo = 0, hi = d->size, wl = strlen (key);

  if (wl == 0)
    return NULL;
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2, start = mid, tab;
      int c;

      /* the line that holds mid: [start, tab) is its key */
      while (start > lo && d->map[start - 1] != '\n')
        start--;
      tab = start;
      while (tab < d->size && d->map[tab] != '\t' && d->map[tab] != '\n')
        tab++;
      c = memcmp (d->map + start, key, MIN (tab - start, wl));
      if (c == 0 && tab - start != wl)
        c = tab - start < wl ? -1 : 1;
      if (c == 0)
        {
          const char *p, *nl;

          if (tab >= d->size || d->map[tab] != '\t')
            return NULL;
          p = d->map + tab + 1;
          nl = memchr (p, '\n', d->size - (p - d->map));
          return g_strndup (p, (nl != NULL ? nl : d->map + d->size) - p);
        }
      if (c < 0)
        {
          const char *nl = memchr (d->map + tab, '\n', d->size - tab);
          lo = nl != NULL ? (gsize) (nl - d->map) + 1 : d->size;
        }
      else
        hi = start;
    }
  return NULL;
}

static gchar *
replace_yo (const gchar *lower)
{
  gchar **parts = g_strsplit (lower, "ё", -1);
  gchar *e = g_strjoinv ("е", parts);

  g_strfreev (parts);
  return e;
}

/* Vosk: stress and ё per vowel from the phonemes */
static gboolean
from_phonemes (const gchar *ph, int n_vowels, gboolean *stressed, gboolean *yo)
{
  gchar **tok = g_strsplit (ph, " ", -1);
  int v = 0, last = -1;
  gboolean ok = TRUE;

  for (int k = 0; tok[k] != NULL && ok; k++)
    {
      gsize l = strlen (tok[k]);
      if (l < 2 || (tok[k][l - 1] != '0' && tok[k][l - 1] != '1'))
        continue;               /* a consonant */
      if (v == n_vowels)
        ok = FALSE;
      else
        {
          /* an "o" on a stressed е is ё; checked against the letter later */
          yo[v] = tok[k][0] == 'o';
          if (tok[k][l - 1] == '1')
            last = v;
          v++;
        }
    }
  g_strfreev (tok);
  if (!ok || v != n_vowels || last < 0)
    return FALSE;
  for (int k = 0; k < n_vowels; k++)
    {
      stressed[k] = k == last;
      yo[k] = yo[k] && k == last;
    }
  return TRUE;
}

/* one variant of the stress format, as a sequence of letters */
typedef struct
{
  const gchar *form;    /* into the value; ends at ',' '/' or '\0' */
  gsize len;
  gboolean proper, rare, def;
} Variant;

static int
parse_variants (gchar *value, Variant *v)
{
  int n = 0;

  for (gchar *p = value; *p != '\0' && n < MAX_VARIANTS;)
    {
      gchar *end = p + strcspn (p, ",");

      v[n].form = p;
      v[n].len = strcspn (p, ",/");
      v[n].proper = v[n].rare = v[n].def = FALSE;
      for (gchar *f = p + v[n].len; f < end; f++)
        {
          v[n].proper |= *f == 'n';
          v[n].rare |= *f == 'r';
          v[n].def |= *f == 'd';
        }
      if (v[n].len > 0)
        n++;
      p = *end == ',' ? end + 1 : end;
    }
  return n;
}

static const Variant *
choose (const Variant *v, int n, Want want)
{
  gboolean ok[MAX_VARIANTS];
  int left = 0;

  /* rare readings only when there is nothing else */
  for (int i = 0; i < n; i++)
    left += ok[i] = !v[i].rare;
  if (left == 0)
    for (int i = 0; i < n; i++)
      left += ok[i] = TRUE;
  /* a proper name or a common word, if the dictionary has one */
  if (want != WANT_ANY)
    {
      int match = 0;
      for (int i = 0; i < n; i++)
        match += ok[i] && v[i].proper == (want == WANT_PROPER);
      if (match > 0)
        for (int i = 0; i < n; i++)
          ok[i] = ok[i] && v[i].proper == (want == WANT_PROPER);
    }
  for (int i = 0; i < n; i++)
    if (ok[i] && v[i].def)
      return &v[i];
  for (int i = 0; i < n; i++)
    if (ok[i])
      return &v[i];
  return NULL;
}

/* stress format: stress and ё per vowel from the chosen variant */
static gboolean
from_variant (const Variant *var, int n_vowels, gboolean *stressed, gboolean *yo)
{
  const gchar *p = var->form, *end = var->form + var->len;
  int v = 0, acute = -1, yo_at = -1;

  while (p < end)
    {
      gunichar c = g_utf8_get_char_validated (p, end - p);
      if (c == (gunichar) -1 || c == (gunichar) -2)
        return FALSE;
      p = g_utf8_next_char (p);
      if (c == ACUTE)
        {
          if (v == 0)
            return FALSE;
          acute = v - 1;
        }
      else if (is_ru_vowel (c))
        {
          if (v == n_vowels)
            return FALSE;
          yo[v] = c == YO;
          if (yo[v])
            yo_at = v;
          v++;
        }
    }
  if (v != n_vowels || (acute < 0 && yo_at < 0))
    return FALSE;
  for (int k = 0; k < n_vowels; k++)
    stressed[k] = k == (acute >= 0 ? acute : yo_at);
  return TRUE;
}

/* For the vowels of word, in order: where the stress is and which е is ё.
 * FALSE when no dictionary knows the word. */
static gboolean
lookup_stress (RuStress *s, const gchar *lower, int n_vowels, Want want,
               gboolean *stressed, gboolean *yo)
{
  gchar *key = replace_yo (lower);
  gboolean found = FALSE;

  for (int i = 0; i < s->n_dicts && !found; i++)
    {
      const Dict *d = &s->dict[i];
      gchar *val;

      if (d->phonemes)
        {
          /* "ё" is often written "е"; Vosk may know only one spelling */
          val = dict_lookup (d, lower);
          if (val == NULL && strcmp (key, lower) != 0)
            val = dict_lookup (d, key);
          if (val != NULL)
            found = from_phonemes (val, n_vowels, stressed, yo);
        }
      else
        {
          Variant var[MAX_VARIANTS];
          const Variant *c;

          val = dict_lookup (d, key);
          if (val != NULL && (c = choose (var, parse_variants (val, var), want)) != NULL)
            found = from_variant (c, n_vowels, stressed, yo);
        }
      g_free (val);
    }
  g_free (key);
  return found;
}

static Want
want_of (const gunichar *w, glong len, gboolean sentence_start)
{
  gboolean first_upper = g_unichar_isupper (w[0]), rest_upper = len > 1;

  for (glong k = 1; k < len; k++)
    if (g_unichar_islower (w[k]))
      rest_upper = FALSE;
  if (!first_upper)
    return WANT_COMMON;
  if (sentence_start || rest_upper)
    return WANT_ANY;
  return WANT_PROPER;
}

static gboolean
ends_sentence (gunichar c)
{
  return c == '.' || c == '!' || c == '?' || c == 0x2026 /* … */ || c == '\n';
}

gchar *
ru_stress_apply (RuStress *s, const gchar *text, RuStressMark mark)
{
  glong n;
  gunichar *t = g_utf8_to_ucs4_fast (text, -1, &n);
  GString *out = g_string_sized_new (strlen (text) + 32);
  gboolean sentence_start = TRUE;

  for (glong i = 0; i < n;)
    {
      glong j = i;
      gboolean marked = FALSE;
      int nv = 0;

      if (!is_ru_letter (t[i]) && !(t[i] == '+' && i + 1 < n && is_ru_vowel (t[i + 1])))
        {
          if (ends_sentence (t[i]))
            sentence_start = TRUE;
          else if (g_unichar_isalnum (t[i]))
            sentence_start = FALSE;
          g_string_append_unichar (out, t[i++]);
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
          gboolean *stressed = g_new0 (gboolean, nv), *yo = g_new0 (gboolean, nv);

          if (lookup_stress (s, lower, nv, want_of (t + i, j - i, sentence_start),
                             stressed, yo))
            {
              int v = 0;
              for (glong k = i; k < j; k++)
                {
                  gunichar c = t[k];
                  gboolean st;

                  if (!is_ru_vowel (c))
                    {
                      g_string_append_unichar (out, c);
                      continue;
                    }
                  st = stressed[v];
                  if (yo[v] && (c == YE || c == YE_UPPER))
                    c = c == YE ? YO : YO_UPPER;
                  v++;
                  if (st && mark == RU_STRESS_PLUS)
                    g_string_append_c (out, '+');
                  g_string_append_unichar (out, c);
                  /* ё needs no acute: it is always stressed */
                  if (st && mark == RU_STRESS_ACUTE && c != YO && c != YO_UPPER)
                    g_string_append_unichar (out, ACUTE);
                }
              i = j;
            }
          g_free (stressed);
          g_free (yo);
          g_free (lower);
          g_free (word);
        }
      sentence_start = FALSE;
      for (; i < j; i++)
        g_string_append_unichar (out, t[i]);
    }
  g_free (t);
  return g_string_free (out, FALSE);
}
