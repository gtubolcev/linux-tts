/*
 * hf-tok - WordPiece and byte-level BPE tokenizers (see hf-tok.h)
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hf-tok.h"

#include <string.h>

#define HF_ERROR (g_quark_from_static_string ("hf-tok"))
#define MAX_FILE (64 * 1024 * 1024)
#define WP_MAX_CHARS 100        /* max_input_chars_per_word */
#define BPE_CACHE_MAX 20000

struct HfTok
{
  gboolean bpe;
  GHashTable *vocab;            /* token -> id + 1 */
  GHashTable *added;            /* added token -> id + 1 */
  gsize added_max;              /* longest added token, bytes */
  GHashTable *ranks;            /* "a b" -> merge rank + 1 */
  GHashTable *cache;            /* BPE: pre-token -> GArray of ids */
  gchar *byte_str[256];         /* BPE: byte -> its unicode stand-in */
  gint64 cls, sep, pad, unk;
};

/* ------------------------------------------------------------------ JSON */

static void
skip_ws (const gchar **p, const gchar *end)
{
  while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
    (*p)++;
}

static gboolean
hex4 (const gchar *p, const gchar *end, gunichar *out)
{
  gunichar v = 0;

  if (end - p < 4)
    return FALSE;
  for (int i = 0; i < 4; i++)
    {
      int d = g_ascii_xdigit_value (p[i]);
      if (d < 0)
        return FALSE;
      v = v * 16 + d;
    }
  *out = v;
  return TRUE;
}

/* a JSON string at *p (on the quote); NULL on bad syntax */
static gchar *
json_string (const gchar **p, const gchar *end)
{
  GString *s;
  const gchar *q = *p;

  if (q >= end || *q != '"')
    return NULL;
  q++;
  s = g_string_new (NULL);
  while (q < end && *q != '"')
    {
      if (*q != '\\')
        {
          g_string_append_c (s, *q++);
          continue;
        }
      if (++q >= end)
        break;
      switch (*q)
        {
        case '"': case '\\': case '/':
          g_string_append_c (s, *q); q++; break;
        case 'b': g_string_append_c (s, '\b'); q++; break;
        case 'f': g_string_append_c (s, '\f'); q++; break;
        case 'n': g_string_append_c (s, '\n'); q++; break;
        case 'r': g_string_append_c (s, '\r'); q++; break;
        case 't': g_string_append_c (s, '\t'); q++; break;
        case 'u':
          {
            gunichar c, lo;

            if (!hex4 (q + 1, end, &c))
              goto bad;
            q += 5;
            if (c >= 0xD800 && c < 0xDC00)
              {
                if (end - q < 6 || q[0] != '\\' || q[1] != 'u' || !hex4 (q + 2, end, &lo)
                    || lo < 0xDC00 || lo > 0xDFFF)
                  goto bad;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                q += 6;
              }
            else if (c >= 0xDC00 && c <= 0xDFFF)
              goto bad;
            if (c == 0)
              goto bad;
            g_string_append_unichar (s, c);
            break;
          }
        default:
          goto bad;
        }
    }
  if (q >= end || !g_utf8_validate (s->str, s->len, NULL) || strlen (s->str) != s->len)
    goto bad;
  *p = q + 1;
  return g_string_free (s, FALSE);
bad:
  g_string_free (s, TRUE);
  return NULL;
}

gboolean
hf_json_flat_parse (const gchar *text, gsize len, HfJsonPair cb, gpointer data, GError **error)
{
  const gchar *p = text, *end = text + len;

  skip_ws (&p, end);
  if (p >= end || *p != '{')
    goto bad;
  p++;
  skip_ws (&p, end);
  if (p < end && *p == '}')
    return TRUE;
  for (;;)
    {
      gchar *key;

      skip_ws (&p, end);
      if ((key = json_string (&p, end)) == NULL)
        goto bad;
      skip_ws (&p, end);
      if (p >= end || *p != ':')
        {
          g_free (key);
          goto bad;
        }
      p++;
      skip_ws (&p, end);
      if (p < end && *p == '[')
        {
          GPtrArray *a = g_ptr_array_new_with_free_func (g_free);

          p++;
          skip_ws (&p, end);
          if (p < end && *p == ']')
            p++;
          else
            for (;;)
              {
                gchar *v;

                skip_ws (&p, end);
                if ((v = json_string (&p, end)) == NULL || a->len > 64)
                  {
                    g_free (v);
                    g_ptr_array_free (a, TRUE);
                    g_free (key);
                    goto bad;
                  }
                g_ptr_array_add (a, v);
                skip_ws (&p, end);
                if (p < end && *p == ',')
                  {
                    p++;
                    continue;
                  }
                if (p < end && *p == ']')
                  {
                    p++;
                    break;
                  }
                g_ptr_array_free (a, TRUE);
                g_free (key);
                goto bad;
              }
          g_ptr_array_add (a, NULL);
          cb (key, -1, (gchar **) a->pdata, data);
          g_ptr_array_free (a, TRUE);
        }
      else
        {
          gint64 v = 0;
          int digits = 0;

          if (p < end && *p == '-')
            goto bad_key;
          while (p < end && g_ascii_isdigit (*p) && digits < 12)
            v = v * 10 + (*p++ - '0'), digits++;
          if (digits == 0 || (p < end && g_ascii_isdigit (*p)))
            goto bad_key;
          cb (key, v, NULL, data);
        }
      g_free (key);
      skip_ws (&p, end);
      if (p < end && *p == ',')
        {
          p++;
          continue;
        }
      if (p < end && *p == '}')
        return TRUE;
      goto bad;
bad_key:
      g_free (key);
      goto bad;
    }
bad:
  g_set_error (error, HF_ERROR, 1, "bad JSON at byte %" G_GSIZE_FORMAT, (gsize) (p - text));
  return FALSE;
}

/* ------------------------------------------------------------------ load */

static gint64
lookup (GHashTable *h, const gchar *tok)
{
  gpointer v = h != NULL ? g_hash_table_lookup (h, tok) : NULL;
  return v != NULL ? (gint64) GPOINTER_TO_SIZE (v) - 1 : -1;
}

static void
put_id (const gchar *key, gint64 num, gchar **strs, gpointer data)
{
  GHashTable *h = data;

  if (num >= 0 && num < G_MAXINT32 && *key != '\0')
    g_hash_table_replace (h, g_strdup (key), GSIZE_TO_POINTER ((gsize) num + 1));
}

static gboolean
read_file (const gchar *path, gchar **text, gsize *len, GError **error)
{
  if (!g_file_get_contents (path, text, len, error))
    return FALSE;
  if (*len > MAX_FILE)
    {
      g_set_error (error, HF_ERROR, 1, "%s: too large", path);
      g_clear_pointer (text, g_free);
      return FALSE;
    }
  return TRUE;
}

static gboolean
load_json_ids (const gchar *path, GHashTable *h, GError **error)
{
  gchar *text;
  gsize len;
  gboolean ok;

  if (!read_file (path, &text, &len, error))
    return FALSE;
  ok = hf_json_flat_parse (text, len, put_id, h, error);
  if (!ok)
    g_prefix_error (error, "%s: ", path);
  g_free (text);
  return ok;
}

/* GPT-2 bytes_to_unicode */
static void
byte_map_init (HfTok *t)
{
  int n = 0;

  for (int b = 0; b < 256; b++)
    {
      gunichar c;
      gchar buf[8];
      int l;

      if ((b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF))
        c = b;
      else
        c = 256 + n++;
      l = g_unichar_to_utf8 (c, buf);
      t->byte_str[b] = g_strndup (buf, l);
    }
}

static gboolean
load_merges (HfTok *t, const gchar *path, GError **error)
{
  gchar *text, **lines;
  gsize len;
  gint64 rank = 0;

  if (!read_file (path, &text, &len, error))
    return FALSE;
  if (!g_utf8_validate (text, len, NULL))
    {
      g_set_error (error, HF_ERROR, 1, "%s: not UTF-8", path);
      g_free (text);
      return FALSE;
    }
  lines = g_strsplit (text, "\n", -1);
  g_free (text);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *sp;

      g_strchomp (*l);
      if (**l == '\0' || g_str_has_prefix (*l, "#version"))
        continue;
      sp = strchr (*l, ' ');
      if (sp == NULL || sp == *l || sp[1] == '\0' || strchr (sp + 1, ' ') != NULL)
        continue;
      if (!g_hash_table_contains (t->ranks, *l))
        g_hash_table_insert (t->ranks, g_strdup (*l), GSIZE_TO_POINTER ((gsize) rank + 1));
      rank++;
    }
  g_strfreev (lines);
  if (rank == 0)
    {
      g_set_error (error, HF_ERROR, 1, "%s: no merges", path);
      return FALSE;
    }
  return TRUE;
}

/* one token a line, the id is the line number; lines that are not valid
 * UTF-8 or hold a NUL (tiny2.1 has some: tokens no text can match) keep
 * their number but no entry */
static gboolean
load_vocab_txt (HfTok *t, const gchar *path, GError **error)
{
  gchar *text;
  const gchar *p, *end;
  gsize len;
  gint64 id = 0;

  if (!read_file (path, &text, &len, error))
    return FALSE;
  for (p = text, end = text + len; p < end; id++)
    {
      const gchar *nl = memchr (p, '\n', end - p), *e = nl != NULL ? nl : end;
      const gchar *q = e;

      while (q > p && g_ascii_isspace (q[-1]))
        q--;
      if (q > p && memchr (p, '\0', q - p) == NULL && g_utf8_validate (p, q - p, NULL))
        {
          gchar *tok = g_strndup (p, q - p);
          if (!g_hash_table_contains (t->vocab, tok))
            g_hash_table_insert (t->vocab, tok, GSIZE_TO_POINTER ((gsize) id + 1));
          else
            g_free (tok);
        }
      p = nl != NULL ? nl + 1 : end;
    }
  g_free (text);
  if (g_hash_table_size (t->vocab) == 0)
    {
      g_set_error (error, HF_ERROR, 1, "%s: empty", path);
      return FALSE;
    }
  return TRUE;
}

static gint64
special (HfTok *t, const gchar *tok)
{
  gint64 id = lookup (t->vocab, tok);
  return id >= 0 ? id : lookup (t->added, tok);
}

static void
added_len (gpointer key, gpointer value, gpointer data)
{
  HfTok *t = data;
  t->added_max = MAX (t->added_max, strlen (key));
}

HfTok *
hf_tok_load (const gchar *dir, GError **error)
{
  HfTok *t = g_new0 (HfTok, 1);
  gchar *vtxt = g_build_filename (dir, "vocab.txt", NULL);
  gchar *vjson = g_build_filename (dir, "vocab.json", NULL);
  gchar *merges = g_build_filename (dir, "merges.txt", NULL);
  gchar *added = g_build_filename (dir, "added_tokens.json", NULL);
  gboolean ok;

  t->vocab = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->added = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  t->bpe = g_file_test (vjson, G_FILE_TEST_EXISTS);
  if (t->bpe)
    {
      t->ranks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      t->cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) g_array_unref);
      byte_map_init (t);
      ok = load_json_ids (vjson, t->vocab, error) && load_merges (t, merges, error);
    }
  else
    ok = load_vocab_txt (t, vtxt, error);
  if (ok && g_file_test (added, G_FILE_TEST_EXISTS))
    ok = load_json_ids (added, t->added, error);
  if (ok)
    {
      g_hash_table_foreach (t->added, added_len, t);
      if (t->bpe)
        t->cls = special (t, "<s>"), t->sep = special (t, "</s>"),
        t->pad = special (t, "<pad>"), t->unk = special (t, "<unk>");
      else
        t->cls = special (t, "[CLS]"), t->sep = special (t, "[SEP]"),
        t->pad = special (t, "[PAD]"), t->unk = special (t, "[UNK]");
      if (t->cls < 0 || t->sep < 0 || t->pad < 0 || (!t->bpe && t->unk < 0))
        {
          g_set_error (error, HF_ERROR, 1, "%s: special tokens missing", dir);
          ok = FALSE;
        }
    }
  g_free (vtxt);
  g_free (vjson);
  g_free (merges);
  g_free (added);
  if (!ok)
    {
      hf_tok_free (t);
      return NULL;
    }
  return t;
}

void
hf_tok_free (HfTok *t)
{
  if (t == NULL)
    return;
  g_clear_pointer (&t->vocab, g_hash_table_unref);
  g_clear_pointer (&t->added, g_hash_table_unref);
  g_clear_pointer (&t->ranks, g_hash_table_unref);
  g_clear_pointer (&t->cache, g_hash_table_unref);
  for (int b = 0; b < 256; b++)
    g_free (t->byte_str[b]);
  g_free (t);
}

gint64
hf_tok_pad_id (HfTok *t)
{
  return t->pad;
}

/* ------------------------------------------------------------- WordPiece */

static gboolean
is_cjk (gunichar c)
{
  return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)
         || (c >= 0x20000 && c <= 0x2A6DF) || (c >= 0x2A700 && c <= 0x2B73F)
         || (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF)
         || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

static gboolean
bert_space (gunichar c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r'
         || g_unichar_type (c) == G_UNICODE_SPACE_SEPARATOR;
}

static gboolean
bert_control (gunichar c)
{
  GUnicodeType ty;

  if (c == '\t' || c == '\n' || c == '\r')
    return FALSE;
  ty = g_unichar_type (c);
  return ty == G_UNICODE_CONTROL || ty == G_UNICODE_FORMAT || ty == G_UNICODE_UNASSIGNED
         || ty == G_UNICODE_PRIVATE_USE;
}

static gboolean
bert_punct (gunichar c)
{
  if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126))
    return TRUE;
  switch (g_unichar_type (c))
    {
    case G_UNICODE_CONNECT_PUNCTUATION: case G_UNICODE_DASH_PUNCTUATION:
    case G_UNICODE_CLOSE_PUNCTUATION: case G_UNICODE_FINAL_PUNCTUATION:
    case G_UNICODE_INITIAL_PUNCTUATION: case G_UNICODE_OTHER_PUNCTUATION:
    case G_UNICODE_OPEN_PUNCTUATION:
      return TRUE;
    default:
      return FALSE;
    }
}

/* BertNormalizer(clean_text, handle_chinese_chars, no lower case) */
static gchar *
bert_normalize (const gchar *s, gssize len)
{
  GString *o = g_string_new (NULL);
  const gchar *end = s + len;

  for (const gchar *p = s; p < end; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == 0 || c == 0xFFFD || bert_control (c))
        continue;
      if (bert_space (c))
        g_string_append_c (o, ' ');
      else if (is_cjk (c))
        {
          g_string_append_c (o, ' ');
          g_string_append_unichar (o, c);
          g_string_append_c (o, ' ');
        }
      else
        g_string_append_unichar (o, c);
    }
  return g_string_free (o, FALSE);
}

static void
wordpiece (HfTok *t, const gchar *word, gsize len, GArray *ids)
{
  GArray *w = g_array_new (FALSE, FALSE, sizeof (gint64));
  const gchar *start = word, *wend = word + len;
  gboolean bad = g_utf8_strlen (word, len) > WP_MAX_CHARS;
  GString *piece = g_string_new (NULL);

  while (!bad && start < wend)
    {
      const gchar *end = wend;
      gint64 id = -1;

      while (end > start)
        {
          g_string_assign (piece, start == word ? "" : "##");
          g_string_append_len (piece, start, end - start);
          if ((id = lookup (t->vocab, piece->str)) >= 0)
            break;
          end = g_utf8_prev_char (end);
        }
      if (id < 0)
        bad = TRUE;
      else
        {
          g_array_append_val (w, id);
          start = end;
        }
    }
  if (bad)
    g_array_append_val (ids, t->unk);
  else
    g_array_append_vals (ids, w->data, w->len);
  g_string_free (piece, TRUE);
  g_array_free (w, TRUE);
}

/* BertPreTokenizer + WordPiece on a normalized segment */
static void
wp_segment (HfTok *t, const gchar *s, gsize len, GArray *ids)
{
  const gchar *end = s + len, *w = NULL;

  for (const gchar *p = s; p < end; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == ' ' || bert_punct (c))
        {
          if (w != NULL)
            wordpiece (t, w, p - w, ids), w = NULL;
          if (c != ' ')
            wordpiece (t, p, g_utf8_next_char (p) - p, ids);
        }
      else if (w == NULL)
        w = p;
    }
  if (w != NULL)
    wordpiece (t, w, end - w, ids);
}

/* ------------------------------------------------------- byte-level BPE */

typedef enum { CL_SPACE, CL_LETTER, CL_NUMBER, CL_OTHER } CharClass;

static CharClass
char_class (gunichar c)
{
  if (g_unichar_isspace (c) || c == 0x85 || g_unichar_type (c) == G_UNICODE_SPACE_SEPARATOR)
    return CL_SPACE;
  switch (g_unichar_type (c))
    {
    case G_UNICODE_UPPERCASE_LETTER: case G_UNICODE_LOWERCASE_LETTER:
    case G_UNICODE_TITLECASE_LETTER: case G_UNICODE_MODIFIER_LETTER:
    case G_UNICODE_OTHER_LETTER:
      return CL_LETTER;
    case G_UNICODE_DECIMAL_NUMBER: case G_UNICODE_LETTER_NUMBER: case G_UNICODE_OTHER_NUMBER:
      return CL_NUMBER;
    default:
      return CL_OTHER;
    }
}

static const gchar *
run_of (const gchar *p, const gchar *end, CharClass cl)
{
  while (p < end && char_class (g_utf8_get_char (p)) == cl)
    p = g_utf8_next_char (p);
  return p;
}

/* the GPT-2 pattern: 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+|
 * ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+ ; returns the end of the piece at p */
static const gchar *
gpt2_piece (const gchar *p, const gchar *end)
{
  static const gchar *const contr[] = { "'s", "'t", "'re", "'ve", "'m", "'ll", "'d" };
  const gchar *q;
  CharClass cl;

  if (*p == '\'')
    for (guint i = 0; i < G_N_ELEMENTS (contr); i++)
      {
        gsize l = strlen (contr[i]);
        if ((gsize) (end - p) >= l && strncmp (p, contr[i], l) == 0)
          return p + l;
      }
  q = p;
  if (*q == ' ' && q + 1 < end && char_class (g_utf8_get_char (q + 1)) != CL_SPACE)
    q++;
  cl = char_class (g_utf8_get_char (q));
  if (cl != CL_SPACE)
    return run_of (q, end, cl);
  /* whitespace: all of it at the end, else all but the last character */
  q = run_of (p, end, CL_SPACE);
  if (q < end)
    {
      const gchar *last = g_utf8_prev_char (q);
      if (last > p)
        return last;
    }
  return q;
}

static gint64
rank_of (HfTok *t, const gchar *a, const gchar *b)
{
  gchar *k = g_strconcat (a, " ", b, NULL);
  gint64 r = lookup (t->ranks, k);
  g_free (k);
  return r;
}

static GArray *
bpe_word (HfTok *t, const gchar *piece)
{
  GPtrArray *sym = g_ptr_array_new_with_free_func (g_free);
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  const gchar *end = piece + strlen (piece);

  for (const gchar *p = piece; p < end; p = g_utf8_next_char (p))
    g_ptr_array_add (sym, g_strndup (p, g_utf8_next_char (p) - p));
  while (sym->len > 1)
    {
      gint64 best = -1;
      guint bi = 0;
      const gchar *a, *b;

      for (guint i = 0; i + 1 < sym->len; i++)
        {
          gint64 r = rank_of (t, sym->pdata[i], sym->pdata[i + 1]);
          if (r >= 0 && (best < 0 || r < best))
            best = r, bi = i;
        }
      if (best < 0)
        break;
      a = sym->pdata[bi];
      b = sym->pdata[bi + 1];
      {
        gchar *ca = g_strdup (a), *cb = g_strdup (b);

        /* merge every occurrence of the pair, left to right */
        for (guint i = 0; i + 1 < sym->len; i++)
          if (strcmp (sym->pdata[i], ca) == 0 && strcmp (sym->pdata[i + 1], cb) == 0)
            {
              gchar *m = g_strconcat (ca, cb, NULL);
              g_free (sym->pdata[i]);
              sym->pdata[i] = m;
              g_ptr_array_remove_index (sym, i + 1);
            }
        g_free (ca);
        g_free (cb);
      }
    }
  for (guint i = 0; i < sym->len; i++)
    {
      gint64 id = lookup (t->vocab, sym->pdata[i]);
      if (id < 0)
        id = lookup (t->added, sym->pdata[i]);
      if (id < 0 && t->unk >= 0)
        id = t->unk;
      if (id >= 0)
        g_array_append_val (ids, id);
    }
  g_ptr_array_free (sym, TRUE);
  return ids;
}

static void
bpe_segment (HfTok *t, const gchar *s, gsize len, GArray *ids)
{
  const gchar *end = s + len, *p = s;
  GString *mapped = g_string_new (NULL);

  while (p < end)
    {
      const gchar *q = gpt2_piece (p, end);
      GArray *w;

      if (q <= p)
        q = g_utf8_next_char (p);
      g_string_truncate (mapped, 0);
      for (const gchar *b = p; b < q; b++)
        g_string_append (mapped, t->byte_str[(guchar) *b]);
      w = g_hash_table_lookup (t->cache, mapped->str);
      if (w == NULL)
        {
          w = bpe_word (t, mapped->str);
          if (g_hash_table_size (t->cache) > BPE_CACHE_MAX)
            g_hash_table_remove_all (t->cache);
          g_hash_table_insert (t->cache, g_strdup (mapped->str), w);
        }
      g_array_append_vals (ids, w->data, w->len);
      p = q;
    }
  g_string_free (mapped, TRUE);
}

/* ----------------------------------------------------------------- encode */

static void
segment (HfTok *t, const gchar *s, gsize len, GArray *ids)
{
  if (len == 0)
    return;
  if (t->bpe)
    bpe_segment (t, s, len, ids);
  else
    wp_segment (t, s, len, ids);
}

/* added tokens first (leftmost-longest), the model on the rest */
static GArray *
encode_one (HfTok *t, const gchar *text)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  gchar *norm = t->bpe ? g_strdup (text) : bert_normalize (text, strlen (text));
  const gchar *p = norm, *seg = norm, *end = norm + strlen (norm);
  gchar *buf = g_malloc (t->added_max + 1);

  while (p < end && t->added_max > 0)
    {
      gsize l = MIN ((gsize) (end - p), t->added_max);
      gint64 id = -1;

      /* longest first; only whole characters */
      while (l > 0)
        {
          if (p + l == end || (p[l] & 0xC0) != 0x80)
            {
              memcpy (buf, p, l);
              buf[l] = '\0';
              if ((id = lookup (t->added, buf)) >= 0)
                break;
            }
          l--;
        }
      if (id >= 0)
        {
          segment (t, seg, p - seg, ids);
          g_array_append_val (ids, id);
          p += l;
          seg = p;
        }
      else
        p = g_utf8_next_char (p);
    }
  segment (t, seg, end - seg, ids);
  g_free (buf);
  g_free (norm);
  return ids;
}

GArray *
hf_tok_encode_pair (HfTok *t, const gchar *a, const gchar *b, guint max_len)
{
  GArray *ia = encode_one (t, a), *ib = encode_one (t, b);
  GArray *out = g_array_new (FALSE, FALSE, sizeof (gint64));
  gboolean pair = *b != '\0';  /* transformers takes an empty b for none */
  guint specials = !pair ? 2 : t->bpe ? 4 : 3;

  while (ia->len + ib->len + specials > max_len && (ia->len > 0 || ib->len > 0))
    {
      if (ia->len >= ib->len)
        g_array_set_size (ia, ia->len - 1);
      else
        g_array_set_size (ib, ib->len - 1);
    }
  g_array_append_val (out, t->cls);
  g_array_append_vals (out, ia->data, ia->len);
  g_array_append_val (out, t->sep);
  if (pair)
    {
      if (t->bpe)
        g_array_append_val (out, t->sep);
      g_array_append_vals (out, ib->data, ib->len);
      g_array_append_val (out, t->sep);
    }
  g_array_free (ia, TRUE);
  g_array_free (ib, TRUE);
  return out;
}
