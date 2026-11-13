/*
 * num-words - numbers spelt out in Russian and English
 *
 * Follows num2words (lang="ru" / "en"), which TeraTTS uses in training:
 * cardinal numbers, nominative case; decimals as "три целых
 * четырнадцать сотых" / "three point one four".
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef NUM_WORDS_H
#define NUM_WORDS_H

#include <glib.h>

/* literal: an optional "-" or U+2212, digits, optionally "." or "," and
 * digits.  lang: "ru" or "en".  NULL when the number is too long
 * (more than 15 digits before or 6 after the point). */
gchar *num_words (const gchar *literal, const gchar *lang);

#endif
