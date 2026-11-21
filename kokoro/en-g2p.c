/*
 * en-g2p - English text to Kokoro phonemes with misaki's dictionaries, see
 * en-g2p.h
 *
 * Reference: misaki/en.py of hexgrad/misaki fba1236 (Apache-2.0).  The
 * functions keep the names and the order of its checks; Python's None
 * stress is NAN here, None phonemes NULL.  spaCy's tokenizer is replaced by
 * splitting at whitespace and peeling punctuation off the ends; the tags come
 * from en-tagger (en_core_web_sm in C), and without its files from guessing
 * by the neighbours.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "en-g2p.h"
#include "en-tagger.h"

#include <math.h>
#include <string.h>

#define NONE NAN
#define PRI "ˈ"
#define SEC "ˌ"

static const gchar VOWELS[] = "AIOQWYaiuæɑɒɔəɛɜɪʊʌᵻ";
static const gchar CONSONANTS[] = "bdfhjklmnpstvwzðŋɡɹɾʃʒʤʧθ";
static const gchar NON_QUOTE_PUNCTS[] = ";:,.!?—…";
static const gchar PUNCTS[] = ";:,.!?—…\"“”";
static const gchar DIPHTHONGS[] = "AIOQWYʤʧ";
static const gchar US_TAUS[] = "AIOWYiuæɑəɛɪɹʊʌ";
static const gchar SUBTOKEN_JUNKS[] = "',-._‘’/";

typedef struct
{
  GMappedFile *mf;
  const gchar *data;
  guint32 *off;         /* line starts */
  guint n;
} Dict;

struct _EnLexicon
{
  gboolean british;
  Dict gold, silver;
  EnTagger *tagger;     /* NULL: guess_tag */
};

typedef struct
{
  int future_vowel;     /* -1 None, 0 False, 1 True */
  gboolean future_to;
} Ctx;

/* ------------------------------------------------------------ strings */

static gboolean
has_char (const gchar *set, gunichar c)
{
  return c != 0 && g_utf8_strchr (set, -1, c) != NULL;
}

static gunichar
last_char (const gchar *s, int back)
{
  const gchar *p = s + strlen (s);
  for (int i = 0; i <= back; i++)
    {
      if (p == s)
        return 0;
      p = g_utf8_prev_char (p);
    }
  return g_utf8_get_char (p);
}

/* s without its last @n characters */
static gchar *
drop_last (const gchar *s, int n)
{
  const gchar *p = s + strlen (s);
  for (int i = 0; i < n && p > s; i++)
    p = g_utf8_prev_char (p);
  return g_strndup (s, p - s);
}

static gchar *
replace (const gchar *s, const gchar *old, const gchar *new)
{
  gchar **parts = g_strsplit (s, old, -1);
  gchar *r = g_strjoinv (new, parts);
  g_strfreev (parts);
  return r;
}

static gboolean
is_upper (const gchar *s)
{
  gchar *u = g_utf8_strup (s, -1);
  gboolean r = strcmp (u, s) == 0;
  g_free (u);
  return r;
}

static gboolean
is_lower (const gchar *s)
{
  gchar *l = g_utf8_strdown (s, -1);
  gboolean r = strcmp (l, s) == 0;
  g_free (l);
  return r;
}

/* Python str.capitalize () */
static gchar *
capitalize (const gchar *s)
{
  gchar *rest, *r, first[8];
  int n;

  if (*s == '\0')
    return g_strdup (s);
  n = g_unichar_to_utf8 (g_unichar_totitle (g_utf8_get_char (s)), first);
  first[n] = '\0';
  rest = g_utf8_strdown (g_utf8_next_char (s), -1);
  r = g_strconcat (first, rest, NULL);
  g_free (rest);
  return r;
}

static gboolean
is_alpha (const gchar *s)
{
  if (*s == '\0')
    return FALSE;
  for (; *s != '\0'; s = g_utf8_next_char (s))
    if (!g_unichar_isalpha (g_utf8_get_char (s)))
      return FALSE;
  return TRUE;
}

static gboolean
has_vowel (const gchar *s)
{
  for (; *s != '\0'; s = g_utf8_next_char (s))
    if (has_char (VOWELS, g_utf8_get_char (s)))
      return TRUE;
  return FALSE;
}

static gboolean
all_junk (const gchar *s)
{
  for (; *s != '\0'; s = g_utf8_next_char (s))
    if (!has_char (SUBTOKEN_JUNKS, g_utf8_get_char (s)))
      return FALSE;
  return TRUE;
}

static gboolean
has_digit (const gchar *s)
{
  for (; *s != '\0'; s = g_utf8_next_char (s))
    if (g_unichar_isdigit (g_utf8_get_char (s)))
      return TRUE;
  return FALSE;
}

/* ------------------------------------------------------- dictionaries */

static gboolean
dict_open (Dict *d, const gchar *path, GError **error)
{
  gsize len;

  d->mf = g_mapped_file_new (path, FALSE, error);
  if (d->mf == NULL)
    return FALSE;
  d->data = g_mapped_file_get_contents (d->mf);
  len = g_mapped_file_get_length (d->mf);
  if (len == 0 || d->data[len - 1] != '\n' || len >= G_MAXUINT32)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: not a lexicon", path);
      return FALSE;
    }
  for (gsize i = 0; i < len; i++)
    d->n += d->data[i] == '\n';
  d->off = g_new (guint32, d->n);
  d->off[0] = 0;
  for (gsize i = 0, k = 1; i + 1 < len; i++)
    if (d->data[i] == '\n')
      d->off[k++] = i + 1;
  for (guint i = 0; i < d->n; i++)
    {
      const gchar *l = d->data + d->off[i];
      const gchar *tab = memchr (l, '\t', d->data + len - l);
      const gchar *nl = memchr (l, '\n', d->data + len - l);
      if (tab == NULL || tab > nl || tab == l)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                       "%s: line %u has no word", path, i + 1);
          return FALSE;
        }
    }
  return TRUE;
}

static void
dict_close (Dict *d)
{
  if (d->mf != NULL)
    g_mapped_file_unref (d->mf);
  g_free (d->off);
}

static int
keycmp (const gchar *w, const gchar *k)
{
  while (*w != '\0' && *k != '\t' && *w == *k)
    w++, k++;
  return (int) (guchar) *w - (*k == '\t' ? 0 : (int) (guchar) *k);
}

/* the readings of @w (the text after its tab) or NULL */
static const gchar *
dict_get (const Dict *d, const gchar *w)
{
  guint lo = 0, hi = d->n;

  while (lo < hi)
    {
      guint mid = lo + (hi - lo) / 2;
      const gchar *l = d->data + d->off[mid];
      int c = keycmp (w, l);
      if (c == 0)
        return strchr (l, '\t') + 1;
      if (c < 0)
        hi = mid;
      else
        lo = mid + 1;
    }
  return NULL;
}

/* dict_get on Lexicon.grow_dictionary(d): a lowercase word also as
 * Capitalized, a Capitalized one also in lowercase */
static const gchar *
grow_get (const Dict *d, const gchar *w)
{
  const gchar *v = dict_get (d, w);
  gchar *l, *c;

  if (v != NULL || g_utf8_strlen (w, -1) < 2)
    return v;
  l = g_utf8_strdown (w, -1);
  c = capitalize (w);
  if (strcmp (l, w) == 0)
    {
      if (strcmp (c, w) != 0)
        v = dict_get (d, c);
    }
  else if (strcmp (c, w) == 0)
    v = dict_get (d, l);
  g_free (l);
  g_free (c);
  return v;
}

static gboolean
val_is_dict (const gchar *v)
{
  return v[strcspn (v, "\t\n")] == '\t';
}

/* whether the readings @v have one for @tag; *ps gets it (NULL for misaki's
 * null) */
static gboolean
val_tag (const gchar *v, const gchar *tag, gchar **ps)
{
  gsize tl;
  const gchar *p = v + strcspn (v, "\t\n");

  if (tag == NULL)
    return FALSE;
  if (strcmp (tag, "DEFAULT") == 0)
    {
      if (ps != NULL)
        *ps = g_strndup (v, p - v);
      return TRUE;
    }
  tl = strlen (tag);
  while (*p == '\t')
    {
      const gchar *f = p + 1, *e = f + strcspn (f, "\t\n");
      if ((gsize) (e - f) > tl && strncmp (f, tag, tl) == 0 && f[tl] == '=')
        {
          if (ps != NULL)
            *ps = e > f + tl + 1 ? g_strndup (f + tl + 1, e - f - tl - 1) : NULL;
          return TRUE;
        }
      p = e;
    }
  return FALSE;
}

/* a single reading, or the default of several */
static gchar *
val_default (const gchar *v)
{
  return g_strndup (v, strcspn (v, "\t\n"));
}

static gboolean
gold_has (EnLexicon *lex, const gchar *w)
{
  return grow_get (&lex->gold, w) != NULL;
}

static gboolean
silver_has (EnLexicon *lex, const gchar *w)
{
  return grow_get (&lex->silver, w) != NULL;
}

static gchar *
gold (EnLexicon *lex, const gchar *w, const gchar *tag)
{
  const gchar *v = grow_get (&lex->gold, w);
  gchar *ps = NULL;

  if (v == NULL)
    return NULL;
  if (tag != NULL && val_tag (v, tag, &ps))
    return ps;
  return val_default (v);
}

EnLexicon *
en_lexicon_open (const gchar *dir, gboolean british, GError **error)
{
  EnLexicon *lex = g_new0 (EnLexicon, 1);
  const gchar *loc = british ? "gb" : "us";
  gchar *g = g_strdup_printf ("%s/%s-gold.tsv", dir, loc);
  gchar *s = g_strdup_printf ("%s/%s-silver.tsv", dir, loc);
  gboolean ok;

  lex->british = british;
  ok = dict_open (&lex->gold, g, error) && dict_open (&lex->silver, s, error);
  g_free (g);
  g_free (s);
  if (!ok)
    {
      en_lexicon_free (lex);
      return NULL;
    }
  {
    GError *e = NULL;
    lex->tagger = en_tagger_open (dir, &e);
    if (lex->tagger == NULL)
      {
        g_debug ("en-g2p: no tagger, guessing the tags: %s", e->message);
        g_error_free (e);
      }
  }
  return lex;
}

void
en_lexicon_free (EnLexicon *lex)
{
  if (lex == NULL)
    return;
  dict_close (&lex->gold);
  dict_close (&lex->silver);
  if (lex->tagger != NULL)
    en_tagger_free (lex->tagger);
  g_free (lex);
}

/* ------------------------------------------------------------- stress */

/* restress(mark + ps): the mark goes before the first vowel */
static gchar *
mark_first_vowel (const gchar *ps, const gchar *mark)
{
  for (const gchar *p = ps; *p != '\0'; p = g_utf8_next_char (p))
    if (has_char (VOWELS, g_utf8_get_char (p)))
      {
        gchar *head = g_strndup (ps, p - ps);
        gchar *r = g_strconcat (head, mark, p, NULL);
        g_free (head);
        return r;
      }
  return g_strdup (ps);
}

static gchar *
apply_stress (const gchar *ps, double stress)
{
  gboolean p, s;

  if (ps == NULL)
    return NULL;
  if (isnan (stress))
    return g_strdup (ps);
  p = strstr (ps, PRI) != NULL;
  s = strstr (ps, SEC) != NULL;
  if (stress < -1)
    {
      gchar *t = replace (ps, PRI, ""), *r = replace (t, SEC, "");
      g_free (t);
      return r;
    }
  if (stress == -1 || ((stress == 0 || stress == -0.5) && p))
    {
      gchar *t = replace (ps, SEC, ""), *r = replace (t, PRI, SEC);
      g_free (t);
      return r;
    }
  if ((stress == 0 || stress == 0.5 || stress == 1) && !p && !s)
    return has_vowel (ps) ? mark_first_vowel (ps, SEC) : g_strdup (ps);
  if (stress >= 1 && !p && s)
    return replace (ps, SEC, PRI);
  if (stress > 1 && !p && !s)
    return has_vowel (ps) ? mark_first_vowel (ps, PRI) : g_strdup (ps);
  return g_strdup (ps);
}

static int
stress_weight (const gchar *ps)
{
  int w = 0;
  for (const gchar *p = ps; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    w += has_char (DIPHTHONGS, g_utf8_get_char (p)) ? 2 : 1;
  return w;
}

/* ------------------------------------------------------------ Lexicon */

static const gchar *
parent_tag (const gchar *tag)
{
  if (tag == NULL)
    return NULL;
  if (g_str_has_prefix (tag, "VB"))
    return "VERB";
  if (g_str_has_prefix (tag, "NN"))
    return "NOUN";
  if (g_str_has_prefix (tag, "ADV") || g_str_has_prefix (tag, "RB"))
    return "ADV";
  if (g_str_has_prefix (tag, "ADJ") || g_str_has_prefix (tag, "JJ"))
    return "ADJ";
  return tag;
}

static gboolean
tag_is (const gchar *tag, const gchar *t)
{
  return tag != NULL && strcmp (tag, t) == 0;
}

static const gchar *
symbol (const gchar *w)
{
  static const char *const s[][2] = {
    { "%", "percent" }, { "&", "and" }, { "+", "plus" }, { "@", "at" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (s); i++)
    if (strcmp (w, s[i][0]) == 0)
      return s[i][1];
  return NULL;
}

static gchar *
get_NNP (EnLexicon *lex, const gchar *word)
{
  GString *acc = g_string_new (NULL);
  gchar *ps, *r;
  const gchar *last;

  for (const gchar *p = word; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gchar u[8], *v;
      if (!g_unichar_isalpha (c))
        continue;
      u[g_unichar_to_utf8 (g_unichar_toupper (c), u)] = '\0';
      v = gold (lex, u, NULL);
      if (v == NULL)
        {
          g_string_free (acc, TRUE);
          return NULL;
        }
      g_string_append (acc, v);
      g_free (v);
    }
  ps = apply_stress (acc->str, 0);
  g_string_free (acc, TRUE);
  /* rsplit(SEC, 1) joined with PRI */
  last = g_strrstr (ps, SEC);
  if (last == NULL)
    return ps;
  r = g_strdup_printf ("%.*s%s%s", (int) (last - ps), ps, PRI, last + strlen (SEC));
  g_free (ps);
  return r;
}

static gchar *lookup (EnLexicon *lex, const gchar *word, const gchar *tag, double stress, const Ctx *ctx);

static gboolean
is_vs (const gchar *w)
{
  return g_ascii_strcasecmp (w, "vs") == 0 || g_ascii_strcasecmp (w, "vs.") == 0;
}

static gchar *
get_special_case (EnLexicon *lex, const gchar *word, const gchar *tag, double stress, const Ctx *ctx)
{
  const gchar *sym = symbol (word);

  if (sym != NULL)
    return lookup (lex, sym, NULL, NONE, ctx);
  {
    /* '.' in word.strip('.') and word.replace('.', '').isalpha() and
     * len(max(word.split('.'), key=len)) < 3 */
    gsize n = strlen (word), a = 0, b = n;
    while (a < n && word[a] == '.')
      a++;
    while (b > a && word[b - 1] == '.')
      b--;
    if (memchr (word + a, '.', b - a) != NULL)
      {
        gchar *nodots = replace (word, ".", "");
        gchar **parts = g_strsplit (word, ".", -1);
        glong longest = 0;
        for (int i = 0; parts[i] != NULL; i++)
          longest = MAX (longest, g_utf8_strlen (parts[i], -1));
        gboolean nnp = is_alpha (nodots) && longest < 3;
        g_free (nodots);
        g_strfreev (parts);
        if (nnp)
          return get_NNP (lex, word);
      }
  }
  if (strcmp (word, "a") == 0 || strcmp (word, "A") == 0)
    return g_strdup (tag_is (tag, "DT") ? "ɐ" : "ˈA");
  if (strcmp (word, "am") == 0 || strcmp (word, "Am") == 0 || strcmp (word, "AM") == 0)
    {
      if (tag != NULL && g_str_has_prefix (tag, "NN"))
        return get_NNP (lex, word);
      if (ctx->future_vowel < 0 || strcmp (word, "am") != 0 || (!isnan (stress) && stress > 0))
        return gold (lex, "am", NULL);
      return g_strdup ("ɐm");
    }
  if (strcmp (word, "an") == 0 || strcmp (word, "An") == 0 || strcmp (word, "AN") == 0)
    {
      if (strcmp (word, "AN") == 0 && tag != NULL && g_str_has_prefix (tag, "NN"))
        return get_NNP (lex, word);
      return g_strdup ("ɐn");
    }
  if (strcmp (word, "I") == 0 && tag_is (tag, "PRP"))
    return g_strdup (SEC "I");
  if (strcmp (word, "to") == 0 || strcmp (word, "To") == 0
      || (strcmp (word, "TO") == 0 && (tag_is (tag, "TO") || tag_is (tag, "IN"))))
    {
      if (ctx->future_vowel < 0)
        return gold (lex, "to", NULL);
      return g_strdup (ctx->future_vowel ? "tʊ" : "tə");
    }
  if (strcmp (word, "in") == 0 || strcmp (word, "In") == 0
      || (strcmp (word, "IN") == 0 && !tag_is (tag, "NNP")))
    return g_strdup (ctx->future_vowel < 0 || !tag_is (tag, "IN") ? PRI "ɪn" : "ɪn");
  if (strcmp (word, "the") == 0 || strcmp (word, "The") == 0
      || (strcmp (word, "THE") == 0 && tag_is (tag, "DT")))
    return g_strdup (ctx->future_vowel == 1 ? "ði" : "ðə");
  if (tag_is (tag, "IN") && is_vs (word))
    return lookup (lex, "versus", NULL, NONE, ctx);
  if (strcmp (word, "used") == 0 || strcmp (word, "Used") == 0 || strcmp (word, "USED") == 0)
    {
      if ((tag_is (tag, "VBD") || tag_is (tag, "JJ")) && ctx->future_to)
        return gold (lex, "used", "VBD");
      return gold (lex, "used", NULL);
    }
  return NULL;
}

static gboolean
in_lexicon_ords (const gchar *w)
{
  for (; *w != '\0'; w++)
    if (!(*w == '\'' || *w == '-' || g_ascii_isalpha (*w)))
      return FALSE;
  return TRUE;
}

static gboolean
is_known (EnLexicon *lex, const gchar *word)
{
  gboolean r;
  gchar *rest;

  if (gold_has (lex, word) || symbol (word) != NULL || silver_has (lex, word))
    return TRUE;
  if (!is_alpha (word) || !in_lexicon_ords (word))
    return FALSE;
  if (g_utf8_strlen (word, -1) == 1)
    return TRUE;
  if (is_upper (word))
    {
      gchar *l = g_utf8_strdown (word, -1);
      r = gold_has (lex, l);
      g_free (l);
      if (r)
        return TRUE;
    }
  rest = (gchar *) g_utf8_next_char (word);
  return is_upper (rest);
}

static gchar *
lookup (EnLexicon *lex, const gchar *word0, const gchar *tag, double stress, const Ctx *ctx)
{
  gboolean is_nnp = FALSE;
  gchar *word = g_strdup (word0), *ps = NULL, *r;
  const gchar *v;

  if (is_upper (word) && !gold_has (lex, word))
    {
      gchar *l = g_utf8_strdown (word, -1);
      g_free (word);
      word = l;
      is_nnp = tag_is (tag, "NNP");
    }
  v = grow_get (&lex->gold, word);
  if (v == NULL && !is_nnp)
    v = grow_get (&lex->silver, word);
  if (v != NULL && val_is_dict (v))
    {
      const gchar *t = tag;
      if (ctx != NULL && ctx->future_vowel < 0 && val_tag (v, "None", NULL))
        t = "None";
      else if (!val_tag (v, t, NULL))
        t = parent_tag (t);
      if (!val_tag (v, t, &ps))
        ps = val_default (v);
    }
  else if (v != NULL)
    ps = val_default (v);
  if (ps == NULL || (is_nnp && strstr (ps, PRI) == NULL))
    {
      g_free (ps);
      ps = get_NNP (lex, word);
      g_free (word);
      /* both when it worked (rating 3) and when it did not (None) */
      return ps;
    }
  r = apply_stress (ps, stress);
  g_free (ps);
  g_free (word);
  return r;
}

static gchar *
add_s (EnLexicon *lex, gchar *stem)
{
  gunichar c;
  gchar *r;

  if (stem == NULL || *stem == '\0')
    {
      g_free (stem);
      return NULL;
    }
  c = last_char (stem, 0);
  if (has_char ("ptkfθ", c))
    r = g_strconcat (stem, "s", NULL);
  else if (has_char ("szʃʒʧʤ", c))
    r = g_strconcat (stem, lex->british ? "ɪ" : "ᵻ", "z", NULL);
  else
    r = g_strconcat (stem, "z", NULL);
  g_free (stem);
  return r;
}

static gchar *
known_stem (EnLexicon *lex, const gchar *word, int drop, const gchar *add)
{
  gchar *base = drop_last (word, drop);
  gchar *stem = g_strconcat (base, add, NULL);

  g_free (base);
  if (is_known (lex, stem))
    return stem;
  g_free (stem);
  return NULL;
}

static gchar *
stem_s (EnLexicon *lex, const gchar *word, const gchar *tag, double stress, const Ctx *ctx)
{
  glong n = g_utf8_strlen (word, -1);
  gchar *stem = NULL;

  if (n < 3 || !g_str_has_suffix (word, "s"))
    return NULL;
  if (!g_str_has_suffix (word, "ss"))
    stem = known_stem (lex, word, 1, "");
  if (stem == NULL && (g_str_has_suffix (word, "'s")
                       || (n > 4 && g_str_has_suffix (word, "es") && !g_str_has_suffix (word, "ies"))))
    stem = known_stem (lex, word, 2, "");
  if (stem == NULL && n > 4 && g_str_has_suffix (word, "ies"))
    stem = known_stem (lex, word, 3, "y");
  if (stem == NULL)
    return NULL;
  {
    gchar *ps = lookup (lex, stem, tag, stress, ctx);
    g_free (stem);
    return add_s (lex, ps);
  }
}

static gchar *
add_ed (EnLexicon *lex, gchar *stem)
{
  gunichar c;
  gchar *r;

  if (stem == NULL || *stem == '\0')
    {
      g_free (stem);
      return NULL;
    }
  c = last_char (stem, 0);
  if (has_char ("pkfθʃsʧ", c))
    r = g_strconcat (stem, "t", NULL);
  else if (c == 'd')
    r = g_strconcat (stem, lex->british ? "ɪ" : "ᵻ", "d", NULL);
  else if (c != 't')
    r = g_strconcat (stem, "d", NULL);
  else if (lex->british || g_utf8_strlen (stem, -1) < 2)
    r = g_strconcat (stem, "ɪd", NULL);
  else if (has_char (US_TAUS, last_char (stem, 1)))
    {
      gchar *b = drop_last (stem, 1);
      r = g_strconcat (b, "ɾᵻd", NULL);
      g_free (b);
    }
  else
    r = g_strconcat (stem, "ᵻd", NULL);
  g_free (stem);
  return r;
}

static gchar *
stem_ed (EnLexicon *lex, const gchar *word, const gchar *tag, double stress, const Ctx *ctx)
{
  glong n = g_utf8_strlen (word, -1);
  gchar *stem = NULL;

  if (n < 4 || !g_str_has_suffix (word, "d"))
    return NULL;
  if (!g_str_has_suffix (word, "dd"))
    stem = known_stem (lex, word, 1, "");
  if (stem == NULL && n > 4 && g_str_has_suffix (word, "ed") && !g_str_has_suffix (word, "eed"))
    stem = known_stem (lex, word, 2, "");
  if (stem == NULL)
    return NULL;
  {
    gchar *ps = lookup (lex, stem, tag != NULL ? tag : "VBN", stress, ctx);
    g_free (stem);
    return add_ed (lex, ps);
  }
}

static gchar *
add_ing (EnLexicon *lex, gchar *stem)
{
  gunichar c;
  gchar *r;

  if (stem == NULL || *stem == '\0')
    {
      g_free (stem);
      return NULL;
    }
  c = last_char (stem, 0);
  if (lex->british)
    {
      if (has_char ("əː", c))
        {
          g_free (stem);
          return NULL;
        }
    }
  else if (g_utf8_strlen (stem, -1) > 1 && c == 't' && has_char (US_TAUS, last_char (stem, 1)))
    {
      gchar *b = drop_last (stem, 1);
      r = g_strconcat (b, "ɾɪŋ", NULL);
      g_free (b);
      g_free (stem);
      return r;
    }
  r = g_strconcat (stem, "ɪŋ", NULL);
  g_free (stem);
  return r;
}

static gchar *
stem_ing (EnLexicon *lex, const gchar *word, const gchar *tag, double stress, const Ctx *ctx)
{
  glong n = g_utf8_strlen (word, -1);
  gsize b = strlen (word);
  gchar *stem = NULL;

  if (n < 5 || !g_str_has_suffix (word, "ing"))
    return NULL;
  if (n > 5)
    stem = known_stem (lex, word, 3, "");
  if (stem == NULL)
    stem = known_stem (lex, word, 3, "e");
  /* ([bcdgklmnprstvxz])\1ing$|cking$ */
  if (stem == NULL && n > 5 && b >= 5
      && ((word[b - 5] == word[b - 4] && strchr ("bcdgklmnprstvxz", word[b - 4]) != NULL)
          || g_str_has_suffix (word, "cking")))
    stem = known_stem (lex, word, 4, "");
  if (stem == NULL)
    return NULL;
  {
    gchar *ps = lookup (lex, stem, tag != NULL ? tag : "VBG", stress, ctx);
    g_free (stem);
    return add_ing (lex, ps);
  }
}

static gchar *
get_word (EnLexicon *lex, const gchar *word0, const gchar *tag, double stress, const Ctx *ctx)
{
  gchar *ps = get_special_case (lex, word0, tag, stress, ctx);
  gchar *word, *wl, *t;

  if (ps != NULL)
    return ps;
  word = g_strdup (word0);
  wl = g_utf8_strdown (word, -1);
  t = replace (word, "'", "");
  if (g_utf8_strlen (word, -1) > 1 && is_alpha (t) && strcmp (word, wl) != 0
      && (!tag_is (tag, "NNP") || g_utf8_strlen (word, -1) > 7)
      && !gold_has (lex, word) && !silver_has (lex, word)
      && (is_upper (word) || is_lower (g_utf8_next_char (word))))
    {
      gboolean known = gold_has (lex, wl) || silver_has (lex, wl);
      gchar *(*fn[]) (EnLexicon *, const gchar *, const gchar *, double, const Ctx *) = {
        stem_s, stem_ed, stem_ing,
      };
      for (guint i = 0; !known && i < G_N_ELEMENTS (fn); i++)
        {
          gchar *r = fn[i] (lex, wl, tag, stress, ctx);
          known = r != NULL;
          g_free (r);
        }
      if (known)
        {
          g_free (word);
          word = g_strdup (wl);
        }
    }
  g_free (t);
  g_free (wl);
  if (is_known (lex, word))
    ps = lookup (lex, word, tag, stress, ctx);
  else if (g_str_has_suffix (word, "s'") && (t = known_stem (lex, word, 2, "'s")) != NULL)
    {
      ps = lookup (lex, t, tag, stress, ctx);
      g_free (t);
    }
  else if (g_str_has_suffix (word, "'") && (t = known_stem (lex, word, 1, "")) != NULL)
    {
      ps = lookup (lex, t, tag, stress, ctx);
      g_free (t);
    }
  else if ((ps = stem_s (lex, word, tag, stress, ctx)) == NULL
           && (ps = stem_ed (lex, word, tag, stress, ctx)) == NULL)
    ps = stem_ing (lex, word, tag, isnan (stress) ? 0.5 : stress, ctx);
  g_free (word);
  return ps;
}

/* Lexicon.__call__ without numbers and currencies */
static gchar *
lexicon (EnLexicon *lex, const gchar *text, const gchar *tag, const Ctx *ctx)
{
  gchar *t = replace (text, "‘", "'"), *t2 = replace (t, "’", "'");
  gchar *word = g_utf8_normalize (t2, -1, G_NORMALIZE_NFKC);
  double stress;
  gchar *ps;

  g_free (t);
  g_free (t2);
  if (word == NULL)
    return NULL;
  stress = is_lower (word) ? NONE : is_upper (word) ? 2 : 0.5;
  ps = get_word (lex, word, tag, stress, ctx);
  g_free (word);
  return ps;
}

/* ------------------------------------------------------------ G2P */

typedef struct
{
  gchar *text;
  gchar *ps;
  gboolean prespace;
  gsize start, end;     /* bytes in the text, for the tags */
} Sub;

typedef struct
{
  GPtrArray *subs;      /* Sub *; one for a plain word or a mark */
  gboolean space;       /* whitespace after */
  gboolean mark;        /* punctuation, ps set */
  gboolean whole;       /* with digits: to the fallback as it is */
  gboolean sentence_start;
} Word;

static void
sub_free (gpointer p)
{
  Sub *s = p;
  g_free (s->text);
  g_free (s->ps);
  g_free (s);
}

static Sub *
sub_new (const gchar *text, gsize len)
{
  Sub *s = g_new0 (Sub, 1);
  s->text = g_strndup (text, len);
  return s;
}

/* the tags spaCy would give that matter here */
static const gchar *
tag_of (const gchar *w, gboolean sentence_start, gboolean mixed_case)
{
  if (strcmp (w, "a") == 0 || (strcmp (w, "A") == 0 && sentence_start))
    return "DT";
  if (strcmp (w, "I") == 0)
    return "PRP";
  if (g_ascii_strcasecmp (w, "to") == 0)
    return "TO";
  if (g_ascii_strcasecmp (w, "in") == 0 || is_vs (w))
    return "IN";
  if (g_ascii_strcasecmp (w, "the") == 0)
    return "DT";
  if (g_ascii_strcasecmp (w, "used") == 0)
    return "VBD";
  /* AM, US, FBI in mixed-case text */
  if (mixed_case && g_utf8_strlen (w, -1) > 1 && is_upper (w) && !is_lower (w))
    return "NNP";
  return NULL;
}

static gboolean
in_list (const gchar *w, const char *const *list)
{
  for (; *list != NULL; list++)
    if (g_ascii_strcasecmp (w, *list) == 0)
      return TRUE;
  return FALSE;
}

/* the words before which a word with readings by part of speech is a noun,
 * and a verb */
static const char *const BEFORE_NOUN[] = {
  "the", "a", "an", "this", "that", "these", "those", "its", "his", "her", "their", "our",
  "my", "your", "any", "each", "every", "some", "no", "of", "for", "such", "another", NULL,
};
static const char *const BEFORE_VERB[] = {
  "to", "will", "would", "can", "could", "may", "might", "must", "shall", "should", "not",
  "please", "don't", "doesn't", "didn't", "won't", "cannot", "can't", "let", "you", "we",
  "they", "I", NULL,
};
/* after "that" these make it a conjunction or a relative pronoun */
static const char *const AFTER_THAT_CONJ[] = {
  "the", "a", "an", "this", "these", "those", "you", "it", "he", "she", "we", "they", "I",
  "his", "her", "its", "their", "our", "my", "your", "there", "is", "are", "was", "were",
  "will", "would", "may", "might", "must", "can", "could", "shall", "should", "has", "have",
  "had", "does", "do", "did", "not", "no", "all", "any", "each", "every", "some", "which",
  "who", "what", "when", "if", "so", "as", "to", "in", "on", "of", "for", "by", "with",
  "from", "at", "be", "been", "only", "also", "one", "such", "but", "either", "neither",
  "nor", "without", "most", "then", "although", "though", "because", "unless", "whether",
  "until", "while", "where", "how", "why", NULL,
};
static const char *const BE[] = {
  "is", "are", "was", "were", "be", "been", "being", "get", "gets", "got", "am", "'s", NULL,
};

static gboolean
caps_word (const Word *w)
{
  const gchar *t;
  if (w == NULL || w->mark || w->subs->len != 1)
    return FALSE;
  t = ((Sub *) w->subs->pdata[0])->text;
  return g_utf8_strlen (t, -1) > 1 && is_upper (t) && !is_lower (t);
}

/* spaCy's tag for a word on its own, guessed from its neighbours */
static const gchar *
guess_tag (GArray *words, guint i, gboolean mixed)
{
  Word *w = &g_array_index (words, Word, i);
  Word *prev = i > 0 ? &g_array_index (words, Word, i - 1) : NULL;
  Word *next = i + 1 < words->len ? &g_array_index (words, Word, i + 1) : NULL;
  const gchar *text = ((Sub *) w->subs->pdata[0])->text;
  const gchar *pt = prev != NULL && !prev->mark && !prev->whole && prev->subs->len == 1
                    ? ((Sub *) prev->subs->pdata[0])->text : NULL;
  const gchar *nt = next != NULL ? ((Sub *) next->subs->pdata[0])->text : NULL;

  /* A in a heading or before a word */
  if (strcmp (text, "A") == 0 && next != NULL && !next->mark && !next->whole
      && g_ascii_strcasecmp (nt, "and") != 0 && g_ascii_strcasecmp (nt, "or") != 0)
    return "DT";
  /* the a) of a list */
  if (g_ascii_strcasecmp (text, "a") == 0 && next != NULL && next->mark && strcmp (nt, ")") == 0)
    return "NN";
  if (g_ascii_strcasecmp (text, "used") == 0 && pt != NULL && in_list (pt, BE))
    return "VBN";
  if (g_ascii_strcasecmp (text, "that") == 0)
    {
      if (next == NULL || (next->mark && strpbrk (nt, ".;!?") != NULL))
        return "DT";
      /* a determiner before a noun, but a relative pronoun before a verb
       * (that supports, that modified) */
      if (!next->mark && !next->whole && next->subs->len == 1 && !in_list (nt, AFTER_THAT_CONJ)
          && !(g_str_has_suffix (nt, "s") && !g_str_has_suffix (nt, "ss") && !g_str_has_suffix (nt, "'s"))
          && !g_str_has_suffix (nt, "ed"))
        return "DT";
      return "IN";
    }
  /* words in capitals: one alone is a name, several a heading */
  if (caps_word (w) && (caps_word (prev) || caps_word (next)))
    mixed = FALSE;
  {
    const gchar *t = tag_of (text, w->sentence_start, mixed);
    if (t != NULL)
      return t;
  }
  if (pt == NULL)
    return nt != NULL && strcmp (nt, "of") == 0 ? "NN" : NULL;
  if (in_list (pt, BEFORE_VERB))
    return "VB";
  if (in_list (pt, BEFORE_NOUN) || g_str_has_suffix (pt, "'s") || g_str_has_suffix (pt, "s'")
      || (nt != NULL && strcmp (nt, "of") == 0))
    return "NN";
  return NULL;
}

/* merge_tokens: the tag of the heaviest spaCy token (a capital weighs 2, any
 * other character 1; the first of equals) among those in bytes
 * [@start, @end) */
static const gchar *
span_tag (GArray *tags, const gchar *text, gsize start, gsize end)
{
  const gchar *best = NULL;
  int bw = -1;

  for (guint i = 0; i < tags->len; i++)
    {
      const EnTag *t = &g_array_index (tags, EnTag, i);
      int w = 0;
      if (t->end <= start)
        continue;
      if (t->start >= end)
        break;
      for (const gchar *p = text + t->start; p < text + t->end; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);
          w += g_unichar_tolower (c) == c ? 1 : 2;
        }
      if (w > bw)
        {
          bw = w;
          best = t->tag;
        }
    }
  return best;
}

static Ctx
token_context (Ctx ctx, const gchar *ps, const gchar *text, const gchar *tag)
{
  Ctx r = ctx;

  for (const gchar *p = ps; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (has_char (NON_QUOTE_PUNCTS, c))
        {
          r.future_vowel = -1;
          break;
        }
      if (has_char (VOWELS, c) || has_char (CONSONANTS, c))
        {
          r.future_vowel = has_char (VOWELS, c);
          break;
        }
    }
  r.future_to = strcmp (text, "to") == 0 || strcmp (text, "To") == 0
                || (strcmp (text, "TO") == 0 && (tag_is (tag, "TO") || tag_is (tag, "IN")));
  return r;
}

static gboolean
is_peeled (gunichar c)
{
  return has_char (".,;:!?…—–\"“”«»„()[]{}*¡¿", c);
}

/* the phonemes misaki gives a punctuation token: brackets and quotes by
 * their tag, else the characters of PUNCTS; NULL for none */
static gchar *
mark_ps (gunichar c, gboolean leading)
{
  gchar buf[8];

  if (has_char ("([{", c))
    return g_strdup ("(");
  if (has_char (")]}", c))
    return g_strdup (")");
  if (c == '"' || c == 0xab || c == 0xbb || c == 0x201e)
    return g_strdup (c == 0xab || c == 0x201e || (c == '"' && leading) ? "“" : "”");
  if (!has_char (PUNCTS, c))
    return NULL;
  buf[g_unichar_to_utf8 (c, buf)] = '\0';
  return g_strdup (buf);
}

static void
add_mark (GArray *words, gunichar c, gboolean leading)
{
  gchar *ps = mark_ps (c, leading);
  Word w = { 0 };
  gchar buf[8];

  if (ps == NULL)
    return;
  buf[g_unichar_to_utf8 (c, buf)] = '\0';
  w.subs = g_ptr_array_new_with_free_func (sub_free);
  w.mark = TRUE;
  g_ptr_array_add (w.subs, sub_new (buf, strlen (buf)));
  ((Sub *) w.subs->pdata[0])->ps = ps;
  g_array_append_val (words, w);
}

/* misaki's subtokenize() for a word without digits: letters with inner
 * apostrophes (split where a lowercase letter meets a capital), runs of
 * - and _, any other character alone; @core is at byte @base of the text */
static GPtrArray *
subtokenize (const gchar *core, gsize base)
{
  GPtrArray *subs = g_ptr_array_new_with_free_func (sub_free);
  const gchar *p = core;

  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      const gchar *q = g_utf8_next_char (p);

      if (g_unichar_isalpha (c))
        {
          const gchar *n1 = q, *n2 = *n1 ? g_utf8_next_char (n1) : n1;
          /* \p{Lu}(?=\p{Lu}\p{Ll}) */
          if (g_unichar_isupper (c) && *n1 && g_unichar_isupper (g_utf8_get_char (n1))
              && *n2 && g_unichar_islower (g_utf8_get_char (n2)))
            ;
          else
            for (gunichar prev = c; *q != '\0';)
              {
                gunichar d = g_utf8_get_char (q);
                if (g_unichar_isalpha (d) || g_unichar_ismark (d))
                  {
                    if (g_unichar_islower (prev) && g_unichar_isupper (d))
                      break;
                    prev = d;
                    q = g_utf8_next_char (q);
                  }
                else if (d == '\'' && q[1] != '\0' && g_unichar_isalpha (g_utf8_get_char (q + 1)))
                  {
                    q++;
                    prev = g_utf8_get_char (q);
                    q = g_utf8_next_char (q);
                  }
                else
                  break;
              }
        }
      else if (c == '-' || c == '_')
        while (*q == '-' || *q == '_')
          q++;
      else if (c == '\'')
        while (*q == '\'')
          q++;
      {
        Sub *x = sub_new (p, q - p);
        x->start = base + (p - core);
        x->end = base + (q - core);
        g_ptr_array_add (subs, x);
      }
      p = q;
    }
  return subs;
}

/* one whitespace-separated chunk */
static void
add_chunk (EnLexicon *lex, GArray *words, const gchar *chunk, gsize base,
           gboolean sentence_start, gboolean *squote)
{
  const gchar *a = chunk, *b = chunk + strlen (chunk);
  GArray *tail = g_array_new (FALSE, FALSE, sizeof (gunichar));
  gchar *core;
  gboolean changed = TRUE;
  Word w = { 0 };

  /* a lone dash */
  if (*chunk != '\0' && strspn (chunk, "-") == strlen (chunk))
    {
      add_mark (words, 0x2014, FALSE);
      g_array_free (tail, TRUE);
      return;
    }
  if (strcmp (chunk, "–") == 0)
    {
      add_mark (words, 0x2014, FALSE);
      g_array_free (tail, TRUE);
      return;
    }
  while (changed && a < b)
    {
      changed = FALSE;
      while (a < b && is_peeled (g_utf8_get_char (a)))
        {
          add_mark (words, g_utf8_get_char (a), TRUE);
          a = g_utf8_next_char (a);
          changed = TRUE;
        }
      /* `quoted' and ‘quoted’ */
      if (a < b && (*a == '`' || g_utf8_get_char (a) == 0x2018))
        {
          add_mark (words, '"', TRUE);
          *squote = TRUE;
          a = g_utf8_next_char (a);
          changed = TRUE;
        }
      if (b > a && *squote
          && (b[-1] == '\'' || g_utf8_get_char (g_utf8_prev_char (b)) == 0x2019))
        {
          gunichar q = '"';
          b = g_utf8_prev_char (b);
          g_array_prepend_val (tail, q);
          *squote = FALSE;
          changed = TRUE;
        }
      while (b > a && is_peeled (g_utf8_get_char (g_utf8_prev_char (b))))
        {
          gunichar c;
          b = g_utf8_prev_char (b);
          c = g_utf8_get_char (b);
          g_array_prepend_val (tail, c);
          changed = TRUE;
        }
      /* apostrophes: kept where the dictionaries have the word with them,
       * and after a plural s */
      if (a < b && *a == '\'')
        {
          gchar *t = g_strndup (a, b - a);
          if (!gold_has (lex, t) && !silver_has (lex, t))
            {
              a++;
              changed = TRUE;
            }
          g_free (t);
        }
      if (b > a + 1 && b[-1] == '\'' && b[-2] != 's' && b[-2] != 'S')
        {
          gchar *t = g_strndup (a, b - a);
          if (!gold_has (lex, t) && !silver_has (lex, t))
            {
              b--;
              changed = TRUE;
            }
          g_free (t);
        }
      else if (b == a + 1 && *a == '\'')
        {
          b--;
          changed = TRUE;
        }
    }
  /* spaCy keeps the dot of Mr., U.S., e.g., vs. */
  if (a < b && tail->len > 0 && g_array_index (tail, gunichar, 0) == '.')
    {
      gchar *with = g_strndup (a, b - a + 1);
      gchar *without = g_strndup (a, b - a);
      gchar *nodots = replace (without, ".", "");
      if (gold_has (lex, with) || is_vs (with) || (strchr (without, '.') != NULL && is_alpha (nodots)))
        {
          b++;
          g_array_remove_index (tail, 0);
        }
      g_free (with);
      g_free (without);
      g_free (nodots);
    }
  core = g_strndup (a, b - a);
  if (*core != '\0')
    {
      w.sentence_start = sentence_start;
      if (has_digit (core))
        {
          Sub *x = sub_new (core, strlen (core));
          x->start = base + (a - chunk);
          x->end = base + (b - chunk);
          w.whole = TRUE;
          w.subs = g_ptr_array_new_with_free_func (sub_free);
          g_ptr_array_add (w.subs, x);
        }
      else
        w.subs = subtokenize (core, base + (a - chunk));
      g_array_append_val (words, w);
    }
  g_free (core);
  for (guint i = 0; i < tail->len; i++)
    add_mark (words, g_array_index (tail, gunichar, i), FALSE);
  g_array_free (tail, TRUE);
}

static GArray *
tokenize (EnLexicon *lex, const gchar *text)
{
  GArray *words = g_array_new (FALSE, TRUE, sizeof (Word));
  const gchar *p = text;
  gboolean start = TRUE, squote = FALSE;

  while (*p != '\0')
    {
      const gchar *e;
      gchar *chunk;
      guint before = words->len;

      while (*p != '\0' && g_unichar_isspace (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      if (*p == '\0')
        break;
      for (e = p; *e != '\0' && !g_unichar_isspace (g_utf8_get_char (e)); e = g_utf8_next_char (e))
        ;
      chunk = g_strndup (p, e - p);
      add_chunk (lex, words, chunk, p - text, start, &squote);
      g_free (chunk);
      if (words->len > before)
        {
          Word *last = &g_array_index (words, Word, words->len - 1);
          last->space = TRUE;
          if (last->mark)
            start = strpbrk (((Sub *) last->subs->pdata[0])->ps, ".!?") != NULL;
          else
            start = FALSE;
        }
      p = e;
    }
  if (words->len > 0)
    g_array_index (words, Word, words->len - 1).space = FALSE;
  return words;
}

static gchar *
joined (GPtrArray *subs, guint from, guint to)
{
  GString *s = g_string_new (NULL);
  for (guint i = from; i < to; i++)
    g_string_append (s, ((Sub *) subs->pdata[i])->text);
  return g_string_free (s, FALSE);
}

typedef struct
{
  gboolean primary;
  int weight;
  guint i;
} Idx;

static int
idx_cmp (gconstpointer a, gconstpointer b)
{
  const Idx *x = a, *y = b;
  if (x->primary != y->primary)
    return x->primary - y->primary;
  if (x->weight != y->weight)
    return x->weight - y->weight;
  return (int) x->i - (int) y->i;
}

/* G2P.resolve_tokens */
static void
resolve_tokens (GPtrArray *subs)
{
  gchar *text = joined (subs, 0, subs->len);
  gboolean classes[3] = { FALSE, FALSE, FALSE };
  gboolean prespace;
  GArray *idx;
  int nprimary = 0;

  for (const gchar *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (has_char (SUBTOKEN_JUNKS, c))
        continue;
      classes[g_unichar_isalpha (c) ? 0 : (c >= '0' && c <= '9') ? 1 : 2] = TRUE;
    }
  prespace = strchr (text, ' ') != NULL || strchr (text, '/') != NULL
             || classes[0] + classes[1] + classes[2] > 1;
  for (guint i = 0; i < subs->len; i++)
    {
      Sub *s = subs->pdata[i];
      if (s->ps == NULL)
        {
          if (i == subs->len - 1 && g_utf8_strlen (s->text, -1) == 1
              && has_char (NON_QUOTE_PUNCTS, g_utf8_get_char (s->text)))
            s->ps = g_strdup (s->text);
          else if (all_junk (s->text))
            s->ps = g_strdup ("");
        }
      else if (i > 0)
        s->prespace = prespace;
    }
  g_free (text);
  if (prespace)
    return;
  idx = g_array_new (FALSE, FALSE, sizeof (Idx));
  for (guint i = 0; i < subs->len; i++)
    {
      Sub *s = subs->pdata[i];
      if (s->ps != NULL && *s->ps != '\0')
        {
          Idx x = { strstr (s->ps, PRI) != NULL, stress_weight (s->ps), i };
          nprimary += x.primary;
          g_array_append_val (idx, x);
        }
    }
  if (idx->len == 2 && g_utf8_strlen (((Sub *) subs->pdata[g_array_index (idx, Idx, 0).i])->text, -1) == 1)
    {
      Sub *s = subs->pdata[g_array_index (idx, Idx, 1).i];
      gchar *t = apply_stress (s->ps, -0.5);
      g_free (s->ps);
      s->ps = t;
    }
  else if (idx->len >= 2 && nprimary > ((int) idx->len + 1) / 2)
    {
      g_array_sort (idx, idx_cmp);
      for (guint k = 0; k < idx->len / 2; k++)
        {
          Sub *s = subs->pdata[g_array_index (idx, Idx, k).i];
          gchar *t = apply_stress (s->ps, -0.5);
          g_free (s->ps);
          s->ps = t;
        }
    }
  g_array_free (idx, TRUE);
}

/* the glued pieces of a word, right to left, as G2P.__call__ does for a
 * list of tokens; TRUE when they all resolved */
static gboolean
resolve_group (EnLexicon *lex, Word *w, Ctx *ctx, gboolean mixed, GArray *tags,
               const gchar *all)
{
  GPtrArray *subs = w->subs;
  guint left = 0, right = subs->len;

  while (left < right)
    {
      gchar *text = joined (subs, left, right);
      const gchar *tag = tags != NULL
                         ? span_tag (tags, all, ((Sub *) subs->pdata[left])->start,
                                     ((Sub *) subs->pdata[right - 1])->end)
                         : tag_of (text, w->sentence_start && left == 0, mixed);
      gchar *ps = lexicon (lex, text, tag, ctx);

      if (ps != NULL)
        {
          Sub *s = subs->pdata[left];
          g_free (s->ps);
          s->ps = ps;
          for (guint k = left + 1; k < right; k++)
            {
              Sub *x = subs->pdata[k];
              g_free (x->ps);
              x->ps = g_strdup ("");
            }
          *ctx = token_context (*ctx, ps, text, tag);
          right = left;
          left = 0;
        }
      else if (left + 1 < right)
        left++;
      else
        {
          Sub *s;
          right--;
          s = subs->pdata[right];
          if (s->ps == NULL)
            {
              if (all_junk (s->text))
                s->ps = g_strdup ("");
              else
                {
                  g_free (text);
                  return FALSE;
                }
            }
          left = 0;
        }
      g_free (text);
    }
  return TRUE;
}

gchar *
en_g2p (EnLexicon *lex, const gchar *text, EnFallback fallback, gpointer data)
{
  GArray *words;
  GPtrArray *out;
  GString *res;
  Ctx ctx = { -1, FALSE };
  gboolean mixed = FALSE;
  GArray *tags;
  gchar *r, *t;

  if (!g_utf8_validate (text, -1, NULL))
    return g_strdup ("");
  res = g_string_new (NULL);
  for (const gchar *p = text; *p != '\0' && !mixed; p = g_utf8_next_char (p))
    mixed = g_unichar_islower (g_utf8_get_char (p));
  words = tokenize (lex, text);
  tags = lex->tagger != NULL ? en_tagger_tag (lex->tagger, text) : NULL;
  out = g_ptr_array_new_full (words->len, g_free);
  g_ptr_array_set_size (out, words->len);
  for (guint i = words->len; i-- > 0;)
    {
      Word *w = &g_array_index (words, Word, i);
      Sub *s0 = w->subs->pdata[0];
      gchar *ps = NULL;

      if (w->mark)
        ps = g_strdup (s0->ps);
      else if (w->whole)
        ps = fallback (s0->text, data);
      else if (w->subs->len == 1)
        {
          const gchar *tag = tags != NULL ? span_tag (tags, text, s0->start, s0->end)
                                          : guess_tag (words, i, mixed);
          ps = lexicon (lex, s0->text, tag, &ctx);
          if (ps == NULL)
            ps = fallback (s0->text, data);
          ctx = token_context (ctx, ps, s0->text, tag);
          out->pdata[i] = ps;
          continue;
        }
      else if (!resolve_group (lex, w, &ctx, mixed, tags, text))
        {
          /* misaki keeps the context here as it was */
          gchar *all = joined (w->subs, 0, w->subs->len);
          out->pdata[i] = fallback (all, data);
          g_free (all);
          continue;
        }
      else
        {
          /* merge_tokens(unk='') */
          GString *m = g_string_new (NULL);
          resolve_tokens (w->subs);
          for (guint k = 0; k < w->subs->len; k++)
            {
              Sub *s = w->subs->pdata[k];
              if (s->prespace && m->len > 0 && !g_ascii_isspace (m->str[m->len - 1])
                  && s->ps != NULL && *s->ps != '\0')
                g_string_append_c (m, ' ');
              if (s->ps != NULL)
                g_string_append (m, s->ps);
            }
          out->pdata[i] = g_string_free (m, FALSE);
          continue;
        }
      ctx = token_context (ctx, ps, s0->text, NULL);
      out->pdata[i] = ps;
    }
  for (guint i = 0; i < words->len; i++)
    {
      Word *w = &g_array_index (words, Word, i);
      if (out->pdata[i] != NULL)
        g_string_append (res, out->pdata[i]);
      if (w->space)
        g_string_append_c (res, ' ');
      g_ptr_array_free (w->subs, TRUE);
    }
  g_array_free (words, TRUE);
  if (tags != NULL)
    g_array_free (tags, TRUE);
  g_ptr_array_free (out, TRUE);
  /* version 1.0 */
  t = replace (res->str, "ɾ", "T");
  r = replace (t, "ʔ", "t");
  g_free (t);
  g_string_free (res, TRUE);
  return g_strstrip (r);
}
