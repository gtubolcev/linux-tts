/*
 * en-g2p - English text to Kokoro phonemes with misaki's dictionaries
 *
 * A port of misaki's English G2P (en.py, Apache-2.0): the gold and silver
 * dictionaries, the special words (a, the, to, used, ...), the -s/-ed/-ing
 * rules, capitals and stress, and the splitting of glued words.  The
 * part-of-speech tags come from en-tagger, a C port of spaCy's
 * en_core_web_sm tagger, when its files are in the dictionary folder;
 * without them they are guessed from the neighbours.  Numbers go to the
 * fallback whole.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EN_G2P_H
#define EN_G2P_H

#include <glib.h>

typedef struct _EnLexicon EnLexicon;

/* phonemes for a word the dictionaries lack (misaki's EspeakFallback) */
typedef gchar *(*EnFallback) (const gchar *word, gpointer data);

/* opens <dir>/{us,gb}-gold.tsv and -silver.tsv (made by misaki-lexicon.py),
 * and the tagger's en-tagger.* if they are there */
EnLexicon *en_lexicon_open (const gchar *dir, gboolean british, GError **error);
void en_lexicon_free (EnLexicon *lex);

/* the phonemes of @text, misaki's G2P version 1.0 output; thread-safe */
gchar *en_g2p (EnLexicon *lex, const gchar *text, EnFallback fallback, gpointer data);

#endif
