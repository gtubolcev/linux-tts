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
  gboolean compat;      /* only what misaki does: no homograph rules */
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
en_lexicon_set_compat (EnLexicon *lex, gboolean compat)
{
  lex->compat = compat;
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

/* ------------------------------------------------------------ numbers */

/* English number words, written here after the rules of num2words (which
 * misaki uses) without its code: cardinals without "and", ordinals, years
 * and decimals, one word per element as misaki gets them by splitting */

static const char *const ONES[] = {
  "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
  "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
  "eighteen", "nineteen",
};
static const char *const TENS[] = {
  "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};
static const char *const SCALES[] = {
  "", "thousand", "million", "billion", "trillion", "quadrillion", "quintillion",
};

typedef struct
{
  const gchar *cur, *unit, *sub;
} Currency;

static const Currency CURRENCIES[] = {
  { "$", "dollar", "cent" }, { "£", "pound", "pence" }, { "€", "euro", "cent" },
};

static const Currency *
currency_of (const gchar *s)
{
  for (guint i = 0; i < G_N_ELEMENTS (CURRENCIES); i++)
    if (s != NULL && strcmp (s, CURRENCIES[i].cur) == 0)
      return &CURRENCIES[i];
  return NULL;
}

static void
words_add (GPtrArray *w, const gchar *s)
{
  g_ptr_array_add (w, g_strdup (s));
}

static void
cardinal_small (GPtrArray *w, guint n)
{
  if (n >= 100)
    {
      words_add (w, ONES[n / 100]);
      words_add (w, "hundred");
      n %= 100;
    }
  if (n >= 20)
    {
      words_add (w, TENS[n / 10]);
      n %= 10;
      if (n > 0)
        words_add (w, ONES[n]);
    }
  else if (n > 0)
    words_add (w, ONES[n]);
}

static void
cardinal (GPtrArray *w, guint64 n)
{
  guint g[G_N_ELEMENTS (SCALES)] = { 0 };
  int top = 0;

  if (n == 0)
    {
      words_add (w, ONES[0]);
      return;
    }
  for (; n > 0; n /= 1000)
    g[top++] = n % 1000;
  while (top-- > 0)
    if (g[top] > 0)
      {
        cardinal_small (w, g[top]);
        if (top > 0)
          words_add (w, SCALES[top]);
      }
}

static void
make_ordinal (GPtrArray *w)
{
  static const char *const ords[][2] = {
    { "one", "first" }, { "two", "second" }, { "three", "third" }, { "five", "fifth" },
    { "eight", "eighth" }, { "nine", "ninth" }, { "twelve", "twelfth" },
  };
  gchar *last = w->pdata[w->len - 1], *r = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (ords) && r == NULL; i++)
    if (strcmp (last, ords[i][0]) == 0)
      r = g_strdup (ords[i][1]);
  if (r == NULL && g_str_has_suffix (last, "y"))
    {
      last[strlen (last) - 1] = '\0';
      r = g_strconcat (last, "ieth", NULL);
    }
  if (r == NULL)
    r = g_strconcat (last, "th", NULL);
  g_free (last);
  w->pdata[w->len - 1] = r;
}

static void
year (GPtrArray *w, guint n)
{
  guint high = n / 100, low = n % 100;

  if (high == 0 || (high % 10 == 0 && low < 10) || high >= 100)
    {
      cardinal (w, n);
      return;
    }
  cardinal (w, high);
  if (low == 0)
    words_add (w, "hundred");
  else if (low < 10)
    {
      words_add (w, "oh");
      words_add (w, ONES[low]);
    }
  else
    cardinal (w, low);
}

static gboolean
all_ascii_digits (const gchar *s)
{
  if (*s == '\0')
    return FALSE;
  for (; *s != '\0'; s++)
    if (!g_ascii_isdigit (*s))
      return FALSE;
  return TRUE;
}

static gchar *
no_commas (const gchar *s, gssize len)
{
  GString *r = g_string_new (NULL);
  for (const gchar *p = s; len < 0 ? *p != '\0' : p < s + len; p++)
    if (*p != ',')
      g_string_append_c (r, *p);
  return g_string_free (r, FALSE);
}

static void
digits (GPtrArray *w, const gchar *s)
{
  for (; *s != '\0'; s++)
    if (g_ascii_isdigit (*s))
      words_add (w, ONES[*s - '0']);
}

/* the cardinal of a digit string, digit by digit when it is too long */
static gboolean
cardinal_str (GPtrArray *w, const gchar *s)
{
  if (!all_ascii_digits (s))
    return FALSE;
  while (s[0] == '0' && s[1] != '\0')
    s++;
  if (strlen (s) > 19)
    digits (w, s);
  else
    cardinal (w, g_ascii_strtoull (s, NULL, 10));
  return TRUE;
}

/* num2words(float(s)): the integer, then "point" and the decimals one by
 * one, nothing when they are all zero */
static gboolean
decimal_str (GPtrArray *w, const gchar *s)
{
  const gchar *dot = strchr (s, '.');
  gchar *pre = g_strndup (s, dot - s), *post = g_strdup (dot + 1);
  gsize n = strlen (post);
  gboolean ok = TRUE;

  while (n > 0 && post[n - 1] == '0')
    post[--n] = '\0';
  if (*pre == '\0')
    words_add (w, ONES[0]);
  else
    ok = cardinal_str (w, pre);
  if (ok && n > 0)
    {
      ok = all_ascii_digits (post);
      words_add (w, "point");
      digits (w, post);
    }
  g_free (pre);
  g_free (post);
  return ok;
}

static gboolean
is_ordinal (const gchar *s)
{
  return s != NULL && (strcmp (s, "st") == 0 || strcmp (s, "nd") == 0
                       || strcmp (s, "rd") == 0 || strcmp (s, "th") == 0);
}

/* Lexicon.is_number */
static gboolean
is_number (const gchar *word, gboolean head)
{
  static const char *const suffixes[] = { "ing", "'d", "ed", "'s", "st", "nd", "rd", "th", "s" };
  gsize n = strlen (word);
  gboolean digit = FALSE;

  for (const gchar *p = word; *p != '\0'; p++)
    digit |= g_ascii_isdigit (*p);
  if (!digit)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (suffixes); i++)
    if (g_str_has_suffix (word, suffixes[i]))
      {
        n -= strlen (suffixes[i]);
        break;
      }
  for (gsize i = 0; i < n; i++)
    if (!(g_ascii_isdigit (word[i]) || word[i] == ',' || word[i] == '.'
          || (head && i == 0 && word[i] == '-')))
      return FALSE;
  return TRUE;
}

/* Lexicon.is_currency */
static gboolean
is_currency (const gchar *word)
{
  const gchar *dot = strchr (word, '.');

  if (dot == NULL)
    return TRUE;
  if (strchr (dot + 1, '.') != NULL)
    return FALSE;
  return strlen (dot + 1) < 3;
}

static gboolean
add_ps (GPtrArray *ps, gchar *p)
{
  if (p == NULL)
    return FALSE;
  g_ptr_array_add (ps, p);
  return TRUE;
}

/* the words looked up one by one; "point" and "O" without stress */
static gboolean
add_words (EnLexicon *lex, GPtrArray *ps, GPtrArray *w)
{
  gboolean ok = TRUE;

  for (guint i = 0; i < w->len && ok; i++)
    {
      const gchar *s = w->pdata[i];
      double stress = strcmp (s, "point") == 0 || strcmp (s, "O") == 0 ? -2 : NONE;
      ok = add_ps (ps, lookup (lex, s, NULL, stress, NULL));
    }
  g_ptr_array_set_size (w, 0);
  return ok;
}

static gchar *
plural (EnLexicon *lex, const gchar *unit)
{
  gchar *s = g_strconcat (unit, "s", NULL);
  gchar *r = stem_s (lex, s, NULL, NONE, NULL);
  g_free (s);
  return r;
}

/* Lexicon.get_number, num_flags left out (they come from markup) */
static gchar *
get_number (EnLexicon *lex, const gchar *word0, const Currency *cur, gboolean head)
{
  GPtrArray *ps = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *w = g_ptr_array_new_with_free_func (g_free);
  gsize e = strlen (word0);
  gchar *suffix, *word, *num = NULL, *r = NULL;
  const gchar *p;
  gboolean ok = TRUE;

  while (e > 0 && (g_ascii_islower (word0[e - 1]) || word0[e - 1] == '\''))
    e--;
  suffix = word0[e] != '\0' ? g_strdup (word0 + e) : NULL;
  word = g_strndup (word0, e);
  p = word;
  if (*p == '-')
    {
      ok = add_ps (ps, lookup (lex, "minus", NULL, NONE, NULL));
      p++;
    }
  if (!ok)
    ;
  else if (all_ascii_digits (p) && is_ordinal (suffix))
    {
      ok = cardinal_str (w, p);
      if (ok)
        make_ordinal (w);
    }
  else if (ps->len == 0 && strlen (p) == 4 && cur == NULL && all_ascii_digits (p))
    year (w, atoi (p));
  else if (!head && strchr (p, '.') == NULL)
    {
      gsize n;
      num = no_commas (p, -1);
      n = strlen (num);
      if (n == 0)
        ok = FALSE;
      else if (num[0] == '0' || n > 3)
        digits (w, num);
      else if (n == 3 && !g_str_has_suffix (num, "00"))
        {
          words_add (w, ONES[num[0] - '0']);
          if (num[1] == '0')
            {
              words_add (w, "O");
              words_add (w, ONES[num[2] - '0']);
            }
          else
            ok = cardinal_str (w, num + 1);
        }
      else
        ok = cardinal_str (w, num);
    }
  else if (!head || strchr (p, '.') != strrchr (p, '.'))
    {
      gchar **parts;
      num = no_commas (p, -1);
      parts = g_strsplit (num, ".", -1);
      for (guint i = 0; parts[i] != NULL && ok; i++)
        {
          const gchar *s = parts[i];
          gboolean nonzero = FALSE;
          for (const gchar *q = s + (*s != '\0'); *q != '\0'; q++)
            nonzero |= *q != '0';
          if (*s == '\0')
            continue;
          if (s[0] == '0' || (strlen (s) != 2 && nonzero))
            digits (w, s);
          else
            ok = cardinal_str (w, s);
        }
      g_strfreev (parts);
    }
  else if (cur != NULL && is_currency (p))
    {
      gchar **parts;
      guint64 v[2] = { 0, 0 };
      const gchar *unit[2] = { cur->unit, cur->sub };
      int from = 0, to;

      num = no_commas (p, -1);
      parts = g_strsplit (num, ".", 2);
      to = parts[0] != NULL && parts[1] != NULL ? 2 : 1;
      for (int i = 0; i < to; i++)
        {
          if (parts[i][0] != '\0' && !all_ascii_digits (parts[i]))
            ok = FALSE;
          v[i] = g_ascii_strtoull (parts[i], NULL, 10);
        }
      g_strfreev (parts);
      if (to == 2 && v[1] == 0)
        to = 1;
      else if (to == 2 && v[0] == 0)
        from = 1;
      for (int i = from; i < to && ok; i++)
        {
          gchar buf[24];
          if (i > from)
            ok = add_ps (ps, lookup (lex, "and", NULL, NONE, NULL));
          g_snprintf (buf, sizeof buf, "%" G_GUINT64_FORMAT, v[i]);
          ok = ok && cardinal_str (w, buf) && add_words (lex, ps, w);
          if (ok)
            ok = add_ps (ps, v[i] != 1 && strcmp (unit[i], "pence") != 0
                                    ? plural (lex, unit[i])
                                    : lookup (lex, unit[i], NULL, NONE, NULL));
        }
    }
  else
    {
      num = no_commas (p, -1);
      if (strchr (num, '.') == NULL)
        {
          ok = cardinal_str (w, num);
          if (ok && strchr (p, ',') != NULL && is_ordinal (suffix))
            make_ordinal (w);
        }
      else if (num[0] == '.')
        {
          ok = all_ascii_digits (num + 1);
          words_add (w, "point");
          digits (w, num + 1);
        }
      else
        ok = decimal_str (w, num);
    }
  ok = ok && add_words (lex, ps, w);
  if (ok && ps->len > 0)
    {
      g_ptr_array_add (ps, NULL);
      r = g_strjoinv (" ", (gchar **) ps->pdata);
      if (suffix != NULL && (strcmp (suffix, "s") == 0 || strcmp (suffix, "'s") == 0))
        r = add_s (lex, r);
      else if (suffix != NULL && (strcmp (suffix, "ed") == 0 || strcmp (suffix, "'d") == 0))
        r = add_ed (lex, r);
      else if (suffix != NULL && strcmp (suffix, "ing") == 0)
        r = add_ing (lex, r);
    }
  g_ptr_array_free (ps, TRUE);
  g_ptr_array_free (w, TRUE);
  g_free (num);
  g_free (word);
  g_free (suffix);
  return r;
}

/* Lexicon.__call__; @head: the first piece of a word, @cur: the currency
 * sign before it */
static gchar *
lexicon (EnLexicon *lex, const gchar *text, const gchar *tag, const Ctx *ctx,
         gboolean head, const Currency *cur)
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
  if (ps != NULL && cur != NULL)
    {
      gchar *c = plural (lex, cur->unit);
      gchar *r = g_strconcat (ps, " ", c, NULL);
      g_free (c);
      g_free (ps);
      ps = r;
    }
  else if (ps == NULL && is_number (word, head))
    ps = get_number (lex, word, cur, head);
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
  const gchar *alias;   /* read as this ("2" in B2B: "to") */
} Sub;

typedef struct
{
  GPtrArray *subs;      /* Sub *; one for a plain word or a mark */
  gboolean space;       /* whitespace after */
  gboolean mark;        /* punctuation, ps set */
  const Currency *cur;  /* a number after a currency sign */
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
  const gchar *pt = prev != NULL && !prev->mark && prev->subs->len == 1
                    ? ((Sub *) prev->subs->pdata[0])->text : NULL;
  const gchar *nt = next != NULL ? ((Sub *) next->subs->pdata[0])->text : NULL;

  /* A in a heading or before a word */
  if (strcmp (text, "A") == 0 && next != NULL && !next->mark
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
      if (!next->mark && next->subs->len == 1 && !in_list (nt, AFTER_THAT_CONJ)
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

/* the number part of misaki's SUBTOKEN_REGEX, (?:\d?[,.]?\d)+, at @p;
 * the end or NULL */
static const gchar *
match_number (const gchar *p)
{
  const gchar *e = NULL;

#define D(x) g_ascii_isdigit (*(x))
#define S(x) (*(x) == ',' || *(x) == '.')
  for (;;)
    {
      const gchar *u = NULL;
      if (D (p) && S (p + 1) && D (p + 2))
        u = p + 3;
      else if (D (p) && D (p + 1))
        u = p + 2;
      else if (S (p) && D (p + 1))
        u = p + 2;
      else if (D (p))
        u = p + 1;
      if (u == NULL)
        return e;
      e = p = u;
    }
#undef D
#undef S
}

/* misaki's subtokenize(): letters with inner apostrophes (split where a
 * lowercase letter meets a capital), numbers (with a leading minus at the
 * start), runs of - and _, any other character alone; @core is at byte
 * @base of the text */
static GPtrArray *
subtokenize (const gchar *core, gsize base)
{
  GPtrArray *subs = g_ptr_array_new_with_free_func (sub_free);
  const gchar *p = core;

  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      const gchar *q = g_utf8_next_char (p), *n;

      if ((n = match_number (p)) != NULL
          || (p == core && c == '-' && (n = match_number (p + 1)) != NULL))
        q = n;
      else if (g_unichar_isalpha (c))
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

static gboolean
alpha_at (const gchar *s, gboolean last)
{
  return *s != '\0' && g_unichar_isalpha (last ? last_char (s, 0) : g_utf8_get_char (s));
}

static void
push_word (GArray *words, GPtrArray *subs, gboolean sentence_start, const Currency *cur)
{
  Word w = { 0 };

  w.subs = subs;
  w.sentence_start = sentence_start;
  w.cur = cur;
  g_array_append_val (words, w);
}

/* the word left after peeling: a currency sign split off as spaCy does
 * (silent, the number after it gets the unit), and a "2" between letters
 * on its own as "to", as misaki's retokenize() does */
static void
add_core (GArray *words, const gchar *core, gsize off, gboolean sentence_start)
{
  gchar sym[8];
  const Currency *cur;
  GPtrArray *subs, *part;

  sym[g_unichar_to_utf8 (g_utf8_get_char (core), sym)] = '\0';
  cur = currency_of (sym);
  if (cur != NULL && core[strlen (sym)] != '\0')
    {
      Word m = { 0 };
      m.subs = g_ptr_array_new_with_free_func (sub_free);
      m.mark = TRUE;
      g_ptr_array_add (m.subs, sub_new (sym, strlen (sym)));
      ((Sub *) m.subs->pdata[0])->ps = g_strdup ("");
      g_array_append_val (words, m);
      core += strlen (sym);
      off += strlen (sym);
    }
  else
    cur = NULL;
  subs = subtokenize (core, off);
  if (cur != NULL && !(subs->len == 1 && is_number (((Sub *) subs->pdata[0])->text, TRUE)))
    cur = NULL;
  part = g_ptr_array_new_with_free_func (sub_free);
  for (guint j = 0; j < subs->len; j++)
    {
      Sub *s = subs->pdata[j];
      if (j > 0 && j + 1 < subs->len && strcmp (s->text, "2") == 0
          && alpha_at (((Sub *) subs->pdata[j - 1])->text, TRUE)
          && alpha_at (((Sub *) subs->pdata[j + 1])->text, FALSE))
        {
          GPtrArray *one = g_ptr_array_new_with_free_func (sub_free);
          if (part->len > 0)
            push_word (words, part, sentence_start, NULL);
          else
            g_ptr_array_free (part, TRUE);
          s->alias = "to";
          g_ptr_array_add (one, s);
          push_word (words, one, FALSE, NULL);
          part = g_ptr_array_new_with_free_func (sub_free);
          sentence_start = FALSE;
        }
      else
        g_ptr_array_add (part, s);
    }
  if (part->len > 0)
    push_word (words, part, sentence_start, cur);
  else
    g_ptr_array_free (part, TRUE);
  g_ptr_array_set_free_func (subs, NULL);
  g_ptr_array_free (subs, TRUE);
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
      /* .5 stays a number */
      while (a < b && is_peeled (g_utf8_get_char (a)) && !(*a == '.' && g_ascii_isdigit (a[1])))
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
    add_core (words, core, base + (a - chunk), sentence_start);
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
      gchar *ps = lexicon (lex, text, tag, ctx, left == 0, NULL);

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

/* retokenize(): no unit when the next token is a number too ("$5 million") */
static const Currency *
word_currency (GArray *words, guint i, GArray *tags, const gchar *text)
{
  static const char *const big[] = { "hundred", "thousand", "million", "billion", "trillion", NULL };
  const Word *w = &g_array_index (words, Word, i), *n;
  const Sub *a, *z;
  gboolean cd;

  if (w->cur == NULL || i + 1 >= words->len)
    return w->cur;
  n = &g_array_index (words, Word, i + 1);
  if (n->mark)
    return w->cur;
  a = n->subs->pdata[0];
  z = n->subs->pdata[n->subs->len - 1];
  if (tags != NULL)
    cd = tag_is (span_tag (tags, text, a->start, z->end), "CD");
  else
    {
      gchar *t = joined (n->subs, 0, n->subs->len), *l = g_ascii_strdown (t, -1);
      cd = has_digit (t) || in_list (l, big);
      g_free (t);
      g_free (l);
    }
  return cd ? NULL : w->cur;
}

/* ------------------------------------------------------- homographs */

/* Homographs the part-of-speech tag cannot tell apart (both readings are
 * nouns): a reading by cue words next to them, our own rules beyond
 * misaki, off in compat mode.  Without a cue the dictionary reading stays. */

typedef struct
{
  const gchar *word, *tag;      /* look this up instead */
  const gchar *ps;              /* or take these phonemes */
} Reading;

static const char *const BASS_MUSIC[] = {
  "guitar", "guitars", "guitarist", "guitarists", "player", "players", "line", "lines",
  "drum", "drums", "clef", "note", "notes", "voice", "voices", "singer", "section",
  "boost", "amp", "amplifier", "speaker", "speakers", "frequencies", "frequency",
  "double", "electric", "slap", "upright", "plays", "played", "playing", "play",
  "solo", "riff", "riffs", "treble", "baritone", "tenor", "cello", "drop", "heavy",
  "subwoofer", "string", "strings", "tone", "part", "parts", "band", NULL,
};
static const char *const BASS_FISH[] = {
  "sea", "striped", "largemouth", "smallmouth", "fishing", "fish", "caught", "catch",
  "lake", "river", "pond", "fillet", "fillets", "grilled", "baked", NULL,
};
static const char *const LEAD_NEXT[] = {
  "poisoning", "pipe", "pipes", "paint", "pencil", "pencils", "shot", "weight", "weights",
  "acid", "sheet", "sheets", "crystal", "glass", "oxide", "ore", "mine", "mines",
  "mining", "levels", "exposure", "contamination", "piping", "solder", "bullet",
  "bullets", "balloon", "smelter", "smelting", "carbonate", "nitrate", "chloride",
  "sulfide", "sulphide", "isotope", "isotopes", "alloy", "alloys", "dust", "fumes",
  "toxicity", NULL,
};
static const char *const METALS[] = {
  "tin", "zinc", "copper", "iron", "mercury", "arsenic", "cadmium", "nickel", "steel",
  "antimony", "uranium", "bismuth", "silver", NULL,
};
static const char *const BOW_TAKE[] = {
  "take", "takes", "took", "taken", "taking", "make", "makes", "made", "making",
  "give", "gives", "gave", "giving", "with", NULL,
};
static const char *const BOW_HOW[] = {
  "deep", "low", "slight", "polite", "courtly", "quick", "curt", "gracious", "stiff",
  "formal", "little", "small", "theatrical", "final", "respectful", NULL,
};
static const char *const SHIPS[] = {
  "ship", "boat", "vessel", "yacht", "canoe", "ferry", "liner", "submarine", "carrier",
  "barge", "tanker", "destroyer", "frigate", NULL,
};
static const char *const ROW_HOW[] = {
  "blazing", "furious", "almighty", "flaming", "bitter", "heated", "diplomatic",
  "political", "public", "violent", "noisy", "family", "domestic", NULL,
};

static gboolean
cue (const gchar *w, const char *const *list)
{
  return w != NULL && in_list (w, list);
}

static gboolean
near (const gchar *const *ctx, guint n, const char *const *list)
{
  for (guint k = 0; k < n; k++)
    if (cue (ctx[k], list))
      return TRUE;
  return FALSE;
}

static gboolean
is_noun_like (const gchar *tag)
{
  return tag == NULL || g_str_has_prefix (tag, "NN") || g_str_has_prefix (tag, "JJ");
}

static gboolean
eq (const gchar *a, const gchar *b)
{
  return a != NULL && strcmp (a, b) == 0;
}

/* @prev, @next: the nearest words before and after, lowercase, NULL past a
 * punctuation mark */
static Reading
homograph (const gchar *word, const gchar *tag, const gchar *const prev[3],
           const gchar *const next[3])
{
  Reading r = { NULL, NULL, NULL };
  gchar *w = g_ascii_strdown (word, -1);
  gboolean plural = g_str_has_suffix (w, "s") && strcmp (w, "bass") != 0;

  if (strcmp (w, "bass") == 0 && is_noun_like (tag))
    {
      /* the fish in the dictionary, the music by its cues */
      int d;
      for (d = 1; d <= 2; d++)
        if (near (prev, d, BASS_FISH) || near (next, d, BASS_FISH)
            || near (prev, d, BASS_MUSIC) || near (next, d, BASS_MUSIC))
          break;
      if (d <= 2 && !near (prev, d, BASS_FISH) && !near (next, d, BASS_FISH))
        r.word = "bass", r.tag = "ADJ";
    }
  else if (strcmp (w, "lead") == 0 && is_noun_like (tag))
    {
      /* the metal: lead pipes, red lead, made of lead, lead and zinc */
      if (cue (next[0], LEAD_NEXT) || eq (prev[0], "red") || eq (prev[0], "white")
          || eq (prev[0], "molten")
          || ((eq (prev[0], "of") || eq (prev[0], "with")) && (next[0] == NULL || eq (next[0], "and")
                                                               || eq (next[0], "or")))
          || near (prev, 2, METALS) || near (next, 2, METALS))
        r.word = "led";
    }
  else if ((strcmp (w, "tear") == 0 || strcmp (w, "tears") == 0)
           && eq (prev[0], "and") && eq (prev[1], "wear"))
    r.word = plural ? "tears" : "tear", r.tag = "VERB";
  else if ((strcmp (w, "bow") == 0 || strcmp (w, "bows") == 0) && is_noun_like (tag))
    {
      /* bending: take a bow, a deep bow; the front: the bow of the ship */
      if ((cue (prev[0], BOW_HOW) && (eq (prev[1], "a") || cue (prev[1], BOW_TAKE)))
          || ((eq (prev[0], "a") || eq (prev[0], "his") || eq (prev[0], "her")
               || eq (prev[0], "their") || eq (prev[0], "my"))
              && cue (prev[1], BOW_TAKE))
          || (plural && cue (prev[0], BOW_TAKE))
          || (eq (next[0], "of") && (cue (next[1], SHIPS) || cue (next[2], SHIPS))))
        r.word = plural ? "bows" : "bow", r.tag = "VERB";
    }
  else if ((strcmp (w, "row") == 0 || strcmp (w, "rows") == 0) && is_noun_like (tag))
    {
      /* a quarrel: a blazing row, had a row with her */
      if (cue (prev[0], ROW_HOW)
          || (eq (prev[0], "a") && (eq (prev[1], "had") || eq (prev[1], "have")
                                    || eq (prev[1], "has") || eq (prev[1], "having"))
              && (eq (next[0], "with") || eq (next[0], "about") || eq (next[0], "over"))))
        r.ps = plural ? "ɹˈWz" : "ɹˈW";
    }
  else if (strcmp (w, "does") == 0
           && ((eq (prev[0], "and") && (eq (prev[1], "bucks") || eq (prev[1], "stags")))
               || (eq (next[0], "and") && (eq (next[1], "bucks") || eq (next[1], "fawns")
                                          || eq (next[1], "stags")))))
    r.word = "doze";
  g_free (w);
  return r;
}

/* the text of the nearest words around word @i, lowercase, up to a mark */
static void
neighbours (GArray *words, guint i, gchar *prev[3], gchar *next[3])
{
  for (int dir = -1; dir <= 1; dir += 2)
    {
      gchar **out = dir < 0 ? prev : next;
      gint j = (gint) i;
      for (int k = 0; k < 3; k++)
        {
          const Word *w;
          out[k] = NULL;
          j += dir;
          if (j < 0 || (guint) j >= words->len)
            continue;
          w = &g_array_index (words, Word, j);
          if (w->mark && *((Sub *) w->subs->pdata[0])->ps != '\0')
            {
              j = dir < 0 ? -1 : (gint) words->len;
              continue;
            }
          if (w->mark)
            {
              k--;
              continue;
            }
          {
            gchar *t = joined (w->subs, 0, w->subs->len);
            out[k] = g_ascii_strdown (t, -1);
            g_free (t);
          }
        }
    }
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
      else if (w->subs->len == 1)
        {
          const gchar *tag = tags != NULL ? span_tag (tags, text, s0->start, s0->end)
                                          : guess_tag (words, i, mixed);
          Reading h = { NULL, NULL, NULL };
          if (!lex->compat && s0->alias == NULL)
            {
              gchar *prev[3], *next[3];
              neighbours (words, i, prev, next);
              h = homograph (s0->text, tag, (const gchar *const *) prev,
                             (const gchar *const *) next);
              for (int k = 0; k < 3; k++)
                {
                  g_free (prev[k]);
                  g_free (next[k]);
                }
            }
          if (h.ps != NULL)
            ps = g_strdup (h.ps);
          else if (h.word != NULL)
            {
              /* keep the capitals of the text */
              gchar *t = is_upper (s0->text) ? g_ascii_strup (h.word, -1)
                         : g_unichar_isupper (g_utf8_get_char (s0->text)) ? capitalize (h.word)
                         : g_strdup (h.word);
              ps = lexicon (lex, t, h.tag != NULL ? h.tag : tag, &ctx, TRUE, NULL);
              g_free (t);
            }
          else
            ps = lexicon (lex, s0->alias != NULL ? s0->alias : s0->text, tag, &ctx, TRUE,
                          word_currency (words, i, tags, text));
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
