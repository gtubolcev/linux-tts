/*
 * sd_sherpa-onnx - speech-dispatcher output module for sherpa-onnx TTS models
 *
 * The models are loaded into the module itself through the sherpa-onnx C API
 * and stay loaded, so only the first message after a (re)start waits for the
 * load.  One model is kept at a time; a voice of another model replaces it.
 * Models are the directories in SherpaModelsDir (and
 * ~/.local/share/sherpa-onnx/tts); the family is told by the files in them:
 *
 *   Supertonic (tts.json + voice.bin)   31 languages, the speakers of voice.bin
 *                                       (F1..F5, M1..M5) for each of them:
 *                                       supertonic-<lang>-<female|male>-<n>
 *   Pocket TTS (lm_main*.onnx)          English, a voice per reference WAV in
 *                                       test_wavs/ and voices/: pocket-en-<name>
 *   Piper (tokens.txt + espeak-ng-data,  the language from the name
 *   as in sherpa-onnx's vits-piper-*)   (vits-piper-ru_RU-irina-medium):
 *                                       piper-<lang>-<name>; with up to 16
 *                                       named speakers piper-<lang>-<name>-<speaker>,
 *                                       else speaker 0. espeak-ng phonemes, or
 *                                       letters for "phoneme_type": "text"
 *   VITS with characters (MMS)          the language from the directory name
 *                                       (mms-ru): one voice, named as the dir
 *
 * The module is installed under two names, each a synthesizer of its own
 * with its configuration: sd_sherpa-onnx (SherpaFamilies "supertonic piper
 * pocket vits") and the link sd_sherpa-onnx-vulkan-rs with Provider "vulkan":
 * the models run on the graphics card through the onnx-vulkan-rs plugin of
 * onnxruntime.  Pocket is not tried with the plugin yet, so it is left out
 * there, and without the plugin the -vulkan-rs synthesizer does not start.
 * Options may carry the prefix "Sherpa" or "Piper" (the old synthesizer
 * piper-sherpa-onnx, PiperFamilies "piper").
 *
 * The protocol, the threads (a loader keeps up to SherpaMaxModels models,
 * the one used longest ago goes), the text and the audio are tts-module's.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <espeak-ng/speak_lib.h>
#include <sherpa-onnx/c-api/c-api.h>

#include "tts-module.h"
#include "tts-stress.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)
#define ERR(...) TTS_ERR (__VA_ARGS__)

typedef enum { FAMILY_SUPERTONIC, FAMILY_PIPER, FAMILY_VITS, FAMILY_POCKET } Family;

/* the order of the families is also the preference among voices of a language */
static const char *const family_names[] = { "supertonic", "piper", "vits", "pocket" };

static const char *const supertonic_langs[] = {
  "en", "ko", "ja", "ar", "bg", "cs", "da", "de", "el", "es", "et",
  "fi", "fr", "hi", "hr", "hu", "id", "it", "lt", "lv", "nl", "pl",
  "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi", NULL
};

typedef struct
{
  gchar *name;    /* supertonic-3 */
  gchar *dir;
  Family family;
  int speakers;
  gchar *espeak_voice;  /* Piper: "ru", "en-us" (from the .onnx.json) */
  gboolean letters;     /* Piper "phoneme_type": "text": fed letters, not phonemes */
  gint reads_acute;     /* Piper: its espeak-ng-data honours U+0301; -1 unknown */
  const SherpaOnnxOfflineTts *tts;  /* set by the loader */
} Model;

typedef struct
{
  gchar *name;      /* supertonic-ru-male-1 */
  gchar *language;  /* ru */
  gchar *gender;    /* "male", "female" or NULL */
  Model *model;
  int sid;
  gchar *wav;       /* Pocket: the reference voice */
  const SherpaOnnxWave *ref;  /* read when first used (in the worker) */
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static int cfg_threads = 0;
static int cfg_steps = 5;
/* sherpa-onnx shortens every pause the model makes to this share; its
 * examples use 0.2, which left Piper voices 60 ms at a full stop where
 * Piper itself makes 300 ms.  1 keeps the pauses the voice was trained with. */
static float cfg_silence_scale = 1.0f;
/* VULKAN_EP_LIBRARY: where the patched sherpa-onnx looks for the plugin,
 * next to its libonnxruntime; meson passes <libdir>/linux-tts/... */
#ifndef VULKAN_EP_LIBRARY
#error "VULKAN_EP_LIBRARY comes from meson"
#endif
static gboolean cfg_vulkan = FALSE;      /* Provider "vulkan" */
static guint cfg_families = ~0u;          /* Families, a bit per Family */
static TtsStressConfig stress_cfg;        /* TextStress... keys */

static GPtrArray *models = NULL;
static GPtrArray *voices = NULL;          /* Voice, sorted */
static TtsStress *stress_stage = NULL;
static gunichar prev_last = 0;            /* the last character of the previous piece */



/* The module is installed under several names (sherpa-onnx, piper, their
 * -vulkan-rs variants) and its options carry the prefix of the name the
 * settings app gives them: "SherpaVoice", "PiperVoice".  Without it: "Voice". */
static const gchar *
option_name (const gchar *key)
{
  static const char *const prefixes[] = { "Sherpa", "Piper", NULL };

  for (const char *const *p = prefixes; *p != NULL; p++)
    if (g_ascii_strncasecmp (key, *p, strlen (*p)) == 0 && key[strlen (*p)] != '\0')
      return key + strlen (*p);

  return key;
}

/* "supertonic pocket vits": the model families this module lists */
static guint
parse_families (const gchar *value)
{
  gchar **names = g_strsplit_set (value, " ,\t", -1);
  guint mask = 0;

  for (gchar **n = names; *n != NULL; n++)
    for (guint f = 0; f < G_N_ELEMENTS (family_names); f++)
      if (g_ascii_strcasecmp (*n, family_names[f]) == 0)
        mask |= 1u << f;
  g_strfreev (names);
  if (mask == 0)
    ERR ("Families: no known family in \"%s\", listing all", value);

  return mask != 0 ? mask : ~0u;
}

/* Whether models of @family are listed: those of Families, and with the
 * Vulkan plugin the VITS graphs (Piper, MMS) and Supertonic.  Supertonic
 * needs the Pad modes edge/reflect of onnxruntime-ep-vulkan 0.0.1 (older
 * plugins accept them but fail to run, and an error there ends the process:
 * sherpa-onnx throws through its C API); the Vulkan package depends on it.
 * Pocket is not tried on Vulkan yet. */
static gboolean
family_wanted (Family family)
{
  if (!(cfg_families & (1u << family)))
    return FALSE;

  return !cfg_vulkan || family == FAMILY_PIPER || family == FAMILY_VITS
         || family == FAMILY_SUPERTONIC;
}

static void
sherpa_setup (void)
{
  cfg_models_dir = g_strdup ("/usr/share/sherpa-onnx/tts");
  /* The mark is chosen per voice (stress_mark_for): a Piper voice gets
   * U+0301 when its espeak-ng-data honours it (piper-espeak-ng-data >= 1.1);
   * the original data, like espeak-ng < 1.52, takes the mark as a reason to
   * stress some other syllable (за́мок -> zamˈok), so those voices, MMS and
   * Supertonic (which read letters) get only ё */
  tts_stress_config_init (&stress_cfg, TRUE, RU_STRESS_NONE);
}



static gboolean
sherpa_config (const gchar *line, const gchar *value)
{
  const gchar *key = option_name (line);

  if (tts_stress_config_parse (&stress_cfg, line, value))
    ;
  else if (g_ascii_strcasecmp (key, "ModelsDir") == 0)
    g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "Threads") == 0)
    cfg_threads = atoi (value);
  else if (g_ascii_strcasecmp (key, "SupertonicSteps") == 0)
    cfg_steps = atoi (value);
  else if (g_ascii_strcasecmp (key, "Families") == 0)
    cfg_families = parse_families (value);
  else if (g_ascii_strcasecmp (key, "Provider") == 0)
    cfg_vulkan = g_ascii_strcasecmp (value, "vulkan") == 0;
  else if (g_ascii_strcasecmp (key, "SilenceScale") == 0)
    {
      float s = (float) g_ascii_strtod (value, NULL);
      cfg_silence_scale = s > 0.0f && s <= 4.0f ? s : 1.0f;
    }
  else
    return FALSE;
  return TRUE;
}



/* ------------------------------------------------------------------ models */

/* the first file in dir matching "<prefix>*<suffix>" */
static gchar *
find_file (const gchar *dir, const gchar *prefix, const gchar *suffix)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *f;
  gchar *found = NULL;

  if (d == NULL)
    return NULL;
  while ((f = g_dir_read_name (d)) != NULL && found == NULL)
    if (g_str_has_prefix (f, prefix) && g_str_has_suffix (f, suffix))
      found = g_build_filename (dir, f, NULL);
  g_dir_close (d);
  return found;
}



static gboolean
has_file (const gchar *dir, const gchar *name)
{
  gchar *p = g_build_filename (dir, name, NULL);
  gboolean ok = g_file_test (p, G_FILE_TEST_EXISTS);
  g_free (p);
  return ok;
}



static Voice *
voice_new (Model *m, const gchar *name, const gchar *language, const gchar *gender, int sid)
{
  Voice *v = g_new0 (Voice, 1);

  v->name = g_strdup (name);
  v->language = g_strdup (language);
  v->gender = g_strdup (gender);
  v->model = m;
  v->sid = sid;
  g_ptr_array_add (voices, v);
  return v;
}



/* the number of speakers in a Supertonic voice.bin: an int64 at the start */
static int
supertonic_speakers (const gchar *dir)
{
  gchar *path = g_build_filename (dir, "voice.bin", NULL);
  FILE *f = fopen (path, "rb");
  int64_t n = 0;

  if (f != NULL)
    {
      if (fread (&n, sizeof n, 1, f) != 1)
        n = 0;
      fclose (f);
    }
  g_free (path);
  return (n > 0 && n < 1000) ? (int) n : 0;
}



static void
add_pocket_voices (Model *m, const gchar *sub)
{
  gchar *dir = g_build_filename (m->dir, sub, NULL);
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *f;

  if (d != NULL)
    {
      while ((f = g_dir_read_name (d)) != NULL)
        {
          gchar *stem, *name;
          Voice *v;

          if (!g_str_has_suffix (f, ".wav"))
            continue;
          stem = g_strndup (f, strlen (f) - 4);
          /* sample_fr_hibiki_crepes -> hibiki; the synthesis is English only,
           * and a sample in another language (this one also starts quietly)
           * makes it mumble, so take only sample_en_* */
          if (g_str_has_prefix (stem, "sample_"))
            {
              gchar **parts = g_strsplit (stem, "_", -1);
              gboolean english = g_strv_length (parts) >= 3
                                 && g_ascii_strcasecmp (parts[1], "en") == 0;
              if (english)
                {
                  g_free (stem);
                  stem = g_strdup (parts[2]);
                }
              g_strfreev (parts);
              if (!english)
                {
                  DBG ("Pocket: %s is not an English sample", f);
                  g_free (stem);
                  continue;
                }
            }
          name = g_strconcat (m->name, "-en-", stem, NULL);
          v = voice_new (m, name, "en", NULL, 0);
          v->wav = g_build_filename (dir, f, NULL);
          g_free (name);
          g_free (stem);
        }
      g_dir_close (d);
    }
  g_free (dir);
}



/* "espeak": {"voice": "ru"} from the .onnx.json next to the model; "en-us" etc.
 * The file is small and flat enough for a regular expression. */
static gchar *
piper_espeak_voice (const gchar *onnx)
{
  gchar *json = g_strconcat (onnx, ".json", NULL), *text = NULL, *voice = NULL;
  GMatchInfo *mi = NULL;
  GRegex *re = g_regex_new ("\"espeak\"\\s*:\\s*\\{[^}]*\"voice\"\\s*:\\s*\"([^\"]+)\"",
                            0, 0, NULL);

  if (g_file_get_contents (json, &text, NULL, NULL)
      && g_regex_match (re, text, 0, &mi))
    voice = g_match_info_fetch (mi, 1);
  g_match_info_free (mi);
  g_regex_unref (re);
  g_free (text);
  g_free (json);
  return voice;
}

/* "phoneme_type": "text" voices (ukrainian_tts) are fed letters, not espeak-ng
 * phonemes */
static gboolean
piper_uses_text (const gchar *json)
{
  return g_regex_match_simple ("\"phoneme_type\"\\s*:\\s*\"text\"", json, 0, 0);
}

/* more named speakers than this (libritts: 904, vctk: 109) give one voice only */
#define PIPER_MAX_SPEAKERS 16

/* "speaker_id_map": {"lada": 0, "mykyta": 1} -> one voice per speaker,
 * <base>-<speaker>; FALSE if the voice has a single (or too many) speakers */
static gboolean
piper_add_speakers (Model *m, const gchar *json, const gchar *base, const gchar *lang)
{
  GRegex *block = g_regex_new ("\"speaker_id_map\"\\s*:\\s*\\{([^}]*)\\}", 0, 0, NULL);
  GRegex *entry = g_regex_new ("\"([^\"]+)\"\\s*:\\s*([0-9]+)", 0, 0, NULL);
  GMatchInfo *mi = NULL, *ei = NULL;
  gchar *map = NULL;
  int n = 0;

  if (g_regex_match (block, json, 0, &mi))
    map = g_match_info_fetch (mi, 1);
  g_match_info_free (mi);
  if (map != NULL)
    {
      for (g_regex_match (entry, map, 0, &ei); g_match_info_matches (ei);
           g_match_info_next (ei, NULL))
        n++;
      g_match_info_free (ei);
    }
  if (n < 2 || n > PIPER_MAX_SPEAKERS)
    n = 0;
  else
    {
      for (g_regex_match (entry, map, 0, &ei); g_match_info_matches (ei);
           g_match_info_next (ei, NULL))
        {
          gchar *speaker = g_match_info_fetch (ei, 1), *sid = g_match_info_fetch (ei, 2);
          gchar *name = g_strdup_printf ("%s-%s", base, speaker);

          g_strcanon (name + strlen (base) + 1,
                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", '_');
          voice_new (m, name, lang, NULL, atoi (sid));
          g_free (name);
          g_free (speaker);
          g_free (sid);
        }
      g_match_info_free (ei);
    }
  g_free (map);
  g_regex_unref (entry);
  g_regex_unref (block);
  return n > 0;
}

/* Text -> letters for "phoneme_type": "text" voices, as Piper does it: lower
 * case, decomposed (NFD: й -> и + U+0306, the way the voice was trained),
 * one sentence per ".", "?" or "!". Characters the voice does not know are
 * left to sherpa-onnx, which skips them. */
static gboolean
piper_letters (const gchar *text, GArray *cps, GArray *lens)
{
  gchar *low = g_utf8_strdown (text, -1);
  gchar *nfd = g_utf8_normalize (low, -1, G_NORMALIZE_NFD);
  gint32 len = 0;

  g_free (low);
  if (nfd == NULL)
    return FALSE;
  for (const gchar *p = nfd; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gint32 cp = (gint32) (g_unichar_isspace (c) ? ' ' : c);

      /* the typographic apostrophes (комп’ютер) as the ASCII one */
      if (c == 0x2019 || c == 0x02BC || c == 0x2018)
        cp = '\'';

      if (cp == ' ' && (len == 0 || g_array_index (cps, gint32, cps->len - 1) == ' '))
        continue;
      g_array_append_val (cps, cp);
      len++;
      if (c == '.' || c == '?' || c == '!')
        {
          const gchar *next = g_utf8_next_char (p);
          if (*next == '\0' || g_unichar_isspace (g_utf8_get_char (next)))
            {
              g_array_append_val (lens, len);
              len = 0;
            }
        }
    }
  if (len > 0)
    g_array_append_val (lens, len);
  g_free (nfd);
  return lens->len > 0;
}

/* espeak-ng clause terminators (its translate.h, not installed); the same
 * values piper-phonemize compares with */
#define CLAUSE_TYPE_SENTENCE 0x00080000
#define CLAUSE_TYPE_CLAUSE   0x00040000
#define CLAUSE_PERIOD        (40 | 0x0000 | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_QUESTION      (40 | 0x2000 | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_EXCLAMATION   (45 | 0x3000 | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COMMA         (20 | 0x1000 | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_COLON         (30 | 0x0000 | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_SEMICOLON     (30 | 0x1000 | CLAUSE_TYPE_CLAUSE)

static gchar *espeak_voice_now = NULL;  /* the voice espeak-ng is set to */

#ifndef HAVE_ESPEAK_TERMINATOR
/* Older espeak-ng does not report how a clause ended: guess it from the
 * last punctuation mark of the text it consumed, from start to end (NULL
 * when it read the whole text). */
static int
clause_terminator (const char *start, const char *end)
{
  const char *p = end != NULL ? end : start + strlen (start);

  while (p > start && g_ascii_isspace (p[-1]))
    p--;
  if (p == start)
    return 0;
  switch (p[-1])
    {
    case '.': return CLAUSE_PERIOD;
    case '?': return CLAUSE_QUESTION;
    case '!': return CLAUSE_EXCLAMATION;
    case ',': return CLAUSE_COMMA;
    case ':': return CLAUSE_COLON;
    case ';': return CLAUSE_SEMICOLON;
    default:  return 0;
    }
}
#endif

/* the espeak-ng-data of the piper-espeak-ng-data package, which the Piper
 * voices were trained on */
#ifndef PIPER_ESPEAK_DATA
#define PIPER_ESPEAK_DATA "/usr/share/sherpa-onnx/piper-espeak-ng-data"
#endif

/* The espeak-ng-data a Piper model is read with, resolved: the voice's own
 * copy for the voices of the system; for downloaded ones the shared copy
 * whenever it is installed, as their own dictionaries are not trusted
 * (espeak-ng had overflows in its dictionary loading). */
static gchar *
piper_espeak_data (const Model *m)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "sherpa-onnx", "tts", "", NULL);
  gchar *own = g_build_filename (m->dir, "espeak-ng-data", NULL);
  gchar *path;

  if (g_str_has_prefix (m->dir, user) && g_file_test (PIPER_ESPEAK_DATA, G_FILE_TEST_IS_DIR))
    path = realpath (PIPER_ESPEAK_DATA, NULL);
  else
    path = realpath (own, NULL);
  g_free (own);
  g_free (user);
  if (path == NULL)
    return NULL;
  /* realpath's memory belongs to malloc */
  own = g_strdup (path);
  free (path);
  return own;
}

/* espeak-ng loaded with the voice's data and set to its language; loaded
 * again when a voice with other data comes */
static gboolean
piper_espeak (const Model *m)
{
  static gchar *data_now = NULL;  /* the espeak-ng-data loaded, resolved */
  gchar *data = piper_espeak_data (m);

  if (data == NULL)
    {
      ERR ("espeak-ng: no espeak-ng-data for %s", m->dir);
      return FALSE;
    }
  if (g_strcmp0 (data, data_now) != 0)
    {
      if (data_now != NULL)
        espeak_Terminate ();
      g_clear_pointer (&data_now, g_free);
      g_clear_pointer (&espeak_voice_now, g_free);
      /* espeak-ng >= 1.51 takes the espeak-ng-data directory itself */
      if (espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, data, 0) < 0)
        {
          ERR ("espeak-ng: cannot load %s", data);
          g_free (data);
          return FALSE;
        }
      DBG ("espeak-ng: data %s", data);
      data_now = data;
    }
  else
    g_free (data);
  if (g_strcmp0 (espeak_voice_now, m->espeak_voice) != 0)
    {
      if (espeak_SetVoiceByName (m->espeak_voice != NULL ? m->espeak_voice : "en-us")
          != EE_OK)
        {
          ERR ("espeak-ng: no voice %s", m->espeak_voice);
          return FALSE;
        }
      g_free (espeak_voice_now);
      espeak_voice_now = g_strdup (m->espeak_voice);
    }
  return TRUE;
}

static gchar *
piper_ipa (const gchar *word)
{
  const void *ptr = word;
  const char *ph = espeak_TextToPhonemes (&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);

  return g_strdup (ph != NULL ? ph : "");
}

/* whether the Russian rules of a Piper voice move the stress to a vowel
 * marked with U+0301; asked once per model */
static gboolean
piper_reads_acute (Model *m)
{
  if (m->reads_acute < 0)
    {
      m->reads_acute = 0;
      if (piper_espeak (m))
        {
          gchar *a = piper_ipa ("за\u0301мок"), *b = piper_ipa ("замо\u0301к");

          m->reads_acute = strcmp (a, b) != 0;
          DBG ("%s: espeak-ng-data %s the stress mark (%s, %s)", m->name,
               m->reads_acute ? "honours" : "ignores", a, b);
          g_free (a);
          g_free (b);
        }
    }
  return m->reads_acute;
}

/* the stress mark a voice reads right */
static RuStressMark
stress_mark_for (Voice *v)
{
  Model *m = v->model;

  if (m->family == FAMILY_PIPER && !m->letters && m->espeak_voice != NULL
      && g_ascii_strncasecmp (m->espeak_voice, "ru", 2) == 0 && piper_reads_acute (m))
    return RU_STRESS_ACUTE;
  return RU_STRESS_NONE;
}

/* Text -> IPA phonemes the way Piper voices were trained (piper-phonemize):
 * espeak-ng clause by clause, the punctuation put back from the clause
 * terminator, language switch marks such as "(en)" removed. sherpa-onnx >= 2.0
 * does not phonemize Piper voices itself. Appends the codepoints of every
 * sentence to cps and its length to lens; FALSE if espeak-ng is unusable. */
static gboolean
piper_phonemize (const Model *m, const gchar *text, GArray *cps, GArray *lens)
{
  static GRegex *flags = NULL;
  const void *ptr = text;
  GString *sentence;

  if (!piper_espeak (m))
    return FALSE;
  if (flags == NULL)
    flags = g_regex_new ("\\([^)]+\\)", 0, 0, NULL);
  sentence = g_string_new (NULL);

  while (ptr != NULL)
    {
      int terminator = 0, punct;
#ifdef HAVE_ESPEAK_TERMINATOR
      const char *ph = espeak_TextToPhonemesWithTerminator (&ptr, espeakCHARS_UTF8,
                                                            espeakPHONEMES_IPA,
                                                            &terminator);
#else
      const char *start = ptr;
      const char *ph = espeak_TextToPhonemes (&ptr, espeakCHARS_UTF8,
                                              espeakPHONEMES_IPA);
      terminator = clause_terminator (start, ptr);
#endif
      if (ph != NULL)
        {
          gchar *clean = g_regex_replace_literal (flags, ph, -1, 0, "", 0, NULL);
          g_string_append (sentence, clean);
          g_free (clean);
        }
      punct = terminator & 0x000FFFFF;
      if (punct == CLAUSE_PERIOD)
        g_string_append_c (sentence, '.');
      else if (punct == CLAUSE_QUESTION)
        g_string_append_c (sentence, '?');
      else if (punct == CLAUSE_EXCLAMATION)
        g_string_append_c (sentence, '!');
      else if (punct == CLAUSE_COMMA)
        g_string_append (sentence, ", ");
      else if (punct == CLAUSE_COLON)
        g_string_append (sentence, ": ");
      else if (punct == CLAUSE_SEMICOLON)
        g_string_append (sentence, "; ");

      if ((terminator & CLAUSE_TYPE_SENTENCE) == CLAUSE_TYPE_SENTENCE || ptr == NULL)
        {
          glong n = 0;
          gunichar *u = g_utf8_to_ucs4_fast (sentence->str, -1, &n);
          if (n > 0)
            {
              gint32 len = (gint32) n;
              for (glong i = 0; i < n; i++)
                {
                  gint32 c = (gint32) u[i];
                  g_array_append_val (cps, c);
                }
              g_array_append_val (lens, len);
            }
          g_free (u);
          g_string_truncate (sentence, 0);
        }
    }
  g_string_free (sentence, TRUE);
  return lens->len > 0;
}

/* vits-piper-ru_RU-irina-medium -> "ru\\0piper-ru-irina-medium" (one block,
 * g_free), NULL if the name has no <lang>_<REGION> part */
static gchar *
piper_voice_name (const gchar *name)
{
  gchar **parts = g_strsplit (name, "-", -1);
  gchar *out = NULL;

  for (guint i = 0; parts[i] != NULL; i++)
    {
      const gchar *p = parts[i], *us = strchr (p, '_');
      gsize n = us != NULL ? (gsize) (us - p) : 0;
      gchar *lang, *rest;

      if (n < 2 || n > 3 || strlen (us + 1) != 2
          || !g_ascii_isupper (us[1]) || !g_ascii_isupper (us[2]))
        continue;
      lang = g_ascii_strdown (p, n);
      rest = parts[i + 1] != NULL ? g_strjoinv ("-", parts + i + 1) : g_strdup ("1");
      out = g_strdup_printf ("%s%cpiper-%s-%s", lang, 0, lang, rest);
      g_free (lang);
      g_free (rest);
      break;
    }
  g_strfreev (parts);
  return out;
}



static void
add_model (const gchar *parent, const gchar *name)
{
  gchar *dir = g_build_filename (parent, name, NULL);
  Model *m;
  gchar *f;

  /* a user's model hides a system one of the same name */
  for (guint i = 0; i < models->len; i++)
    if (strcmp (((Model *) g_ptr_array_index (models, i))->name, name) == 0)
      {
        g_free (dir);
        return;
      }

  m = g_new0 (Model, 1);
  m->reads_acute = -1;
  m->name = g_strdup (name);
  m->dir = dir;

  if (has_file (dir, "tts.json") && has_file (dir, "voice.bin"))
    {
      m->family = FAMILY_SUPERTONIC;
      if (!family_wanted (m->family))
        goto skip;
      m->speakers = supertonic_speakers (dir);
      if (m->speakers == 0)
        goto skip;
      /* voice.bin is made from the sorted style files F1..F5, M1..M5 */
      for (const char *const *l = supertonic_langs; *l != NULL; l++)
        for (int s = 0; s < m->speakers; s++)
          {
            gchar *vname;
            const gchar *gender = NULL;
            int n = s + 1;

            if (m->speakers == 10)
              {
                gender = s < 5 ? "female" : "male";
                n = s % 5 + 1;
              }
            vname = gender != NULL
                    ? g_strdup_printf ("supertonic-%s-%s-%d", *l, gender, n)
                    : g_strdup_printf ("supertonic-%s-%d", *l, n);
            voice_new (m, vname, *l, gender, s);
            g_free (vname);
          }
    }
  else if ((f = find_file (dir, "lm_main", ".onnx")) != NULL)
    {
      g_free (f);
      m->family = FAMILY_POCKET;
      if (!family_wanted (m->family))
        goto skip;
      add_pocket_voices (m, "test_wavs");
      add_pocket_voices (m, "voices");
    }
  else if (has_file (dir, "tokens.txt") && has_file (dir, "espeak-ng-data")
           && !has_file (dir, "voices.bin") && (f = piper_voice_name (name)) != NULL)
    {
      gchar *onnx = find_file (dir, "", ".onnx"), *json = NULL, *path;

      if (onnx == NULL || !family_wanted (FAMILY_PIPER))
        {
          g_free (onnx);
          g_free (f);
          goto skip;
        }
      path = g_strconcat (onnx, ".json", NULL);
      if (!g_file_get_contents (path, &json, NULL, NULL))
        json = g_strdup ("");
      g_free (path);
      m->espeak_voice = piper_espeak_voice (onnx);
      m->letters = piper_uses_text (json);
      g_free (onnx);
      m->family = FAMILY_PIPER;
      /* f is "<lang>\0<voice name>" */
      if (!piper_add_speakers (m, json, f + strlen (f) + 1, f))
        voice_new (m, f + strlen (f) + 1, f, NULL, 0);
      g_free (json);
      g_free (f);
    }
  else if (has_file (dir, "tokens.txt") && !has_file (dir, "voices.bin")
           && !has_file (dir, "espeak-ng-data") && !has_file (dir, "lexicon.txt")
           && (f = find_file (dir, "", ".onnx")) != NULL)
    {
      /* a VITS model on characters, like MMS: the language from the name */
      const gchar *dash = strrchr (name, '-');
      g_free (f);
      if (dash == NULL || strlen (dash + 1) < 2 || strlen (dash + 1) > 3
          || !family_wanted (FAMILY_VITS))
        goto skip;
      m->family = FAMILY_VITS;
      voice_new (m, name, dash + 1, NULL, 0);
    }
  else
    goto skip;   /* Kitten, Kokoro: later */

  g_ptr_array_add (models, m);
  return;

skip:
  DBG ("skipping %s", dir);
  g_free (m->name);
  g_free (m->dir);
  g_free (m->espeak_voice);
  g_free (m);
}



static void
scan_models (const gchar *parent)
{
  GDir *d = g_dir_open (parent, 0, NULL);
  const gchar *f;
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

  if (d == NULL)
    return;
  while ((f = g_dir_read_name (d)) != NULL)
    g_ptr_array_add (names, g_strdup (f));
  g_dir_close (d);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      if (g_file_test (p, G_FILE_TEST_IS_DIR))
        add_model (parent, g_ptr_array_index (names, i));
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static gint
voice_compare (gconstpointer a, gconstpointer b)
{
  const Voice *va = *(Voice * const *) a, *vb = *(Voice * const *) b;

  if (va->model->family != vb->model->family)
    return (int) va->model->family - (int) vb->model->family;
  if (va->model != vb->model)
    return strcmp (va->model->name, vb->model->name);
  if (strcmp (va->language, vb->language) != 0)
    return strcmp (va->language, vb->language);
  return va->sid != vb->sid ? va->sid - vb->sid : g_utf8_collate (va->name, vb->name);
}



static void
load_voices (void)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "sherpa-onnx", "tts", NULL);

  models = g_ptr_array_new ();
  voices = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);
  g_ptr_array_sort (voices, voice_compare);
  for (guint i = 0; i < voices->len; i++)
    {
      Voice *v = g_ptr_array_index (voices, i);
      tts_voice_add (v->name, v->language, v->gender, v->model, v);
    }
}



static gboolean
sherpa_init (gchar **msg)
{
  /* without the plugin sherpa-onnx would quietly use the CPU; this module
   * exists for the graphics card, so it stays out of the list */
  if (cfg_vulkan && getenv ("SHERPA_ONNX_VULKAN_EP") == NULL
      && !g_file_test (VULKAN_EP_LIBRARY, G_FILE_TEST_EXISTS))
    {
      *msg = g_strdup ("the Vulkan plugin " VULKAN_EP_LIBRARY " is not installed");
      return FALSE;
    }
  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no sherpa-onnx TTS models in %s", cfg_models_dir);
      return FALSE;
    }
  *msg = g_strdup_printf ("sherpa-onnx ready, %u voices", voices->len);
  return TRUE;
}



static int
wanted_threads (void)
{
  long n = sysconf (_SC_NPROCESSORS_ONLN);

  if (cfg_threads > 0)
    return cfg_threads;
  /* more threads were slower on a 4-core / 8-thread Ryzen */
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}



/* in the loader thread */
static gboolean
sherpa_load (gpointer model, GError **error)
{
  Model *m = model;
  SherpaOnnxOfflineTtsConfig c;
  GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
  int saved;

#define FILE_(prefix, suffix) ({ gchar *f_ = find_file (m->dir, prefix, suffix); \
                                  g_ptr_array_add (keep, f_); f_; })
#define PATH_(name) ({ gchar *p_ = g_build_filename (m->dir, name, NULL); \
                       g_ptr_array_add (keep, p_); p_; })

  memset (&c, 0, sizeof c);
  c.model.num_threads = wanted_threads ();
  c.model.provider = cfg_vulkan ? "vulkan" : "cpu";
  c.model.debug = tts_debug > 1;
  c.max_num_sentences = 1;
  c.silence_scale = cfg_silence_scale;
  switch (m->family)
    {
    case FAMILY_SUPERTONIC:
      c.model.supertonic.duration_predictor = FILE_ ("duration_predictor", ".onnx");
      c.model.supertonic.text_encoder = FILE_ ("text_encoder", ".onnx");
      c.model.supertonic.vector_estimator = FILE_ ("vector_estimator", ".onnx");
      c.model.supertonic.vocoder = FILE_ ("vocoder", ".onnx");
      c.model.supertonic.tts_json = PATH_ ("tts.json");
      c.model.supertonic.unicode_indexer = PATH_ ("unicode_indexer.bin");
      c.model.supertonic.voice_style = PATH_ ("voice.bin");
      break;
    case FAMILY_POCKET:
      c.model.pocket.lm_flow = FILE_ ("lm_flow", ".onnx");
      c.model.pocket.lm_main = FILE_ ("lm_main", ".onnx");
      c.model.pocket.encoder = FILE_ ("encoder", ".onnx");
      c.model.pocket.decoder = FILE_ ("decoder", ".onnx");
      c.model.pocket.text_conditioner = FILE_ ("text_conditioner", ".onnx");
      c.model.pocket.vocab_json = PATH_ ("vocab.json");
      c.model.pocket.token_scores_json = PATH_ ("token_scores.json");
      c.model.pocket.voice_embedding_cache_capacity = 8;
      break;
    case FAMILY_PIPER:
      c.model.vits.model = FILE_ ("", ".onnx");
      c.model.vits.tokens = PATH_ ("tokens.txt");
      c.model.vits.noise_scale = 0.667f;
      c.model.vits.noise_scale_w = 0.8f;
      c.model.vits.length_scale = 1.0f;
      break;
    case FAMILY_VITS:
      c.model.vits.model = FILE_ ("", ".onnx");
      c.model.vits.tokens = PATH_ ("tokens.txt");
      c.model.vits.noise_scale = 0.667f;
      c.model.vits.noise_scale_w = 0.8f;
      c.model.vits.length_scale = 1.0f;
      break;
    }
#undef FILE_
#undef PATH_

  saved = tts_stderr_off ();
  m->tts = SherpaOnnxCreateOfflineTts (&c);
  tts_stderr_restore (saved);
  g_ptr_array_free (keep, TRUE);
  if (m->tts == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot load the model %s", m->dir);
      return FALSE;
    }
  DBG ("loaded %s (%s, %s, %d threads)", m->name, family_names[m->family],
       cfg_vulkan ? "vulkan" : "cpu", wanted_threads ());
  return TRUE;
}



static void
sherpa_unload (gpointer model)
{
  Model *m = model;

  SherpaOnnxDestroyOfflineTts (m->tts);
  m->tts = NULL;
  /* give the freed heap back to the system */
  malloc_trim (0);
}



/* ------------------------------------------------------------------ speech */

static int32_t
progress_cb (const float *samples, int32_t n, float p, void *arg)
{
  return tts_stopped () ? 0 : 1;
}



/* The pause after a piece, sent before the next one.  Each piece is synthesized on
 * its own, so the pause sherpa-onnx puts between the sentences of one call
 * (0.2 s times silence_scale for VITS models) is missing; Piper and MMS end
 * a piece with only about 60 ms of silence, which ran the sentences
 * together.  Supertonic and Pocket TTS end with a long silence of their own. */
static void
send_pause (gunichar last, Voice *v)
{
  float seconds;
  int sr;
  size_t n;
  float *zeros;

  if (v->model->family != FAMILY_PIPER && v->model->family != FAMILY_VITS)
    return;
  if (last == 0 || v->model->tts == NULL)
    return;
  /* a sentence end (or line break) as sherpa-onnx; half after a comma cut */
  seconds = (last == ',' || last == ':' || last == 0x2014 ? 0.1f : 0.2f) * cfg_silence_scale;
  sr = SherpaOnnxOfflineTtsSampleRate (v->model->tts);
  n = (size_t) (seconds * sr);
  if (n == 0)
    return;
  zeros = g_new0 (float, n);
  tts_audio_float (zeros, n, sr);
  g_free (zeros);
}



static gboolean
speak_sentence (const gchar *text, const TtsPiece *p, GError **error)
{
  SherpaOnnxGenerationConfig g;
  const SherpaOnnxGeneratedAudio *audio;
  gchar *extra = NULL, *lowered = NULL;
  GArray *cps = NULL, *lens = NULL;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, p->settings->rate / 100.0);
  Voice *v = p->voice->data;
  const SherpaOnnxOfflineTts *tts = v->model->tts;
  gboolean ok = TRUE;

  memset (&g, 0, sizeof g);
  g.sid = v->sid;
  g.silence_scale = cfg_silence_scale;
  switch (v->model->family)
    {
    case FAMILY_SUPERTONIC:
      /* 1.05 is Supertonic's own normal speed */
      g.speed = 1.05f * speed;
      g.num_steps = cfg_steps > 0 ? cfg_steps : 5;
      extra = g_strdup_printf ("{\"lang\": \"%s\"}", v->language);
      g.extra = extra;
      break;
    case FAMILY_POCKET:
      g.speed = speed;
      if (v->ref == NULL)
        v->ref = SherpaOnnxReadWave (v->wav);
      if (v->ref == NULL)
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot read %s", v->wav);
          return FALSE;
        }
      g.reference_audio = v->ref->samples;
      g.reference_audio_len = v->ref->num_samples;
      g.reference_sample_rate = v->ref->sample_rate;
      break;
    case FAMILY_PIPER:
      g.speed = speed;
      cps = g_array_new (FALSE, FALSE, sizeof (gint32));
      lens = g_array_new (FALSE, FALSE, sizeof (gint32));
      if (v->model->letters ? !piper_letters (text, cps, lens)
                            : !piper_phonemize (v->model, text, cps, lens))
        {
          g_array_free (cps, TRUE);
          g_array_free (lens, TRUE);
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "no phonemes");
          return FALSE;
        }
      g.phoneme_codepoints = (const int32_t *) cps->data;
      g.phoneme_codepoints_lens = (const int32_t *) lens->data;
      g.phoneme_codepoints_num_sentences = (int32_t) lens->len;
      break;
    case FAMILY_VITS:
      g.speed = speed;
      /* MMS knows only lower case */
      text = lowered = g_utf8_strdown (text, -1);
      break;
    }

  audio = SherpaOnnxOfflineTtsGenerateWithConfig (tts, text, &g, progress_cb, NULL);
  if (audio != NULL)
    {
      tts_audio_float (audio->samples, audio->n, audio->sample_rate);
      SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);
    }
  else if (!tts_stopped ())
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "sherpa-onnx returned nothing");
      ok = FALSE;
    }
  g_free (extra);
  g_free (lowered);
  if (cps != NULL)
    g_array_free (cps, TRUE);
  if (lens != NULL)
    g_array_free (lens, TRUE);
  return ok;
}



/* in the worker */
static gboolean
sherpa_speak (const TtsPiece *p, GError **error)
{
  Voice *v = p->voice->data;
  const gchar *end = p->text + strlen (p->text);
  gchar *ready;
  gboolean ok;

  if (!p->first)
    send_pause (prev_last, v);
  prev_last = end > p->text ? g_utf8_get_char (g_utf8_prev_char (end)) : 0;
  if (tts_stopped ())
    return TRUE;
  if (stress_stage == NULL && stress_cfg.enabled)
    stress_stage = tts_stress_new (&stress_cfg);
  ready = tts_stress_apply_mark (stress_stage, p->text, v->language, stress_mark_for (v));
  ok = speak_sentence (ready, p, error);
  g_free (ready);
  return ok;
}



static void
sherpa_close (void)
{
  tts_stress_free (stress_stage);
  stress_stage = NULL;
}



const TtsEngine tts_engine = {
  .name = "sd_sherpa-onnx",
  .prefix = "Sherpa",
  .alt_prefix = "Piper",
  .max_models = 2,
  .idle_timeout = 300,
  .first_chunk = 40,
  .setup = sherpa_setup,
  .config = sherpa_config,
  .init = sherpa_init,
  .load = sherpa_load,
  .unload = sherpa_unload,
  .speak = sherpa_speak,
  .close = sherpa_close,
};
