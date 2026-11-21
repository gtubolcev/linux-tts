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
  /* numbers and currencies (Lexicon.get_number) */
  { FALSE, "He is 21, she is 45 and grandpa is 99.",
    "hˌi ɪz twˈɛnti wˈʌn, ʃi ɪz fˈɔɹTi fˈIv ænd ɡɹˈændpˌɑ ɪz nˈIndi nˈIn." },
  { FALSE, "It costs 100 or 101 or 110 or 115 or 120 or 999.",
    "ˌɪt kˈɔsts wˈʌn hˈʌndɹəd ɔɹ wˈʌn hˈʌndɹəd wˈʌn ɔɹ wˈʌn hˈʌndɹəd tˈɛn ɔɹ wˈʌn hˈʌndɹəd fˌɪftˈin ɔɹ wˈʌn hˈʌndɹəd twˈɛnti ɔɹ nˈIn hˈʌndɹəd nˈIndi nˈIn." },
  { FALSE, "About 2500 people and 12345 votes.",
    "əbˈWt twˈɛnti fˈIv hˈʌndɹəd pˈipᵊl ænd twˈɛlv θˈWzᵊnd θɹˈi hˈʌndɹəd fˈɔɹTi fˈIv vˈOts." },
  { FALSE, "In 1984 and 1905 and 1900 and 2000 and 2007 and 2010 and 2024.",
    "ɪn nˌIntˈin ˈATi fˈɔɹ ænd nˌIntˈin ˈO fˈIv ænd nˌIntˈin hˈʌndɹəd ænd tˈu θˈWzᵊnd ænd tˈu θˈWzᵊnd sˈɛvən ænd twˈɛnti tˈɛn ænd twˈɛnti twˈɛnti fˈɔɹ." },
  { FALSE, "Born in 1066, died in 1215, back in 1800.",
    "bˈɔɹn ɪn tˈɛn sˈɪksti sˈɪks, dˈId ɪn twˈɛlv fˌɪftˈin, bˈæk ɪn ˌAtˈin hˈʌndɹəd." },
  { FALSE, "The 1st, 2nd, 3rd, 4th, 11th, 12th, 13th, 21st, 22nd, 23rd, 101st and 1000th.",
    "ðə fˈɜɹst, sˈɛkənd, θˈɜɹd, fˈɔɹθ, əlˈɛvənθ, twˈɛlfθ, θˌɜɹtˈinθ, twˈɛnti fˈɜɹst, twˈɛnti sˈɛkənd, twˈɛnti θˈɜɹd, wˈʌn hˈʌndɹəd fˈɜɹst ænd wˈʌn θˈWzᵊnθ." },
  { FALSE, "The 80s and the 1990s were fun.",
    "ði ˈATiz ænd ðə nˌIntˈin nˈIndiz wɜɹ fˈʌn." },
  { FALSE, "Pi is 3.14 and e is 2.718.",
    "pˈI ɪz θɹˈi pYnt wˈʌn fˈɔɹ ænd ˈi ɪz tˈu pYnt sˈɛvən wˈʌn ˈAt." },
  { FALSE, "Version 2.0 and 1.5 and 10.04 and 0.5 and .5 here.",
    "vˈɜɹʒən tˈu ænd wˈʌn pYnt fˈIv ænd tˈɛn pYnt zˈɪɹO fˈɔɹ ænd zˈɪɹO pYnt fˈIv ænd pYnt fˈIv hˈɪɹ." },
  { FALSE, "It cost $5.99 each, or $1, or $1.01, or $0.50, or $100, or $2,500.",
    "ˌɪt kˈɔst fˈIv dˈɑləɹz ænd nˈIndi nˈIn sˈɛnts ˈiʧ, ɔɹ wˈʌn dˈɑləɹ, ɔɹ wˈʌn dˈɑləɹ ænd wˈʌn sˈɛnt, ɔɹ fˈɪfti sˈɛnts, ɔɹ wˈʌn hˈʌndɹəd dˈɑləɹz, ɔɹ tˈu θˈWzᵊnd fˈIv hˈʌndɹəd dˈɑləɹz." },
  { FALSE, "It cost £5 and £1.50 and €2.05 and €10.",
    "ˌɪt kˈɔst fˈIv pˈWndz ænd wˈʌn pˈWnd ænd fˈɪfti pˈɛns ænd tˈu jˈʊɹOz ænd fˈIv sˈɛnts ænd tˈɛn jˈʊɹOz." },
  { FALSE, "It was -5 degrees, then -12.5.",
    "ˌɪt wʌz mˈInəs fˈIv dəɡɹˈiz, ðˈɛn mˈInəs twˈɛlv pYnt fˈIv." },
  { FALSE, "James Bond 007 and agent 0042.",
    "ʤˈAmz bˈɑnd sˈɛvən ænd ˈAʤᵊnt fˈɔɹTi tˈu." },
  { FALSE, "See section 6b and item 3a.",
    "sˈi sˈɛkʃən sˈɪks bˈi ænd ˈITəm θɹˈi ˈA." },
  { FALSE, "Steps: (1) first, (2) second, (10) tenth.",
    "stˈɛps: (wˈʌn) fˈɜɹst, (tˈu) sˈɛkənd, (tˈɛn) tˈɛnθ." },
  { FALSE, "It was 9/11, open 24/7, cut 1/2.",
    "ˌɪt wʌz nˈIn əlˈɛvᵊn, ˈOpᵊn twˈɛnti fˈɔɹ sˈɛvən, kˈʌt wˈʌn tˈu." },
  { FALSE, "He was 30ish, they were 1980's kids.",
    "hˌi wʌz θˈɜɹTi ˈɪʃ, ðA wɜɹ nˌIntˈin ˈATiz kˈɪdz." },
  /* misaki drops the dot after B2B (its retokenize quirk); kept here */
  { FALSE, "MP3 files, 4K screens, COVID-19 and B2B.",
    "ˌɛmpˈi θɹˈi fˈIlz, fˈɔɹ kˈA skɹˈinz, kˈOvˌɪd nˌIntˈin ænd bˈitəbˈi." },
};

/* our homograph rules (not in misaki): the default, compat off */
static const struct
{
  gboolean british;
  const char *text, *ps;
} ours[] = {
  { FALSE, "The bass player caught a bass in the lake.",
    "ðə bˈAs plˈAəɹ kˈɔt ɐ bˈæs ɪn ðə lˈAk." },
  { FALSE, "He plays bass in a band, and the bass line is great.",
    "hˌi plˈAz bˈAs ɪn ɐ bˈænd, ænd ðə bˈAs lˈIn ɪz ɡɹˈAt." },
  { FALSE, "Turn up the bass on the speakers.",
    "tˈɜɹn ˌʌp ðə bˈæs ˌɔn ðə spˈikəɹz." },
  { FALSE, "Old houses still have lead pipes and lead paint.",
    "ˈOld hˈWsᵻz stˈɪl hæv lˈɛd pˈIps ænd lˈɛd pˈAnt." },
  { FALSE, "The roof was made of lead, and the pipes of lead and tin.",
    "ðə ɹˈuf wʌz mˌAd ʌv lˈɛd, ænd ðə pˈIps ʌv lˈɛd ænd tˈɪn." },
  { FALSE, "She took the lead in the race; the lead singer smiled.",
    "ʃˌi tˈʊk ðə lˈid ɪn ðə ɹˈAs; ðə lˈid sˈɪŋəɹ smˈIld." },
  { FALSE, "Wear and tear is normal; a tear rolled down her cheek.",
    "wˈɛɹ ænd tˈɛɹ ɪz nˈɔɹmᵊl; ɐ tˈɪɹ ɹˈOld dˌWn hɜɹ ʧˈik." },
  { FALSE, "He took a deep bow; she tied a bow in her hair.",
    "hˌi tˈʊk ɐ dˈip bˈW; ʃi tˈId ɐ bˈO ɪn hɜɹ hˈɛɹ." },
  { FALSE, "The bow of the ship cut the waves; he shot an arrow from his bow.",
    "ðə bˈW ʌv ðə ʃˈɪp kˈʌt ðə wˈAvz; hi ʃˈɑt ɐn ˈɛɹO fɹʌm hɪz bˈO." },
  { FALSE, "They had a row about money; the seats in the third row were empty.",
    "ðˌA hæd ɐ ɹˈW əbˈWt mˈʌni; ðə sˈits ɪn ðə θˈɜɹd ɹˈO wɜɹ ˈɛmpti." },
  { FALSE, "There was a blazing row with the neighbours.",
    "ðˌɛɹ wʌz ɐ blˈAzɪŋ ɹˈW wɪð ðə nˈAbəɹz." },
  { FALSE, "Bucks and does graze in the meadow; she does her homework.",
    "bˈʌks ænd dˈOz ɡɹˈAz ɪn ðə mˈɛdO; ʃi dˈʌz hɜɹ hˈOmwˌɜɹk." },
  { TRUE, "He plays bass in a band, and the bass line is great.",
    "hˌiː plˈAz bˈAs ɪn ɐ bˈand, and ðə bˈAs lˈIn ɪz ɡɹˈAt." },
  { TRUE, "Old houses still have lead pipes and lead paint.",
    "ˈQld hˈWsɪz stˈɪl hav lˈɛd pˈIps and lˈɛd pˈAnt." },
  { TRUE, "He took a deep bow; she tied a bow in her hair.",
    "hˌiː tˈʊk ɐ dˈiːp bˈW; ʃiː tˈId ɐ bˈQ ɪn hɜː hˈɛː." },
  { TRUE, "They had a row about money; the seats in the third row were empty.",
    "ðˌA had ɐ ɹˈW əbˈWt mˈʌni; ðə sˈiːts ɪn ðə θˈɜːd ɹˈQ wɜː ˈɛmpti." },
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
  for (guint i = 0; i < G_N_ELEMENTS (cases) + G_N_ELEMENTS (ours); i++)
    {
      gboolean mine = i >= G_N_ELEMENTS (cases);
      guint k = mine ? i - G_N_ELEMENTS (cases) : i;
      gboolean british = mine ? ours[k].british : cases[k].british;
      const char *text = mine ? ours[k].text : cases[k].text;
      const char *want = mine ? ours[k].ps : cases[k].ps;
      gchar *ps;

      en_lexicon_set_compat (lex[british], !mine);
      ps = en_g2p (lex[british], text, brackets, NULL);
      if (strcmp (ps, want) != 0)
        {
          printf ("FAIL %s\n  want %s\n  got  %s\n", text, want, ps);
          failed++;
        }
      g_free (ps);
    }
  /* no crash on junk */
  {
    const char *junk[] = { "", " ", "'", "''", "-", "--", "...", "`", "‘’", "a'b'c'", "A.B.C.",
                           "x\xff", "((((", "Mr.", "s'", "'s", "ÀÉÎ", "日本語", "$", "$$5", "£.",
                           "-", "-.", "1.", ".,", "1,,2", "-1st", "1.2.3.4.5.6.7.8", "$1.2.3",
                           "99999999999999999999999999", "0.000000000000000000001",
                           "$99999999999999999999999.99", "2B2", "a2", "2a2b2c", "bass", "lead",
                           "does", "row bow", "1990's", "007th", "-0", "-$5", "5$", "€" };
    for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
      g_free (en_g2p (lex[0], junk[i], brackets, NULL));
  }
  en_lexicon_free (lex[0]);
  en_lexicon_free (lex[1]);
  printf ("%u cases, %d failed\n", (guint) (G_N_ELEMENTS (cases) + G_N_ELEMENTS (ours)), failed);
  return failed != 0;
}
