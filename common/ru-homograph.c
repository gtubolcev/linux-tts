/*
 * ru-homograph - stress of Russian homographs from the context (see
 * ru-homograph.h)
 *
 * The model input is built as RUAccent 1.6 builds it (ruaccent.py
 * _apply_omographs, text_preprocessor.py analyze/join_for_model,
 * omograph_model.py classify), so the scores match its own.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ru-homograph.h"

#include <math.h>
#include <string.h>

#include "hf-tok.h"

#ifdef HAVE_ORT_HEADERS
#include <dlfcn.h>
#include <zlib.h>
#include <onnxruntime_c_api.h>
#endif

#define MAX_LEN 512             /* max_length of the tokenizer call */
#define RH_ERROR (g_quark_from_static_string ("ru-homograph"))
#define BATCH 32                /* pairs per model run, as RUAccent */
#define MAX_GZ (32 * 1024 * 1024)
#define RETRY_US (300 * G_USEC_PER_SEC)  /* after a failed load */

#ifndef TTS_ORT_LIB
#define TTS_ORT_LIB "libonnxruntime.so.1"
#endif

struct RuHomograph
{
  gchar *dir, *size;
  int threads, idle;
  GMutex lock;
  GCond cond;
  GHashTable *omographs;        /* lower-case form -> variants (strv) */
  gboolean dict_failed;
  HfTok *tok;
  gpointer session;             /* OrtSession */
  gint64 failed_at;             /* model load failed then; 0 = not */
  gint64 last_use;
  GThread *reaper;
  gboolean stop;
  RuHomographTrace trace;
  gpointer trace_data;
};

/* ------------------------------------------------------------ onnxruntime */

#ifdef HAVE_ORT_HEADERS

static const OrtApi *ort;
static OrtEnv *ort_env;
static OrtMemoryInfo *ort_mem;
static GMutex ort_init_lock;

static gboolean
ort_ok (OrtStatus *st, GError **error)
{
  if (st == NULL)
    return TRUE;
  g_set_error (error, RH_ERROR, 1, "onnxruntime: %s", ort->GetErrorMessage (st));
  ort->ReleaseStatus (st);
  return FALSE;
}

static gboolean
ort_init (GError **error)
{
  static gboolean tried;
  static gchar *why;
  gboolean ok;

  g_mutex_lock (&ort_init_lock);
  if (!tried)
    {
      const OrtApiBase *(*get_base) (void) = NULL;
      void *lib = dlopen (TTS_ORT_LIB, RTLD_NOW | RTLD_LOCAL);
      GError *e = NULL;

      tried = TRUE;
      /* then the distribution's: Debian names it libonnxruntime.so.1.23 */
      if (lib == NULL)
        lib = dlopen ("libonnxruntime.so.1", RTLD_NOW | RTLD_LOCAL);
      if (lib == NULL)
        lib = dlopen ("libonnxruntime.so", RTLD_NOW | RTLD_LOCAL);
      if (lib != NULL)
        get_base = (const OrtApiBase *(*) (void)) dlsym (lib, "OrtGetApiBase");
      if (get_base != NULL)
        for (int v = ORT_API_VERSION; v >= 16 && ort == NULL; v--)
          ort = get_base ()->GetApi (v);
      if (ort == NULL)
        why = g_strdup_printf ("cannot load %s", TTS_ORT_LIB);
      else if (!ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "ru-homograph", &ort_env), &e)
               || !ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &ort_mem), &e))
        {
          why = g_strdup (e->message);
          g_error_free (e);
          ort = NULL;
        }
    }
  ok = ort != NULL;
  if (!ok)
    g_set_error (error, RH_ERROR, 1, "%s", why);
  g_mutex_unlock (&ort_init_lock);
  return ok;
}

static gpointer
session_new (const gchar *path, int threads, GError **error)
{
  OrtSessionOptions *o = NULL;
  OrtSession *s = NULL;
  gboolean ok;

  ok = ort_ok (ort->CreateSessionOptions (&o), error)
       && ort_ok (ort->SetIntraOpNumThreads (o, MAX (threads, 1)), error)
       && ort_ok (ort->SetInterOpNumThreads (o, 1), error)
       && ort_ok (ort->SetSessionGraphOptimizationLevel (o, ORT_ENABLE_ALL), error)
       && ort_ok (ort->CreateSession (ort_env, path, o, &s), error);
  if (o != NULL)
    ort->ReleaseSessionOptions (o);
  return ok ? s : NULL;
}

static void
session_free (gpointer s)
{
  if (s != NULL)
    ort->ReleaseSession (s);
}

/* probabilities of class 1 for n rows of ids, padded to the longest */
static gboolean
session_run (RuHomograph *h, GArray **rows, guint n, float *probs, GError **error)
{
  static const char *in_names[] = { "input_ids", "attention_mask" };
  static const char *out_names[] = { "logits" };
  guint len = 0;
  gint64 *ids, *mask, pad = hf_tok_pad_id (h->tok);
  int64_t shape[2];
  OrtValue *in[2] = { NULL, NULL }, *out = NULL;
  float *logits;
  gboolean ok;

  for (guint i = 0; i < n; i++)
    len = MAX (len, rows[i]->len);
  ids = g_new (gint64, (gsize) n * len);
  mask = g_new (gint64, (gsize) n * len);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < len; j++)
      {
        gboolean real = j < rows[i]->len;
        ids[i * len + j] = real ? g_array_index (rows[i], gint64, j) : pad;
        mask[i * len + j] = real;
      }
  shape[0] = n;
  shape[1] = len;
  ok = ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_mem, ids, sizeof (gint64) * n * len, shape, 2,
                                                    ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), error)
       && ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_mem, mask, sizeof (gint64) * n * len, shape, 2,
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1]), error)
       && ort_ok (ort->Run (h->session, NULL, in_names, (const OrtValue *const *) in, 2,
                            out_names, 1, &out), error)
       && ort_ok (ort->GetTensorMutableData (out, (void **) &logits), error);
  if (ok)
    {
      OrtTensorTypeAndShapeInfo *info = NULL;
      size_t count = 0;

      ok = ort_ok (ort->GetTensorTypeAndShape (out, &info), error)
           && ort_ok (ort->GetTensorShapeElementCount (info, &count), error);
      if (info != NULL)
        ort->ReleaseTensorTypeAndShapeInfo (info);
      if (ok && count != (size_t) n * 2)
        {
          g_set_error (error, RH_ERROR, 1, "model output: %zu values", count);
          ok = FALSE;
        }
      for (guint i = 0; ok && i < n; i++)
        probs[i] = 1.0f / (1.0f + expf (logits[2 * i] - logits[2 * i + 1]));
    }
  for (int i = 0; i < 2; i++)
    if (in[i] != NULL)
      ort->ReleaseValue (in[i]);
  if (out != NULL)
    ort->ReleaseValue (out);
  g_free (ids);
  g_free (mask);
  return ok;
}

static gchar *
gz_read (const gchar *path, gsize *len, GError **error)
{
  gzFile f = gzopen (path, "rb");
  GByteArray *b;
  guint8 buf[65536];
  int n;

  if (f == NULL)
    {
      g_set_error (error, RH_ERROR, 1, "%s: cannot open", path);
      return NULL;
    }
  b = g_byte_array_new ();
  while ((n = gzread (f, buf, sizeof buf)) > 0 && b->len <= MAX_GZ)
    g_byte_array_append (b, buf, n);
  if (n < 0 || b->len > MAX_GZ)
    {
      g_set_error (error, RH_ERROR, 1, "%s: bad or too large", path);
      gzclose (f);
      g_byte_array_free (b, TRUE);
      return NULL;
    }
  gzclose (f);
  *len = b->len;
  g_byte_array_append (b, (const guint8 *) "", 1);
  return (gchar *) g_byte_array_free (b, FALSE);
}

#else /* no onnxruntime headers at build time */

static gboolean
ort_init (GError **error)
{
  g_set_error (error, RH_ERROR, 1, "built without onnxruntime");
  return FALSE;
}
static gpointer session_new (const gchar *p, int t, GError **e) { return NULL; }
static void session_free (gpointer s) { }
static gboolean session_run (RuHomograph *h, GArray **r, guint n, float *p, GError **e) { return FALSE; }
static gchar *
gz_read (const gchar *path, gsize *len, GError **error)
{
  g_set_error (error, RH_ERROR, 1, "built without zlib");
  return NULL;
}

#endif

/* ---------------------------------------------------------------- letters */

static gboolean
is_vowel (gunichar c)
{
  static const gunichar v[] = { 0x430, 0x435, 0x451, 0x438, 0x43E, 0x443, 0x44B, 0x44D, 0x44E, 0x44F };

  c = g_unichar_tolower (c);
  for (guint i = 0; i < G_N_ELEMENTS (v); i++)
    if (c == v[i])
      return TRUE;
  return FALSE;
}

static gunichar
fold (gunichar c)
{
  c = g_unichar_tolower (c);
  return c == 0x451 ? 0x435 : c;
}

/* the key: lower case, ё as е */
static gchar *
fold_word (const gchar *w, gssize len)
{
  GString *s = g_string_new (NULL);
  const gchar *end = w + (len < 0 ? (gssize) strlen (w) : len);

  for (const gchar *p = w; p < end; p = g_utf8_next_char (p))
    g_string_append_unichar (s, fold (g_utf8_get_char (p)));
  return g_string_free (s, FALSE);
}

/* a variant from omographs.json: one "+" before a vowel, the letters of key */
static gboolean
variant_ok (const gchar *key, const gchar *v)
{
  const gchar *plus = strchr (v, '+');
  GString *s;
  gchar *letters;
  gboolean ok;

  if (plus == NULL || strchr (plus + 1, '+') != NULL || !is_vowel (g_utf8_get_char (plus + 1)))
    return FALSE;
  s = g_string_new (NULL);
  for (const gchar *p = v; *p != '\0'; p++)
    if (*p != '+')
      g_string_append_c (s, *p);
  letters = fold_word (s->str, -1);
  g_string_free (s, TRUE);
  ok = strcmp (letters, key) == 0;
  g_free (letters);
  return ok;
}

static void
put_omograph (const gchar *key, gint64 num, gchar **strs, gpointer data)
{
  GHashTable *h = data;
  GPtrArray *ok;
  gchar *k;

  if (strs == NULL || g_utf8_strlen (key, -1) > 64)
    return;
  k = fold_word (key, -1);
  ok = g_ptr_array_new ();
  for (gchar **v = strs; *v != NULL; v++)
    if (variant_ok (k, *v))
      g_ptr_array_add (ok, g_strdup (*v));
  g_ptr_array_add (ok, NULL);
  if (ok->len > 2)
    g_hash_table_replace (h, k, g_ptr_array_free (ok, FALSE));
  else
    {
      g_strfreev ((gchar **) g_ptr_array_free (ok, FALSE));
      g_free (k);
    }
}

/* ------------------------------------------------------------------ files */

static gchar *
first_existing (const gchar *a, const gchar *b)
{
  if (g_file_test (a, G_FILE_TEST_EXISTS))
    return g_strdup (a);
  return g_strdup (b);
}

static gboolean
load_dict (RuHomograph *h, GError **error)
{
  gchar *pa = g_build_filename (h->dir, "omographs.json.gz", NULL);
  gchar *pb = g_build_filename (h->dir, "dictionary", "omographs.json.gz", NULL);
  gchar *path = first_existing (pa, pb), *text;
  gsize len;
  gboolean ok = FALSE;

  if ((text = gz_read (path, &len, error)) != NULL)
    {
      h->omographs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_strfreev);
      ok = hf_json_flat_parse (text, len, put_omograph, h->omographs, error);
      if (ok)
        {
          /* as RUAccent adds it */
          gchar *kosa[] = { "к+оса", "кос+а", NULL };
          put_omograph ("коса", -1, kosa, h->omographs);
        }
      else
        {
          g_prefix_error (error, "%s: ", path);
          g_clear_pointer (&h->omographs, g_hash_table_unref);
        }
      g_free (text);
    }
  g_free (pa);
  g_free (pb);
  g_free (path);
  return ok;
}

static gpointer reaper_run (gpointer data);

/* with the lock held */
static gboolean
load_model (RuHomograph *h, GError **error)
{
  gchar *pa = g_build_filename (h->dir, h->size, NULL);
  gchar *pb = g_build_filename (h->dir, "nn", "nn_omograph", h->size, NULL);
  gchar *mdir = first_existing (pa, pb);
  gchar *onnx = g_build_filename (mdir, "model.onnx", NULL);
  gboolean ok;

  ok = ort_init (error) && (h->tok = hf_tok_load (mdir, error)) != NULL
       && (h->session = session_new (onnx, h->threads, error)) != NULL;
  if (!ok)
    g_clear_pointer (&h->tok, hf_tok_free);
  else if (h->idle > 0 && h->reaper == NULL)
    h->reaper = g_thread_new ("ru-homograph", reaper_run, h);
  else
    g_cond_broadcast (&h->cond);
  g_free (pa);
  g_free (pb);
  g_free (mdir);
  g_free (onnx);
  return ok;
}

/* unloads the model after h->idle seconds without use */
static gpointer
reaper_run (gpointer data)
{
  RuHomograph *h = data;

  g_mutex_lock (&h->lock);
  while (!h->stop)
    {
      if (h->session == NULL)
        g_cond_wait (&h->cond, &h->lock);
      else
        {
          gint64 due = h->last_use + (gint64) h->idle * G_USEC_PER_SEC;
          if (g_get_monotonic_time () >= due)
            {
              session_free (h->session);
              h->session = NULL;
              g_clear_pointer (&h->tok, hf_tok_free);
            }
          else
            g_cond_wait_until (&h->cond, &h->lock, due);
        }
    }
  g_mutex_unlock (&h->lock);
  return NULL;
}

RuHomograph *
ru_homograph_new (const gchar *dir, const gchar *size, int threads, int idle)
{
  RuHomograph *h;

  if (dir == NULL || size == NULL)
    return NULL;
  if (g_ascii_strcasecmp (size, "tiny") == 0)
    size = "tiny2.1";
  else if (g_ascii_strcasecmp (size, "turbo") == 0)
    size = "turbo3.1";
  for (const gchar *p = size; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '.' && *p != '_' && *p != '-')
      return NULL;
  if (*size == '\0' || *size == '.')
    return NULL;
  h = g_new0 (RuHomograph, 1);
  h->dir = g_strdup (dir);
  h->size = g_strdup (size);
  h->threads = CLAMP (threads, 1, 16);
  h->idle = idle;
  g_mutex_init (&h->lock);
  g_cond_init (&h->cond);
  return h;
}

void
ru_homograph_free (RuHomograph *h)
{
  if (h == NULL)
    return;
  g_mutex_lock (&h->lock);
  h->stop = TRUE;
  g_cond_broadcast (&h->cond);
  g_mutex_unlock (&h->lock);
  if (h->reaper != NULL)
    g_thread_join (h->reaper);
  session_free (h->session);
  hf_tok_free (h->tok);
  g_clear_pointer (&h->omographs, g_hash_table_unref);
  g_mutex_clear (&h->lock);
  g_cond_clear (&h->cond);
  g_free (h->dir);
  g_free (h->size);
  g_free (h);
}

void
ru_homograph_set_trace (RuHomograph *h, RuHomographTrace trace, gpointer data)
{
  if (h == NULL)
    return;
  h->trace = trace;
  h->trace_data = data;
}

/* -------------------------------------------------------------- the text */

/* a word for the alignment of the text before and after the dictionary:
 * letters, U+0301 after them, "+" before a letter */
typedef struct
{
  gsize start, end;             /* bytes in marked */
  gboolean user;                /* marked in the text before the dictionary */
  gboolean changed;
  gchar *repl;
} Unit;

static gboolean
plus_starts (const gchar *p)
{
  return *p == '+' && p[1] != '\0' && g_unichar_isalpha (g_utf8_get_char (p + 1));
}

/* the units of s; with_marks says for each whether it has a mark */
static GArray *
units_of (const gchar *s, GArray **with_marks)
{
  GArray *u = g_array_new (FALSE, TRUE, sizeof (Unit));
  const gchar *p = s;

  *with_marks = g_array_new (FALSE, FALSE, sizeof (gboolean));
  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isalpha (c) || plus_starts (p))
        {
          Unit x = { 0 };
          gboolean mark = FALSE;

          x.start = p - s;
          for (;;)
            {
              c = g_utf8_get_char (p);
              if (c == 0x301)
                mark = TRUE;
              else if (plus_starts (p))
                mark = TRUE;
              else if (!g_unichar_isalpha (c))
                break;
              p = g_utf8_next_char (p);
            }
          x.end = p - s;
          g_array_append_val (u, x);
          g_array_append_val (*with_marks, mark);
        }
      else
        p = g_utf8_next_char (p);
    }
  return u;
}

/* Python's \s for str, close enough */
static gboolean
py_space (gunichar c)
{
  return g_unichar_isspace (c) || (c >= 0x1C && c <= 0x1F) || c == 0x85
         || g_unichar_type (c) == G_UNICODE_SPACE_SEPARATOR;
}

/* MODEL_CHARS of text_preprocessor.py */
static gboolean
model_char (gunichar c)
{
  if (c < 128)
    return g_ascii_isalnum (c) || py_space (c) || strchr (".,!?:;\"'(){}[]-", (int) c) != NULL;
  return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451 || py_space (c)
         || c == 0x2014 || c == 0xAB || c == 0xBB || c == 0x201E || c == 0x201C || c == 0x201D;
}

static gboolean
py_word (gunichar c)
{
  return g_unichar_isalnum (c) || c == '_';
}

typedef struct
{
  gchar *text;
  int unit;                     /* the unit it is all of, or -1 */
  gboolean clean, is_word;
} Tok;

static void
tok_clear (gpointer p)
{
  g_free (((Tok *) p)->text);
}

static void
py_replace (GString *s, const gchar *from, const gchar *to)
{
  g_string_replace (s, from, to, 0);
}

/* TextPreprocessor.join_for_model */
static gchar *
join_for_model (gchar **words)
{
  GString *s = g_string_new (NULL);
  gchar *joined = g_strjoinv (" ", words);
  static const gchar chars[] = "!\"#$%&'()*,./:;<=>?@[\\]^_`{|}-";

  g_string_assign (s, joined);
  g_free (joined);
  for (const gchar *c = chars; *c != '\0'; c++)
    {
      gchar from[3] = { ' ', *c, '\0' }, to[2] = { *c, '\0' };

      if (*c == '-')
        {
          py_replace (s, " -", "-");
          py_replace (s, "- ", "-");
        }
      py_replace (s, from, to);
    }
  py_replace (s, "~", "-");
  return g_string_free (s, FALSE);
}

/* _SPACE_BEFORE_PUNCT: \s+(?=[,.?!:;…]) */
static gchar *
drop_space_before_punct (const gchar *t)
{
  GString *o = g_string_new (NULL);
  const gchar *p = t;

  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);

      if (py_space (c))
        {
          const gchar *q = p;
          gunichar n;

          while (*q != '\0' && py_space (g_utf8_get_char (q)))
            q = g_utf8_next_char (q);
          n = g_utf8_get_char (q);
          if (*q != '\0' && (strchr (",.?!:;", (int) (n < 128 ? n : 1)) != NULL || n == 0x2026))
            {
              p = q;
              continue;
            }
          g_string_append_len (o, p, q - p);
          p = q;
          continue;
        }
      g_string_append_unichar (o, c);
      p = g_utf8_next_char (p);
    }
  return g_string_free (o, FALSE);
}

static gboolean
caps_word (const gchar *w)
{
  int n = 0;

  for (const gchar *p = w; *p != '\0'; p = g_utf8_next_char (p), n++)
    {
      gunichar c = g_utf8_get_char (p);
      if (!((c >= 'A' && c <= 'Z') || (c >= 0x410 && c <= 0x42F) || c == 0x401))
        return FALSE;
    }
  return n >= 2;
}

/* the stressed form of unit text u (letters only) by variant v, in mark;
 * NULL if they do not fit */
static gchar *
apply_variant (const gchar *u, const gchar *v, RuStressMark mark)
{
  glong k = 0, i = 0;
  const gchar *plus = strchr (v, '+');
  GString *o;
  gchar *fu = fold_word (u, -1), *fv;
  GString *vl = g_string_new (NULL);

  for (const gchar *p = v; *p != '\0'; p++)
    if (*p != '+')
      g_string_append_c (vl, *p);
  fv = fold_word (vl->str, -1);
  g_string_free (vl, TRUE);
  if (plus == NULL || strcmp (fu, fv) != 0)
    {
      g_free (fu);
      g_free (fv);
      return NULL;
    }
  g_free (fu);
  g_free (fv);
  k = g_utf8_strlen (v, plus - v);
  o = g_string_new (NULL);
  for (const gchar *p = u; *p != '\0'; p = g_utf8_next_char (p), i++)
    {
      gunichar c = g_utf8_get_char (p);

      if (i == k && mark == RU_STRESS_PLUS)
        g_string_append_c (o, '+');
      g_string_append_unichar (o, c);
      if (i == k && mark == RU_STRESS_ACUTE)
        g_string_append_unichar (o, 0x301);
    }
  return g_string_free (o, FALSE);
}

typedef struct
{
  int tok;                      /* index in the sentence tokens */
  gchar **variants;
  int first, n;                 /* their pairs */
} Homo;

/* one sentence: plain[s0..s1) with unit_of; updates units */
static void
resolve_sentence (RuHomograph *h, const gunichar *plain, const int *unit_of, glong s0, glong s1,
                  GArray *units, const gchar *marked, RuStressMark mark)
{
  GArray *keep = g_array_new (FALSE, FALSE, sizeof (glong));
  GArray *toks = g_array_new (FALSE, TRUE, sizeof (Tok));
  GArray *homos = g_array_new (FALSE, TRUE, sizeof (Homo));
  GPtrArray *texts = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref);
  gunichar *mt;
  glong n = 0, first_word = -1;
  float *probs = NULL;
  GError *error = NULL;

  g_array_set_clear_func (toks, tok_clear);
  for (glong i = s0; i < s1; i++)
    if (model_char (plain[i]))
      g_array_append_val (keep, i);
  n = keep->len;
  mt = g_new (gunichar, n + 1);
  for (glong i = 0; i < n; i++)
    mt[i] = plain[g_array_index (keep, glong, i)];
  /* " - " -> " ~ ", left to right */
  for (glong i = 0; i + 2 < n; i++)
    if (mt[i] == ' ' && mt[i + 1] == '-' && mt[i + 2] == ' ')
      mt[i + 1] = '~', i += 2;
  /* TOKEN: \w+|[^\w\s]+ */
  for (glong i = 0; i < n;)
    {
      glong j = i;
      Tok t = { 0 };
      GString *s;

      if (py_space (mt[i]))
        {
          i++;
          continue;
        }
      if (py_word (mt[i]))
        while (j < n && py_word (mt[j]))
          j++;
      else
        while (j < n && !py_word (mt[j]) && !py_space (mt[j]))
          j++;
      s = g_string_new (NULL);
      for (glong k = i; k < j; k++)
        g_string_append_unichar (s, mt[k]);
      t.text = g_string_free (s, FALSE);
      t.is_word = py_word (mt[i]);
      /* clean: nothing dropped inside, no "~" */
      t.clean = g_array_index (keep, glong, j - 1) - g_array_index (keep, glong, i) == j - 1 - i
                && strchr (t.text, '~') == NULL;
      t.unit = unit_of[g_array_index (keep, glong, i)];
      for (glong k = i; k < j && t.unit >= 0; k++)
        if (unit_of[g_array_index (keep, glong, k)] != t.unit)
          t.unit = -1;
      if (t.unit >= 0)
        {
          /* the token must be the whole unit */
          glong a = g_array_index (keep, glong, i), b = g_array_index (keep, glong, j - 1);
          if ((a > 0 && unit_of[a - 1] == t.unit) || (b + 1 < s1 && unit_of[b + 1] == t.unit))
            t.unit = -1;
        }
      if (t.is_word && first_word < 0)
        first_word = toks->len;
      g_array_append_val (toks, t);
      i = j;
    }
  g_free (mt);

  /* the homographs */
  for (guint i = 0; i < toks->len; i++)
    {
      Tok *t = &g_array_index (toks, Tok, i);
      gchar *key, **vars;
      gunichar c0 = g_utf8_get_char (t->text);
      Homo ho = { 0 };

      if (!t->is_word || !t->clean || t->unit < 0 || g_array_index (units, Unit, t->unit).user
          || strstr (t->text, "ё") != NULL || strstr (t->text, "Ё") != NULL)
        continue;
      /* a capital inside the sentence: a name, the dictionary knows */
      if (g_unichar_isupper (c0) && (int) i != first_word && !caps_word (t->text))
        continue;
      key = fold_word (t->text, -1);
      vars = g_hash_table_lookup (h->omographs, key);
      g_free (key);
      if (vars == NULL)
        continue;
      ho.tok = i;
      ho.variants = vars;
      g_array_append_val (homos, ho);
    }
  if (homos->len == 0)
    goto out;

  /* the model input */
  for (guint k = 0; k < homos->len; k++)
    {
      Homo *ho = &g_array_index (homos, Homo, k);
      gchar **words = g_new0 (gchar *, toks->len + 1);
      gchar *joined, *text, *low;

      for (guint i = 0; i < toks->len; i++)
        {
          Tok *t = &g_array_index (toks, Tok, i);
          words[i] = caps_word (t->text) ? g_utf8_strdown (t->text, -1) : g_strdup (t->text);
        }
      low = g_utf8_strdown (g_array_index (toks, Tok, ho->tok).text, -1);
      g_free (words[ho->tok]);
      words[ho->tok] = g_strconcat (" <w>", low, "</w> ", NULL);
      g_free (low);
      joined = join_for_model (words);
      g_strfreev (words);
      text = drop_space_before_punct (joined);
      g_free (joined);
      ho->first = pairs->len;
      for (gchar **v = ho->variants; *v != NULL; v++)
        g_ptr_array_add (pairs, hf_tok_encode_pair (h->tok, text, *v, MAX_LEN));
      ho->n = pairs->len - ho->first;
      g_ptr_array_add (texts, text);
    }
  probs = g_new0 (float, pairs->len);
  for (guint i = 0; i < pairs->len; i += BATCH)
    if (!session_run (h, (GArray **) pairs->pdata + i, MIN (BATCH, pairs->len - i), probs + i, &error))
      {
        g_printerr ("ru-homograph: %s\n", error->message);
        g_clear_error (&error);
        goto out;
      }
  for (guint k = 0; k < homos->len; k++)
    {
      Homo *ho = &g_array_index (homos, Homo, k);
      Tok *t = &g_array_index (toks, Tok, ho->tok);
      Unit *u = &g_array_index (units, Unit, t->unit);
      int best = 0;
      gchar *re;

      for (int j = 1; j < ho->n; j++)
        if (probs[ho->first + j] > probs[ho->first + best])
          best = j;
      if (h->trace != NULL)
        h->trace (texts->pdata[k], ho->variants, (GArray **) pairs->pdata + ho->first,
                  probs + ho->first, best, h->trace_data);
      re = apply_variant (t->text, ho->variants[best], mark);
      if (re != NULL)
        {
          g_free (u->repl);
          u->repl = re;
          u->changed = TRUE;
        }
    }
out:
  g_free (probs);
  g_ptr_array_free (pairs, TRUE);
  g_ptr_array_free (texts, TRUE);
  g_array_free (homos, TRUE);
  g_array_free (toks, TRUE);
  g_array_free (keep, TRUE);
}

/* the end of a sentence at p[i], close to razdel: a newline, or the last
 * of [.!?…] and closing quotes followed by a space, unless the text goes on
 * in lower case after spaces, quotes and dashes ("правда! — сказал он",
 * "и т. д.") */
static gboolean
sentence_end (const gunichar *p, glong s0, glong i, glong n)
{
  glong j, k;

  if (p[i] == '\n')
    return TRUE;
  if (p[i] != '.' && p[i] != '!' && p[i] != '?' && p[i] != 0x2026 && p[i] != 0xBB && p[i] != '"'
      && p[i] != ')' && p[i] != 0x201C && p[i] != 0x201D)
    return FALSE;
  /* the last of the terminators and closing quotes, then a space */
  if (i + 1 < n && !py_space (p[i + 1]))
    return FALSE;
  for (j = i; j >= 0 && (p[j] == 0xBB || p[j] == '"' || p[j] == ')' || p[j] == 0x201C
                         || p[j] == 0x201D); j--)
    ;
  if (j < s0 || (p[j] != '.' && p[j] != '!' && p[j] != '?' && p[j] != 0x2026))
    return FALSE;
  /* nothing before it yet */
  for (k = s0; k < j && !g_unichar_isalnum (p[k]); k++)
    ;
  if (k == j)
    return FALSE;
  /* an abbreviation (ул., д., стр.) or an initial (А. С.) */
  if (p[j] == '.' && (j == s0 || p[j - 1] != '.'))
    {
      for (k = j; k > s0 && g_unichar_isalpha (p[k - 1]); k--)
        ;
      if (j - k >= 1 && (k == s0 || !g_unichar_isalnum (p[k - 1]))
          && ((j - k <= 3 && g_unichar_islower (p[k])) || (j - k == 1 && g_unichar_isupper (p[k]))))
        return FALSE;
    }
  for (j = i + 1; j < n && (py_space (p[j]) || p[j] == 0x2014 || p[j] == 0x2013 || p[j] == '-'
                            || p[j] == 0xAB || p[j] == '"' || p[j] == 0x201E || p[j] == 0x201C
                            || p[j] == '('); j++)
    if (p[j] == '\n')
      return TRUE;
  return j >= n || !g_unichar_islower (p[j]);
}

/* with the lock held: dictionary and model ready? */
static gboolean
ready (RuHomograph *h)
{
  GError *error = NULL;

  if (h->omographs == NULL)
    {
      if (h->dict_failed)
        return FALSE;
      if (!load_dict (h, &error))
        {
          g_printerr ("ru-homograph: %s; homographs stay as the dictionary marks them\n",
                      error->message);
          g_error_free (error);
          h->dict_failed = TRUE;
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
model_ready (RuHomograph *h)
{
  GError *error = NULL;

  if (h->session != NULL)
    return TRUE;
  if (h->failed_at != 0 && g_get_monotonic_time () - h->failed_at < RETRY_US)
    return FALSE;
  if (!load_model (h, &error))
    {
      g_printerr ("ru-homograph: %s/%s: %s; homographs stay as the dictionary marks them\n",
                  h->dir, h->size, error->message);
      g_error_free (error);
      h->failed_at = g_get_monotonic_time ();
      return FALSE;
    }
  h->failed_at = 0;
  return TRUE;
}

/* any word of the text in the table? (cheap test before the model) */
static gboolean
has_homograph (RuHomograph *h, const gchar *plain_text)
{
  const gchar *p = plain_text;

  while (*p != '\0')
    {
      if (g_unichar_isalpha (g_utf8_get_char (p)))
        {
          const gchar *q = p;
          gchar *k;
          gboolean hit;

          while (*q != '\0' && g_unichar_isalpha (g_utf8_get_char (q)))
            q = g_utf8_next_char (q);
          k = fold_word (p, q - p);
          hit = g_hash_table_contains (h->omographs, k);
          g_free (k);
          if (hit)
            return TRUE;
          p = q;
        }
      else
        p = g_utf8_next_char (p);
    }
  return FALSE;
}

gchar *
ru_homograph_resolve (RuHomograph *h, const gchar *orig, const gchar *marked, RuStressMark mark)
{
  GArray *uo, *um, *mo, *mm;
  gunichar *plain;
  int *unit_of;
  glong n = 0, s0 = 0;
  GString *ptext, *out = NULL;
  gboolean any = FALSE;
  gsize pos = 0;

  if (h == NULL || orig == NULL || marked == NULL || mark == RU_STRESS_NONE
      || !g_utf8_validate (marked, -1, NULL) || !g_utf8_validate (orig, -1, NULL))
    return NULL;
  g_mutex_lock (&h->lock);
  if (!ready (h))
    {
      g_mutex_unlock (&h->lock);
      return NULL;
    }
  uo = units_of (orig, &mo);
  um = units_of (marked, &mm);
  if (uo->len != um->len)
    goto done;
  for (guint i = 0; i < um->len; i++)
    g_array_index (um, Unit, i).user = g_array_index (mo, gboolean, i);

  /* the plain text: marked without the marks, each character with its unit */
  plain = g_new (gunichar, strlen (marked) + 1);
  unit_of = g_new (int, strlen (marked) + 1);
  ptext = g_string_new (NULL);
  {
    guint u = 0;
    for (const gchar *p = marked; *p != '\0'; p = g_utf8_next_char (p))
      {
        gsize off = p - marked;
        gunichar c = g_utf8_get_char (p);

        while (u < um->len && g_array_index (um, Unit, u).end <= off)
          u++;
        if (c == 0x301 || plus_starts (p))
          continue;
        plain[n] = c;
        unit_of[n] = u < um->len && g_array_index (um, Unit, u).start <= off ? (int) u : -1;
        g_string_append_unichar (ptext, c);
        n++;
      }
  }
  if (has_homograph (h, ptext->str) && model_ready (h))
    {
      h->last_use = g_get_monotonic_time ();
      for (glong i = 0; i < n; i++)
        if (sentence_end (plain, s0, i, n) || i == n - 1)
          {
            resolve_sentence (h, plain, unit_of, s0, i + 1, um, marked, mark);
            s0 = i + 1;
          }
      h->last_use = g_get_monotonic_time ();
      g_cond_broadcast (&h->cond);
    }
  g_string_free (ptext, TRUE);
  g_free (plain);
  g_free (unit_of);

  /* the output: units with a new reading replaced */
  out = g_string_new (NULL);
  for (guint i = 0; i < um->len; i++)
    {
      Unit *u = &g_array_index (um, Unit, i);

      if (!u->changed)
        continue;
      g_string_append_len (out, marked + pos, u->start - pos);
      g_string_append (out, u->repl);
      pos = u->end;
      any = TRUE;
    }
  g_string_append (out, marked + pos);
done:
  for (guint i = 0; i < um->len; i++)
    g_free (g_array_index (um, Unit, i).repl);
  g_array_free (uo, TRUE);
  g_array_free (um, TRUE);
  g_array_free (mo, TRUE);
  g_array_free (mm, TRUE);
  g_mutex_unlock (&h->lock);
  if (out == NULL)
    return NULL;
  if (!any)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}
