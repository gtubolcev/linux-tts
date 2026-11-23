/*
 * linux-tts-text: the text stages of the speech-dispatcher-modules
 * (normalization, then ё and stress marks) for other engines' modules.
 *
 * A private library (<libdir>/linux-tts/liblinux-tts-text.so.0) that a
 * module loads with dlopen; the PPA's sd_rhvoice uses it.  The same code as
 * in sd_sherpa-onnx and the others: rn for Russian, zyphra-textnorm for the
 * other languages, the stress dictionaries for Russian.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LINUX_TTS_TEXT_H
#define LINUX_TTS_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINUX_TTS_TEXT_SONAME "liblinux-tts-text.so.0"

typedef struct LinuxTtsText LinuxTtsText;

/* configfile: the module's configuration file as speech-dispatcher passes
 * it (the user's file is read on top of the system one), or NULL for the
 * defaults.  Its keys: TextNormalization*, TextStress, TextStressDictionary,
 * TextStressMark (the others are ignored).  Never NULL. */
LinuxTtsText *linux_tts_text_new (const char *configfile);
void linux_tts_text_free (LinuxTtsText *t);

/* Normalizes text (len bytes, or -1 up to the NUL) for a voice of language
 * lang ("ru", "en-US", ...) and marks ё and the stress in Russian.  ssml:
 * the text is SSML; the tags stay as they are and only the text between them
 * changes.  mark: "+" before the stressed vowel, "acute" after it (U+0301),
 * NULL or "none" for ё only; TextStressMark in the configuration wins.
 * Returns a new string (free with linux_tts_text_string_free), or NULL
 * when nothing had to change or the input is not valid UTF-8. */
char *linux_tts_text_prepare (LinuxTtsText *t, const char *text, long len, const char *lang,
                              int ssml, const char *mark);
void linux_tts_text_string_free (char *s);

#ifdef __cplusplus
}
#endif

#endif
