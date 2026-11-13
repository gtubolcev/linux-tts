/*
 * sd_teratts - speech-dispatcher output module for TeraTTSv2 voices
 *
 * TeraTTSv2 (TeraSpace) is one model for Russian and English; a voice is a
 * style (styles/<name>/style_ttl.npy, style_dp.npy) of the model release
 * in TerattsModelDir.  styles/ru_f1 becomes the voice teratts-ru-f1,
 * styles/eng_m2_whisper teratts-en-m2-whisper.  Every voice reads both
 * languages: the Latin words of a text are tagged as English, the rest as
 * Russian.  The Russian stress marks come from the Vosk TTS dictionary
 * (common/ru-stress.c); a "+" written before a vowel wins.
 *
 * Loading the four graphs takes a few seconds, so it starts in a thread
 * when the module starts; a message waits for it.
 *
 * The text is cut into sentences; each one is synthesized and handed to the
 * speech-dispatcher server at once, so the next one is synthesized while the
 * previous one plays.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <speech-dispatcher/spd_module_main.h>

#include "ru-stress.h"
#include "teratts.h"

#define DBG(...) do { if (debug_on) { fprintf (stderr, "sd_teratts: " __VA_ARGS__); fputc ('\n', stderr); } } while (0)
#define ERR(...) do { fprintf (stderr, "sd_teratts: " __VA_ARGS__); fputc ('\n', stderr); } while (0)

/* longer text gets a wrong duration and is slow: cut it */
#define MAX_CHARS 300

typedef struct
{
  gchar *name;              /* teratts-ru-f1 */
  gchar *style;             /* ru_f1 */
  gchar *language;          /* ru */
  gchar *gender;            /* "male", "female" or NULL */
  TeraVoice *tv;
} Voice;

/* configuration */
static gchar *cfg_model_dir = NULL;
static gchar *cfg_dictionary = NULL;
static GHashTable *cfg_lang_voice = NULL;  /* TerattsVoice "<lang>" "<voice>" */
static int cfg_threads = 0;
static int cfg_first_chunk = 40;
static gboolean cfg_preload = TRUE;
static int debug_on = 0;
/* module_close called by module_loop, not as the reply to QUIT */
static gboolean closing_from_loop;

static gchar *model_dir = NULL;            /* the one found */
static GPtrArray *voices = NULL;           /* Voice */
static SPDVoice **spd_voices = NULL;
static RuStress *stress = NULL;

/* message settings */
static gchar *set_voice_name = NULL;
static const gchar *set_gender = NULL;
static int set_type_index = 0;
static gchar *set_language = NULL;
static int set_rate = 0;
static int set_volume = 0;

/* loading, guarded by load_lock */
static GMutex load_lock;
static GCond load_cond;
static TeraTts *model = NULL;
static gboolean loading = FALSE, load_failed = FALSE;
static GThread *load_thread = NULL;

static volatile int stop_requested = 0;
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



static gboolean
parse_bool (const gchar *v)
{
  return g_ascii_strcasecmp (v, "1") == 0 || g_ascii_strcasecmp (v, "yes") == 0
         || g_ascii_strcasecmp (v, "true") == 0 || g_ascii_strcasecmp (v, "on") == 0;
}



int
module_config (const char *configfile)
{
  gchar *contents = NULL;
  gchar **lines;

  cfg_model_dir = g_strdup ("/usr/share/teratts/teratts-v2");
  cfg_dictionary = g_strdup ("/usr/share/vosk-tts/vosk-model-tts-ru-0.9-multi/dictionary.sorted");

  if (configfile == NULL || !g_file_get_contents (configfile, &contents, NULL, NULL))
    return 0;

  lines = g_strsplit (contents, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *line = g_strstrip (*l);
      gchar *value;

      if (*line == '\0' || *line == '#')
        continue;
      value = line;
      while (*value != '\0' && !isspace ((guchar) *value))
        value++;
      if (*value != '\0')
        *value++ = '\0';
      value = unquote (value);

      if (g_ascii_strcasecmp (line, "TerattsModelDir") == 0)
        g_free (cfg_model_dir), cfg_model_dir = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "TerattsDictionary") == 0)
        g_free (cfg_dictionary), cfg_dictionary = g_strdup (value);
      else if (g_ascii_strcasecmp (line, "TerattsThreads") == 0)
        cfg_threads = atoi (value);
      else if (g_ascii_strcasecmp (line, "TerattsFirstChunk") == 0)
        cfg_first_chunk = atoi (value);
      else if (g_ascii_strcasecmp (line, "TerattsPreload") == 0)
        cfg_preload = parse_bool (value);
      else if (g_ascii_strcasecmp (line, "TerattsVoice") == 0)
        {
          /* value was unquoted as one string: ru" "teratts-ru-f1 */
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



/* ------------------------------------------------------------------ voices */

/* ru_f1 -> teratts-ru-f1, ru, female; eng_m2_whisper -> teratts-en-m2-whisper */
static void
add_voice (const gchar *style)
{
  GError *error = NULL;
  TeraVoice *tv = teratts_voice_load (model_dir, style, &error);
  gchar **f;
  Voice *v;

  if (tv == NULL)
    {
      DBG ("skipping %s: %s", style, error->message);
      g_error_free (error);
      return;
    }
  f = g_strsplit (style, "_", -1);
  v = g_new0 (Voice, 1);
  v->style = g_strdup (style);
  v->language = g_strdup (g_str_equal (f[0], "eng") ? "en" : f[0]);
  if (f[1] != NULL && f[1][0] == 'f')
    v->gender = g_strdup ("female");
  else if (f[1] != NULL && f[1][0] == 'm')
    v->gender = g_strdup ("male");
  {
    gchar *rest = g_strjoinv ("-", f + 1);
    v->name = *rest != '\0' ? g_strdup_printf ("teratts-%s-%s", v->language, rest)
                            : g_strdup_printf ("teratts-%s", style);
    g_free (rest);
  }
  v->tv = tv;
  g_strfreev (f);
  g_ptr_array_add (voices, v);
}



static void
load_voices (void)
{
  gchar *user = g_build_filename (g_get_user_data_dir (), "teratts", "teratts-v2", NULL);
  gchar **styles;

  voices = g_ptr_array_new ();
  model_dir = g_strdup (cfg_model_dir);
  if (!g_file_test (model_dir, G_FILE_TEST_IS_DIR) && g_file_test (user, G_FILE_TEST_IS_DIR))
    {
      g_free (model_dir);
      model_dir = g_strdup (user);
    }
  g_free (user);
  styles = teratts_list_voices (model_dir);
  /* Russian voices first: they are the defaults */
  for (int pass = 0; pass < 2; pass++)
    for (gchar **s = styles; *s != NULL; s++)
      if (g_str_has_prefix (*s, "ru_") == (pass == 0))
        add_voice (*s);
  g_strfreev (styles);

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
  /* teratts.py takes the physical cores; more were slower with Kokoro
   * on a 4-core / 8-thread Ryzen */
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}



static gchar *
stress_cb (const gchar *text, gpointer data)
{
  return ru_stress_apply (data, text, RU_STRESS_PLUS);
}



/* runs in load_thread */
static gpointer
load_worker (gpointer data)
{
  GError *error = NULL;
  gint64 t0 = g_get_monotonic_time ();
  int saved = -1, null_fd;
  TeraTts *t;

  /* Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: ... already
   * registered" lines; harmless, keep them out of the log */
  if (debug_on < 2 && (null_fd = open ("/dev/null", O_WRONLY)) >= 0)
    {
      fflush (stderr);
      saved = dup (STDERR_FILENO);
      dup2 (null_fd, STDERR_FILENO);
      close (null_fd);
    }
  t = teratts_new (model_dir, wanted_threads (), &error);
  if (saved >= 0)
    {
      fflush (stderr);
      dup2 (saved, STDERR_FILENO);
      close (saved);
    }
  if (t != NULL)
    {
      /* the first run allocates the buffers; pay it here, not on the
       * first message */
      gchar *text = teratts_prepare (t, "Привет.", "ru", NULL, NULL);
      gsize n;
      g_free (teratts_synth (t, ((Voice *) g_ptr_array_index (voices, 0))->tv, text,
                             1.0f, 1, NULL, 0, &n, NULL));
      g_free (text);
      DBG ("loaded %s (%d threads) in %.1f s", model_dir, wanted_threads (),
           (g_get_monotonic_time () - t0) / 1e6);
    }
  else
    {
      ERR ("cannot load %s: %s", model_dir, error->message);
      g_error_free (error);
    }

  g_mutex_lock (&load_lock);
  model = t;
  load_failed = t == NULL;
  loading = FALSE;
  g_cond_broadcast (&load_cond);
  g_mutex_unlock (&load_lock);
  return NULL;
}



/* starts loading the model; the caller holds load_lock */
static void
request_model_locked (void)
{
  if (model != NULL || loading || load_failed)
    return;
  loading = TRUE;
  load_thread = g_thread_new ("teratts-load", load_worker, NULL);
}



int
module_init (char **msg)
{
  load_voices ();
  if (voices->len == 0)
    {
      *msg = g_strdup_printf ("no TeraTTS voices in %s", model_dir);
      return -1;
    }
  if (g_file_test (cfg_dictionary, G_FILE_TEST_EXISTS))
    {
      GError *error = NULL;
      stress = ru_stress_new (cfg_dictionary, &error);
      if (stress == NULL)
        {
          ERR ("%s", error->message);
          g_error_free (error);
        }
    }
  else
    ERR ("no stress dictionary %s, the model guesses the stress", cfg_dictionary);
  module_audio_set_server ();
  *msg = g_strdup_printf ("TeraTTS ready, %u voices", voices->len);
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



/* the synthesis voice, else the language's voice from the configuration,
 * else the n-th voice of the voice type's gender of the language (else of
 * any language) */
static Voice *
pick_voice (void)
{
  gchar *lang = NULL;
  Voice *found = voice_by_name (set_voice_name);

  if (set_language != NULL)
    lang = g_ascii_strdown (set_language, strcspn (set_language, "-_"));

  if (found == NULL && cfg_lang_voice != NULL && lang != NULL)
    found = voice_by_name (g_hash_table_lookup (cfg_lang_voice, lang));

  if (found == NULL)
    {
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();

      for (guint i = 0; i < voices->len && lang != NULL; i++)
        {
          Voice *v = g_ptr_array_index (voices, i);
          if (strcmp (v->language, lang) == 0)
            g_ptr_array_add (cand, v);
        }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
      for (guint i = 0; i < cand->len; i++)
        {
          Voice *v = g_ptr_array_index (cand, i);
          if (set_gender != NULL && g_strcmp0 (v->gender, set_gender) == 0)
            g_ptr_array_add (same, v);
        }
      if (same->len > 0)
        found = g_ptr_array_index (same, set_type_index % same->len);
      else
        found = g_ptr_array_index (cand, 0);
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
    if (module_process (STDIN_FILENO, 0) == 0 || (fd.revents & (POLLHUP | POLLERR)))
      {
        /* QUIT while we speak, or the server is gone (the pipe stays
         * readable at its end, so polling again would spin): stop the
         * message, module_loop then quits */
        quit_requested = TRUE;
        stop_requested = 1;
      }
}



/* waits until the model is loaded; FALSE if it failed or the message was
 * stopped */
static gboolean
wait_for_model (void)
{
  gboolean ok;

  g_mutex_lock (&load_lock);
  request_model_locked ();
  while (model == NULL && loading && !stop_requested)
    {
      gint64 until = g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND;
      g_cond_wait_until (&load_cond, &load_lock, until);
      g_mutex_unlock (&load_lock);
      process_input ();
      g_mutex_lock (&load_lock);
    }
  ok = model != NULL;
  g_mutex_unlock (&load_lock);
  return ok;
}



static void
send_audio (const float *f, size_t n)
{
  short *s;
  /* 0 and above keep the level the model made; below 0 it gets quieter */
  double gain = set_volume >= 0 ? 1.0 : CLAMP ((set_volume + 100) / 100.0, 0.0, 1.0);
  AudioTrack track;

  if (n == 0)
    return;
  s = g_new (short, n);
  for (size_t i = 0; i < n; i++)
    s[i] = (short) (CLAMP (f[i] * gain, -1.0, 1.0) * 32767.0);
  track.bits = 16;
  track.num_channels = 1;
  track.sample_rate = TERATTS_SAMPLE_RATE;
  track.num_samples = (int) n;
  track.samples = s;
  module_tts_output_server (&track, SPD_AUDIO_LE);
  g_free (s);
}



/* where to cut a text that is too long: the last comma-like mark before the
 * middle, else the last space before it, else the middle */
static glong
cut_point (const gchar *text)
{
  glong n = g_utf8_strlen (text, -1), half = n / 2, space = -1, mark = -1, i = 0;

  for (const gchar *p = text; *p != '\0' && i < half; p = g_utf8_next_char (p), i++)
    {
      gunichar c = g_utf8_get_char (p);
      if (c == ' ')
        {
          space = i;
          if (i > 0)
            {
              gunichar prev = g_utf8_get_char (g_utf8_prev_char (p));
              if (prev == ',' || prev == ':' || prev == ';' || prev == 0x2014)
                mark = i;
            }
        }
    }
  if (mark > n / 4)
    return mark;
  return space > 0 ? space : MAX (half, 1);
}



static void
speak_text (const gchar *text, Voice *v)
{
  GError *error = NULL;
  /* rate -100..100 -> half .. double speed */
  float scale = (float) pow (2.0, -CLAMP (set_rate, -100, 100) / 100.0);
  gint64 t0 = g_get_monotonic_time ();
  const gchar *lang = set_language != NULL && g_ascii_strncasecmp (set_language, "en", 2) == 0
                      ? "en" : "ru";
  gchar *prepared;
  gsize n = 0;
  float *wav;

  if (g_utf8_strlen (text, -1) > MAX_CHARS)
    {
      glong cut = cut_point (text);
      const gchar *mid = g_utf8_offset_to_pointer (text, cut);
      gchar *first = g_strndup (text, mid - text);
      speak_text (g_strstrip (first), v);
      g_free (first);
      if (!stop_requested)
        {
          gchar *rest = g_strstrip (g_strdup (mid));
          process_input ();
          if (!stop_requested && *rest != '\0')
            speak_text (rest, v);
          g_free (rest);
        }
      return;
    }
  prepared = teratts_prepare (model, text, lang, stress != NULL ? stress_cb : NULL, stress);
  if (prepared == NULL)
    return;
  wav = teratts_synth (model, v->tv, prepared, scale, g_str_hash (text), NULL, 0, &n, &error);
  if (wav == NULL)
    {
      ERR ("synthesis failed: %s: %s", error->message, text);
      g_error_free (error);
      g_free (prepared);
      return;
    }
  if (debug_on)
    {
      gchar *nfc = g_utf8_normalize (prepared, -1, G_NORMALIZE_NFC);
      DBG ("%.2f s of speech in %.2f s: %s", n / (double) TERATTS_SAMPLE_RATE,
           (g_get_monotonic_time () - t0) / 1e6, nfc);
      g_free (nfc);
    }
  if (!stop_requested)
    send_audio (wav, n);
  g_free (wav);
  g_free (prepared);
}



void
module_speak_sync (const char *data, size_t bytes, SPDMessageType msgtype)
{
  gchar *text;
  GPtrArray *sentences;
  Voice *voice;

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
  sentences = split_sentences (text);
  DBG ("speaking %u sentence(s) with %s: %s", sentences->len, voice->name, text);

  if (wait_for_model ())
    for (guint i = 0; i < sentences->len && !stop_requested; i++)
      {
        process_input ();
        if (!stop_requested)
          speak_text (g_ptr_array_index (sentences, i), voice);
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



int
module_close (void)
{
  gboolean busy;

  g_mutex_lock (&load_lock);
  busy = loading;
  g_mutex_unlock (&load_lock);
  if (busy)
    {
      /* ORT cannot be interrupted while it loads a model; speechd waits for
       * the reply to QUIT meanwhile (every SIGHUP closes all modules), so
       * answer now and leave the memory to the kernel */
      DBG ("quitting while the model loads");
      if (!closing_from_loop)
        fputs ("210 OK QUIT\n", stdout);
      fflush (stdout);
      _exit (0);
    }
  if (load_thread != NULL)
    g_thread_join (load_thread);
  load_thread = NULL;
  teratts_free (model);
  model = NULL;
  ru_stress_free (stress);
  stress = NULL;
  return 0;
}



int
module_loop (void)
{
  if (cfg_preload)
    {
      g_mutex_lock (&load_lock);
      request_model_locked ();
      g_mutex_unlock (&load_lock);
    }
  for (;;)
    {
      struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
      int ret = poll (&fd, 1, -1);

      if (ret < 0 && errno != EINTR)
        return -1;
      if (ret > 0)
        {
          if ((fd.revents & POLLIN) && module_process (STDIN_FILENO, 0) == 0)
            quit_requested = TRUE;
          if (quit_requested)
            {
              closing_from_loop = TRUE;
              module_close ();
              return 0;
            }
          if (fd.revents & (POLLHUP | POLLERR))
            {
              closing_from_loop = TRUE;
              module_close ();
              return -1;
            }
        }
    }
}
