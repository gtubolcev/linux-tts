/*
 * espeak-g2p - text to IPA with espeak-ng, as Kokoro's front ends do
 *
 * espeak-ng is one global engine: the calls take a lock and switch its voice
 * when needed.  A program that also loads audio.cpp must run every call into
 * libaudiocpp between espeak_g2p_foreign_begin/end: the library drives the same
 * engine with its own voice, data and lock.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ESPEAK_G2P_H
#define ESPEAK_G2P_H

#include <glib.h>

/* starts espeak-ng with the espeak-ng-data in @path; later calls keep the
 * first path */
gboolean espeak_g2p_init (const gchar *path, GError **error);
/* around a call into code that uses espeak-ng by itself (libaudiocpp): keeps
 * our own calls out meanwhile and forgets the voice we had set.  @uses_espeak:
 * the code really phonemizes with espeak-ng (it then gets the default data,
 * ours come back with our next call) */
void espeak_g2p_foreign_begin (gboolean uses_espeak);
void espeak_g2p_foreign_end (void);
/* whether espeak-ng has the voice @voice ("ru", "en-us") */
gboolean espeak_g2p_check_voice (const gchar *voice, GError **error);
/* phonemizer's phonemize([text], preserve_punctuation=True, with_stress=True,
 * tie='^', language_switch='remove-flags', strip=False) with @voice;
 * "" when the voice is missing */
gchar *espeak_g2p_phonemize (const gchar *voice, const gchar *text);

/* Kokoro v1.0 phonemes for @lang ("en-US", "en-GB", "es", "fr", "hi", "it",
 * "pt-BR") as misaki makes them: English with its dictionaries (en-g2p.c)
 * and espeak-ng for the rest; "" for another language */
gchar *kokoro_g2p (const gchar *lang, const gchar *text);
/* where misaki's English dictionaries are (misaki-lexicon.py); without
 * them English goes through espeak-ng alone */
void kokoro_g2p_set_lexicon_dir (const gchar *dir);
/* whether there is a front end for @lang and espeak-ng has its voice */
gboolean kokoro_g2p_speaks (const gchar *lang);

#endif
