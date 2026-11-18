/*
 * tts-stress - the stress stage of the speech-dispatcher modules
 *
 * Runs after the normalizer: marks the stressed vowel of each Russian word
 * and writes "ё", in the form the engine understands ("+" before the vowel
 * or U+0301 after it).  Words the user marked by hand keep their marks.
 *
 * Now: the Vosk TTS dictionary (common/ru-stress.c), one stress per word.
 * Later, in this order (xfce4-speech-settings PLAN.md, "Russian stress
 * library in C"): a user dictionary from the GUI, a model for unknown
 * words, and homographs (замок/замок, все/всё) chosen from the context
 * by a resolver plugged in with tts_stress_set_resolver().
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#ifndef TTS_STRESS_H
#define TTS_STRESS_H

#include <glib.h>

#include "ru-stress.h"

typedef struct
{
  gboolean enabled;     /* TextStress yes|no (module default) */
  gchar *dictionary;    /* TextStressDictionary (the Vosk dictionary) */
  RuStressMark mark;    /* TextStressMark plus|acute (module default) */
} TtsStressConfig;

typedef struct TtsStress TtsStress;

/* Homograph resolver: gets the sentence with the dictionary marks and
 * returns a new string with the marks of homographs corrected, or NULL to
 * keep it.  None exists yet; the hook fixes where it will sit. */
typedef gchar *(*TtsStressResolver) (const gchar *marked, RuStressMark mark, gpointer data);

void tts_stress_config_init (TtsStressConfig *c, gboolean enabled, RuStressMark mark);
void tts_stress_config_clear (TtsStressConfig *c);
gboolean tts_stress_config_parse (TtsStressConfig *c, const gchar *key, const gchar *value);

/* NULL when the stage is off; the dictionary is opened on first use */
TtsStress *tts_stress_new (const TtsStressConfig *c);
void tts_stress_free (TtsStress *s);
void tts_stress_set_resolver (TtsStress *s, TtsStressResolver resolver, gpointer data);

/* a new string: text with stress marks for Russian, else a copy; s may be
 * NULL */
gchar *tts_stress_apply (TtsStress *s, const gchar *text, const gchar *lang);

#endif
