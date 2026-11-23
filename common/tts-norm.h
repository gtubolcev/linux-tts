/*
 * tts-norm - text normalization for the speech-dispatcher modules
 *
 * Turns "5 кг" into "пять килограммов" before the engine sees the text.
 * Two engines:
 *  - c (default): rn, the C port of ru_normalizr (textnorm/rn), in the
 *    module itself.  Its data (TextNormalizationData: cmorphy3-ru.bin,
 *    en-ipa.tsv) is loaded on the first message, or earlier with
 *    tts_norm_start(), and stays until the module exits (~0.2 s, the
 *    dictionary is mapped and shared).  Without the data, or in a build
 *    without rn, the python engine is used.
 *  - python: ru_normalizr in a resident helper process, one per user for
 *    all modules.  The helper is started on the first message, or earlier
 *    with tts_norm_start(); tts_norm_stop() disconnects when the module
 *    unloads its model, and the helper exits when no module has used it
 *    for a while.
 *
 * Every failure falls back to the text as it was: no helper installed, a
 * helper that crashes, hangs, answers garbage or takes longer than the
 * timeout.  A helper that failed is not started again for a minute, so a
 * broken installation costs one timeout per minute, not one per message.
 *
 * Other languages: zyphra-textnorm (MIT/Apache-2.0, Rust), a private
 * library loaded with dlopen on the first non-Russian message, for the
 * languages it has a grammar for (en, de, es, fr, cs, uk, ...); without it,
 * or for other languages, the text passes through.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TTS_NORM_H
#define TTS_NORM_H

#include <glib.h>

/* longer messages are normalized sentence by sentence, so the first one
 * starts without waiting for the whole text */
#define TTS_NORM_WHOLE_MAX 4000

typedef struct
{
  gboolean enabled;     /* TextNormalization yes|no (yes) */
  gboolean python_only; /* TextNormalizationEngine c|python (c) */
  gchar *data;          /* TextNormalizationData: the rn data directory */
  gchar *python;        /* TextNormalizationPython (python3) */
  gchar *helper;        /* TextNormalizationHelper (the installed script) */
  gchar *zt_lib;        /* TextNormalizationLibrary: zyphra-textnorm for the
                         * other languages ("" or "none": they pass through) */
  int timeout_ms;       /* TextNormalizationTimeout: per request (1500) */
  int start_ms;         /* TextNormalizationStartTimeout: helper load (8000) */
  int idle_s;           /* TextNormalizationIdleTimeout: helper exits unused (60) */
} TtsNormConfig;

typedef struct TtsNorm TtsNorm;

/* defaults; tts_norm_config_clear() frees the strings */
void tts_norm_config_init (TtsNormConfig *c);
void tts_norm_config_clear (TtsNormConfig *c);
/* TRUE if key is one of ours; the value is then taken */
gboolean tts_norm_config_parse (TtsNormConfig *c, const gchar *key, const gchar *value);

/* NULL when normalization is off; the helper is not started yet */
TtsNorm *tts_norm_new (const TtsNormConfig *c);
void tts_norm_free (TtsNorm *n);

/* called about every 100 ms while waiting for the helper; TRUE stops the
 * wait (the message was stopped) and the text passes unnormalized */
typedef gboolean (*TtsNormCancel) (void);
void tts_norm_set_cancel (TtsNorm *n, TtsNormCancel cancel);

/* start the helper now so the first message does not wait for it */
void tts_norm_start (TtsNorm *n);
/* let go of the helper (idle); the next message connects again */
void tts_norm_stop (TtsNorm *n);

/* lang: the voice's language ("ru", "ru-RU", "en"...).  Returns a new
 * string: the normalized text, or a copy of text.  n may be NULL. */
gchar *tts_norm_text (TtsNorm *n, const gchar *text, const gchar *lang);

/* For a message: normalizes it whole if it is short enough (replacing
 * *text) and returns TRUE; FALSE means "normalize each sentence". */
gboolean tts_norm_message (TtsNorm *n, gchar **text, const gchar *lang);

#endif
