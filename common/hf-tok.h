/*
 * hf-tok - the two Hugging Face tokenizers the RUAccent homograph models
 * use, without the tokenizers library (Ubuntu does not package it):
 *
 * - WordPiece as BertTokenizer(do_lower_case=False): clean the text, split
 *   at whitespace and punctuation, longest match with "##"
 *   (vocab.txt, tiny2.1);
 * - byte-level BPE as RobertaTokenizer (vocab.json, merges.txt, turbo3.1).
 *
 * Both take the added tokens of added_tokens.json first ("<w>", "</w>" and,
 * for turbo3.1, 10000 stressed words such as "п+осле"): leftmost-longest
 * match in the text, the rest goes through the model, as the tokenizers
 * library does for added tokens that are not special.
 *
 * The files come from the internet: every parser checks its input and
 * fails with an error, never reads out of bounds.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HF_TOK_H
#define HF_TOK_H

#include <glib.h>

typedef struct HfTok HfTok;

/* dir holds vocab.txt (WordPiece) or vocab.json + merges.txt (BPE), and
 * optionally added_tokens.json */
HfTok *hf_tok_load (const gchar *dir, GError **error);
void hf_tok_free (HfTok *t);

/* ids of the pair with the special tokens ([CLS] a [SEP] b [SEP], or
 * <s> a </s></s> b </s>), cut to max_len by dropping tokens from the
 * longer text; a new array of gint64 */
GArray *hf_tok_encode_pair (HfTok *t, const gchar *a, const gchar *b, guint max_len);
gint64 hf_tok_pad_id (HfTok *t);

/* a flat JSON object {"key": value, ...} as the model files use it: value
 * is an integer or an array of strings.  Calls cb for each pair (ints is
 * the integer, strs the NULL-terminated strings or NULL); FALSE with error
 * on bad syntax. */
typedef void (*HfJsonPair) (const gchar *key, gint64 num, gchar **strs, gpointer data);
gboolean hf_json_flat_parse (const gchar *text, gsize len, HfJsonPair cb, gpointer data,
                             GError **error);

#endif
