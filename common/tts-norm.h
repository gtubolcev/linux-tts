/*
 * tts-norm - text normalization for the speech-dispatcher modules
 *
 * Runs the normalizer (ru-normalizr, Python) as a resident helper process
 * and asks it to turn "5 кг" into "пять килограммов" before the engine
 * sees the text.  The helper is started on the first message, or earlier
 * with tts_norm_start(); it is stopped with tts_norm_stop() when the module
 * unloads its model.
 *
 * Every failure falls back to the text as it was: no helper installed, a
 * helper that crashes, hangs, answers garbage or takes longer than the
 * timeout.  A helper that failed is not started again for a minute, so a
 * broken installation costs one timeout per minute, not one per message.
 *
 * Only Russian text is normalized now; other languages pass through.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
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
  gchar *python;        /* TextNormalizationPython (python3) */
  gchar *helper;        /* TextNormalizationHelper (the installed script) */
  int timeout_ms;       /* TextNormalizationTimeout: per request (1500) */
  int start_ms;         /* TextNormalizationStartTimeout: helper load (8000) */
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
/* stop the helper (idle); the next message starts it again */
void tts_norm_stop (TtsNorm *n);

/* lang: the voice's language ("ru", "ru-RU", "en"...).  Returns a new
 * string: the normalized text, or a copy of text.  n may be NULL. */
gchar *tts_norm_text (TtsNorm *n, const gchar *text, const gchar *lang);

/* For a message: normalizes it whole if it is short enough (replacing
 * *text) and returns TRUE; FALSE means "normalize each sentence". */
gboolean tts_norm_message (TtsNorm *n, gchar **text, const gchar *lang);

#endif
