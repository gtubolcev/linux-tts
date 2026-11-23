/*
 * tera-text - TeraTTS's text front end: plain text -> the text the model
 * reads (language tags, numbers spelt out, stress marks).  Shared by
 * sd_teratts (onnxruntime) and sd_audiocpp (the model on audio.cpp).
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TERA_TEXT_H
#define TERA_TEXT_H

#include <glib.h>

typedef struct TeraText TeraText;

/* Adds stress marks ("+" before the vowel) to Russian text. */
typedef gchar *(*TeraStressFunc) (const gchar *text, gpointer data);

/* indexer: the model's unicode_indexer.json (65536 ids, -1: none) */
TeraText *tera_text_new (const gchar *indexer, GError **error);
void tera_text_free (TeraText *t);

/* the model's id of c, -1 if the model lacks it */
gint tera_text_id (const TeraText *t, gunichar c);

/* Plain text -> the text the model reads.  Spaces after punctuation,
 * numbers spelt out, unsupported characters dropped, every run of
 * Cyrillic or Latin letters tagged <ru> or <en> (lang for text without
 * letters), stress marks by stress() in the <ru> runs, NFKD.  NULL when
 * nothing speakable is left. */
gchar *tera_text_prepare (TeraText *t, const gchar *text, const gchar *lang,
                          TeraStressFunc stress, gpointer stress_data);

#endif
