/*
 * en-g2p-test - en_g2p against misaki itself: the expected lines are what
 * misaki fba1236 (with spaCy en_core_web_sm 3.8) gives, unknown words in
 * brackets
 *
 *   en-g2p-test <dir of the .tsv>
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "en-g2p.h"

#include <stdio.h>
#include <string.h>

static const struct
{
  gboolean british;
  const char *text, *ps;
} cases[] = {
  { FALSE, "Hello, world! This is a test of the Kokoro voice.",
    "həlˈO, wˈɜɹld! ðˌɪs ɪz ɐ tˈɛst ʌv ðə [Kokoro] vˈYs." },
  { FALSE, "I used to read books; now I'm reading \"The Lead\" in the U.S.",
    "ˌI jˈust tə ɹˈid bˈʊks; nˈW ˌIm ɹˈidɪŋ “ðə lˈid” ɪn ðə jˌuˈɛs" },
  { FALSE, "The NASA and FBI agents went to the AM show with the US team.",
    "ðə nˈæsə ænd ˌɛfbˌiˈI ˈAʤᵊnts wˈɛnt tə ði ˌAˈɛm ʃˈO wɪð ðə jˌuˈɛs tˈim." },
  { FALSE, "She tried walking, then stopped, then kept running and jumped.",
    "ʃˌi tɹˈId wˈɔkɪŋ, ðˈɛn stˈɑpt, ðˈɛn kˈɛpt ɹˈʌnɪŋ ænd ʤˈʌmpt." },
  { FALSE, "An apple a day keeps the doctor away; to err is human.",
    "ɐn ˈæpᵊl ɐ dˈA kˈips ðə dˈɑktəɹ əwˈA; tʊ ˈɜɹ ɪz hjˈumən." },
  { FALSE, "e-mail me at AT&T about the camelCase iPhone-related state-of-the-art stuff.",
    "ˈimˌAl mˌi æt ˌAtˈi ænd tˈi əbˈWt ðə kˈæmᵊlkˌAs ˌIfˈOnɹəlˈATᵻd stˈAtʌvðiˈɑɹt stˈʌf." },
  { FALSE, "Don't you think it's \"wonderful\"? (Yes.) He said: ‘quite’.",
    "dˈOnt ju θˈɪŋk ɪts “wˈʌndəɹfᵊl”? (jˈɛs.) hˌi sˈɛd: “kwˈIt”." },
  { TRUE, "Hello, world! This is a test of the Kokoro voice.",
    "həlˈQ, wˈɜːld! ðˌɪs ɪz ɐ tˈɛst ɒv ðə [Kokoro] vˈYs." },
  { TRUE, "I used to read books; now I'm reading \"The Lead\" in the U.S.",
    "ˌI jˈuːst tə ɹˈiːd bˈʊks; nˈW ˌIm ɹˈiːdɪŋ “ðə lˈiːd” ɪn ðə jˌuːˈɛs" },
  { TRUE, "The NASA and FBI agents went to the AM show with the US team.",
    "ðə nˈasə and ˌɛfbˌiːˈI ˈAʤᵊnts wˈɛnt tə ði ˌAˈɛm ʃˈQ wɪð ðə jˌuːˈɛs tˈiːm." },
  { TRUE, "She tried walking, then stopped, then kept running and jumped.",
    "ʃˌiː tɹˈId wˈɔːkɪŋ, ðˈɛn stˈɒpt, ðˈɛn kˈɛpt ɹˈʌnɪŋ and ʤˈʌmpt." },
  { TRUE, "An apple a day keeps the doctor away; to err is human.",
    "ɐn ˈapᵊl ɐ dˈA kˈiːps ðə dˈɒktə əwˈA; tʊ ˈɜː ɪz hjˈuːmən." },
  { TRUE, "e-mail me at AT&T about the camelCase iPhone-related state-of-the-art stuff.",
    "ˈiːmˌAl mˌiː at ˌAtˈiː and tˈiː əbˈWt ðə kˈamᵊlkˌAs ˌIfˈQnɹɪlˈAtɪd stˈAtɒvðiˈɑːt stˈʌf." },
  { TRUE, "Don't you think it's \"wonderful\"? (Yes.) He said: ‘quite’.",
    "dˈQnt juː θˈɪŋk ɪts “wˈʌndəfʊl”? (jˈɛs.) hˌiː sˈɛd: “kwˈIt”." },
  /* homographs, by the tagger */
  { FALSE, "She read the book yesterday; today she will read another one.",
    "ʃˌi ɹˈɛd ðə bˈʊk jˈɛstəɹdˌA; tədˈA ʃi wɪl ɹˈid ənˈʌðəɹ wˈʌn." },
  { FALSE, "He wound the bandage around the wound, then went home.",
    "hˌi wˈWnd ðə bˈændɪʤ əɹˈWnd ðə wˈund, ðˈɛn wˈɛnt hˈOm." },
  { FALSE, "Don't you dare close the door; we're close to finishing.",
    "dˈOnt ju dˈɛɹ klˈOz ðə dˈɔɹ; wɪɹ klˈOs tə fˈɪnɪʃɪŋ." },
  { FALSE, "The dove dove into the bushes, and the bass player caught a bass.",
    "ðə dˈʌv dˈOv ˈɪntu ðə bˈʊʃᵻz, ænd ðə bˈæs plˈAəɹ kˈɔt ɐ bˈæs." },
  { FALSE, "The farm was used to produce produce.",
    "ðə fˈɑɹm wʌz jˈuzd tə pɹədˈus pɹˈOdˌus." },
  { FALSE, "He tears up when he sees the tears in her dress.",
    "hˌi tˈɛɹz ˌʌp wˌɛn hi sˈiz ðə tˈɪɹz ɪn hɜɹ dɹˈɛs." },
};

static gchar *
brackets (const gchar *word, G_GNUC_UNUSED gpointer data)
{
  return g_strdup_printf ("[%s]", word);
}

int
main (int argc, char **argv)
{
  EnLexicon *lex[2];
  GError *error = NULL;
  int failed = 0;

  if (argc != 2)
    {
      fprintf (stderr, "usage: en-g2p-test <dir of the .tsv>\n");
      return 2;
    }
  for (int b = 0; b < 2; b++)
    if ((lex[b] = en_lexicon_open (argv[1], b, &error)) == NULL)
      {
        fprintf (stderr, "%s\n", error->message);
        return 1;
      }
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *ps = en_g2p (lex[cases[i].british], cases[i].text, brackets, NULL);
      if (strcmp (ps, cases[i].ps) != 0)
        {
          printf ("FAIL %s\n  want %s\n  got  %s\n", cases[i].text, cases[i].ps, ps);
          failed++;
        }
      g_free (ps);
    }
  /* no crash on junk */
  {
    const char *junk[] = { "", " ", "'", "''", "-", "--", "...", "`", "‘’", "a'b'c'", "A.B.C.",
                           "x\xff", "((((", "Mr.", "s'", "'s", "ÀÉÎ", "日本語" };
    for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
      g_free (en_g2p (lex[0], junk[i], brackets, NULL));
  }
  en_lexicon_free (lex[0]);
  en_lexicon_free (lex[1]);
  printf ("%u cases, %d failed\n", (guint) G_N_ELEMENTS (cases), failed);
  return failed != 0;
}
