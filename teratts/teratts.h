/*
 * teratts - TeraTTSv2 (TeraSpace) in C with onnxruntime
 *
 * A port of teratts.py of the model release: four ONNX graphs run in turn.
 *
 *   text_encoder        character ids + voice style -> text embedding
 *   duration_predictor  character ids (no stress marks) -> seconds
 *   sampler             Gaussian noise -> speech latent (8 flow steps,
 *                       guidance 3 built into the graph)
 *   vocoder             latent -> 44.1 kHz samples
 *
 * The model reads characters, not phonemes: Russian and English letters,
 * some punctuation, "+" before a stressed vowel, and the language tags
 * <ru>...</ru>, <en>...</en>, all in NFKD form (й is и + U+0306).
 * Digits are not in the vocabulary, numbers are spelt out first.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef TERATTS_H
#define TERATTS_H

#include <glib.h>

#define TERATTS_SAMPLE_RATE 44100

typedef struct TeraTts TeraTts;
typedef struct TeraVoice TeraVoice;

/* Adds stress marks ("+" before the vowel) to Russian text. */
typedef gchar *(*TeraStressFunc) (const gchar *text, gpointer data);

/* dir: the model release (models/, styles/, unicode_indexer.json);
 * threads: onnxruntime threads per graph (the physical cores) */
TeraTts *teratts_new (const gchar *dir, int threads, GError **error);
void teratts_free (TeraTts *t);

/* the names under styles/, sorted; free with g_strfreev */
gchar **teratts_list_voices (const gchar *dir);
/* styles/<name>/style_ttl.npy and style_dp.npy */
TeraVoice *teratts_voice_load (const gchar *dir, const gchar *name, GError **error);
void teratts_voice_free (TeraVoice *v);

/* Plain text -> the text the model reads.  Spaces after punctuation,
 * numbers spelt out, unsupported characters dropped, every run of
 * Cyrillic or Latin letters tagged <ru> or <en> (lang for text without
 * letters), stress marks by stress() in the <ru> runs, NFKD.  NULL when
 * nothing speakable is left. */
gchar *teratts_prepare (TeraTts *t, const gchar *text, const gchar *lang,
                        TeraStressFunc stress, gpointer stress_data);

/* Speech for prepared text.  duration_scale > 1 is slower.  noise:
 * NULL for Gaussian noise from seed, or 144 x n_frames values
 * (n_frames from the duration predictor; a mismatch is an error) to
 * compare with the Python reference.  Mono floats at 44.1 kHz. */
gfloat *teratts_synth (TeraTts *t, const TeraVoice *v, const gchar *model_text,
                       gfloat duration_scale, guint64 seed,
                       const gfloat *noise, gsize n_noise,
                       gsize *n_samples, GError **error);

#endif
