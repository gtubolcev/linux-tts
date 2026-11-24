/*
 * ru-homograph - stress of Russian homographs from the context, with the
 * homograph models of RUAccent (MIT, github.com/Den4ikAI/ruaccent,
 * huggingface.co/ruaccent/accentuator)
 *
 * A temporary resolver for tts-stress: the dictionary marks the sentence
 * first, then every word of omographs.json.gz (19741 forms: замок, дома,
 * руки ...) gets the reading the model scores highest.  The model is a
 * cross-encoder, as RUAccent runs it: the sentence with the word wrapped in
 * <w></w>, paired with each stressed variant ("з+амок"); softmax, class 1.
 *
 * Kept as the dictionary marked them: words the user marked (a mark in the
 * text before the dictionary), words with ё, and words with a capital
 * letter inside a sentence (proper names: the dictionary decides).
 *
 * Files in dir (the layout of the RUAccent download):
 *   dir/omographs.json.gz (or dictionary/omographs.json.gz)
 *   dir/<size>/model.onnx + the tokenizer files (or nn/nn_omograph/<size>/)
 * size: "tiny2.1" (DistilBERT, 42 MB, ~5 ms a sentence) or "turbo3.1"
 * (DeBERTa, 360 MB, ~60-200 ms, +530 MB of memory).
 *
 * onnxruntime is loaded with dlopen on first use, so modules without it
 * need nothing.  The model is unloaded after idle seconds without
 * homographs and loaded again on the next one.  Thread safe.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RU_HOMOGRAPH_H
#define RU_HOMOGRAPH_H

#include <glib.h>

#include "ru-stress.h"

typedef struct RuHomograph RuHomograph;

/* nothing is read until the first homograph; idle <= 0 keeps the model */
RuHomograph *ru_homograph_new (const gchar *dir, const gchar *size, int threads, int idle);
void ru_homograph_free (RuHomograph *h);

/* orig: the text before the dictionary; marked: after it.  A new string
 * with the homographs re-marked, or NULL to keep marked (nothing to do, or
 * the model is missing: logged once) */
gchar *ru_homograph_resolve (RuHomograph *h, const gchar *orig, const gchar *marked,
                             RuStressMark mark);

/* for tests: called for every homograph scored */
typedef void (*RuHomographTrace) (const gchar *text, gchar **variants, GArray **ids,
                                  const float *probs, int chosen, gpointer data);
void ru_homograph_set_trace (RuHomograph *h, RuHomographTrace trace, gpointer data);

#endif
