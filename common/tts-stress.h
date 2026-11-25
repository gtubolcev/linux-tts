/*
 * tts-stress - the stress stage of the speech-dispatcher modules
 *
 * Runs after the normalizer: marks the stressed vowel of each Russian word
 * and writes "ё", in the form the engine understands ("+" before the vowel
 * or U+0301 after it).  Words the user marked by hand keep their marks.
 *
 * Now: the stress dictionary of the project, then the Vosk TTS dictionary
 * (common/ru-stress.c); case and the default reading pick among
 * homographs.  With TextStressHomographs rules the homographs (замок/замок,
 * руки/руки) are then chosen from the context by rules where they can
 * (common/ru-homograph-rules.c: у реки, эти реки, висячий замок); with
 * tiny|turbo the rest by the RUAccent models (common/ru-homograph.c), a
 * temporary solution.  Later
 * (xfce4-speech-settings PLAN.md, "Russian stress library in C"): a user
 * dictionary from the GUI and a model for unknown words.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TTS_STRESS_H
#define TTS_STRESS_H

#include <glib.h>

#include "ru-stress.h"

typedef struct
{
  gboolean enabled;     /* TextStress yes|no (module default) */
  gchar *dictionary;    /* TextStressDictionary: files separated by ":" */
  RuStressMark mark;    /* TextStressMark plus|acute|none (module default) */
  gboolean mark_set;    /* TextStressMark was given: it overrides a voice's */
  gchar *homographs;    /* TextStressHomographs none|rules|tiny|turbo:
                         * NULL = none; tiny|turbo = rules + model */
  gchar *homographs_dir; /* TextStressHomographsDir: the RUAccent files */
  int homographs_threads; /* TextStressHomographsThreads (2) */
  int homographs_idle;  /* TextStressHomographsIdle: seconds before the model
                         * is unloaded (300), 0 = never */
} TtsStressConfig;

typedef struct TtsStress TtsStress;

/* Homograph resolver: gets the text before the dictionary (to see the
 * user's own marks) and after it, and returns a new string with the marks
 * of homographs corrected, or NULL to keep marked.  TextStressHomographs
 * plugs in ru-homograph; tts_stress_set_resolver() replaces it. */
typedef gchar *(*TtsStressResolver) (const gchar *orig, const gchar *marked, RuStressMark mark,
                                     gpointer data);

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
/* the same with the mark a voice reads; TextStressMark, when given, wins */
gchar *tts_stress_apply_mark (TtsStress *s, const gchar *text, const gchar *lang,
                              RuStressMark voice_mark);

#endif
