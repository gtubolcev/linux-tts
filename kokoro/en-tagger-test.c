/*
 * en-tagger-test - en_tagger_tag against spaCy 3.8 with en_core_web_sm 3.8.0:
 * the expected lines are its tokens and tags; also junk, and several
 * threads at once
 *
 *   en-tagger-test <dir of the en-tagger files>
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "en-tagger.h"

#include <stdio.h>
#include <string.h>

static const struct
{
  const char *text, *tags;
} cases[] = {
  { "I don't think we'll make it, but they're trying hard.",
    "I|PRP do|VBP n't|RB think|VB we|PRP 'll|MD make|VB it|PRP ,|, but|CC they|PRP 're|VBP trying|VBG hard|RB .|." },
  { "The U.S. economy grew 3.5% in Q2, according to Dr. Smith's report.",
    "The|DT U.S.|NNP economy|NN grew|VBD 3.5|CD %|NN in|IN Q2|NNP ,|, according|VBG to|IN Dr.|NNP Smith|NNP 's|POS report|NN .|." },
  { "She read the book yesterday; today she will read another one.",
    "She|PRP read|VBD the|DT book|NN yesterday|NN ;|: today|NN she|PRP will|MD read|VB another|DT one|NN .|." },
  { "\"Can you lead the team?\" asked Mr. Brown. \"The pipes are full of lead.\"",
    "\"|`` Can|MD you|PRP lead|VB the|DT team|NN ?|. \"|'' asked|VBD Mr.|NNP Brown|NNP .|. \"|`` The|DT pipes|NNS are|VBP full|JJ of|IN lead|NN .|. \"|''" },
  { "We need to record the record before the live show goes live.",
    "We|PRP need|VBP to|TO record|VB the|DT record|NN before|IN the|DT live|JJ show|NN goes|VBZ live|RB .|." },
  { "He wound the bandage around the wound, then went home at 5 p.m.",
    "He|PRP wound|VBD the|DT bandage|NN around|IN the|DT wound|NN ,|, then|RB went|VBD home|RB at|IN 5|CD p.m.|NN" },
  { "Visit https://example.com/path?q=1 or email info@example.org for details.",
    "Visit|VB https://example.com/path?q=1|NNS or|CC email|NN info@example.org|JJ for|IN details|NNS .|." },
  { "The state-of-the-art system (version 2.0) costs $1,200 -- that's a lot!",
    "The|DT state|NN -|HYPH of|IN -|HYPH the|DT -|HYPH art|NN system|NN (|-LRB- version|NN 2.0|CD )|-RRB- costs|VBZ $|$ 1,200|CD --|: that|DT 's|VBZ a|DT lot|NN !|." },
  { "Don't you dare close the door; we're close to finishing.",
    "Do|VBP n't|RB you|PRP dare|VB close|VB the|DT door|NN ;|: we|PRP 're|VBP close|JJ to|IN finishing|VBG .|." },
  { "They refuse to collect the refuse on Mondays.",
    "They|PRP refuse|VBP to|TO collect|VB the|DT refuse|NN on|IN Mondays|NNP .|." },
  { "Wait...what?! That's impossible, isn't it?",
    "Wait|VB ...|: what|WP ?|. !|. That|DT 's|VBZ impossible|JJ ,|, is|VBZ n't|RB it|PRP ?|." },
  { "I'm gonna use the use case from the docs, y'all.",
    "I|PRP 'm|VBP gon|VBG na|TO use|VB the|DT use|NN case|NN from|IN the|DT docs|NNS ,|, y'|PRP all|DT .|." },
  { "The wind was too strong to wind the sail.",
    "The|DT wind|NN was|VBD too|RB strong|JJ to|TO wind|VB the|DT sail|NN .|." },
  { "Please present the present to the winner at 10:30am.",
    "Please|UH present|VB the|DT present|NN to|IN the|DT winner|NN at|IN 10:30am|NNP .|." },
  { "The dove dove into the bushes, and the bass player caught a bass.",
    "The|DT dove|NN dove|VBD into|IN the|DT bushes|NNS ,|, and|CC the|DT bass|NN player|NN caught|VBD a|DT bass|NN .|." },
  { "The first non-flag argument is always a program to debug, but the second non-flag argument may either be a corefile or a process id.",
    "The|DT first|JJ non|JJ -|JJ flag|JJ argument|NN is|VBZ always|RB a|DT program|NN to|IN debug|NNP ,|, but|CC the|DT second|JJ non|JJ -|JJ flag|JJ argument|NN may|MD either|CC be|VB a|DT corefile|NN or|CC a|DT process|NN i|PRP d.|VBP" },
  { "Changes since udisks 1.98.0: Ayan George (1): Mark Realtek rts5229 based card readers as flash drives.",
    "Changes|NNS since|IN udisks|VBZ 1.98.0|CD :|: Ayan|NNP George|NNP (|-LRB- 1|CD ):|: Mark|NNP Realtek|NNP rts5229|VBD based|VBN card|NN readers|NNS as|IN flash|NN drives|NNS .|." },
  { "PATH path Specify a colon-separated (':') list of directories to search when attempting to load modules.",
    "PATH|NNP path|NN Specify|VB a|DT colon|NN -|HYPH separated|VBN (|-LRB- '|NNP :')|NNP list|NN of|IN directories|NNS to|TO search|VB when|WRB attempting|VBG to|TO load|VB modules|NNS .|." },
  { "Click on the X in the keyboard mapper and search with \"Next\" until you find the mapped key X.",
    "Click|VB on|IN the|DT X|NN in|IN the|DT keyboard|NN mapper|NN and|CC search|VB with|IN \"|`` Next|JJ \"|'' until|IN you|PRP find|VBP the|DT mapped|VBN key|JJ X.|NN" },
};

static gchar *
show (EnTagger *t, const gchar *text)
{
  GArray *a = en_tagger_tag (t, text);
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < a->len; i++)
    {
      EnTag *g = &g_array_index (a, EnTag, i);
      g_string_append_printf (s, "%s%.*s|%s", i ? " " : "", (int) (g->end - g->start),
                              text + g->start, g->tag);
    }
  g_array_free (a, TRUE);
  return g_string_free (s, FALSE);
}

static int
run (EnTagger *t, gboolean quiet)
{
  int failed = 0;
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *got = show (t, cases[i].text);
      if (strcmp (got, cases[i].tags) != 0)
        {
          if (!quiet)
            printf ("FAIL %s\n  want %s\n  got  %s\n", cases[i].text, cases[i].tags, got);
          failed++;
        }
      g_free (got);
    }
  return failed;
}

static gpointer
thread (gpointer t)
{
  int failed = 0;
  for (int k = 0; k < 5; k++)
    failed += run (t, TRUE);
  return GINT_TO_POINTER (failed);
}

int
main (int argc, char **argv)
{
  GError *error = NULL;
  EnTagger *t;
  GThread *th[4];
  int failed;

  if (argc != 2)
    {
      fprintf (stderr, "usage: en-tagger-test <dir>\n");
      return 2;
    }
  if ((t = en_tagger_open (argv[1], &error)) == NULL)
    {
      fprintf (stderr, "%s\n", error->message);
      return 1;
    }
  failed = run (t, FALSE);
  /* no crash on junk */
  {
    const char *junk[] = { "", " ", "\t\n", "'", "''", "-", "--", "...", "`", "‘’", "a'b'c'",
                           "A.B.C.", "x\xff", "((((", "Mr.", "s'", "'s", "ÀÉÎ", "日本語",
                           "   a   b   ", ":-) :) <3", "http://", "e.g.", "\xcc\x81" };
    for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
      g_free (show (t, junk[i]));
  }
  for (int i = 0; i < 4; i++)
    th[i] = g_thread_new ("tag", thread, t);
  for (int i = 0; i < 4; i++)
    if (GPOINTER_TO_INT (g_thread_join (th[i])) != 0)
      {
        printf ("FAIL in a thread\n");
        failed++;
      }
  en_tagger_free (t);
  printf ("%u cases, %d failed\n", (guint) G_N_ELEMENTS (cases), failed);
  return failed != 0;
}
