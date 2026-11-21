/*
 * espeak-g2p - text to IPA with espeak-ng the way Kokoro was trained, see
 * espeak-g2p.h
 *
 * Reference: phonemizer 3.6 (EspeakBackend, punctuation preserved, ties),
 * whose output this reproduces character for character, and misaki's
 * EspeakG2P and EspeakFallback (Apache-2.0).
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "espeak-g2p.h"
#include "en-g2p.h"

#include <espeak-ng/speak_lib.h>
#include <string.h>

static gboolean espeak_ready;
static gchar *espeak_voice;     /* the voice espeak-ng has loaded */
static GRegex *marks_re;        /* phonemizer's punctuation */
G_LOCK_DEFINE_STATIC (espeak);



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

gboolean
espeak_g2p_init (const gchar *path, GError **error)
{
  gboolean ok = TRUE;

  G_LOCK (espeak);
  if (!espeak_ready)
    {
      /* espeak_Initialize() only prints a missing phontab and goes on with
         sample rate garbage, so later calls divide by zero: check first */
      gchar *phontab = path ? g_build_filename (path, "espeak-ng-data", "phontab", NULL)
                            : NULL;
      gboolean have = !path || g_file_test (phontab, G_FILE_TEST_IS_REGULAR);

      g_free (phontab);
      if (!have)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                       "espeak-ng: no espeak-ng-data/phontab in %s", path);
          ok = FALSE;
        }
      else if (espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, path,
                             espeakINITIALIZE_DONT_EXIT) < 0)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "espeak-ng: cannot start with the data in %s", path);
          ok = FALSE;
        }
      else
        {
          /* phonemizer: (\s*(?:[others]|(?<![0-9])[,.]|[,.](?![0-9]))+\s*)+ */
          marks_re = g_regex_new ("(\\s*(?:[;:!?¡¿—…\"«»“”(){}\\[\\]]"
                                  "|(?<![0-9])[,.]|[,.](?![0-9]))+\\s*)+",
                                  G_REGEX_OPTIMIZE, 0, NULL);
          espeak_ready = TRUE;
        }
    }
  G_UNLOCK (espeak);
  return ok;
}

/* with the lock held */
static gboolean
use_voice (const gchar *voice)
{
  if (g_strcmp0 (espeak_voice, voice) == 0)
    return TRUE;
  g_free (espeak_voice);
  espeak_voice = NULL;
  /* by file name ("en-us" is gmw/en-US), else by language ("en-gb" is the
   * language of gmw/en, "fr-fr" of roa/fr), as phonemizer does */
  if (espeak_SetVoiceByName (voice) != EE_OK)
    {
      espeak_VOICE props = { 0 };
      espeak_VOICE *v;

      props.languages = voice;
      if (espeak_SetVoiceByProperties (&props) != EE_OK
          || (v = espeak_GetCurrentVoice ()) == NULL || v->languages == NULL
          || g_ascii_strcasecmp (v->languages + 1, voice) != 0)
        return FALSE;
    }
  espeak_voice = g_strdup (voice);
  return TRUE;
}

gboolean
espeak_g2p_check_voice (const gchar *voice, GError **error)
{
  gboolean ok;

  G_LOCK (espeak);
  ok = espeak_ready && use_voice (voice);
  G_UNLOCK (espeak);
  if (!ok)
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                 "espeak-ng: no voice \"%s\"", voice);
  return ok;
}



/* phonemizer EspeakBackend._phonemize_aux + _postprocess_line for one chunk,
 * tie '^', with stress, remove-flags, strip=False (a trailing space) */
static gchar *
espeak_chunk (const gchar *text)
{
  const void *ptr = text;
  GString *res = g_string_new (NULL);
  gchar *line, **words;
  GString *out;
  GRegex *re;

  while (ptr != NULL)
    {
      const char *ph = espeak_TextToPhonemes (&ptr, espeakCHARS_UTF8,
                                              espeakPHONEMES_IPA | espeakPHONEMES_TIE
                                              | (0x0361 << 8));
      if (ph != NULL && *ph != '\0')
        {
          if (res->len > 0)
            g_string_append_c (res, ' ');
          g_string_append (res, ph);
        }
    }
  line = g_strstrip (g_string_free (res, FALSE));
  line = replace_all (line, "\n", " ");
  /* Python str.replace('  ', ' '): one pass, non-overlapping */
  line = replace_all (line, "  ", " ");
  if (strstr (line, "__") != NULL)
    {
      re = g_regex_new ("_+", 0, 0, NULL);
      gchar *t = g_regex_replace_literal (re, line, -1, 0, "_", 0, NULL);
      g_regex_unref (re);
      g_free (line);
      line = t;
    }
  line = replace_all (line, "_ ", " ");
  /* RemoveFlags: (en) ... */
  re = g_regex_new ("\\(.+?\\)", 0, 0, NULL);
  {
    gchar *t = g_regex_replace_literal (re, line, -1, 0, "", 0, NULL);
    g_free (line);
    line = t;
  }
  g_regex_unref (re);
  if (*line == '\0')
    return line;
  words = g_strsplit (line, " ", -1);
  out = g_string_new (NULL);
  for (int i = 0; words[i] != NULL; i++)
    {
      gchar *w = replace_all (g_strdup (g_strstrip (words[i])), "͡", "^");
      if (i > 0)
        g_string_append_c (out, ' ');
      g_string_append (out, w);
      g_free (w);
    }
  g_string_append_c (out, ' ');
  g_strfreev (words);
  g_free (line);
  return g_string_free (out, FALSE);
}

static gboolean
is_inline_mark (const gchar *mark)
{
  gboolean any = FALSE;
  for (const gchar *p = mark; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isspace (c))
        continue;
      if (g_utf8_strchr ("\"«»“”‘’„‹›()[]{}", -1, c) == NULL)
        return FALSE;
      any = TRUE;
    }
  return any;
}

static int
count_words (const gchar *s)
{
  int n = 0;
  gchar **w = g_strsplit (s, " ", -1);
  for (int i = 0; w[i] != NULL; i++)
    if (*w[i] != '\0')
      n++;
  g_strfreev (w);
  return n;
}

/* Part of a line: a chunk (index into chunks) or a mark */
typedef struct
{
  int chunk;            /* -1 for a mark */
  gchar *mark;
} Part;

/* phonemizer phonemize([text], preserve_punctuation=True, strip=False) */
static gchar *
phonemize_punct (const gchar *text)
{
  GMatchInfo *mi;
  GPtrArray *chunks = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *ph;
  GArray *parts = g_array_new (FALSE, FALSE, sizeof (Part));
  gchar **in_context;
  gint cursor = 0;
  GString *out = g_string_new (NULL);
  const gchar *mark = NULL;

  /* preserve() */
  g_regex_match (marks_re, text, 0, &mi);
  while (g_match_info_matches (mi))
    {
      gint s, e;
      Part p;
      g_match_info_fetch_pos (mi, 0, &s, &e);
      if (s > cursor)
        {
          p.chunk = chunks->len;
          p.mark = NULL;
          g_array_append_val (parts, p);
          g_ptr_array_add (chunks, g_strndup (text + cursor, s - cursor));
        }
      p.chunk = -1;
      p.mark = g_strndup (text + s, e - s);
      g_array_append_val (parts, p);
      cursor = e;
      g_match_info_next (mi, NULL);
    }
  g_match_info_free (mi);
  if (text[cursor] != '\0')
    {
      Part p = { chunks->len, NULL };
      g_array_append_val (parts, p);
      g_ptr_array_add (chunks, g_strdup (text + cursor));
    }

  ph = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < chunks->len; i++)
    g_ptr_array_add (ph, espeak_chunk (chunks->pdata[i]));
  in_context = g_new0 (gchar *, chunks->len + 1);

  /* inline_groups(): chunks separated by inline marks only, phonemized as a
   * whole too; restore() uses them when the word counts agree */
  for (guint i = 0; i < parts->len;)
    {
      GArray *group = g_array_new (FALSE, FALSE, sizeof (int));
      guint j = i;
      while (j < parts->len && g_array_index (parts, Part, j).chunk < 0)
        j++;
      if (j >= parts->len)
        {
          g_array_free (group, TRUE);
          break;
        }
      g_array_append_val (group, g_array_index (parts, Part, j).chunk);
      while (j + 2 < parts->len
             && g_array_index (parts, Part, j + 2).chunk >= 0
             && is_inline_mark (g_array_index (parts, Part, j + 1).mark))
        {
          j += 2;
          g_array_append_val (group, g_array_index (parts, Part, j).chunk);
        }
      i = j + 1;
      if (group->len > 1)
        {
          GString *joined = g_string_new (NULL);
          gchar *whole, **pieces;
          int total = 0, *counts = g_new (int, group->len);
          gboolean ok = TRUE;

          for (guint k = 0; k < group->len; k++)
            {
              int c = g_array_index (group, int, k);
              if (k > 0)
                g_string_append_c (joined, ' ');
              g_string_append (joined, chunks->pdata[c]);
              counts[k] = count_words (ph->pdata[c]);
              ok = ok && counts[k] > 0;
              total += counts[k];
            }
          whole = espeak_chunk (joined->str);
          if (ok && count_words (whole) == total)
            {
              /* cut before the first word of each chunk but the first */
              GString *piece = g_string_new (NULL);
              int nwords = 0, k = 0, start = counts[0];
              gboolean first = TRUE;
              pieces = g_strsplit (whole, " ", -1);
              for (int n = 0; pieces[n] != NULL; n++)
                {
                  if (*pieces[n] != '\0')
                    {
                      if (nwords == start && k + 1 < (int) group->len)
                        {
                          in_context[g_array_index (group, int, k)] = g_string_free (piece, FALSE);
                          piece = g_string_new (NULL);
                          first = TRUE;
                          k++;
                          start += counts[k];
                        }
                      nwords++;
                    }
                  if (!first)
                    g_string_append_c (piece, ' ');
                  g_string_append (piece, pieces[n]);
                  first = FALSE;
                }
              in_context[g_array_index (group, int, k)] = g_string_free (piece, FALSE);
              g_strfreev (pieces);
            }
          g_free (whole);
          g_free (counts);
          g_string_free (joined, TRUE);
        }
      g_array_free (group, TRUE);
    }

  /* restore() */
  for (guint i = 0; i < parts->len; i++)
    {
      Part *p = &g_array_index (parts, Part, i);
      if (p->chunk < 0)
        {
          if (out->len > 0 && out->str[out->len - 1] == ' ')
            g_string_truncate (out, out->len - 1);
          g_string_append (out, p->mark);
          mark = p->mark;
        }
      else
        {
          const gchar *c = in_context[p->chunk] != NULL ? in_context[p->chunk]
                                                         : ph->pdata[p->chunk];
          if (*c != '\0')
            {
              g_string_append (out, c);
              mark = NULL;
            }
        }
    }
  if (mark != NULL && !g_str_has_suffix (mark, " "))
    g_string_append_c (out, ' ');

  for (guint i = 0; i < parts->len; i++)
    g_free (g_array_index (parts, Part, i).mark);
  g_array_free (parts, TRUE);
  g_strfreev (in_context);
  g_ptr_array_free (ph, TRUE);
  g_ptr_array_free (chunks, TRUE);
  return g_string_free (out, FALSE);
}


gchar *
espeak_g2p_phonemize (const gchar *voice, const gchar *text)
{
  gchar *r;

  G_LOCK (espeak);
  if (!espeak_ready || !use_voice (voice))
    r = g_strdup ("");
  else
    r = phonemize_punct (text);
  G_UNLOCK (espeak);
  return r;
}



/* ------------------------------------------------------------- Kokoro */

/* the espeak-ng voice for a language of Kokoro's (its pipeline.py) */
static const struct
{
  const gchar *lang;
  const gchar *voice;
} kokoro_langs[] = {
  { "en-US", "en-us" }, { "en-GB", "en-gb" }, { "es", "es" },
  { "fr", "fr-fr" }, { "hi", "hi" }, { "it", "it" }, { "pt-BR", "pt-br" },
};

static const gchar *
kokoro_voice (const gchar *lang)
{
  for (guint i = 0; i < G_N_ELEMENTS (kokoro_langs); i++)
    if (g_ascii_strcasecmp (kokoro_langs[i].lang, lang) == 0)
      return kokoro_langs[i].voice;
  return NULL;
}

gboolean
kokoro_g2p_speaks (const gchar *lang)
{
  const gchar *voice = kokoro_voice (lang);

  return voice != NULL && espeak_g2p_check_voice (voice, NULL);
}

/* (\S)̩ -> ᵊ\1, then the remaining U+0329 out */
static gchar *
syllabic (gchar *ps)
{
  GRegex *re = g_regex_new ("(\\S)̩", 0, 0, NULL);
  gchar *r = g_regex_replace (re, ps, -1, 0, "ᵊ\\1", 0, NULL);

  g_regex_unref (re);
  g_free (ps);
  return replace_all (r, "̩", "");
}

/* misaki EspeakFallback (British or not, version 1.0): for the words the
 * dictionaries lack, or for the whole text without them */
static gchar *
english (const gchar *voice, gboolean british, const gchar *text)
{
  /* sorted by the length of the key, longest first, as misaki does */
  static const char *const e2m[][2] = {
    { "ʔˌn̩", "ʔn" }, { "ʔn̩", "ʔn" },
    { "a^ɪ", "I" }, { "a^ʊ", "W" }, { "d^ʒ", "ʤ" }, { "e^ɪ", "A" },
    { "t^ʃ", "ʧ" }, { "ɔ^ɪ", "Y" }, { "ə^l", "ᵊl" },
    { "ʲo", "jo" }, { "ʲə", "jə" },
    { "e", "A" }, { "ʲ", "" }, { "ɚ", "əɹ" }, { "r", "ɹ" }, { "x", "k" },
    { "ç", "k" }, { "ɐ", "ə" }, { "ɬ", "l" }, { "̃", "" },
  };
  gchar *ps = g_strstrip (espeak_g2p_phonemize (voice, text));

  for (guint i = 0; i < G_N_ELEMENTS (e2m); i++)
    ps = replace_all (ps, e2m[i][0], e2m[i][1]);
  ps = syllabic (ps);
  if (british)
    {
      ps = replace_all (ps, "e^ə", "ɛː");
      ps = replace_all (ps, "iə", "ɪə");
      ps = replace_all (ps, "ə^ʊ", "Q");
    }
  else
    {
      ps = replace_all (ps, "o^ʊ", "O");
      ps = replace_all (ps, "ɜːɹ", "ɜɹ");
      ps = replace_all (ps, "ɜː", "ɜɹ");
      ps = replace_all (ps, "ɪə", "iə");
      ps = replace_all (ps, "ː", "");
    }
  ps = replace_all (ps, "o", "ɔ");
  ps = replace_all (ps, "ɾ", "T");
  ps = replace_all (ps, "ʔ", "t");
  return replace_all (ps, "^", "");
}

/* misaki EspeakG2P, version 1.0 */
static gchar *
other (const gchar *voice, const gchar *text)
{
  static const char *const e2m[][2] = {
    /* sorted() by key */
    { "a^ɪ", "I" }, { "a^ʊ", "W" }, { "d^z", "ʣ" }, { "d^ʒ", "ʤ" },
    { "e^ɪ", "A" }, { "o^ʊ", "O" }, { "s^s", "S" }, { "t^s", "ʦ" },
    { "t^ʃ", "ʧ" }, { "ɔ^ɪ", "Y" }, { "ə^ʊ", "Q" },
  };
  gchar *s = g_strdup (text), *ps;

  s = replace_all (s, "«", "“");
  s = replace_all (s, "»", "”");
  s = replace_all (s, "(", "«");
  s = replace_all (s, ")", "»");
  ps = g_strstrip (espeak_g2p_phonemize (voice, s));
  g_free (s);
  for (guint i = 0; i < G_N_ELEMENTS (e2m); i++)
    ps = replace_all (ps, e2m[i][0], e2m[i][1]);
  ps = replace_all (ps, "^", "");
  ps = replace_all (ps, "-", "");
  ps = replace_all (ps, "«", "(");
  return replace_all (ps, "»", ")");
}

/* misaki's English dictionaries, opened when first needed */
static gchar *lexicon_dir;
static EnLexicon *lexicons[2];
static gboolean lexicon_tried[2];
G_LOCK_DEFINE_STATIC (lexicon);

void
kokoro_g2p_set_lexicon_dir (const gchar *dir)
{
  G_LOCK (lexicon);
  g_free (lexicon_dir);
  lexicon_dir = g_strdup (dir);
  for (int i = 0; i < 2; i++)
    {
      en_lexicon_free (lexicons[i]);
      lexicons[i] = NULL;
      lexicon_tried[i] = FALSE;
    }
  G_UNLOCK (lexicon);
}

static EnLexicon *
lexicon (gboolean british)
{
  EnLexicon *lex;

  G_LOCK (lexicon);
  if (!lexicon_tried[british] && lexicon_dir != NULL)
    {
      GError *error = NULL;
      lexicon_tried[british] = TRUE;
      lexicons[british] = en_lexicon_open (lexicon_dir, british, &error);
      if (lexicons[british] == NULL)
        {
          g_warning ("%s; English goes through espeak-ng alone", error->message);
          g_error_free (error);
        }
    }
  lex = lexicons[british];
  G_UNLOCK (lexicon);
  return lex;
}

typedef struct
{
  const gchar *voice;
  gboolean british;
} Fallback;

/* EspeakFallback for one word */
static gchar *
english_word (const gchar *word, gpointer data)
{
  Fallback *fb = data;
  return english (fb->voice, fb->british, word);
}

gchar *
kokoro_g2p (const gchar *lang, const gchar *text)
{
  const gchar *voice = kokoro_voice (lang);

  if (voice == NULL)
    return g_strdup ("");
  if (g_ascii_strncasecmp (lang, "en-", 3) == 0)
    {
      gboolean british = g_ascii_strcasecmp (lang, "en-GB") == 0;
      EnLexicon *lex = lexicon (british);
      Fallback fb = { voice, british };

      if (lex == NULL)
        return english (voice, british, text);
      return en_g2p (lex, text, english_word, &fb);
    }
  return other (voice, text);
}
