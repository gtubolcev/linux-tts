/*
 * ru-stress - stress marks for Russian text from the Vosk TTS dictionary
 *
 * The dictionary of vosk-model-tts-ru (word<TAB>phonemes, one word per
 * line, sorted by bytes) gives the phonemes of a word; the stressed vowel
 * ends in "1", the others in "0".  The file is mapped, not read, so the
 * 96 MB cost only the pages a lookup touches.
 *
 * Words that already carry a mark (U+0301 after or "+" before a vowel)
 * are left as they are, so a user can correct a word by hand.  Words of
 * one vowel get no mark.  "е" becomes "ё" where the dictionary says so.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef RU_STRESS_H
#define RU_STRESS_H

#include <glib.h>

typedef struct RuStress RuStress;

typedef enum
{
  RU_STRESS_ACUTE,      /* U+0301 after the vowel: "молоко́" */
  RU_STRESS_PLUS,       /* "+" before the vowel: "молок+о" */
} RuStressMark;

/* dict: the sorted Vosk dictionary */
RuStress *ru_stress_new (const gchar *dict, GError **error);
void ru_stress_free (RuStress *s);

/* text with the marks added; letter case is kept */
gchar *ru_stress_apply (RuStress *s, const gchar *text, RuStressMark mark);

#endif
