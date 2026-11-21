/*
 * en-tagger - English part-of-speech tags as spaCy's en_core_web_sm gives them
 *
 * The tokenizer of spaCy (its prefix, suffix, infix and URL expressions and
 * special cases, exported as data) and the tagger of en_core_web_sm 3.8:
 * hash embeddings of NORM, PREFIX, SUFFIX, SHAPE, SPACY and IS_SPACE, a
 * maxout layer, four residual maxout window layers and a linear layer.  The
 * weights are the model's own (MIT), exported by en-tagger-export.py as
 * float16 and widened to float32 on loading; the tags match spaCy's.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "en-tagger.h"

#include <string.h>
#include <math.h>

#define WIDTH 96
#define PIECES 3
#define NFEAT 6
#define NLAYERS 4
#define NTAGS 50
#define PAD 4

static const guint ROWS[NFEAT] = { 5000, 1000, 2500, 2500, 50, 50 };
static const guint32 SEEDS[NFEAT] = { 8, 9, 10, 11, 12, 13 };

static const char *const LABELS[NTAGS] = {
  "$", "''", ",", "-LRB-", "-RRB-", ".", ":", "ADD", "AFX", "CC", "CD", "DT", "EX", "FW",
  "HYPH", "IN", "JJ", "JJR", "JJS", "LS", "MD", "NFP", "NN", "NNP", "NNPS", "NNS", "PDT",
  "POS", "PRP", "PRP$", "RB", "RBR", "RBS", "RP", "SYM", "TO", "UH", "VB", "VBD", "VBG",
  "VBN", "VBP", "VBZ", "WDT", "WP", "WP$", "WRB", "XX", "_SP", "``",
};

typedef struct
{
  const gfloat *W;      /* [WIDTH][PIECES][nI] */
  const gfloat *b;      /* [WIDTH][PIECES] */
  const gfloat *G;      /* layer norm */
  const gfloat *gb;
  guint nI;
} Maxout;

typedef struct
{
  gchar *orth;
  gchar *norm;          /* NULL: the lexeme's */
} Special;

struct _EnTagger
{
  gfloat *weights;      /* all of them, in the file's order */
  const gfloat *E[NFEAT];
  Maxout embed;
  Maxout layers[NLAYERS];
  const gfloat *outW;   /* [NTAGS][WIDTH] */
  const gfloat *outb;
  GHashTable *norms;    /* string -> norm */
  GHashTable *specials; /* string -> GPtrArray of Special */
  GHashTable *symbols;  /* spaCy's symbol strings -> their ids */
  GRegex *prefix, *suffix, *infix, *url;
  GHashTable *matcher;  /* first token -> GPtrArray of Pattern */
  gboolean loading;     /* tokenizing without the special cases */
};

/* a special case that the affix rules split: spaCy's special matcher
 * merges its tokens back */
typedef struct
{
  gchar **orths;
  guint n;
  gchar *key;
} Pattern;

static void patterns_free (gpointer p);

/* IEEE 754 half precision (little-endian in the file) to float */
static gfloat
half_to_float (guint16 h)
{
  guint32 sign = (guint32) (h & 0x8000) << 16;
  guint32 exp = (h >> 10) & 0x1f;
  guint32 man = h & 0x3ff;
  union { guint32 u; gfloat f; } v;

  if (exp == 0x1f)              /* inf, NaN */
    v.u = sign | 0x7f800000 | (man << 13);
  else if (exp != 0)            /* normal */
    v.u = sign | ((exp + 112) << 23) | (man << 13);
  else                          /* zero, subnormal: man * 2^-24 */
    {
      v.f = ldexpf ((gfloat) man, -24);
      v.u |= sign;
    }
  return v.f;
}
static void build_matcher (EnTagger *t);

/* ---- hashes: spaCy's strings (MurmurHash64A, seed 1) and thinc's
 * HashEmbed (MurmurHash3_x86_128 of the 64-bit key) */

static guint64
murmur64a (const void *key, gsize len, guint64 seed)
{
  const guint64 m = G_GUINT64_CONSTANT (0xc6a4a7935bd1e995);
  const int r = 47;
  guint64 h = seed ^ (len * m);
  const guchar *data = key;
  const guchar *end = data + (len / 8) * 8;

  for (; data != end; data += 8)
    {
      guint64 k;
      memcpy (&k, data, 8);
      k = GUINT64_FROM_LE (k);
      k *= m;
      k ^= k >> r;
      k *= m;
      h ^= k;
      h *= m;
    }
  switch (len & 7)
    {
    case 7: h ^= (guint64) data[6] << 48; /* fall through */
    case 6: h ^= (guint64) data[5] << 40; /* fall through */
    case 5: h ^= (guint64) data[4] << 32; /* fall through */
    case 4: h ^= (guint64) data[3] << 24; /* fall through */
    case 3: h ^= (guint64) data[2] << 16; /* fall through */
    case 2: h ^= (guint64) data[1] << 8;  /* fall through */
    case 1: h ^= (guint64) data[0];
      h *= m;
    }
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

/* StringStore: a symbol's id, else the hash */
static guint64
string_id (EnTagger *t, const gchar *s)
{
  gpointer id;
  if (*s == '\0')
    return 0;
  if (g_hash_table_lookup_extended (t->symbols, s, NULL, &id))
    return GPOINTER_TO_UINT (id);
  return murmur64a (s, strlen (s), 1);
}

/* thinc's MurmurHash3_x86_128_uint64: MurmurHash3_x64_128 of the 8 bytes of
 * @key, as four 32-bit words */
static inline guint64
fmix64 (guint64 k)
{
  k ^= k >> 33;
  k *= G_GUINT64_CONSTANT (0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= G_GUINT64_CONSTANT (0xc4ceb9fe1a85ec53);
  k ^= k >> 33;
  return k;
}

static void
murmur3_128_u64 (guint64 key, guint32 seed, guint32 out[4])
{
  guint64 h1 = key, h2;

  h1 *= G_GUINT64_CONSTANT (0x87c37b91114253d5);
  h1 = (h1 << 31) | (h1 >> 33);
  h1 *= G_GUINT64_CONSTANT (0x4cf5ad432745937f);
  h1 ^= seed;
  h1 ^= 8;
  h2 = seed;
  h2 ^= 8;
  h1 += h2;
  h2 += h1;
  h1 = fmix64 (h1);
  h2 = fmix64 (h2);
  h1 += h2;
  h2 += h1;
  out[0] = (guint32) h1;
  out[1] = (guint32) (h1 >> 32);
  out[2] = (guint32) h2;
  out[3] = (guint32) (h2 >> 32);
}

/* ---- lexical attributes */

/* Python's str.isspace() for a whole string */
static gboolean
is_space (const gchar *s)
{
  if (*s == '\0')
    return FALSE;
  for (; *s != '\0'; s = g_utf8_next_char (s))
    if (!g_unichar_isspace (g_utf8_get_char (s)))
      return FALSE;
  return TRUE;
}

/* spaCy's word_shape */
static gchar *
word_shape (const gchar *s)
{
  GString *out;
  gunichar last = 0;
  int seq = 0;

  if (g_utf8_strlen (s, -1) >= 100)
    return g_strdup ("LONG");
  out = g_string_new (NULL);
  for (; *s != '\0'; s = g_utf8_next_char (s))
    {
      gunichar c = g_utf8_get_char (s), sh;
      if (g_unichar_isalpha (c))
        sh = g_unichar_isupper (c) ? 'X' : 'x';
      else if (g_unichar_isdigit (c))
        sh = 'd';
      else
        sh = c;
      if (sh == last)
        seq++;
      else
        {
          seq = 0;
          last = sh;
        }
      if (seq < 4)
        g_string_append_unichar (out, sh);
    }
  return g_string_free (out, FALSE);
}

static gchar *
lexeme_norm (EnTagger *t, const gchar *s)
{
  const gchar *n = g_hash_table_lookup (t->norms, s);
  return n != NULL ? g_strdup (n) : g_utf8_strdown (s, -1);
}

/* ---- loading */

static gboolean
load_tsv (const gchar *path, GHashTable *norms, GHashTable *specials, GHashTable *symbols,
          GError **error)
{
  gchar *text, **lines;
  gboolean ok = TRUE;

  if (!g_file_get_contents (path, &text, NULL, error))
    return FALSE;
  lines = g_strsplit (text, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar **f;
      if (**l == '\0')
        continue;
      f = g_strsplit (*l, "\t", -1);
      if (f[0] == NULL || f[1] == NULL)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: bad line", path);
          g_strfreev (f);
          ok = FALSE;
          break;
        }
      if (norms != NULL)
        g_hash_table_replace (norms, g_strdup (f[0]), g_strdup (f[1]));
      else if (symbols != NULL)
        {
          guint64 id;
          if (!g_ascii_string_to_unsigned (f[1], 10, 1, G_MAXUINT, &id, error))
            {
              g_strfreev (f);
              ok = FALSE;
              break;
            }
          g_hash_table_replace (symbols, g_strdup (f[0]), GUINT_TO_POINTER ((guint) id));
        }
      else
        {
          GPtrArray *a = g_ptr_array_new ();
          for (gchar **x = f + 1; *x != NULL; x++)
            {
              Special *sp = g_new0 (Special, 1);
              gchar *sep = strchr (*x, '\x1f');
              if (sep != NULL)
                {
                  sp->orth = g_strndup (*x, sep - *x);
                  sp->norm = sep[1] != '\0' ? g_strdup (sep + 1) : NULL;
                }
              else
                sp->orth = g_strdup (*x);
              g_ptr_array_add (a, sp);
            }
          g_hash_table_replace (specials, g_strdup (f[0]), a);
        }
      g_strfreev (f);
    }
  g_strfreev (lines);
  g_free (text);
  return ok;
}

static void
specials_free (gpointer p)
{
  GPtrArray *a = p;
  for (guint i = 0; i < a->len; i++)
    {
      Special *s = a->pdata[i];
      g_free (s->orth);
      g_free (s->norm);
      g_free (s);
    }
  g_ptr_array_free (a, TRUE);
}

static gboolean
load_regexes (EnTagger *t, const gchar *path, GError **error)
{
  gchar *text, **lines;
  gboolean ok = TRUE;

  if (!g_file_get_contents (path, &text, NULL, error))
    return FALSE;
  lines = g_strsplit (text, "\n", -1);
  for (gchar **l = lines; *l != NULL && ok; l++)
    {
      gchar *tab = strchr (*l, '\t');
      GRegex **dst = NULL;
      if (tab == NULL)
        continue;
      *tab = '\0';
      if (strcmp (*l, "prefix") == 0)
        dst = &t->prefix;
      else if (strcmp (*l, "suffix") == 0)
        dst = &t->suffix;
      else if (strcmp (*l, "infix") == 0)
        dst = &t->infix;
      else if (strcmp (*l, "url") == 0)
        dst = &t->url;
      if (dst == NULL || *dst != NULL)
        continue;
      *dst = g_regex_new (tab + 1, G_REGEX_OPTIMIZE, 0, error);
      ok = *dst != NULL;
    }
  if (ok && (t->prefix == NULL || t->suffix == NULL || t->infix == NULL || t->url == NULL))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: an expression is missing", path);
      ok = FALSE;
    }
  g_strfreev (lines);
  g_free (text);
  return ok;
}

EnTagger *
en_tagger_open (const gchar *dir, GError **error)
{
  EnTagger *t = g_new0 (EnTagger, 1);
  gchar *path;
  GMappedFile *file;
  const guchar *h;
  const gfloat *p;
  gsize n = 0;

  for (int i = 0; i < NFEAT; i++)
    n += (gsize) ROWS[i] * WIDTH;
  n += WIDTH * PIECES * NFEAT * WIDTH + WIDTH * PIECES + 2 * WIDTH;
  n += NLAYERS * (WIDTH * PIECES * 3 * WIDTH + WIDTH * PIECES + 2 * WIDTH);
  n += NTAGS * WIDTH + NTAGS;

  t->norms = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  t->specials = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, specials_free);
  t->symbols = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->matcher = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, patterns_free);

  path = g_build_filename (dir, "en-tagger.bin", NULL);
  file = g_mapped_file_new (path, FALSE, error);
  if (file == NULL)
    goto fail;
  h = (const guchar *) g_mapped_file_get_contents (file);
  if (g_mapped_file_get_length (file) != 8 + 2 * n || memcmp (h, "ENTAGh1", 8) != 0)
    {
      g_mapped_file_unref (file);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: not a tagger of this kind", path);
      goto fail;
    }
  t->weights = g_new (gfloat, n);
  for (gsize i = 0; i < n; i++)
    t->weights[i] = half_to_float (h[8 + 2 * i] | (guint16) h[9 + 2 * i] << 8);
  g_mapped_file_unref (file);
  p = t->weights;
  for (int i = 0; i < NFEAT; i++)
    {
      t->E[i] = p;
      p += (gsize) ROWS[i] * WIDTH;
    }
  for (int l = -1; l < NLAYERS; l++)
    {
      Maxout *m = l < 0 ? &t->embed : &t->layers[l];
      m->nI = l < 0 ? NFEAT * WIDTH : 3 * WIDTH;
      m->W = p; p += WIDTH * PIECES * m->nI;
      m->b = p; p += WIDTH * PIECES;
      m->G = p; p += WIDTH;
      m->gb = p; p += WIDTH;
    }
  t->outW = p; p += NTAGS * WIDTH;
  t->outb = p;
  g_free (path);

  path = g_build_filename (dir, "en-tagger-norm.tsv", NULL);
  if (!load_tsv (path, t->norms, NULL, NULL, error))
    goto fail;
  g_free (path);
  path = g_build_filename (dir, "en-tagger-rules.tsv", NULL);
  if (!load_tsv (path, NULL, t->specials, NULL, error))
    goto fail;
  g_free (path);
  path = g_build_filename (dir, "en-tagger-symbols.tsv", NULL);
  if (!load_tsv (path, NULL, NULL, t->symbols, error))
    goto fail;
  g_free (path);
  path = g_build_filename (dir, "en-tagger-tokenizer.txt", NULL);
  if (!load_regexes (t, path, error))
    goto fail;
  g_free (path);
  build_matcher (t);
  return t;

fail:
  if (error != NULL && *error != NULL)
    g_prefix_error (error, "en-tagger: ");
  g_free (path);
  en_tagger_free (t);
  return NULL;
}

void
en_tagger_free (EnTagger *t)
{
  if (t == NULL)
    return;
  g_free (t->weights);
  g_hash_table_destroy (t->norms);
  g_hash_table_destroy (t->specials);
  g_hash_table_destroy (t->symbols);
  g_hash_table_destroy (t->matcher);
  g_clear_pointer (&t->prefix, g_regex_unref);
  g_clear_pointer (&t->suffix, g_regex_unref);
  g_clear_pointer (&t->infix, g_regex_unref);
  g_clear_pointer (&t->url, g_regex_unref);
  g_free (t);
}

/* ---- tokenizer: spaCy's Tokenizer._tokenize_affixes for one language */

typedef struct
{
  gsize start, end;     /* bytes in the text */
  gchar *norm;          /* NULL: the lexeme's */
  gboolean space;       /* one space after */
} Tok;

static void
push (GArray *toks, gsize start, gsize end, const gchar *norm)
{
  Tok k = { start, end, g_strdup (norm), FALSE };
  if (end > start)
    g_array_append_val (toks, k);
}

static GPtrArray *
special (EnTagger *t, const gchar *text, gsize start, gsize end)
{
  gchar *s;
  if (t->loading)
    return NULL;
  s = g_strndup (text + start, end - start);
  GPtrArray *a = g_hash_table_lookup (t->specials, s);
  g_free (s);
  return a;
}

/* adds the special case's tokens when their text is that of the span */
static gboolean
push_special (EnTagger *t, GArray *toks, const gchar *text, gsize start, gsize end)
{
  GPtrArray *a = special (t, text, start, end);
  gsize pos = start, total = 0;

  if (a == NULL)
    return FALSE;
  for (guint i = 0; i < a->len; i++)
    total += strlen (((Special *) a->pdata[i])->orth);
  if (total != end - start)
    {
      /* a norm-changing case (the orth differs): one token */
      push (toks, start, end, NULL);
      return TRUE;
    }
  for (guint i = 0; i < a->len; i++)
    {
      Special *sp = a->pdata[i];
      gsize n = strlen (sp->orth);
      push (toks, pos, pos + n, sp->norm);
      pos += n;
    }
  return TRUE;
}

/* the length of the match of @re in text[start, end) (search semantics;
 * anchors decide where); -1 for none */
static gssize
search (GRegex *re, const gchar *text, gsize start, gsize end, gsize *mstart)
{
  GMatchInfo *mi = NULL;
  gssize r = -1;
  gint s, e;

  if (g_regex_match_full (re, text + start, end - start, 0, 0, &mi, NULL)
      && g_match_info_fetch_pos (mi, 0, &s, &e))
    {
      r = e - s;
      if (mstart != NULL)
        *mstart = start + s;
    }
  g_match_info_free (mi);
  return r;
}

static void
split_span (EnTagger *t, GArray *toks, const gchar *text, gsize start, gsize end)
{
  GArray *prefixes = g_array_new (FALSE, FALSE, sizeof (gsize) * 2);
  GArray *suffixes = g_array_new (FALSE, FALSE, sizeof (gsize) * 2);
  gsize last = G_MAXSIZE;

  if (push_special (t, toks, text, start, end))
    goto done;
  while (end > start && end - start != last)
    {
      gssize pre, suf;
      gsize ms;
      if (special (t, text, start, end) != NULL)
        break;
      last = end - start;
      pre = search (t->prefix, text, start, end, NULL);
      if (pre <= 0)
        pre = 0;
      if (pre > 0 && start + pre < end && special (t, text, start + pre, end) != NULL)
        {
          gsize span[2] = { start, start + pre };
          g_array_append_val (prefixes, span);
          start += pre;
          break;
        }
      suf = search (t->suffix, text, start + pre, end, &ms);
      if (suf <= 0)
        suf = 0;
      if (suf > 0 && end - suf > start && special (t, text, start, end - suf) != NULL)
        {
          gsize span[2] = { end - suf, end };
          g_array_append_val (suffixes, span);
          end -= suf;
          break;
        }
      if (pre > 0 && suf > 0 && (gsize) (pre + suf) <= end - start)
        {
          gsize ps[2] = { start, start + pre }, ss[2] = { end - suf, end };
          g_array_append_val (prefixes, ps);
          g_array_append_val (suffixes, ss);
          start += pre;
          end -= suf;
        }
      else if (pre > 0)
        {
          gsize ps[2] = { start, start + pre };
          g_array_append_val (prefixes, ps);
          start += pre;
        }
      else if (suf > 0)
        {
          gsize ss[2] = { end - suf, end };
          g_array_append_val (suffixes, ss);
          end -= suf;
        }
      if (end > start && special (t, text, start, end) != NULL)
        break;
    }
  /* the affixes are plain lexemes, not special cases */
  for (guint i = 0; i < prefixes->len; i++)
    {
      gsize *s = &g_array_index (prefixes, gsize, 2 * i);
      push (toks, s[0], s[1], NULL);
    }
  if (end > start && !push_special (t, toks, text, start, end))
    {
      GMatchInfo *mi = NULL;
      gsize pos = start;
      if (search (t->url, text, start, end, NULL) >= 0)
        push (toks, start, end, NULL);
      else
        {
          g_regex_match_full (t->infix, text + start, end - start, 0, 0, &mi, NULL);
          while (g_match_info_matches (mi))
            {
              gint s, e;
              g_match_info_fetch_pos (mi, 0, &s, &e);
              /* spaCy skips an infix at the very start */
              if (s != 0)
                {
                  if (start + s > pos)
                    push (toks, pos, start + s, NULL);
                  if (e > s)
                    push (toks, start + s, start + e, NULL);
                  pos = start + e;
                }
              g_match_info_next (mi, NULL);
            }
          g_match_info_free (mi);
          push (toks, pos, end, NULL);
        }
    }
  for (guint i = suffixes->len; i-- > 0;)
    {
      gsize *s = &g_array_index (suffixes, gsize, 2 * i);
      push (toks, s[0], s[1], NULL);
    }
done:
  g_array_free (prefixes, TRUE);
  g_array_free (suffixes, TRUE);
}

static GArray *
tokenize_affixes (EnTagger *t, const gchar *text)
{
  GArray *toks = g_array_new (FALSE, FALSE, sizeof (Tok));
  gsize i = 0, start = 0, n = strlen (text);
  gboolean in_ws = n > 0 && g_unichar_isspace (g_utf8_get_char (text));

  while (i < n)
    {
      gunichar c = g_utf8_get_char (text + i);
      gsize next = g_utf8_next_char (text + i) - text;
      if ((g_unichar_isspace (c) != 0) != in_ws)
        {
          if (start < i)
            {
              if (in_ws)
                push (toks, start, i, NULL);    /* a whitespace token */
              else
                split_span (t, toks, text, start, i);
            }
          if (!in_ws && c == ' ')
            {
              if (toks->len > 0)
                g_array_index (toks, Tok, toks->len - 1).space = TRUE;
              start = next;
            }
          else
            start = i;
          in_ws = !in_ws;
        }
      i = next;
    }
  if (start < n)
    {
      if (in_ws)
        push (toks, start, n, NULL);
      else
        split_span (t, toks, text, start, n);
      if (toks->len > 0 && text[n - 1] == ' ')
        g_array_index (toks, Tok, toks->len - 1).space = TRUE;
    }
  return toks;
}

static void
toks_free (GArray *toks)
{
  for (guint i = 0; i < toks->len; i++)
    g_free (g_array_index (toks, Tok, i).norm);
  g_array_free (toks, TRUE);
}

typedef struct
{
  guint start, n;
  const gchar *key;
} Match;

static gint
match_cmp (gconstpointer a, gconstpointer b)
{
  const Match *x = a, *y = b;
  if (x->n != y->n)
    return x->n > y->n ? -1 : 1;
  return x->start < y->start ? -1 : x->start > y->start;
}

/* Tokenizer._apply_special_cases: special cases split by the affix rules,
 * longest first, then leftmost */
static GArray *
tokenize (EnTagger *t, const gchar *text)
{
  GArray *toks = tokenize_affixes (t, text), *res;
  GArray *matches = g_array_new (FALSE, FALSE, sizeof (Match));
  gboolean *taken;
  const Match **at;

  for (guint i = 0; i < toks->len; i++)
    {
      Tok *k = &g_array_index (toks, Tok, i);
      gchar *orth = g_strndup (text + k->start, k->end - k->start);
      GPtrArray *pats = g_hash_table_lookup (t->matcher, orth);
      g_free (orth);
      for (guint p = 0; pats != NULL && p < pats->len; p++)
        {
          Pattern *pat = pats->pdata[p];
          gboolean ok = i + pat->n <= toks->len;
          for (guint j = 0; ok && j < pat->n; j++)
            {
              Tok *x = &g_array_index (toks, Tok, i + j);
              ok = strlen (pat->orths[j]) == x->end - x->start
                   && memcmp (pat->orths[j], text + x->start, x->end - x->start) == 0
                   && (j == 0 || (g_array_index (toks, Tok, i + j - 1).end == x->start));
            }
          if (ok)
            {
              Match m = { i, pat->n, pat->key };
              g_array_append_val (matches, m);
            }
        }
    }
  if (matches->len == 0)
    {
      g_array_free (matches, TRUE);
      return toks;
    }
  g_array_sort (matches, match_cmp);
  taken = g_new0 (gboolean, toks->len);
  at = g_new0 (const Match *, toks->len);
  for (guint m = 0; m < matches->len; m++)
    {
      const Match *x = &g_array_index (matches, Match, m);
      gboolean free_ = TRUE;
      for (guint j = 0; j < x->n && free_; j++)
        free_ = !taken[x->start + j];
      if (!free_)
        continue;
      for (guint j = 0; j < x->n; j++)
        taken[x->start + j] = TRUE;
      at[x->start] = x;
    }
  res = g_array_new (FALSE, FALSE, sizeof (Tok));
  for (guint i = 0; i < toks->len;)
    {
      Tok *k = &g_array_index (toks, Tok, i);
      if (at[i] == NULL)
        {
          g_array_append_val (res, *k);
          k->norm = NULL;
          i++;
          continue;
        }
      {
        const Match *x = at[i];
        gsize end = g_array_index (toks, Tok, i + x->n - 1).end;
        gboolean space = g_array_index (toks, Tok, i + x->n - 1).space;
        if (!push_special (t, res, text, k->start, end))
          push (res, k->start, end, NULL);
        g_array_index (res, Tok, res->len - 1).space = space;
        i += x->n;
      }
    }
  g_free (taken);
  g_free (at);
  g_array_free (matches, TRUE);
  toks_free (toks);
  return res;
}

static void
patterns_free (gpointer p)
{
  GPtrArray *a = p;
  for (guint i = 0; i < a->len; i++)
    {
      Pattern *x = a->pdata[i];
      g_strfreev (x->orths);
      g_free (x->key);
      g_free (x);
    }
  g_ptr_array_free (a, TRUE);
}

static void
build_matcher (EnTagger *t)
{
  GHashTableIter it;
  gpointer key;

  t->loading = TRUE;
  g_hash_table_iter_init (&it, t->specials);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      GArray *toks = tokenize_affixes (t, key);
      if (toks->len > 1)
        {
          Pattern *pat = g_new0 (Pattern, 1);
          GPtrArray *a;
          gchar *first;
          pat->n = toks->len;
          pat->orths = g_new0 (gchar *, toks->len + 1);
          for (guint i = 0; i < toks->len; i++)
            {
              Tok *k = &g_array_index (toks, Tok, i);
              pat->orths[i] = g_strndup ((const gchar *) key + k->start, k->end - k->start);
            }
          pat->key = g_strdup (key);
          first = g_strdup (pat->orths[0]);
          a = g_hash_table_lookup (t->matcher, first);
          if (a == NULL)
            {
              a = g_ptr_array_new ();
              g_hash_table_insert (t->matcher, first, a);
            }
          else
            g_free (first);
          g_ptr_array_add (a, pat);
        }
      toks_free (toks);
    }
  t->loading = FALSE;
}

/* ---- the network */

/* GCC/clang vector extensions: SSE/NEON where there is one, plain code
 * elsewhere; @n is a multiple of 8 */
typedef gfloat V4 __attribute__ ((vector_size (16)));

static inline gfloat
dot (const gfloat *w, const gfloat *x, guint n)
{
  V4 a = { 0 }, b = { 0 }, u, v;
  for (guint i = 0; i < n; i += 8)
    {
      memcpy (&u, w + i, sizeof u);
      memcpy (&v, x + i, sizeof v);
      a += u * v;
      memcpy (&u, w + i + 4, sizeof u);
      memcpy (&v, x + i + 4, sizeof v);
      b += u * v;
    }
  a += b;
  return (a[0] + a[1]) + (a[2] + a[3]);
}

static void
maxout_norm (const Maxout *m, const gfloat *x, gfloat *y)
{
  double mean = 0, var = 0;

  for (int o = 0; o < WIDTH; o++)
    {
      gfloat best = -INFINITY;
      for (int p = 0; p < PIECES; p++)
        {
          const gfloat *w = m->W + ((gsize) o * PIECES + p) * m->nI;
          gfloat s = m->b[o * PIECES + p] + dot (w, x, m->nI);
          if (s > best)
            best = s;
        }
      y[o] = best;
      mean += best;
    }
  mean /= WIDTH;
  for (int o = 0; o < WIDTH; o++)
    var += (y[o] - mean) * (y[o] - mean);
  var /= WIDTH;
  for (int o = 0; o < WIDTH; o++)
    y[o] = (gfloat) ((y[o] - mean) / sqrt (var + 1e-8)) * m->G[o] + m->gb[o];
}

static void
features (EnTagger *t, const gchar *text, const Tok *k, guint64 f[NFEAT])
{
  gchar *s = g_strndup (text + k->start, k->end - k->start);
  gchar *norm = k->norm != NULL ? g_strdup (k->norm) : lexeme_norm (t, s);
  glong len = g_utf8_strlen (s, -1);
  gchar *pre = g_utf8_substring (s, 0, 1);
  gchar *suf = g_utf8_substring (s, MAX (len - 3, 0), len);
  gchar *shape = word_shape (s);

  f[0] = string_id (t, norm);
  f[1] = string_id (t, pre);
  f[2] = string_id (t, suf);
  f[3] = string_id (t, shape);
  f[4] = k->space ? 1 : 0;
  f[5] = is_space (s) ? 1 : 0;
  g_free (s);
  g_free (norm);
  g_free (pre);
  g_free (suf);
  g_free (shape);
}

GArray *
en_tagger_tag (EnTagger *t, const gchar *text)
{
  GArray *toks, *res;
  gsize n;
  gfloat *x, *y, *win, *emb;

  res = g_array_new (FALSE, FALSE, sizeof (EnTag));
  if (t == NULL || text == NULL || !g_utf8_validate (text, -1, NULL))
    return res;
  toks = tokenize (t, text);
  n = toks->len;
  if (n == 0)
    {
      g_array_free (toks, TRUE);
      return res;
    }
  /* rows: PAD zero rows, the tokens, PAD zero rows (thinc's with_array pad) */
  x = g_new0 (gfloat, (n + 2 * PAD) * WIDTH);
  y = g_new0 (gfloat, (n + 2 * PAD) * WIDTH);
  win = g_new0 (gfloat, 3 * WIDTH);
  emb = g_new0 (gfloat, NFEAT * WIDTH);
  for (gsize i = 0; i < n; i++)
    {
      guint64 f[NFEAT];
      features (t, text, &g_array_index (toks, Tok, i), f);
      memset (emb, 0, sizeof (gfloat) * NFEAT * WIDTH);
      for (int c = 0; c < NFEAT; c++)
        {
          guint32 h[4];
          murmur3_128_u64 (f[c], SEEDS[c], h);
          for (int j = 0; j < 4; j++)
            {
              const gfloat *row = t->E[c] + (gsize) (h[j] % ROWS[c]) * WIDTH;
              for (int d = 0; d < WIDTH; d++)
                emb[c * WIDTH + d] += row[d];
            }
        }
      maxout_norm (&t->embed, emb, x + (PAD + i) * WIDTH);
    }
  for (int l = 0; l < NLAYERS; l++)
    {
      gsize rows = n + 2 * PAD;
      for (gsize r = 0; r < rows; r++)
        {
          memset (win, 0, sizeof (gfloat) * 3 * WIDTH);
          if (r > 0)
            memcpy (win, x + (r - 1) * WIDTH, sizeof (gfloat) * WIDTH);
          memcpy (win + WIDTH, x + r * WIDTH, sizeof (gfloat) * WIDTH);
          if (r + 1 < rows)
            memcpy (win + 2 * WIDTH, x + (r + 1) * WIDTH, sizeof (gfloat) * WIDTH);
          maxout_norm (&t->layers[l], win, y + r * WIDTH);
        }
      for (gsize j = 0; j < rows * WIDTH; j++)
        x[j] += y[j];
    }
  for (gsize i = 0; i < n; i++)
    {
      const gfloat *v = x + (PAD + i) * WIDTH;
      Tok *k = &g_array_index (toks, Tok, i);
      EnTag tag = { k->start, k->end, NULL };
      gfloat best = -INFINITY;
      for (int c = 0; c < NTAGS; c++)
        {
          gfloat s = t->outb[c];
          for (int d = 0; d < WIDTH; d++)
            s += t->outW[c * WIDTH + d] * v[d];
          if (s > best)
            {
              best = s;
              tag.tag = LABELS[c];
            }
        }
      g_array_append_val (res, tag);
      g_free (k->norm);
    }
  g_free (x);
  g_free (y);
  g_free (win);
  g_free (emb);
  g_array_free (toks, TRUE);
  return res;
}
