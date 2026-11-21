/*
 * en-tagger - English part-of-speech tags as spaCy's en_core_web_sm gives them
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EN_TAGGER_H
#define EN_TAGGER_H

#include <glib.h>

typedef struct _EnTagger EnTagger;

typedef struct
{
  gsize start, end;     /* the token's bytes in the text */
  const gchar *tag;     /* Penn Treebank tag, static */
} EnTag;

/* opens <dir>/en-tagger.bin, -norm.tsv, -rules.tsv, -symbols.tsv and
 * -tokenizer.txt (made by en-tagger-export.py) */
EnTagger *en_tagger_open (const gchar *dir, GError **error);
void en_tagger_free (EnTagger *t);

/* spaCy's tokens of @text with their tags (an empty array for invalid
 * UTF-8); thread-safe */
GArray *en_tagger_tag (EnTagger *t, const gchar *text);

#endif
