/*
 * user-stress - keeps the stress marks a user put in a text through the
 * normalizer
 *
 * A user may mark stress by hand: "за́мок" (U+0301 after the vowel) or "зам+ок"
 * ("+" before the vowel). The normalizer's patterns see a marked word as two
 * pieces ("кни́га" is not a noun to agree "21" with, "ве́к" after a Roman
 * numeral comes out as "век+ек"), so the marks are taken out before it and put
 * back on the same words after it, in the user's form. Words the normalizer
 * changed lose their marks; the stress stage after it puts each word's marks
 * in the form the voice reads.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef USER_STRESS_H
#define USER_STRESS_H

#include <glib.h>

typedef struct UserStress UserStress;

/* NULL when text has no marks; else *plain is text without them (g_free) */
UserStress *user_stress_take (const gchar *text, gchar **plain);

/* text (normalized plain) with the marks back on the words they were on */
gchar *user_stress_put (const UserStress *u, const gchar *text);

void user_stress_free (UserStress *u);

#endif
