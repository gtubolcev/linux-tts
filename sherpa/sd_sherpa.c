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
 * The module is installed under four names, each a synthesizer of its own
 * with its configuration: sd_sherpa-onnx (SherpaFamilies "supertonic pocket
 * vits"), sd_piper-sherpa-onnx (PiperFamilies "piper"), and both again with
 * "-vulkan-rs" and Provider "vulkan": the models run on the graphics card
 * through the onnx-vulkan-rs plugin of onnxruntime.  That plugin cannot run
 * Supertonic and Pocket yet, so they are left out there, and without the
 * plugin the -vulkan-rs synthesizers do not start.  Options may carry the
 * prefix "Sherpa" or "Piper".
 *
 * The text is cut into sentences; each one is synthesized and handed to the
 * speech-dispatcher server at once, so the next one is synthesized while the
 * previous one plays.  The first piece is cut at a comma after
 * SherpaFirstChunk characters, so the first sound comes sooner.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <espeak-ng/speak_lib.h>
#include <sherpa-onnx/c-api/c-api.h>
#include <speech-dispatcher/spd_module_main.h>

#include "tts-norm.h"
#include "tts-stress.h"

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_sherpa-onnx: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_sherpa-onnx: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

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
} Model;

typedef struct
{
  gchar *name;      /* supertonic-ru-male-1 */
  gchar *language;  /* ru */
  gchar *gender;    /* "male", "female" or NULL */
  Model *model;
  int sid;
  gchar *wav;       /* Pocket: the reference voice */
  const SherpaOnnxWave *ref;  /* read when first used */
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static GHashTable *cfg_lang_voice = NULL;  /* SherpaVoice "<lang>" "<voice>" */
static int cfg_threads = 0;
static int cfg_steps = 5;
static int cfg_first_chunk = 40;
static gchar *cfg_preload = NULL;          /* a voice or model name */
static int cfg_idle_timeout = 300;          /* s; 0 keeps the model */
/* sherpa-onnx shortens every pause the model makes to this share; its
 * examples use 0.2, which left Piper voices 60 ms at a full stop where
 * Piper itself makes 300 ms.  1 keeps the pauses the voice was trained with. */
static float cfg_silence_scale = 1.0f;
#ifndef VULKAN_EP_LIBRARY
/* where the patched sherpa-onnx looks for it: next to its libonnxruntime */
#define VULKAN_EP_LIBRARY "/usr/lib/x86_64-linux-gnu/sherpa-onnx/libonnxruntime_ep_vulkan.so"
#endif
static gboolean cfg_vulkan = FALSE;      /* Provider "vulkan" */
static guint cfg_families = ~0u;          /* Families, a bit per Family */
static gint64 last_use = 0;                 /* the last command of the server */
static int debug_on = 0;

static GPtrArray *models = NULL;
static GPtrArray *voices = NULL;
static SPDVoice **spd_voices = NULL;

/* message settings */
static gchar *set_voice_name = NULL;
static const gchar *set_gender = NULL;
static int set_type_index = 0;
static gchar *set_language = NULL;
static int set_rate = 0;
static int set_volume = 0;

/* The loaded models, at most SherpaMaxModels of them: a text in two languages
 * spoken by voices of two models (Piper for Russian, Supertonic for English)
 * would otherwise load a model at every change of language.  When another one
 * is needed, the one used longest ago goes. */
#define MAX_MODELS_LIMIT 4
typedef struct
{
  Model *model;
  const SherpaOnnxOfflineTts *tts;
  gint64 used;
} Loaded;
static Loaded loaded[MAX_MODELS_LIMIT];
static int cfg_max_models = 2;
/* the model of the message being spoken (one of loaded[]) */
static const SherpaOnnxOfflineTts *tts = NULL;

static volatile int stop_requested = 0;
static TtsNormConfig norm_cfg;  /* TextNormalization... keys */
static TtsNorm *norm = NULL;
static TtsStressConfig stress_cfg;  /* TextStress... keys */
static TtsStress *stress_stage = NULL;
static gboolean quit_requested = FALSE;



static gchar *
unquote (gchar *s)
{
  gsize n;

  g_strstrip (s);
  n = strlen (s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
    {
      s[n - 1] = '\0';
      return s + 1;
    }
  return s;
}



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
 * Vulkan plugin only the VITS graphs (Piper, MMS).  Supertonic uses Pad mode
 * "edge", which the plugin accepts but fails to run, and an error there ends
 * the process (sherpa-onnx throws through its C API). */
static gboolean
family_wanted (Family family)
{
  if (!(cfg_families & (1u << family)))
    return FALSE;

  return !cfg_vulkan || family == FAMILY_PIPER || family == FAMILY_VITS;
}

int
module_config (const char *configfile)
{
  gchar *contents = NULL;
  gchar **lines;

  cfg_models_dir = g_strdup ("/usr/share/sherpa-onnx/tts");
  tts_norm_config_init (&norm_cfg);
  /* off until the engine is tested with marks */
  tts_stress_config_init (&stress_cfg, FALSE, RU_STRESS_ACUTE);

  if (configfile == NULL || !g_file_get_contents (configfile, &contents, NULL, NULL))
    return 0;

  lines = g_strsplit (contents, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *line = g_strstrip (*l);
      const gchar *key;
      gchar *value;

      if (*line == '\0' || *line == '#')
        continue;
      value = line;
      while (*value != '\0' && !isspace ((guchar) *value))
        value++;
      if (*value != '\0')
        *value++ = '\0';
      value = unquote (value);
      key = option_name (line);

      if (tts_norm_config_parse (&norm_cfg, line, value)
          || tts_stress_config_parse (&stress_cfg, line, value))
        ;
      else if (g_ascii_strcasecmp (key, "ModelsDir") == 0)
        g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (key, "Threads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (key, "SupertonicSteps") == 0)
        cfg_steps = atoi (value);
      else if (g_ascii_strcasecmp (key, "FirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (key, "Families") == 0)
        cfg_families = parse_families (value);
      else if (g_ascii_strcasecmp (key, "Provider") == 0)
        cfg_vulkan = g_ascii_strcasecmp (value, "vulkan") == 0;
      else if (g_ascii_strcasecmp (key, "IdleTimeout") == 0)
        cfg_idle_timeout = atoi (value);
      else if (g_ascii_strcasecmp (key, "MaxModels") == 0)
        cfg_max_models = CLAMP (atoi (value), 1, MAX_MODELS_LIMIT);
      else if (g_ascii_strcasecmp (key, "SilenceScale") == 0)
        {
          float s = (float) g_ascii_strtod (value, NULL);
          cfg_silence_scale = s > 0.0f && s <= 4.0f ? s : 1.0f;
        }
      else if (g_ascii_strcasecmp (key, "Preload") == 0)
        {
          g_free (cfg_preload);
          cfg_preload = (*value != '\0' && strcmp (value, "0") != 0) ? g_strdup (value) : NULL;
        }
      else if (g_ascii_strcasecmp (key, "Voice") == 0)
        {
          /* value was unquoted as one string: ru" "supertonic-ru-male-1 */
          gchar **parts = g_strsplit_set (value, "\" \t", -1);
          gchar *lang = NULL, *name = NULL;

          for (gchar **p = parts; *p != NULL; p++)
            if (**p != '\0')
              {
                if (lang == NULL)
                  lang = *p;
                else if (name == NULL)
                  name = *p;
              }
          if (lang != NULL && name != NULL)
            {
              if (cfg_lang_voice == NULL)
                cfg_lang_voice = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
              g_hash_table_replace (cfg_lang_voice, g_ascii_strdown (lang, -1), g_strdup (name));
            }
          g_strfreev (parts);
        }
      else if (g_ascii_strcasecmp (line, "Debug") == 0)
        debug_on = atoi (value);
    }
  g_strfreev (lines);
  g_free (contents);
  return 0;
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

/* Text -> IPA phonemes the way Piper voices were trained (piper-phonemize):
 * espeak-ng clause by clause, the punctuation put back from the clause
 * terminator, language switch marks such as "(en)" removed. sherpa-onnx >= 2.0
 * does not phonemize Piper voices itself. Appends the codepoints of every
 * sentence to cps and its length to lens; FALSE if espeak-ng is unusable. */
static gboolean
piper_phonemize (const Model *m, const gchar *text, GArray *cps, GArray *lens)
{
  static gboolean ready = FALSE;
  static GRegex *flags = NULL;
  const void *ptr = text;
  GString *sentence = g_string_new (NULL);

  if (!ready)
    {
      /* the espeak-ng-data shipped with the voice: the one it was trained on */
      if (espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, m->dir, 0) < 0)
        {
          ERR ("espeak-ng: cannot load %s/espeak-ng-data", m->dir);
          g_string_free (sentence, TRUE);
          return FALSE;
        }
      flags = g_regex_new ("\\([^)]+\\)", 0, 0, NULL);
      ready = TRUE;
    }
  if (g_strcmp0 (espeak_voice_now, m->espeak_voice) != 0)
    {
      if (espeak_SetVoiceByName (m->espeak_voice != NULL ? m->espeak_voice : "en-us")
          != EE_OK)
        {
          ERR ("espeak-ng: no voice %s", m->espeak_voice);
          g_string_free (sentence, TRUE);
          return FALSE;
        }
      g_free (espeak_voice_now);
      espeak_voice_now = g_strdup (m->espeak_voice);
    }

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

  spd_voices = g_new0 (SPDVoice *, voices->len + 1);
  for (guint i = 0; i < voices->len; i++)
    {
      Voice *v = g_ptr_array_index (voices, i);
      spd_voices[i] = g_new0 (SPDVoice, 1);
      spd_voices[i]->name = v->name;
      spd_voices[i]->language = v->language;
      spd_voices[i]->variant = v->gender;
    }
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



static gboolean
load_model (Model *m)
{
  SherpaOnnxOfflineTtsConfig c;
  GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
  gint64 t0 = g_get_monotonic_time ();

#define FILE_(prefix, suffix) ({ gchar *f_ = find_file (m->dir, prefix, suffix); \
                                  g_ptr_array_add (keep, f_); f_; })
#define PATH_(name) ({ gchar *p_ = g_build_filename (m->dir, name, NULL); \
                       g_ptr_array_add (keep, p_); p_; })

  Loaded *slot = NULL;

  for (int i = 0; i < cfg_max_models; i++)
    if (loaded[i].model == m && loaded[i].tts != NULL)
      {
        loaded[i].used = t0;
        tts = loaded[i].tts;
        g_ptr_array_free (keep, TRUE);
        return TRUE;
      }
  /* a free slot, or the one used longest ago */
  for (int i = 0; i < cfg_max_models; i++)
    if (slot == NULL || loaded[i].tts == NULL
        || (slot->tts != NULL && loaded[i].used < slot->used))
      {
        slot = &loaded[i];
        if (slot->tts == NULL)
          break;
      }
  if (slot->tts != NULL)
    {
      DBG ("unloading %s for %s", slot->model->name, m->name);
      SherpaOnnxDestroyOfflineTts (slot->tts);
      slot->tts = NULL;
      slot->model = NULL;
      malloc_trim (0);
    }
  tts = NULL;

  memset (&c, 0, sizeof c);
  c.model.num_threads = wanted_threads ();
  c.model.provider = cfg_vulkan ? "vulkan" : "cpu";
  c.model.debug = debug_on > 1;
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

  {
    /* Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: ... already
     * registered" lines while a model is loaded; harmless, keep them out of the log */
    int saved = -1, null_fd;

    if (debug_on < 2 && (null_fd = open ("/dev/null", O_WRONLY)) >= 0)
      {
        fflush (stderr);
        saved = dup (STDERR_FILENO);
        dup2 (null_fd, STDERR_FILENO);
        close (null_fd);
      }
    tts = SherpaOnnxCreateOfflineTts (&c);
    if (saved >= 0)
      {
        fflush (stderr);
        dup2 (saved, STDERR_FILENO);
        close (saved);
      }
  }
  g_ptr_array_free (keep, TRUE);
  if (tts == NULL)
    {
      ERR ("cannot load the model %s", m->dir);
      return FALSE;
    }
  slot->model = m;
  slot->tts = tts;
  slot->used = g_get_monotonic_time ();
  DBG ("loaded %s (%s, %s, %d threads) in %.1f s", m->name, family_names[m->family],
       c.model.provider, c.model.num_threads, (g_get_monotonic_time () - t0) / 1e6);
  return TRUE;
}



int
module_init (char **msg)
{
  /* without the plugin sherpa-onnx would quietly use the CPU; this module
   * exists for the graphics card, so it stays out of the list */
  if (cfg_vulkan && getenv ("SHERPA_ONNX_VULKAN_EP") == NULL
      && !g_file_test (VULKAN_EP_LIBRARY, G_FILE_TEST_EXISTS))
    {
      *msg = g_strdup ("the Vulkan plugin " VULKAN_EP_LIBRARY " is not installed");
      return -1;
    }
  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no sherpa-onnx TTS models in %s", cfg_models_dir);
      return -1;
    }
  module_audio_set_server ();
  *msg = g_strdup_printf ("sherpa-onnx ready, %u voices", voices->len);
  return 0;
}



SPDVoice **
module_list_voices (void)
{
  return spd_voices;
}



int
module_set (const char *var, const char *val)
{
  if (strcmp (var, "synthesis_voice") == 0)
    {
      g_free (set_voice_name);
      set_voice_name = (val != NULL && strcmp (val, "NULL") != 0) ? g_strdup (val) : NULL;
    }
  else if (strcmp (var, "language") == 0)
    {
      g_free (set_language);
      set_language = (val != NULL && strcmp (val, "NULL") != 0) ? g_strdup (val) : NULL;
    }
  else if (strcmp (var, "voice") == 0 || strcmp (var, "voice_type") == 0)
    {
      /* male1..3, female1..3, child_male, child_female */
      const gchar *t = val != NULL ? val : "";
      set_gender = NULL;
      set_type_index = 0;
      if (g_ascii_strncasecmp (t, "child_", 6) == 0)
        t += 6;
      if (g_ascii_strncasecmp (t, "female", 6) == 0)
        set_gender = "female", t += 6;
      else if (g_ascii_strncasecmp (t, "male", 4) == 0)
        set_gender = "male", t += 4;
      if (g_ascii_isdigit (*t) && atoi (t) > 0)
        set_type_index = atoi (t) - 1;
    }
  else if (strcmp (var, "rate") == 0)
    set_rate = atoi (val);
  else if (strcmp (var, "volume") == 0)
    set_volume = atoi (val);
  /* pitch, pitch_range, punctuation, spelling, capitals: not supported */
  return 0;
}



int
module_audio_set (const char *var, const char *val)
{
  if (strcmp (var, "audio_output_method") == 0)
    return strcmp (val, "server") == 0 ? 0 : -1;
  return 0;
}



int
module_audio_init (char **status)
{
  *status = strdup ("audio through the server");
  return 0;
}



int
module_loglevel_set (const char *var, const char *val)
{
  return 0;
}



int
module_debug (int enable, const char *file)
{
  debug_on = enable;
  return 0;
}



/* -------------------------------------------------------------------- text */

/* SSML (speech-dispatcher wraps all text in <speak>) to plain text */
static gchar *
ssml_to_text (const char *data, size_t bytes)
{
  GString *out = g_string_sized_new (bytes);
  const char *p = data, *end = data + bytes;

  while (p < end && *p != '\0')
    {
      if (*p == '<')
        {
          const char *close = memchr (p, '>', end - p);
          if (close == NULL)
            break;
          g_string_append_c (out, ' ');
          p = close + 1;
        }
      else if (*p == '&')
        {
          static const struct { const char *ent; char ch; } ents[] = {
            { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' },
            { "&quot;", '"' }, { "&apos;", '\'' }
          };
          gboolean done = FALSE;
          for (guint i = 0; i < G_N_ELEMENTS (ents); i++)
            if (strncmp (p, ents[i].ent, strlen (ents[i].ent)) == 0)
              {
                g_string_append_c (out, ents[i].ch);
                p += strlen (ents[i].ent);
                done = TRUE;
                break;
              }
          if (!done)
            g_string_append_c (out, *p++);
        }
      else
        g_string_append_c (out, *p++);
    }
  return g_strstrip (g_string_free (out, FALSE));
}



/* Cuts the text after . ! ? … ; and line breaks; long pieces also after commas.
 * The first piece is cut at a comma already after cfg_first_chunk characters. */
static GPtrArray *
split_sentences (const gchar *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);

  for (const gchar *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      gunichar next = g_utf8_get_char (g_utf8_next_char (p));
      gboolean end_here = FALSE;

      if (c == '\n')
        {
          end_here = TRUE;
          c = ' ';
        }
      g_string_append_unichar (cur, c);
      if (!end_here && (next == ' ' || next == '\n' || next == '\0'))
        {
          if (c == '.' || c == '!' || c == '?' || c == 0x2026 || c == ';')
            end_here = TRUE;
          else if ((c == ',' || c == ':' || c == 0x2014)
                   && g_utf8_strlen (cur->str, -1)
                      > (out->len == 0 && cfg_first_chunk > 0 ? cfg_first_chunk : 200))
            end_here = TRUE;
        }
      if (end_here)
        {
          g_strstrip (cur->str);
          if (*cur->str != '\0')
            g_ptr_array_add (out, g_strdup (cur->str));
          g_string_truncate (cur, 0);
        }
    }
  g_strstrip (cur->str);
  if (*cur->str != '\0')
    g_ptr_array_add (out, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  return out;
}



static Voice *
voice_by_name (const gchar *name)
{
  for (guint i = 0; name != NULL && i < voices->len; i++)
    {
      Voice *v = g_ptr_array_index (voices, i);
      if (g_ascii_strcasecmp (v->name, name) == 0)
        return v;
    }
  return NULL;
}



static Voice *
pick_voice (void)
{
  gchar *lang = NULL;
  Voice *found = voice_by_name (set_voice_name);

  if (set_language != NULL)
    lang = g_ascii_strdown (set_language, strcspn (set_language, "-_"));

  /* the voice chosen for the language in the configuration */
  if (found == NULL && cfg_lang_voice != NULL && lang != NULL)
    found = voice_by_name (g_hash_table_lookup (cfg_lang_voice, lang));

  /* the voices of the language (else of English, else all), in the order of
   * the families; among them the n-th one of the voice type's gender */
  if (found == NULL)
    {
      const gchar *langs[] = { lang, "en" };
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();
      Model *first = NULL;

      for (guint l = 0; l < 2 && cand->len == 0; l++)
        for (guint i = 0; i < voices->len && langs[l] != NULL; i++)
          {
            Voice *v = g_ptr_array_index (voices, i);
            if (strcmp (v->language, langs[l]) == 0)
              g_ptr_array_add (cand, v);
          }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
      /* only the first (preferred) model of the language */
      first = ((Voice *) g_ptr_array_index (cand, 0))->model;
      for (guint i = 0; i < cand->len; i++)
        {
          Voice *v = g_ptr_array_index (cand, i);
          if (v->model == first && set_gender != NULL && g_strcmp0 (v->gender, set_gender) == 0)
            g_ptr_array_add (same, v);
        }
      if (same->len > 0)
        found = g_ptr_array_index (same, set_type_index % same->len);
      else
        {
          guint n = 0;
          while (n < cand->len && ((Voice *) g_ptr_array_index (cand, n))->model == first)
            n++;
          found = g_ptr_array_index (cand, set_type_index % n);
        }
      g_ptr_array_free (same, TRUE);
      g_ptr_array_free (cand, TRUE);
    }
  g_free (lang);
  return found;
}



/* ------------------------------------------------------------------ speech */

/* Lets the speech-dispatcher server interrupt us (STOP) while we synthesize. */
static void
process_input (void)
{
  struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };

  while (!quit_requested && poll (&fd, 1, 0) > 0 && (fd.revents & (POLLIN | POLLHUP)))
    if (module_process (STDIN_FILENO, 0) == 0)
      {
        /* QUIT while we speak: finish the message, module_loop then quits */
        quit_requested = TRUE;
        stop_requested = 1;
      }
}


/* lets STOP interrupt a wait for the normalizer */
static gboolean
norm_cancel (void)
{
  process_input ();
  return stop_requested != 0;
}

/* the normalizer, made with the first message; NULL when it is off */
static TtsNorm *
get_norm (void)
{
  if (norm == NULL && norm_cfg.enabled)
    {
      norm = tts_norm_new (&norm_cfg);
      tts_norm_set_cancel (norm, norm_cancel);
    }
  return norm;
}

/* the stress stage, made with the first message; NULL when it is off */
static TtsStress *
get_stress (void)
{
  if (stress_stage == NULL && stress_cfg.enabled)
    stress_stage = tts_stress_new (&stress_cfg);
  return stress_stage;
}



static int32_t
progress_cb (const float *samples, int32_t n, float p, void *arg)
{
  process_input ();
  return stop_requested ? 0 : 1;
}



static void
send_audio (const float *f, size_t n, int sample_rate)
{
  short *s;
  /* 0 and above keep the level the model made (its peaks are already near
   * full scale, any boost clips); below 0 it gets quieter. */
  double gain = set_volume >= 0 ? 1.0 : CLAMP ((set_volume + 100) / 100.0, 0.0, 1.0);
  AudioTrack track;

  if (n == 0)
    return;
  s = g_new (short, n);
  for (size_t i = 0; i < n; i++)
    s[i] = (short) (CLAMP (f[i] * gain, -1.0, 1.0) * 32767.0);
  track.bits = 16;
  track.num_channels = 1;
  track.sample_rate = sample_rate;
  track.num_samples = (int) n;
  track.samples = s;
  module_tts_output_server (&track, SPD_AUDIO_LE);
  g_free (s);
}



/* The pause after a piece of split_sentences.  Each piece is synthesized on
 * its own, so the pause sherpa-onnx puts between the sentences of one call
 * (0.2 s times silence_scale for VITS models) is missing; Piper and MMS end
 * a piece with only about 60 ms of silence, which ran the sentences
 * together.  Supertonic and Pocket TTS end with a long silence of their own. */
static void
send_pause (const gchar *piece, Voice *v)
{
  const gchar *end = piece + strlen (piece);
  gunichar last;
  float seconds;
  int sr;
  size_t n;
  float *zeros;

  if (v->model->family != FAMILY_PIPER && v->model->family != FAMILY_VITS)
    return;
  if (end == piece || tts == NULL)
    return;
  last = g_utf8_get_char (g_utf8_prev_char (end));
  /* a sentence end (or line break) as sherpa-onnx; half after a comma cut */
  seconds = (last == ',' || last == ':' || last == 0x2014 ? 0.1f : 0.2f) * cfg_silence_scale;
  sr = SherpaOnnxOfflineTtsSampleRate (tts);
  n = (size_t) (seconds * sr);
  if (n == 0)
    return;
  zeros = g_new0 (float, n);
  send_audio (zeros, n, sr);
  g_free (zeros);
}



static void
speak_sentence (const gchar *text, Voice *v)
{
  SherpaOnnxGenerationConfig g;
  const SherpaOnnxGeneratedAudio *audio;
  gchar *extra = NULL, *lowered = NULL;
  GArray *cps = NULL, *lens = NULL;
  /* rate -100..100 -> half .. double speed */
  float speed = (float) pow (2.0, CLAMP (set_rate, -100, 100) / 100.0);
  gint64 t0 = g_get_monotonic_time ();

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
          ERR ("cannot read %s", v->wav);
          return;
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
          return;
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
      DBG ("%.2f s of speech in %.2f s: %s", audio->n / (double) audio->sample_rate,
           (g_get_monotonic_time () - t0) / 1e6, text);
      if (!stop_requested)
        send_audio (audio->samples, audio->n, audio->sample_rate);
      SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);
    }
  else
    ERR ("synthesis failed: %s", text);
  g_free (extra);
  g_free (lowered);
  if (cps != NULL)
    g_array_free (cps, TRUE);
  if (lens != NULL)
    g_array_free (lens, TRUE);
}



void
module_speak_sync (const char *data, size_t bytes, SPDMessageType msgtype)
{
  gchar *text;
  GPtrArray *sentences;
  Voice *voice;
  gboolean ok, whole;

  stop_requested = 0;
  text = ssml_to_text (data, bytes);
  if (*text == '\0')
    {
      /* nothing to say: finish the message at once, an error would leave
       * clients waiting for its end */
      g_free (text);
      module_speak_ok ();
      module_report_event_begin ();
      module_report_event_end ();
      return;
    }
  module_speak_ok ();
  module_report_event_begin ();

  voice = pick_voice ();
  /* the helper loads while the model does */
  tts_norm_start (get_norm ());
  ok = load_model (voice->model);
  whole = !ok || tts_norm_message (norm, &text, voice->language);
  sentences = split_sentences (text);
  DBG ("speaking %u sentence(s) with %s: %s", sentences->len, voice->name, text);

  if (ok)
    for (guint i = 0; i < sentences->len && !stop_requested; i++)
      {
        const gchar *piece = g_ptr_array_index (sentences, i);

        process_input ();
        if (!stop_requested)
          {
            gchar *norm_piece = whole ? NULL : tts_norm_text (norm, piece, voice->language);
            gchar *ready = tts_stress_apply (get_stress (), norm_piece != NULL ? norm_piece : piece,
                                             voice->language);

            if (!stop_requested)
              speak_sentence (ready, voice);
            g_free (ready);
            g_free (norm_piece);
          }
        if (!stop_requested && i + 1 < sentences->len)
          send_pause (piece, voice);
      }

  g_ptr_array_free (sentences, TRUE);
  g_free (text);
  if (stop_requested)
    module_report_event_stop ();
  else
    module_report_event_end ();
}



size_t
module_pause (void)
{
  stop_requested = 1;
  return 0;
}



int
module_stop (void)
{
  stop_requested = 1;
  return 0;
}



/* frees every loaded model; FALSE if none was loaded */
static gboolean
unload_all (void)
{
  gboolean any = FALSE;

  for (int i = 0; i < MAX_MODELS_LIMIT; i++)
    if (loaded[i].tts != NULL)
      {
        SherpaOnnxDestroyOfflineTts (loaded[i].tts);
        loaded[i].tts = NULL;
        loaded[i].model = NULL;
        any = TRUE;
      }
  tts = NULL;
  return any;
}

int
module_close (void)
{
  unload_all ();
  tts_norm_free (norm);
  tts_stress_free (stress_stage);
  stress_stage = NULL;
  norm = NULL;
  return 0;
}



/* frees the models after SherpaIdleTimeout seconds without a command from
 * the server; the next message loads its model again.  sherpa loads in this
 * thread, so nothing is being loaded or spoken now. */
static void
unload_if_idle (void)
{
  if (cfg_idle_timeout <= 0
      || g_get_monotonic_time () - last_use < (gint64) cfg_idle_timeout * G_TIME_SPAN_SECOND)
    return;
  tts_norm_stop (norm);
  if (!unload_all ())
    return;
  /* give the freed heap back to the system */
  malloc_trim (0);
  DBG ("models unloaded after %d s without speech", cfg_idle_timeout);
}



int
module_loop (void)
{
  last_use = g_get_monotonic_time ();
  if (cfg_preload != NULL)
    {
      /* SherpaPreload names a voice or a model */
      Voice *v = voice_by_name (cfg_preload);
      Model *m = v != NULL ? v->model : NULL;

      for (guint i = 0; i < models->len && m == NULL; i++)
        if (strcmp (((Model *) g_ptr_array_index (models, i))->name, cfg_preload) == 0)
          m = g_ptr_array_index (models, i);
      if (m != NULL)
        load_model (m);
      else
        ERR ("SherpaPreload: no voice or model %s", cfg_preload);
    }
  for (;;)
    {
      struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
      int ret = poll (&fd, 1, cfg_idle_timeout > 0 ? 5000 : -1);

      if (ret < 0 && errno != EINTR)
        return -1;
      if (ret > 0)
        {
          if ((fd.revents & POLLIN) && module_process (STDIN_FILENO, 0) == 0)
            quit_requested = TRUE;
          last_use = g_get_monotonic_time ();
          if (quit_requested)
            {
              module_close ();
              return 0;
            }
          if (fd.revents & (POLLHUP | POLLERR))
            {
              module_close ();
              return -1;
            }
        }
      unload_if_idle ();
    }
}
