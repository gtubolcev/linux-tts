/*
 * sd_audiocpp - speech-dispatcher output module for the TTS models of
 * audio.cpp (ggml: CPU, Vulkan and the other ggml back ends)
 *
 * The models are the directories in AudiocppModelsDir (and
 * ~/.local/share/audiocpp-tts) with a file audiocpp.ini that tells what the
 * model is and which voices it has:
 *
 *   [model]
 *   family=pocket_tts                  audio.cpp's family of the model
 *   file=shtorm-ru-fast-q8_0.gguf      the model, relative to the directory
 *   name=pocket                        first part of the voice names
 *                                      (default: the family without "_tts")
 *   language=ru                        language of the voices
 *   text-language=                     passed with the text; default none
 *   speed=false                        the model takes no "speed" option:
 *                                      the audio is sped up instead
 *   warmup=Привет.                     the text of the first run (load)
 *
 *   [load-options]       key=value     audiocpp's --load-option
 *   [session-options]    key=value     --session-option
 *   [request-options]    key=value     options of every request
 *
 *   [voice sup1]                       the voice pocket-ru-sup1
 *   gender=female
 *   reference=voices/sup1.wav          a speaker reference (16-bit or
 *                                      float WAV), for cloning families
 *   voice-id=                          or a voice the model has itself
 *   language=                          another language than the model's
 *
 * A model without [voice ...] sections has one voice, <name>-<language>.
 * A voice of the user's directory hides a system one of the same name.
 *
 * Models that can stream (Pocket TTS, Supertonic) are run in a streaming
 * session: the audio goes out chunk by chunk, and STOP ends the synthesis
 * between two chunks.  The others are run in one go per piece.
 *
 * AudiocppBackend "vulkan" (the link sd_audiocpp-vulkan-rs and its
 * audiocpp-vulkan-rs.conf) runs the models on the graphics card; the name
 * matches the graphics card variants of the other modules.
 *
 * One model is kept at a time.  The protocol, the threads, the text and the
 * audio are tts-module's.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <audiocpp.h>

#include "tts-module.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)
#define ERR(...) TTS_ERR (__VA_ARGS__)

#define MANIFEST "audiocpp.ini"

typedef struct
{
  gchar *dir;
  gchar *path;               /* the model file */
  gchar *family;
  gchar *text_language;      /* NULL: none */
  gchar *warmup;
  gboolean speed;            /* takes the request option "speed" */
  gchar **load_opts;         /* key, value, key, value, ..., NULL */
  gchar **session_opts;
  gchar **request_opts;

  /* set by the loader */
  audiocpp_model *am;
  audiocpp_session *ses;
  gboolean streaming;
} Model;

typedef struct
{
  gchar *reference;          /* path; NULL: none */
  gchar *voice_id;           /* NULL: none */
  /* the reference while the model is loaded */
  float *samples;
  gsize frames;
  int rate, channels;
} Voice;

/* configuration */
static gchar *cfg_models_dir = NULL;
static gchar *cfg_backend = NULL;
static int cfg_device = 0;
static int cfg_threads = 0;

static GPtrArray *models = NULL;           /* Model */
static audiocpp_registry *registry = NULL;



static void
audiocpp_setup (void)
{
  cfg_models_dir = g_strdup ("/usr/share/audiocpp-tts");
  cfg_backend = g_strdup ("cpu");
}



static gboolean
audiocpp_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "AudiocppModelsDir") == 0)
    g_free (cfg_models_dir), cfg_models_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "AudiocppBackend") == 0)
    g_free (cfg_backend), cfg_backend = g_ascii_strdown (value, -1);
  else if (g_ascii_strcasecmp (key, "AudiocppDevice") == 0)
    cfg_device = MAX (atoi (value), 0);
  else if (g_ascii_strcasecmp (key, "AudiocppThreads") == 0)
    cfg_threads = atoi (value);
  else
    return FALSE;
  return TRUE;
}



/* audio.cpp's and ggml's messages: errors always, the rest with Debug */
static void
log_message (audiocpp_log_level level, const char *text, void *user_data)
{
  gchar *line;

  (void) user_data;
  if (text == NULL || (level < AUDIOCPP_LOG_LEVEL_ERROR && !tts_debug))
    return;
  line = g_strchomp (g_strdup (text));
  if (*line != '\0')
    tts_log ("audio.cpp: %s", line);
  g_free (line);
}



/* ------------------------------------------------------------------- WAV */

static guint32
le32 (const guchar *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (guint32) p[3] << 24;
}

static guint16
le16 (const guchar *p)
{
  return p[0] | p[1] << 8;
}

/* a 16-bit PCM or 32-bit float WAV, interleaved floats */
static gboolean
read_wav (const gchar *path, float **samples, gsize *frames, int *rate,
          int *channels, GError **error)
{
  guchar *data;
  gsize len, pos = 12;
  int format = 0, bits = 0;
  gboolean ok = FALSE;

  if (!g_file_get_contents (path, (gchar **) &data, &len, error))
    return FALSE;
  if (len < 12 || memcmp (data, "RIFF", 4) != 0 || memcmp (data + 8, "WAVE", 4) != 0)
    goto bad;
  *channels = 0;
  while (pos + 8 <= len)
    {
      guint32 size = le32 (data + pos + 4);
      const guchar *body = data + pos + 8;

      if (size > len - pos - 8)
        size = (guint32) (len - pos - 8);
      if (memcmp (data + pos, "fmt ", 4) == 0 && size >= 16)
        {
          format = le16 (body);
          *channels = le16 (body + 2);
          *rate = (int) le32 (body + 4);
          bits = le16 (body + 14);
          /* WAVE_FORMAT_EXTENSIBLE: the format is the subformat's */
          if (format == 0xFFFE && size >= 26)
            format = le16 (body + 24);
        }
      else if (memcmp (data + pos, "data", 4) == 0 && *channels > 0)
        {
          gsize n;

          if (*channels > 8 || *rate < 4000 || *rate > 384000)
            goto bad;
          if (format == 1 && bits == 16)
            {
              n = size / 2;
              *samples = g_new (float, n);
              for (gsize i = 0; i < n; i++)
                (*samples)[i] = (gint16) le16 (body + 2 * i) / 32768.0f;
            }
          else if (format == 3 && bits == 32)
            {
              n = size / 4;
              *samples = g_new (float, n);
              memcpy (*samples, body, n * 4);
            }
          else
            goto bad;
          *frames = n / *channels;
          ok = *frames > 0;
          if (!ok)
            g_clear_pointer (samples, g_free);
          break;
        }
      pos += 8 + size + (size & 1);
    }

bad:
  if (!ok)
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "%s: not a 16-bit or float WAV file", path);
  g_free (data);
  return ok;
}



/* ------------------------------------------------------------------ models */

static gboolean
voice_exists (const gchar *name)
{
  GPtrArray *voices = tts_voices ();

  for (guint i = 0; i < voices->len; i++)
    if (strcmp (((TtsVoice *) g_ptr_array_index (voices, i))->name, name) == 0)
      return TRUE;
  return FALSE;
}

/* the keys of a group as key, value, ..., NULL; NULL without the group */
static gchar **
read_options (GKeyFile *kf, const gchar *group)
{
  gchar **keys = g_key_file_get_keys (kf, group, NULL, NULL);
  GPtrArray *kv;

  if (keys == NULL)
    return NULL;
  kv = g_ptr_array_new ();
  for (gchar **k = keys; *k != NULL; k++)
    {
      gchar *v = g_key_file_get_string (kf, group, *k, NULL);
      if (v == NULL)
        continue;
      g_ptr_array_add (kv, g_strdup (*k));
      g_ptr_array_add (kv, v);
    }
  g_ptr_array_add (kv, NULL);
  g_strfreev (keys);
  return (gchar **) g_ptr_array_free (kv, FALSE);
}

static gchar *
key_string (GKeyFile *kf, const gchar *group, const gchar *key)
{
  gchar *v = g_key_file_get_string (kf, group, key, NULL);

  if (v != NULL && *g_strstrip (v) == '\0')
    g_clear_pointer (&v, g_free);
  return v;
}

/* a file named in the manifest: relative to the model's directory */
static gchar *
model_file (const gchar *dir, const gchar *name)
{
  return g_path_is_absolute (name) ? g_strdup (name) : g_build_filename (dir, name, NULL);
}

static void
add_voice (Model *m, const gchar *synth, const gchar *lang, const gchar *id,
           const gchar *gender, const gchar *reference, const gchar *voice_id)
{
  gchar *full = id != NULL ? g_strdup_printf ("%s-%s-%s", synth, lang, id)
                           : g_strdup_printf ("%s-%s", synth, lang);
  gchar *name = g_ascii_strdown (full, -1);
  Voice *v;

  g_free (full);
  if (voice_exists (name))
    {
      DBG ("%s: %s is there already", m->dir, name);
      g_free (name);
      return;
    }
  if (reference != NULL && !g_file_test (reference, G_FILE_TEST_IS_REGULAR))
    {
      ERR ("%s: the reference %s of %s is missing", m->dir, reference, name);
      g_free (name);
      return;
    }
  v = g_new0 (Voice, 1);
  v->reference = g_strdup (reference);
  v->voice_id = g_strdup (voice_id);
  if (gender != NULL && strcmp (gender, "male") != 0 && strcmp (gender, "female") != 0)
    gender = NULL;
  tts_voice_add (name, lang, gender, m, v);
  DBG ("voice %s (%s)", name, m->path);
  g_free (name);
}

static void
add_model_dir (const gchar *dir)
{
  gchar *ini = g_build_filename (dir, MANIFEST, NULL);
  GKeyFile *kf = g_key_file_new ();
  GError *error = NULL;
  gchar *family = NULL, *file = NULL, *synth = NULL, *lang = NULL;
  gchar **groups = NULL;
  Model *m;
  guint sections = 0;

  if (!g_key_file_load_from_file (kf, ini, G_KEY_FILE_NONE, &error))
    {
      ERR ("%s: %s", ini, error->message);
      g_error_free (error);
      goto out;
    }
  family = key_string (kf, "model", "family");
  file = key_string (kf, "model", "file");
  lang = key_string (kf, "model", "language");
  if (family == NULL || file == NULL || lang == NULL)
    {
      ERR ("%s: family, file and language are needed", ini);
      goto out;
    }
  synth = key_string (kf, "model", "name");
  if (synth == NULL)
    synth = g_str_has_suffix (family, "_tts") ? g_strndup (family, strlen (family) - 4)
                                               : g_strdup (family);
  if (strchr (synth, '-') != NULL)
    {
      /* the first part of a voice name tells the model (the dialog reads it) */
      ERR ("%s: the name %s has a '-'", ini, synth);
      goto out;
    }

  m = g_new0 (Model, 1);
  m->dir = g_strdup (dir);
  m->path = model_file (dir, file);
  m->family = g_strdup (family);
  m->text_language = key_string (kf, "model", "text-language");
  m->warmup = key_string (kf, "model", "warmup");
  if (m->warmup == NULL)
    m->warmup = g_strdup (g_str_has_prefix (lang, "ru") ? "Привет." : "Hello.");
  m->speed = !g_key_file_has_key (kf, "model", "speed", NULL)
             || g_key_file_get_boolean (kf, "model", "speed", NULL);
  m->load_opts = read_options (kf, "load-options");
  m->session_opts = read_options (kf, "session-options");
  m->request_opts = read_options (kf, "request-options");

  groups = g_key_file_get_groups (kf, NULL);
  for (gchar **g = groups; *g != NULL; g++)
    {
      gchar *gender, *ref, *vid, *vlang, *refpath;

      if (!g_str_has_prefix (*g, "voice "))
        continue;
      sections++;
      gender = key_string (kf, *g, "gender");
      ref = key_string (kf, *g, "reference");
      vid = key_string (kf, *g, "voice-id");
      vlang = key_string (kf, *g, "language");
      refpath = ref != NULL ? model_file (dir, ref) : NULL;
      add_voice (m, synth, vlang != NULL ? vlang : lang, g_strstrip (*g + 6), gender, refpath, vid);
      g_free (gender);
      g_free (ref);
      g_free (vid);
      g_free (vlang);
      g_free (refpath);
    }
  if (sections == 0)
    add_voice (m, synth, lang, NULL, NULL, NULL, NULL);
  /* the model stays known even without voices: they point at it */
  g_ptr_array_add (models, m);

out:
  g_strfreev (groups);
  g_free (family);
  g_free (file);
  g_free (synth);
  g_free (lang);
  g_key_file_free (kf);
  g_free (ini);
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
  g_ptr_array_sort (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      gchar *p = g_build_filename (parent, g_ptr_array_index (names, i), NULL);
      gchar *ini = g_build_filename (p, MANIFEST, NULL);
      if (g_file_test (ini, G_FILE_TEST_IS_REGULAR))
        add_model_dir (p);
      g_free (ini);
      g_free (p);
    }
  g_ptr_array_free (names, TRUE);
}



static gboolean
audiocpp_init (gchar **msg)
{
  gchar *user;

  audiocpp_log_set (log_message, NULL);
  /* audio.cpp unpacks the files a GGUF carries (tokenizer, config) into
   * $TMPDIR/audiocpp-gguf/<hash>, a name every user of /tmp shares:
   * another user could own it and change the files.  Ours is private. */
  {
    gchar *tmp = g_build_filename (g_get_user_runtime_dir (), "linux-tts", NULL);
    if (g_mkdir_with_parents (tmp, 0700) == 0)
      g_setenv ("TMPDIR", tmp, TRUE);
    else
      ERR ("%s: %s", tmp, g_strerror (errno));
    g_free (tmp);
  }
  if (audiocpp_registry_create (NULL, &registry) != AUDIOCPP_OK)
    {
      *msg = g_strdup_printf ("audio.cpp: %s", audiocpp_last_error ());
      return FALSE;
    }
  user = g_build_filename (g_get_user_data_dir (), "audiocpp-tts", NULL);
  models = g_ptr_array_new ();
  scan_models (user);
  scan_models (cfg_models_dir);
  g_free (user);
  if (tts_voices ()->len == 0)
    {
      *msg = g_strdup_printf ("no audio.cpp voices in %s", cfg_models_dir);
      return FALSE;
    }
  *msg = g_strdup_printf ("audio.cpp %s ready, %u voices, on %s", audiocpp_build_version (),
                          tts_voices ()->len, cfg_backend);
  return TRUE;
}



static int
wanted_threads (void)
{
  long n = sysconf (_SC_NPROCESSORS_ONLN);

  if (cfg_threads > 0)
    return cfg_threads;
  return n < 1 ? 1 : (n > 4 ? 4 : (int) n);
}

static audiocpp_options *
make_options (gchar **kv)
{
  audiocpp_options *o;

  if (kv == NULL)
    return NULL;
  o = audiocpp_options_create ();
  for (; kv[0] != NULL && kv[1] != NULL; kv += 2)
    audiocpp_options_set (o, kv[0], kv[1]);
  return o;
}

static gboolean
api_failed (audiocpp_status st, const gchar *what, GError **error)
{
  if (st == AUDIOCPP_OK)
    return FALSE;
  g_set_error (error, g_quark_from_static_string ("audiocpp"), st, "%s: %s (%s)", what,
               audiocpp_last_error (), audiocpp_status_string (st));
  return TRUE;
}

static gboolean synthesize (Model *m, TtsVoice *tv, const gchar *text, double speed,
                            gboolean send, GError **error);

/* in the loader thread */
static gboolean
audiocpp_load (gpointer model, GError **error)
{
  Model *m = model;
  audiocpp_model_config mc = { m->family, NULL, NULL, NULL };
  audiocpp_backend_config bc = { cfg_backend, cfg_device, wanted_threads () };
  audiocpp_options *lo = make_options (m->load_opts);
  audiocpp_options *so = make_options (m->session_opts);
  GPtrArray *voices = tts_voices ();
  TtsVoice *first = NULL;
  gint64 t0 = g_get_monotonic_time ();
  audiocpp_status st;

  st = audiocpp_model_load (registry, m->path, &mc, lo, &m->am);
  audiocpp_options_free (lo);
  if (api_failed (st, m->path, error))
    {
      audiocpp_options_free (so);
      m->am = NULL;
      return FALSE;
    }
  m->streaming = audiocpp_model_supports (m->am, "tts", "streaming");
  st = audiocpp_session_create (m->am, "tts", m->streaming ? "streaming" : "offline",
                                &bc, so, &m->ses);
  audiocpp_options_free (so);
  if (api_failed (st, m->path, error))
    goto fail;

  /* the references of the voices, while the model is loaded */
  for (guint i = 0; i < voices->len; i++)
    {
      TtsVoice *tv = g_ptr_array_index (voices, i);
      Voice *v = tv->data;
      if (tv->model != m)
        continue;
      if (v->reference != NULL && v->samples == NULL
          && !read_wav (v->reference, &v->samples, &v->frames, &v->rate, &v->channels, error))
        goto fail;
      if (first == NULL)
        first = tv;
    }

  /* the first run allocates the buffers (and builds the Vulkan pipelines,
   * prepares the first voice); pay it here, not on the first message.  A
   * model the back end accepts but cannot run fails here. */
  if (first != NULL && !synthesize (m, first, m->warmup, 1.0, FALSE, error))
    goto fail;
  DBG ("loaded %s (%s, %s, %d threads) in %.2f s", m->path, m->family, cfg_backend,
       wanted_threads (), (g_get_monotonic_time () - t0) / 1e6);
  return TRUE;

fail:
  tts_engine.unload (m);
  return FALSE;
}



static void
audiocpp_unload (gpointer model)
{
  Model *m = model;
  GPtrArray *voices = tts_voices ();

  for (guint i = 0; i < voices->len; i++)
    {
      TtsVoice *tv = g_ptr_array_index (voices, i);
      if (tv->model == m)
        g_clear_pointer (&((Voice *) tv->data)->samples, g_free);
    }
  g_clear_pointer (&m->ses, audiocpp_session_free);
  g_clear_pointer (&m->am, audiocpp_model_free);
}



/* ------------------------------------------------------------------ speech */

static audiocpp_request *
make_request (Model *m, TtsVoice *tv, const gchar *text, double speed, GError **error)
{
  audiocpp_request *rq = audiocpp_request_create ();
  Voice *v = tv->data;

  if (api_failed (audiocpp_request_set_text (rq, text, m->text_language), "text", error))
    goto fail;
  if (v->voice_id != NULL
      && api_failed (audiocpp_request_set_voice_id (rq, v->voice_id), "voice-id", error))
    goto fail;
  if (v->samples != NULL
      && api_failed (audiocpp_request_set_voice_audio (rq, v->samples, v->frames, v->rate,
                                                       v->channels), "reference", error))
    goto fail;
  for (gchar **kv = m->request_opts; kv != NULL && kv[0] != NULL && kv[1] != NULL; kv += 2)
    if (api_failed (audiocpp_request_set_option (rq, kv[0], kv[1]), kv[0], error))
      goto fail;
  if (m->speed && (speed < 0.99 || speed > 1.01))
    {
      gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd (buf, sizeof buf, "%.3f", speed);
      if (api_failed (audiocpp_request_set_option (rq, "speed", buf), "speed", error))
        goto fail;
    }
  return rq;

fail:
  audiocpp_request_free (rq);
  return NULL;
}

/* the audio of a result or event: Pocket and Supertonic put their stream
 * chunks into the first named output */
static gboolean
result_audio (const audiocpp_result *r, const float **s, gsize *frames, int *rate)
{
  const char *id;
  size_t n = 0;
  int ch = 0;

  if (r == NULL)
    return FALSE;
  if ((audiocpp_result_audio (r, s, &n, rate, &ch) != AUDIOCPP_OK || n == 0)
      && (audiocpp_result_named_audio_count (r) == 0
          || audiocpp_result_named_audio (r, 0, &id, s, &n, rate, &ch) != AUDIOCPP_OK))
    return FALSE;
  /* the models speak mono; take the first channel otherwise */
  if (ch > 1)
    {
      static _Thread_local float *mono = NULL;
      g_free (mono);
      mono = g_new (float, n);
      for (size_t i = 0; i < n; i++)
        mono[i] = (*s)[i * ch];
      *s = mono;
    }
  *frames = n;
  return n > 0;
}

/* sends audio unless warming up: FALSE once stopped */
static gboolean
send (Model *m, const float *s, gsize n, int rate, gboolean really)
{
  if (!really)
    return TRUE;
  /* the frame speeds the audio up by what the model did not do */
  return m->speed ? tts_audio_float (s, n, rate) : tts_audio_float_at (s, n, rate, 1.0);
}

static gboolean
synthesize (Model *m, TtsVoice *tv, const gchar *text, double speed, gboolean really,
            GError **error)
{
  audiocpp_request *rq = make_request (m, tv, text, speed, error);
  const float *s;
  gsize n = 0, total = 0;
  int rate = 0;
  gboolean ok = TRUE;
  gint64 t0 = g_get_monotonic_time (), first = 0;

  if (rq == NULL)
    return FALSE;

  if (!m->streaming)
    {
      audiocpp_result *res = NULL;
      ok = !api_failed (audiocpp_session_run (m->ses, rq, &res), "synthesis", error);
      if (ok && result_audio (res, &s, &n, &rate))
        {
          first = g_get_monotonic_time ();
          total = n;
          send (m, s, n, rate, really);
        }
      if (res != NULL)
        audiocpp_result_free (res);
      goto done;
    }

  if (api_failed (audiocpp_stream_start (m->ses, rq), "stream", error))
    {
      ok = FALSE;
      goto done;
    }
  for (;;)
    {
      audiocpp_event *ev = NULL;
      gboolean final;

      if (really && tts_stopped ())
        break;
      if (api_failed (audiocpp_stream_next_event (m->ses, &ev), "synthesis", error))
        {
          ok = FALSE;
          break;
        }
      if (ev == NULL)
        break;
      if (result_audio (audiocpp_event_as_result (ev), &s, &n, &rate))
        {
          if (first == 0)
            first = g_get_monotonic_time ();
          total += n;
          if (!send (m, s, n, rate, really))
            {
              audiocpp_event_free (ev);
              break;
            }
        }
      final = audiocpp_event_is_final (ev);
      audiocpp_event_free (ev);
      if (final)
        break;
    }
  if (ok && total > 0 && !(really && tts_stopped ()))
    {
      audiocpp_result *res = NULL;
      audiocpp_stream_finish (m->ses, &res);
      if (res != NULL)
        audiocpp_result_free (res);
    }
  else if (ok && total == 0 && !(really && tts_stopped ()))
    {
      /* a family that streams only its final result */
      audiocpp_result *res = NULL;
      ok = !api_failed (audiocpp_stream_finish (m->ses, &res), "synthesis", error);
      if (ok && result_audio (res, &s, &n, &rate))
        {
          first = g_get_monotonic_time ();
          total = n;
          send (m, s, n, rate, really);
        }
      if (res != NULL)
        audiocpp_result_free (res);
    }
  /* stopped or failed in the middle: drop the rest */
  audiocpp_stream_reset (m->ses);

done:
  audiocpp_request_free (rq);
  if (ok && rate > 0)
    DBG ("%.2f s of speech in %.2f s (first audio after %.2f s)%s: %s",
         total / (double) rate, (g_get_monotonic_time () - t0) / 1e6,
         first > 0 ? (first - t0) / 1e6 : 0.0, really ? "" : " (warm-up)", text);
  return ok;
}



/* in the worker */
static gboolean
audiocpp_speak (const TtsPiece *p, GError **error)
{
  Model *m = p->voice->model;

  if (*p->text == '\0')
    return TRUE;
  return synthesize (m, p->voice, p->text, m->speed ? tts_model_speed (p->settings) : 1.0,
                     TRUE, error);
}



static void
audiocpp_close (void)
{
  if (registry != NULL)
    audiocpp_registry_free (registry);
  registry = NULL;
  audiocpp_log_set (NULL, NULL);
}



/* the back end and the library, for the verdict of the test load */
static gchar *
audiocpp_probe_salt (void)
{
  return g_strdup_printf ("%s %d %s abi %u", cfg_backend, cfg_device,
                          audiocpp_build_version (), audiocpp_abi_version ());
}

/* the model file, for the test load in a child process */
static const gchar *
audiocpp_model_path (gpointer model)
{
  return ((Model *) model)->path;
}



const TtsEngine tts_engine = {
  .name = "sd_audiocpp",
  .prefix = "Audiocpp",
  .max_models = 1,
  .idle_timeout = 300,
  .first_chunk = 40,
  .setup = audiocpp_setup,
  .config = audiocpp_config,
  .init = audiocpp_init,
  .load = audiocpp_load,
  .unload = audiocpp_unload,
  .model_path = audiocpp_model_path,
  .probe_salt = audiocpp_probe_salt,
  .speak = audiocpp_speak,
  .close = audiocpp_close,
};
