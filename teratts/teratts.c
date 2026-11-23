/*
 * teratts - see teratts.h
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "teratts.h"

#include <json-glib/json-glib.h>
#include <onnxruntime_c_api.h>
#include <math.h>
#include <string.h>

#define TERATTS_ERROR (g_quark_from_static_string ("teratts-error"))

#define SAMPLES_PER_FRAME 3072  /* vocoder samples per latent frame */
#define LATENT_DIM 144
#define TTL_ROWS 50
#define TTL_DIM 256
#define DP_ROWS 8
#define DP_DIM 16
#define SPEED 1.05f             /* teratts.py divides every duration by it */
#define GUIDANCE 3.0f

struct TeraTts
{
  OrtSession *encoder, *duration, *sampler, *vocoder;
  TeraText *text;
};

struct TeraVoice
{
  gfloat ttl[TTL_ROWS * TTL_DIM];
  gfloat dp[DP_ROWS * DP_DIM];
};

static const OrtApi *ort = NULL;
static OrtEnv *ort_env = NULL;
static OrtMemoryInfo *ort_memory = NULL;



/* --------------------------------------------------------- onnxruntime */

static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (status == NULL)
    return TRUE;
  g_set_error (error, TERATTS_ERROR, 1, "onnxruntime: %s", ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

static gboolean
ort_init (GError **error)
{
  if (ort != NULL)
    return TRUE;
  ort = OrtGetApiBase ()->GetApi (ORT_API_VERSION);
  if (ort == NULL)
    {
      g_set_error (error, TERATTS_ERROR, 1, "onnxruntime: API version %d not available",
                   ORT_API_VERSION);
      return FALSE;
    }
  if (!ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "teratts", &ort_env), error))
    return FALSE;
  return ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &ort_memory),
                 error);
}

static OrtSession *
session_new (const gchar *dir, const gchar *name, int threads, GError **error)
{
  gchar *path = g_build_filename (dir, "models", name, NULL);
  OrtSessionOptions *so = NULL;
  OrtSession *s = NULL;

  if (ort_ok (ort->CreateSessionOptions (&so), error))
    {
      /* as teratts.py: sequential graphs, the threads inside the operators */
      if (!(ort_ok (ort->SetIntraOpNumThreads (so, threads), error)
            && ort_ok (ort->SetInterOpNumThreads (so, 1), error)
            && ort_ok (ort->SetSessionExecutionMode (so, ORT_SEQUENTIAL), error)
            && ort_ok (ort->SetSessionGraphOptimizationLevel (so, ORT_ENABLE_ALL), error)
            && ort_ok (ort->CreateSession (ort_env, path, so, &s), error)))
        s = NULL;
      ort->ReleaseSessionOptions (so);
    }
  g_free (path);
  return s;
}

static OrtValue *
tensor (void *data, gsize bytes, const gint64 *shape, gsize rank,
        ONNXTensorElementDataType type, GError **error)
{
  OrtValue *v = NULL;
  if (!ort_ok (ort->CreateTensorWithDataAsOrtValue (ort_memory, data, bytes, shape, rank,
                                                    type, &v), error))
    return NULL;
  return v;
}

static OrtValue *
tensor_f (gfloat *data, const gint64 *shape, gsize rank, GError **error)
{
  gsize n = 1;
  for (gsize i = 0; i < rank; i++)
    n *= shape[i];
  return tensor (data, n * sizeof (gfloat), shape, rank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                 error);
}

/* runs s on n inputs (all released here), returns its one output and the
 * output's element count */
static OrtValue *
run (OrtSession *s, const char *const *names, OrtValue **in, gsize n, gsize *count,
     GError **error)
{
  const char *out_names[] = { NULL };
  OrtAllocator *alloc;
  char *out_name = NULL;
  OrtValue *out = NULL;
  OrtTensorTypeAndShapeInfo *info = NULL;
  gboolean ok = TRUE;

  for (gsize i = 0; i < n; i++)
    ok = ok && in[i] != NULL;
  ok = ok
       && ort_ok (ort->GetAllocatorWithDefaultOptions (&alloc), error)
       && ort_ok (ort->SessionGetOutputName (s, 0, alloc, &out_name), error);
  if (ok)
    {
      out_names[0] = out_name;
      ok = ort_ok (ort->Run (s, NULL, names, (const OrtValue *const *) in, n,
                             out_names, 1, &out), error)
           && ort_ok (ort->GetTensorTypeAndShape (out, &info), error)
           && ort_ok (ort->GetTensorShapeElementCount (info, count), error);
      alloc->Free (alloc, out_name);
    }
  if (info != NULL)
    ort->ReleaseTensorTypeAndShapeInfo (info);
  for (gsize i = 0; i < n; i++)
    if (in[i] != NULL)
      ort->ReleaseValue (in[i]);
  if (!ok && out != NULL)
    {
      ort->ReleaseValue (out);
      out = NULL;
    }
  return out;
}

static gfloat *
data_of (OrtValue *v)
{
  gfloat *p = NULL;
  if (ort->GetTensorMutableData (v, (void **) &p) != NULL)
    return NULL;
  return p;
}



/* ------------------------------------------------------------- loading */

TeraTts *
teratts_new (const gchar *dir, int threads, GError **error)
{
  TeraTts *t;
  gchar *table;

  if (!ort_init (error))
    return NULL;
  t = g_new0 (TeraTts, 1);
  table = g_build_filename (dir, "unicode_indexer.json", NULL);
  if ((t->text = tera_text_new (table, error)) == NULL
      || (t->encoder = session_new (dir, "text_encoder.onnx", threads, error)) == NULL
      || (t->duration = session_new (dir, "duration_predictor.onnx", threads, error)) == NULL
      || (t->sampler = session_new (dir, "sampler_distilled_cfg3_8step.onnx", threads,
                                    error)) == NULL
      || (t->vocoder = session_new (dir, "vocoder.onnx", threads, error)) == NULL)
    {
      g_free (table);
      teratts_free (t);
      return NULL;
    }
  g_free (table);
  return t;
}

void
teratts_free (TeraTts *t)
{
  if (t == NULL)
    return;
  if (t->encoder != NULL)
    ort->ReleaseSession (t->encoder);
  if (t->duration != NULL)
    ort->ReleaseSession (t->duration);
  if (t->sampler != NULL)
    ort->ReleaseSession (t->sampler);
  if (t->vocoder != NULL)
    ort->ReleaseSession (t->vocoder);
  tera_text_free (t->text);
  g_free (t);
}

static gint
compare_str (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const gchar *const *) a, *(const gchar *const *) b);
}

gchar **
teratts_list_voices (const gchar *dir)
{
  gchar *styles = g_build_filename (dir, "styles", NULL);
  GDir *d = g_dir_open (styles, 0, NULL);
  GPtrArray *names = g_ptr_array_new ();
  const gchar *name;

  while (d != NULL && (name = g_dir_read_name (d)) != NULL)
    {
      gchar *ttl = g_build_filename (styles, name, "style_ttl.npy", NULL);
      if (g_file_test (ttl, G_FILE_TEST_IS_REGULAR))
        g_ptr_array_add (names, g_strdup (name));
      g_free (ttl);
    }
  if (d != NULL)
    g_dir_close (d);
  g_free (styles);
  g_ptr_array_sort (names, compare_str);
  g_ptr_array_add (names, NULL);
  return (gchar **) g_ptr_array_free (names, FALSE);
}

/* a little-endian float32 .npy of exactly n values */
static gboolean
npy_load (const gchar *path, gfloat *dst, gsize n, GError **error)
{
  gchar *data;
  gsize len, hlen, off;
  gboolean ok = FALSE;

  if (!g_file_get_contents (path, &data, &len, error))
    return FALSE;
  /* "\x93NUMPY", major, minor, header length (2 bytes in v1, 4 in v2+) */
  if (len < 10 || memcmp (data, "\x93NUMPY", 6) != 0)
    goto bad;
  if (data[6] == 1)
    hlen = (guint8) data[8] | (guint8) data[9] << 8, off = 10;
  else if (len >= 12)
    hlen = (guint8) data[8] | (guint8) data[9] << 8 | (guint8) data[10] << 16
           | (gsize) (guint8) data[11] << 24, off = 12;
  else
    goto bad;
  if (off + hlen > len || g_strstr_len (data + off, hlen, "'<f4'") == NULL
      || g_strstr_len (data + off, hlen, "'fortran_order': False") == NULL
      || len - off - hlen != n * sizeof (gfloat))
    goto bad;
  memcpy (dst, data + off + hlen, n * sizeof (gfloat));
  ok = TRUE;
  goto out;
bad:
  g_set_error (error, TERATTS_ERROR, 1, "%s: not a float32 array of %zu values", path, n);
out:
  g_free (data);
  return ok;
}

TeraVoice *
teratts_voice_load (const gchar *dir, const gchar *name, GError **error)
{
  TeraVoice *v = g_new (TeraVoice, 1);
  gchar *ttl = g_build_filename (dir, "styles", name, "style_ttl.npy", NULL);
  gchar *dp = g_build_filename (dir, "styles", name, "style_dp.npy", NULL);

  if (!npy_load (ttl, v->ttl, G_N_ELEMENTS (v->ttl), error)
      || !npy_load (dp, v->dp, G_N_ELEMENTS (v->dp), error))
    g_clear_pointer (&v, g_free);
  g_free (ttl);
  g_free (dp);
  return v;
}

void
teratts_voice_free (TeraVoice *v)
{
  g_free (v);
}



/* ---------------------------------------------------------------- text */

gchar *
teratts_prepare (TeraTts *t, const gchar *text, const gchar *lang,
                 TeraStressFunc stress, gpointer stress_data)
{
  return tera_text_prepare (t->text, text, lang, stress, stress_data);
}

/* ------------------------------------------------------------- synthesis */

/* splitmix64 + Box-Muller: standard normal values */
static void
gaussian_fill (gfloat *dst, gsize n, guint64 seed)
{
  guint64 s = seed;
  for (gsize i = 0; i < n; i += 2)
    {
      double u[2];
      for (int k = 0; k < 2; k++)
        {
          guint64 z = (s += 0x9E3779B97F4A7C15ULL);
          z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
          z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
          z ^= z >> 31;
          u[k] = ((z >> 11) + 0.5) / 9007199254740992.0;    /* (0, 1) */
        }
      double r = sqrt (-2.0 * log (u[0])), a = 2.0 * G_PI * u[1];
      dst[i] = (gfloat) (r * cos (a));
      if (i + 1 < n)
        dst[i + 1] = (gfloat) (r * sin (a));
    }
}

/* the ids of text; with_stress FALSE drops the "+" marks */
static GArray *
text_ids (TeraTts *t, const gchar *text, gboolean with_stress, GError **error)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  for (const gchar *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gint64 id;
      if (c == '+' && !with_stress)
        continue;
      if ((id = tera_text_id (t->text, c)) < 0)
        {
          g_set_error (error, TERATTS_ERROR, 1, "U+%04X is not in the model", c);
          g_array_free (ids, TRUE);
          return NULL;
        }
      g_array_append_val (ids, id);
    }
  return ids;
}

gfloat *
teratts_synth (TeraTts *t, const TeraVoice *v, const gchar *model_text,
               gfloat duration_scale, guint64 seed,
               const gfloat *noise, gsize n_noise,
               gsize *n_samples, GError **error)
{
  static const char *const enc_in[] = { "text_ids", "style_ttl", "text_mask" };
  static const char *const dur_in[] = { "text_ids", "style_dp", "text_mask" };
  static const char *const smp_in[] = { "initial_latent", "text_emb", "style_ttl",
                                        "latent_mask", "text_mask", "guidance" };
  static const char *const voc_in[] = { "latent" };
  GArray *ids = NULL, *dids = NULL;
  gfloat *mask = NULL, *dmask = NULL, *lmask = NULL, *lat0 = NULL, *wav = NULL;
  gfloat ttl[TTL_ROWS * TTL_DIM], dp[DP_ROWS * DP_DIM], guidance = GUIDANCE;
  OrtValue *emb = NULL, *dur = NULL, *lat = NULL, *audio = NULL;
  gint64 ttl_shape[] = { 1, TTL_ROWS, TTL_DIM }, dp_shape[] = { 1, DP_ROWS, DP_DIM };
  gint64 one[] = { 1 }, ids_shape[2], mask_shape[3], dids_shape[2], dmask_shape[3];
  gint64 lat_shape[3], lmask_shape[3], emb_shape[3];
  gsize count, frames, keep;
  double secs;

  /* the graphs take non-const buffers */
  memcpy (ttl, v->ttl, sizeof ttl);
  memcpy (dp, v->dp, sizeof dp);
  if ((ids = text_ids (t, model_text, TRUE, error)) == NULL
      || (dids = text_ids (t, model_text, FALSE, error)) == NULL)
    goto out;
  if (ids->len == 0 || dids->len == 0)
    {
      g_set_error (error, TERATTS_ERROR, 1, "empty text");
      goto out;
    }
  mask = g_new (gfloat, ids->len);
  dmask = g_new (gfloat, dids->len);
  for (guint i = 0; i < ids->len; i++)
    mask[i] = 1.0f;
  for (guint i = 0; i < dids->len; i++)
    dmask[i] = 1.0f;
  ids_shape[0] = 1, ids_shape[1] = ids->len;
  mask_shape[0] = 1, mask_shape[1] = 1, mask_shape[2] = ids->len;
  dids_shape[0] = 1, dids_shape[1] = dids->len;
  dmask_shape[0] = 1, dmask_shape[1] = 1, dmask_shape[2] = dids->len;

  {
    OrtValue *in[] = {
      tensor (ids->data, ids->len * sizeof (gint64), ids_shape, 2,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, error),
      tensor_f (ttl, ttl_shape, 3, error),
      tensor_f (mask, mask_shape, 3, error),
    };
    if ((emb = run (t->encoder, enc_in, in, 3, &count, error)) == NULL)
      goto out;
    if (count != (gsize) TTL_DIM * ids->len)
      {
        g_set_error (error, TERATTS_ERROR, 1, "text_encoder: %zu values", count);
        goto out;
      }
  }
  {
    OrtValue *in[] = {
      tensor (dids->data, dids->len * sizeof (gint64), dids_shape, 2,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, error),
      tensor_f (dp, dp_shape, 3, error),
      tensor_f (dmask, dmask_shape, 3, error),
    };
    if ((dur = run (t->duration, dur_in, in, 3, &count, error)) == NULL)
      goto out;
  }
  secs = data_of (dur)[0] * duration_scale / SPEED;
  if (!isfinite (secs) || secs <= 0)
    {
      g_set_error (error, TERATTS_ERROR, 1, "duration predictor: %g s", secs);
      goto out;
    }
  frames = MAX (1, (gsize) ceil (secs * TERATTS_SAMPLE_RATE / SAMPLES_PER_FRAME));

  lat0 = g_new (gfloat, LATENT_DIM * frames);
  if (noise != NULL)
    {
      if (n_noise != LATENT_DIM * frames)
        {
          g_set_error (error, TERATTS_ERROR, 1, "noise: %zu values, %zu frames need %zu",
                       n_noise, frames, LATENT_DIM * frames);
          goto out;
        }
      memcpy (lat0, noise, n_noise * sizeof (gfloat));
    }
  else
    gaussian_fill (lat0, LATENT_DIM * frames, seed);
  lmask = g_new (gfloat, frames);
  for (gsize i = 0; i < frames; i++)
    lmask[i] = 1.0f;
  lat_shape[0] = 1, lat_shape[1] = LATENT_DIM, lat_shape[2] = frames;
  lmask_shape[0] = 1, lmask_shape[1] = 1, lmask_shape[2] = frames;
  emb_shape[0] = 1, emb_shape[1] = TTL_DIM, emb_shape[2] = ids->len;
  {
    OrtValue *in[] = {
      tensor_f (lat0, lat_shape, 3, error),
      tensor_f (data_of (emb), emb_shape, 3, error),
      tensor_f (ttl, ttl_shape, 3, error),
      tensor_f (lmask, lmask_shape, 3, error),
      tensor_f (mask, mask_shape, 3, error),
      tensor_f (&guidance, one, 1, error),
    };
    if ((lat = run (t->sampler, smp_in, in, 6, &count, error)) == NULL)
      goto out;
    if (count != LATENT_DIM * frames)
      {
        g_set_error (error, TERATTS_ERROR, 1, "sampler: %zu values", count);
        goto out;
      }
  }
  {
    OrtValue *in[] = { tensor_f (data_of (lat), lat_shape, 3, error) };
    if ((audio = run (t->vocoder, voc_in, in, 1, &count, error)) == NULL)
      goto out;
  }
  keep = MIN (count, (gsize) lround (secs * TERATTS_SAMPLE_RATE));
  wav = g_memdup2 (data_of (audio), keep * sizeof (gfloat));
  *n_samples = keep;

out:
  if (emb != NULL)
    ort->ReleaseValue (emb);
  if (dur != NULL)
    ort->ReleaseValue (dur);
  if (lat != NULL)
    ort->ReleaseValue (lat);
  if (audio != NULL)
    ort->ReleaseValue (audio);
  if (ids != NULL)
    g_array_free (ids, TRUE);
  if (dids != NULL)
    g_array_free (dids, TRUE);
  g_free (mask);
  g_free (dmask);
  g_free (lmask);
  g_free (lat0);
  return wav;
}
