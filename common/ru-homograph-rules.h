/*
 * ru-homograph-rules - context rules for Russian homographs, tried before
 * the model of ru-homograph: a preposition or an agreeing word next to the
 * word (у реки́, эти ре́ки, моей руки́) or a word of the topic near it
 * (средневековый за́мок, висячий замо́к).  The rules are a table in
 * ru-homograph-rules.c; a word no rule decides goes to the model.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RU_HOMOGRAPH_RULES_H
#define RU_HOMOGRAPH_RULES_H

#include <glib.h>

typedef struct RuHomoRules RuHomoRules;

/* the built-in table, parsed on first use; never freed */
RuHomoRules *ru_homo_rules_builtin (void);

/* a table from lines in the format of the built-in one (for tests); NULL
 * and error on a bad line */
RuHomoRules *ru_homo_rules_parse (const gchar *const *lines, GError **error);
void ru_homo_rules_free (RuHomoRules *r);

/* calls func for every form and stressed form of the table (NULL: the
 * sense does not fit the form) */
typedef void (*RuHomoRulesFunc) (const gchar *form, const gchar *stressed, gpointer data);
void ru_homo_rules_foreach (RuHomoRules *r, RuHomoRulesFunc func, gpointer data);

/* is there a rule for the word (lower case, е for ё)? */
gboolean ru_homo_rules_has (RuHomoRules *r, const gchar *word);

/* words: the words of one sentence, lower case, е for ё; punct[k]: there
 * is punctuation between words k - 1 and k (punct[0] is ignored).  The
 * stressed form of words[i] ("рук+и") as a new string, or NULL: no rule
 * fits, or rules for two readings do.  why (may be NULL) gets the trigger. */
gchar *ru_homo_rules_decide (RuHomoRules *r, gchar **words, const gboolean *punct, guint n,
                             guint i, const gchar **why);

#endif
