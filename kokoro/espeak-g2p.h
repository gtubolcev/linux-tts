/*
 * espeak-g2p - text to IPA with espeak-ng, as Kokoro's front ends do
 *
 * espeak-ng is one global engine: the calls take a lock and switch its voice
 * when needed.
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
/* whether espeak-ng has the voice @voice ("ru", "en-us") */
gboolean espeak_g2p_check_voice (const gchar *voice, GError **error);
/* phonemizer's phonemize([text], preserve_punctuation=True, with_stress=True,
 * tie='^', language_switch='remove-flags', strip=False) with @voice;
 * "" when the voice is missing */
gchar *espeak_g2p_phonemize (const gchar *voice, const gchar *text);

/* Kokoro v1.0 phonemes for @lang ("en-US", "en-GB", "es", "fr", "hi", "it",
 * "pt-BR") as misaki makes them with espeak-ng; "" for another language */
gchar *kokoro_g2p (const gchar *lang, const gchar *text);
/* whether there is a front end for @lang and espeak-ng has its voice */
gboolean kokoro_g2p_speaks (const gchar *lang);

#endif
