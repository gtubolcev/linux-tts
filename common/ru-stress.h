/*
 * ru-stress - stress marks and ё for Russian text from dictionaries
 *
 * Dictionaries come in two formats, told apart by their first line; both
 * are sorted by bytes and mapped, not read, so only the pages a lookup
 * touches cost memory:
 *
 * - stress dictionary (data/ru-stress, built from Wiktionary and the
 *   user's verdicts): key<TAB>variant[,variant...]; the key is the word in
 *   lower case with ё written е; a variant is the word with U+0301 after
 *   the stressed vowel (ё marks itself), then optionally "/" and flags:
 *   n proper name, r rare reading, d default reading;
 * - the Vosk TTS dictionary: word<TAB>phonemes, the stressed vowel ends
 *   in "1", the others in "0" (the last "1" of a compound is the main one).
 *
 * The first dictionary that knows a word decides.  For a word with several
 * readings the context chooses what it can: a capital letter inside a
 * sentence takes a proper name (Ангары́), a small letter a common word
 * (анга́ры); rare readings only when nothing else is left; then the
 * default reading, then the first one.  Other homographs are left to a
 * resolver (tts-stress.h).
 *
 * Words that already carry a mark (U+0301 after or "+" before a vowel)
 * are left as they are, so a user can correct a word by hand.  Words of
 * one vowel get no mark.  "е" becomes "ё" where the dictionary says so.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RU_STRESS_H
#define RU_STRESS_H

#include <glib.h>

typedef struct RuStress RuStress;

typedef enum
{
  RU_STRESS_ACUTE,      /* U+0301 after the vowel: "молоко́" */
  RU_STRESS_PLUS,       /* "+" before the vowel: "молок+о" */
  RU_STRESS_NONE,       /* no marks, only ё: for front ends that ignore or
                           misread marks, as espeak-ng does with both */
} RuStressMark;

/* the dictionaries tried by default, in this order */
#define RU_STRESS_DEFAULT_DICTIONARIES \
  "/usr/share/linux-tts/ru-stress/ru-stress.tsv:" \
  "/usr/share/vosk-tts/vosk-model-tts-ru-0.9-multi/dictionary.sorted"

/* dicts: dictionary files separated by ":", the first that knows a word
 * wins; missing ones are skipped, NULL with error when none opens */
RuStress *ru_stress_new (const gchar *dicts, GError **error);
void ru_stress_free (RuStress *s);

/* text with the marks added; letter case is kept */
gchar *ru_stress_apply (RuStress *s, const gchar *text, RuStressMark mark);

#endif
