/*
 * tts-copy - a copy of what a module speaks, in files, for debugging
 *
 * With "AudioCopyDir DIR" in a module's .conf, every message the module
 * speaks also goes to DIR/outputN.wav, N counting up from the highest
 * number already there, and the text the voice got (after the normalizer
 * and the stress stage) to DIR/outputN.txt.  The audio is played as
 * usual.  Errors (no such directory, disk full) are logged and cost
 * only the copy.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef TTS_COPY_H
#define TTS_COPY_H

#include <glib.h>

typedef struct
{
  gchar *dir;           /* AudioCopyDir; NULL or "" when off */
} TtsCopyConfig;

typedef struct TtsCopy TtsCopy;

void tts_copy_config_init (TtsCopyConfig *c);
void tts_copy_config_clear (TtsCopyConfig *c);
/* TRUE if key is one of ours */
gboolean tts_copy_config_parse (TtsCopyConfig *c, const gchar *key, const gchar *value);

/* NULL when the copy is off; every function below accepts NULL */
TtsCopy *tts_copy_new (const TtsCopyConfig *c);
void tts_copy_free (TtsCopy *t);

/* one piece of text as the voice got it */
void tts_copy_text (TtsCopy *t, const gchar *text);
/* 16-bit little-endian samples of the message being spoken */
void tts_copy_audio (TtsCopy *t, const gint16 *samples, gsize num_samples,
                     gint sample_rate, gint channels);
/* the message ended (or was stopped): closes its files */
void tts_copy_end (TtsCopy *t);

#endif
