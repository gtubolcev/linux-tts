/*
 * tts-module - the common part of the speech-dispatcher output modules;
 * see tts-module.h.
 *
 * Threads:
 *  - main: module_loop -> module_process -> module_speak / module_stop /
 *    module_set...  Never blocks on the engine.  module_speak runs with
 *    module_stdout_mutex held (the library prints "200 OK SPEAKING" after
 *    it), so it must not print.
 *  - worker: takes the messages from the queue and speaks them; the only
 *    thread that calls the engine's speak callback and sends 70x events.
 *  - loader: the engine's load and unload callbacks.
 *
 * Events: the worker sends 701 BEGIN, the audio and the marks; the final
 * event (702 / 703 / 704) is sent by whoever comes first: the main thread
 * at STOP or PAUSE of a begun message (so the server hears 703 at once, not
 * after the sentence being synthesized), or the worker at the end.  The
 * check "still running?" and the printing happen under module_stdout_mutex,
 * so nothing of a message follows its final event.
 *
 * Load requests and state: the settings dialog writes
 * $XDG_RUNTIME_DIR/linux-tts/<module>.load (a voice name, or "1") when the
 * user chooses this synthesizer, and the module loads that voice then, not
 * with the first sentence; the file removed means "the user is done with
 * it", and the models go at once (unless IdleTimeout is 0).  The loader
 * writes <module>.state: "loading", "ready", "failed <why>" or "idle", for
 * the dialog to show.  <module> is the name of the configuration file
 * without ".conf".
 * Test loads: see TtsEngine.model_path.  The loader runs the child and
 * waits for it (<Prefix>ProbeTimeout, 300 s; 0 turns the test off); a
 * crash or a timeout is a failed load.
 *
 * Lock order: q_lock and m_lock are never held together; the main thread
 * takes module_stdout_mutex inside q_lock (stop_all) and q_lock inside
 * module_stdout_mutex (module_speak), the worker never nests them.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef HAVE_SONIC
#include <sonic.h>
#endif

#include "tts-config.h"
#include "tts-copy.h"
#include "tts-module.h"
#include "tts-norm.h"
#include "tts-stress.h"

#define E (&tts_engine)
/* how often the main thread looks whether the configuration changed */
#define CONFIG_CHECK_MS 2000

static void take_config (void);
static void probe_child (void) G_GNUC_NORETURN;

/* in the child: the voice to load (LINUX_TTS_PROBE); NULL in the module */
static const gchar *probe_voice = NULL;
/* the configuration file name without ".conf" */
static gchar *module_base = NULL;
/* the running child, for close_module; 0: none */
static volatile pid_t probe_pid = 0;

/* in libspeechd_module, not in its header: prints under module_stdout_mutex */
void module_send (const char *format, ...);

int tts_debug = 0;

/* configuration */
static GHashTable *cfg_lang_voice = NULL;  /* <Prefix>Voice "<lang>" "<voice>" */
static int cfg_idle_timeout = -1;          /* -1: the engine's default */
static int cfg_first_chunk = -1;
static int cfg_max_models = 0;             /* 0: the engine's */
static double cfg_max_rate = 4.0;          /* speed at rate 100 */
static double cfg_model_max_speed = 1.3;   /* what the model does of it: faster
                                              * spoils Supertonic, Kokoro */
static gchar *cfg_preload = NULL;          /* "1", a voice name; NULL: off */
static int cfg_probe_timeout = -1;         /* s; -1: PROBE_TIMEOUT, 0: no test loads */
static TtsNormConfig norm_cfg;
static TtsStressConfig stress_cfg;
static TtsCopyConfig copy_cfg;

static GPtrArray *voices = NULL;           /* TtsVoice */
static SPDVoice **spd_voices = NULL;

/* the settings for the next message (main thread) */
static gchar *set_voice_name = NULL;
static gchar *set_language = NULL;
static const gchar *set_gender = NULL;
static int set_type_index = 0;
static int set_rate = 0, set_pitch = 0, set_volume = 0, set_punctuation = 0;
static gboolean set_spelling = FALSE;

/* ------------------------------------------------------------- messages */

enum { RUN = 0, STOP = 1, PAUSE = 2 };

typedef struct
{
  gchar *data;
  gsize bytes;
  SPDMessageType type;
  TtsSettings s;          /* its strings are owned by the message */
  TtsVoice *voice;
  gint state;             /* RUN, STOP, PAUSE (atomic) */
  /* 701 / the final event sent; guarded by module_stdout_mutex */
  gboolean began, finished;
} Msg;

static GMutex q_lock;
static GCond q_cond;
static GQueue queue = G_QUEUE_INIT;        /* Msg, not started yet */
static Msg *current = NULL;                /* being spoken (worker) */
static gboolean quitting = FALSE;
static gboolean worker_idle = TRUE;
static GThread *worker = NULL;
/* idle unload, q_lock: since when nothing was spoken or asked to load, and
 * whether the models are already unloaded since then */
static gint64 idle_since = 0;
static gboolean idle_done = TRUE;

static TtsNorm *norm = NULL;
static TtsStress *stress = NULL;         /* the stress stage, in the worker */
static TtsCopy *copy = NULL;
static gboolean copy_made = FALSE;

/* ---------------------------------------------------------------- models */

static GMutex m_lock;
static GCond m_cond;
static GQueue loaded = G_QUEUE_INIT;       /* models, most recently used first */
static gpointer wanted = NULL;             /* what the loader should load */
static guint wanted_gen = 0;               /* counts the requests for wanted */
/* the model the message being spoken waits for: goes before wanted, which a
 * .load request or Preload may change meanwhile */
static gpointer speak_wanted = NULL;
static gpointer loading = NULL;            /* what it loads now */
static gpointer in_use = NULL;             /* the worker's; never unloaded */
static gboolean unload_all = FALSE;
static gboolean loader_quit = FALSE;
static GHashTable *failed = NULL;          /* model -> monotonic time of the failure */
static GThread *loader = NULL;

#define RETRY_FAILED (60 * G_TIME_SPAN_SECOND)

/* $XDG_RUNTIME_DIR/linux-tts/<module>.load and .state; NULL without a
 * configuration file */
static gchar *load_path = NULL, *state_path = NULL;

/* tells the dialog what the loader does; any thread */
static void
write_state (const gchar *fmt, ...)
{
  va_list ap;
  gchar *text;

  if (state_path == NULL)
    return;
  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  /* a temporary file and rename: the reader never sees half of it */
  if (!g_file_set_contents (state_path, text, -1, NULL))
    TTS_DBG ("cannot write %s", state_path);
  g_free (text);
}

static gboolean closed = FALSE;

/* ------------------------------------------------------------------ misc */

void
tts_log (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "%s: ", E->name);
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
}

gchar *
tts_unquote (gchar *s)
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

gboolean
tts_parse_bool (const gchar *v)
{
  return g_ascii_strcasecmp (v, "1") == 0 || g_ascii_strcasecmp (v, "yes") == 0
         || g_ascii_strcasecmp (v, "true") == 0 || g_ascii_strcasecmp (v, "on") == 0;
}

int
tts_stderr_off (void)
{
  int saved, null_fd;

  if (tts_debug >= 2 || (null_fd = open ("/dev/null", O_WRONLY | O_CLOEXEC)) < 0)
    return -1;
  fflush (stderr);
  saved = fcntl (STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
  dup2 (null_fd, STDERR_FILENO);
  close (null_fd);
  return saved;
}

void
tts_stderr_restore (int saved)
{
  if (saved < 0)
    return;
  fflush (stderr);
  dup2 (saved, STDERR_FILENO);
  close (saved);
}

static int
idle_timeout (void)
{
  return cfg_idle_timeout >= 0 ? cfg_idle_timeout : E->idle_timeout;
}

int
tts_idle_timeout (void)
{
  return idle_timeout ();
}

gboolean
tts_closing (void)
{
  return closed;
}

static int
first_chunk (void)
{
  return cfg_first_chunk >= 0 ? cfg_first_chunk : E->first_chunk;
}

/* ---------------------------------------------------------- configuration */

static gboolean
has_prefix (const gchar *key, const char *prefix, const gchar *suffix)
{
  gsize n;

  if (prefix == NULL)
    return FALSE;
  n = strlen (prefix);
  return g_ascii_strncasecmp (key, prefix, n) == 0 && g_ascii_strcasecmp (key + n, suffix) == 0;
}

/* <Prefix><suffix> or <AltPrefix><suffix> */
static gboolean
is_key (const gchar *key, const gchar *suffix)
{
  return has_prefix (key, E->prefix, suffix) || has_prefix (key, E->alt_prefix, suffix);
}

static int
max_models (void)
{
  return MAX (cfg_max_models > 0 ? cfg_max_models : E->max_models, 1);
}

/* Takes the lines of CONTENTS: the frame's keys into the globals and C,
 * the engine's through E->config (only with ENGINE: those need a restart) */
typedef struct
{
  GHashTable *lang_voice;
  TtsNormConfig norm;
  TtsStressConfig stress;
  TtsCopyConfig copy;
} Parsed;

static void
reset_scalars (void)
{
  cfg_idle_timeout = -1;
  cfg_first_chunk = -1;
  cfg_max_rate = 4.0;
  cfg_model_max_speed = 1.3;
  cfg_max_models = 0;
  cfg_probe_timeout = -1;
  g_clear_pointer (&cfg_preload, g_free);
}

static void
parse_config (gchar *contents, Parsed *c, gboolean engine)
{
  gchar **lines = g_strsplit (contents, "\n", -1);

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
      value = tts_unquote (value);

      if (tts_norm_config_parse (&c->norm, line, value)
          || tts_stress_config_parse (&c->stress, line, value)
          || tts_copy_config_parse (&c->copy, line, value))
        ;
      else if (is_key (line, "IdleTimeout"))
        cfg_idle_timeout = MAX (atoi (value), 0);
      else if (is_key (line, "FirstChunk"))
        cfg_first_chunk = MAX (atoi (value), 0);
      else if (is_key (line, "MaxRate"))
        cfg_max_rate = CLAMP (g_ascii_strtod (value, NULL), 1.0, 6.0);
      else if (is_key (line, "ModelMaxSpeed"))
        cfg_model_max_speed = CLAMP (g_ascii_strtod (value, NULL), 1.0, 6.0);
      else if (is_key (line, "Preload"))
        {
          g_free (cfg_preload);
          cfg_preload = *value != '\0' && strcmp (value, "0") != 0
                        && g_ascii_strcasecmp (value, "no") != 0
                        && g_ascii_strcasecmp (value, "false") != 0
                        && g_ascii_strcasecmp (value, "off") != 0 ? g_strdup (value) : NULL;
        }
      else if (is_key (line, "ProbeTimeout"))
        cfg_probe_timeout = MAX (atoi (value), 0);
      else if (is_key (line, "MaxModels"))
        cfg_max_models = CLAMP (atoi (value), 1, 8);
      else if (is_key (line, "Voice"))
        {
          /* value was unquoted as one string: ru" "vosk-ru-female-1 */
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
              if (c->lang_voice == NULL)
                c->lang_voice = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
              g_hash_table_replace (c->lang_voice, g_ascii_strdown (lang, -1), g_strdup (name));
            }
          g_strfreev (parts);
        }
      else if (g_ascii_strcasecmp (line, "Debug") == 0)
        tts_debug = atoi (value);
      else if (!engine)
        ;
      else if (E->config != NULL && E->config (line, value))
        ;
      else
        TTS_DBG ("unknown key %s", line);
    }
  g_strfreev (lines);
}

/* the two files read and their state when read, to see them change */
typedef struct
{
  gboolean exists;
  dev_t dev;
  ino_t ino;
  off_t size;
  struct timespec mtime;
} Stamp;

static gchar *conf_given = NULL, *conf_user = NULL, *conf_sys = NULL;
static Stamp stamp_user, stamp_sys;
static Stamp stamp_load;                  /* of load_path */
/* what the worker has (or gets) of the normalizer and copy settings */
static gchar *norm_sig = NULL, *stress_sig = NULL, *copy_sig = NULL;

static gchar *
norm_signature (const TtsNormConfig *c)
{
  return g_strdup_printf ("%d\x1f%s\x1f%s\x1f%d\x1f%d\x1f%d", c->enabled, c->python ? c->python : "",
                          c->helper ? c->helper : "", c->timeout_ms, c->start_ms, c->idle_s);
}

static gchar *
stress_signature (const TtsStressConfig *c)
{
  return g_strdup_printf ("%d\x1f%s\x1f%d\x1f%d", c->enabled, c->dictionary ? c->dictionary : "",
                          c->mark, c->mark_set);
}

static gchar *
copy_signature (const TtsCopyConfig *c)
{
  return g_strdup (c->dir != NULL ? c->dir : "");
}

static Stamp
stamp_of (const gchar *path)
{
  Stamp t = { 0 };
  struct stat st;

  if (path != NULL && stat (path, &st) == 0)
    {
      t.exists = TRUE;
      t.dev = st.st_dev;
      t.ino = st.st_ino;
      t.size = st.st_size;
      t.mtime = st.st_mtim;
    }
  return t;
}

static gboolean
stamp_same (const Stamp *a, const Stamp *b)
{
  return a->exists == b->exists && a->dev == b->dev && a->ino == b->ino && a->size == b->size
         && a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec;
}

int
module_config (const char *configfile)
{
  gchar *contents = NULL;
  Parsed c = { NULL };

  tts_norm_config_init (&c.norm);
  tts_stress_config_init (&c.stress, TRUE, RU_STRESS_NONE);
  tts_copy_config_init (&c.copy);
  if (E->setup != NULL)
    E->setup ();
  probe_voice = g_getenv ("LINUX_TTS_PROBE");
  if (configfile != NULL)
    {
      gchar *base = g_path_get_basename (configfile);
      gchar *dir = g_build_filename (g_get_user_runtime_dir (), "linux-tts", NULL);

      conf_given = g_strdup (configfile);
      tts_config_paths (configfile, &conf_user, &conf_sys);
      stamp_user = stamp_of (conf_user);
      stamp_sys = stamp_of (conf_sys);
      if (g_str_has_suffix (base, ".conf"))
        base[strlen (base) - 5] = '\0';
      module_base = g_strdup (base);
      if (probe_voice == NULL && *base != '\0' && g_mkdir_with_parents (dir, 0700) == 0)
        {
          gchar *name = g_strconcat (base, ".load", NULL);

          load_path = g_build_filename (dir, name, NULL);
          g_free (name);
          name = g_strconcat (base, ".state", NULL);
          state_path = g_build_filename (dir, name, NULL);
          g_free (name);
          /* a request left from before this module started is old news */
          stamp_load = stamp_of (load_path);
          write_state ("idle\n");
        }
      g_free (dir);
      g_free (base);
    }
  if (tts_config_read (configfile, &contents))
    parse_config (contents, &c, TRUE);
  g_free (contents);
  cfg_lang_voice = c.lang_voice;
  norm_cfg = c.norm;
  stress_cfg = c.stress;
  copy_cfg = c.copy;
  norm_sig = norm_signature (&norm_cfg);
  stress_sig = stress_signature (&stress_cfg);
  copy_sig = copy_signature (&copy_cfg);
  if (probe_voice != NULL)
    probe_child ();
  return 0;
}

/* ----------------------------------------------------------------- voices */

/* a copy of @text fit for a LIST VOICES line and for the dialog's
 * configuration: valid UTF-8, no control character, tab or quote (names and
 * languages come from the directory names of downloaded models) */
static gchar *
voice_field (const gchar *text, const gchar *what)
{
  gchar *r;
  gboolean changed = FALSE;

  if (text == NULL)
    return NULL;
  r = g_utf8_make_valid (text, -1);
  for (gchar *c = r; *c != '\0'; c++)
    if ((guchar) *c < 0x20 || *c == 0x7f || *c == '"')
      {
        *c = '_';
        changed = TRUE;
      }
  if (changed || strcmp (r, text) != 0)
    TTS_ERR ("a voice %s with characters it cannot have, using \"%s\"", what, r);
  return r;
}

TtsVoice *
tts_voice_add (const gchar *name, const gchar *language, const gchar *gender,
               gpointer model, gpointer data)
{
  TtsVoice *v = g_new0 (TtsVoice, 1);

  if (voices == NULL)
    voices = g_ptr_array_new ();
  v->name = voice_field (name, "name");
  v->language = voice_field (language, "language");
  v->gender = voice_field (gender, "gender");
  v->model = model;
  v->data = data;
  g_ptr_array_add (voices, v);
  return v;
}

GPtrArray *
tts_voices (void)
{
  if (voices == NULL)
    voices = g_ptr_array_new ();
  return voices;
}

static TtsVoice *
voice_by_name (const gchar *name)
{
  for (guint i = 0; name != NULL && i < voices->len; i++)
    {
      TtsVoice *v = g_ptr_array_index (voices, i);
      if (g_ascii_strcasecmp (v->name, name) == 0)
        return v;
    }
  return NULL;
}

/* the synthesis voice, else the language's voice from the configuration,
 * else among the voices of the language (else of English, else all) of the
 * first model the n-th one of the voice type's gender, else the n-th one */
static TtsVoice *
pick_voice (const TtsSettings *s)
{
  gchar *lang = NULL;
  TtsVoice *found = voice_by_name (s->voice_name);

  if (s->language != NULL)
    lang = g_ascii_strdown (s->language, strcspn (s->language, "-_"));

  if (found == NULL && cfg_lang_voice != NULL && lang != NULL)
    found = voice_by_name (g_hash_table_lookup (cfg_lang_voice, lang));

  if (found == NULL)
    {
      const gchar *langs[] = { lang, "en" };
      GPtrArray *cand = g_ptr_array_new ();
      GPtrArray *same = g_ptr_array_new ();
      gpointer first;
      guint n = 0;

      for (guint l = 0; l < 2 && cand->len == 0; l++)
        for (guint i = 0; i < voices->len && langs[l] != NULL; i++)
          {
            TtsVoice *v = g_ptr_array_index (voices, i);
            if (g_ascii_strcasecmp (v->language, langs[l]) == 0)
              g_ptr_array_add (cand, v);
          }
      if (cand->len == 0)
        for (guint i = 0; i < voices->len; i++)
          g_ptr_array_add (cand, g_ptr_array_index (voices, i));
      first = ((TtsVoice *) g_ptr_array_index (cand, 0))->model;
      for (guint i = 0; i < cand->len; i++)
        {
          TtsVoice *v = g_ptr_array_index (cand, i);
          if (v->model == first && s->gender != NULL && g_strcmp0 (v->gender, s->gender) == 0)
            g_ptr_array_add (same, v);
        }
      while (n < cand->len && ((TtsVoice *) g_ptr_array_index (cand, n))->model == first)
        n++;
      if (same->len > 0)
        found = g_ptr_array_index (same, s->type_index % same->len);
      else
        found = g_ptr_array_index (cand, s->type_index % n);
      g_ptr_array_free (same, TRUE);
      g_ptr_array_free (cand, TRUE);
    }
  g_free (lang);
  return found;
}

/* the voice a message without settings gets: the first voice of the
 * configuration that exists, else the default one */
static TtsVoice *
default_voice (void)
{
  TtsSettings none = { 0 };

  if (cfg_lang_voice != NULL)
    {
      GHashTableIter it;
      gpointer value;

      g_hash_table_iter_init (&it, cfg_lang_voice);
      while (g_hash_table_iter_next (&it, NULL, &value))
        if (voice_by_name (value) != NULL)
          return voice_by_name (value);
    }
  return pick_voice (&none);
}

/* ---------------------------------------------------------------- loader */

static gboolean
failed_recently_locked (gpointer m)
{
  gpointer t;

  if (failed == NULL || !g_hash_table_lookup_extended (failed, m, NULL, &t))
    return FALSE;
  return g_get_monotonic_time () - *(gint64 *) t < RETRY_FAILED;
}

static gboolean
needs_load_locked (gpointer m)
{
  return m != NULL && g_queue_find (&loaded, m) == NULL && !failed_recently_locked (m);
}

/* ------------------------------------------------------------ test loads */

#define PROBE_TIMEOUT 300


/* the child: init and load, then _exit (no destructors: an engine that
 * crashes at exit would look like a broken model).  0 loaded, 2 the load
 * failed normally, 3 no such voice */
static void
probe_child (void)
{
  gchar *msg = NULL;
  GError *error = NULL;
  TtsVoice *v;

  if (E->init != NULL && !E->init (&msg))
    _exit (3);
  v = voice_by_name (probe_voice);
  if (v == NULL || v->model == NULL || E->load == NULL)
    _exit (3);
  if (!E->load (v->model, &error))
    {
      fprintf (stderr, "%s: test load of %s: %s\n", E->name, probe_voice,
               error != NULL ? error->message : "?");
      _exit (2);
    }
  _exit (0);
}

static int
cmp_str (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const gchar *const *) a, *(const gchar *const *) b);
}

#define STAMP_DEPTH 4        /* directory levels looked into */
#define STAMP_ENTRIES 20000  /* entries at most */

static void
stamp_add (GPtrArray *lines, const gchar *name, const struct stat *st)
{
  g_ptr_array_add (lines, g_strdup_printf ("%s %lld %lld.%09ld", name, (long long) st->st_size,
                                           (long long) st->st_mtim.tv_sec,
                                           st->st_mtim.tv_nsec));
}

/* the entries of directory PATH, named REL/..., and of its subdirectories */
static void
stamp_dir (GPtrArray *lines, const gchar *path, const gchar *rel, int depth)
{
  GDir *dir = g_dir_open (path, 0, NULL);
  const gchar *name;

  if (dir == NULL)
    return;
  while ((name = g_dir_read_name (dir)) != NULL && lines->len < STAMP_ENTRIES)
    {
      gchar *f = g_build_filename (path, name, NULL);
      gchar *r = g_build_filename (rel, name, NULL);
      struct stat st;

      if (stat (f, &st) == 0)
        {
          stamp_add (lines, r, &st);
          if (S_ISDIR (st.st_mode) && depth > 1)
            stamp_dir (lines, f, r, depth - 1);
        }
      g_free (f);
      g_free (r);
    }
  g_dir_close (dir);
}

/* what the engine looks like: this program and the libraries it has mapped
 * for running (sherpa-onnx, onnxruntime, ...) when first asked, and the
 * engine's own probe_salt (its provider, a plugin): a verdict is kept for
 * one build of the engine.  Once per process. */
static const gchar *
engine_stamp (void)
{
  static gchar *result = NULL;
  GPtrArray *lines;
  GHashTable *seen;
  GChecksum *sum;
  gchar *maps = NULL;
  struct stat st;

  if (result != NULL)
    return result;
  lines = g_ptr_array_new_with_free_func (g_free);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (stat ("/proc/self/exe", &st) == 0)
    stamp_add (lines, "exe", &st);
  if (g_file_get_contents ("/proc/self/maps", &maps, NULL, NULL))
    {
      gchar **rows = g_strsplit (maps, "\n", -1);

      for (gchar **r = rows; *r != NULL; r++)
        {
          /* addresses perms offset dev inode path */
          gchar **f = g_strsplit_set (*r, " ", 6);
          const gchar *path = g_strv_length (f) == 6 ? g_strchug (f[5]) : NULL;

          if (path != NULL && path[0] == '/' && strchr (f[1], 'x') != NULL
              && !g_hash_table_contains (seen, path) && stat (path, &st) == 0)
            {
              g_hash_table_add (seen, g_strdup (path));
              stamp_add (lines, path, &st);
            }
          g_strfreev (f);
        }
      g_strfreev (rows);
      g_free (maps);
    }
  if (E->probe_salt != NULL)
    {
      gchar *salt = E->probe_salt ();
      g_ptr_array_add (lines, g_strconcat ("salt ", salt != NULL ? salt : "", NULL));
      g_free (salt);
    }
  g_ptr_array_sort (lines, cmp_str);
  sum = g_checksum_new (G_CHECKSUM_SHA256);
  for (guint i = 0; i < lines->len; i++)
    {
      g_checksum_update (sum, g_ptr_array_index (lines, i), -1);
      g_checksum_update (sum, (const guchar *) "\n", 1);
    }
  result = g_strdup (g_checksum_get_string (sum));
  g_checksum_free (sum);
  g_hash_table_destroy (seen);
  g_ptr_array_free (lines, TRUE);
  return result;
}

/* what the files of PATH look like (a file, or a directory and what is in
 * it, a few levels down) and the engine that loads them; NULL if PATH does
 * not exist */
static gchar *
model_stamp (const gchar *path)
{
  GChecksum *sum;
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  struct stat st;
  gchar *result;

  if (stat (path, &st) != 0)
    {
      g_ptr_array_free (lines, TRUE);
      return NULL;
    }
  stamp_add (lines, ".", &st);
  if (S_ISDIR (st.st_mode))
    stamp_dir (lines, path, "", STAMP_DEPTH);
  g_ptr_array_sort (lines, cmp_str);
  g_ptr_array_add (lines, g_strconcat ("engine ", engine_stamp (), NULL));
  sum = g_checksum_new (G_CHECKSUM_SHA256);
  for (guint i = 0; i < lines->len; i++)
    {
      g_checksum_update (sum, g_ptr_array_index (lines, i), -1);
      g_checksum_update (sum, (const guchar *) "\n", 1);
    }
  result = g_strdup (g_checksum_get_string (sum));
  g_checksum_free (sum);
  g_ptr_array_free (lines, TRUE);
  return result;
}

static gchar *
probe_cache_path (void)
{
  gchar *name = g_strdup_printf ("probe-%s.ini", module_base);
  gchar *path = g_build_filename (g_get_user_cache_dir (), "linux-tts", name, NULL);

  g_free (name);
  return path;
}

static void
probe_cache_put (const gchar *path, const gchar *stamp, const gchar *result)
{
  gchar *file = probe_cache_path ();
  gchar *dir = g_path_get_dirname (file);
  GKeyFile *kf = g_key_file_new ();

  g_key_file_load_from_file (kf, file, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_key_file_set_string (kf, path, "stamp", stamp);
  g_key_file_set_string (kf, path, "result", result);
  if (g_mkdir_with_parents (dir, 0700) == 0)
    g_key_file_save_to_file (kf, file, NULL);
  g_key_file_free (kf);
  g_free (dir);
  g_free (file);
}

static void
probe_setup (gpointer data)
{
  /* the child goes with the loader thread that waits for it */
  prctl (PR_SET_PDEATHSIG, SIGKILL);
}

/* signals that come from the engine itself, not from outside */
static gboolean
crash_signal (int sig)
{
  return sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS || sig == SIGILL || sig == SIGFPE
         || sig == SIGTRAP || sig == SIGSYS;
}

/* in the loader, before the first load of M: FALSE (with *error) if the
 * test load in a child crashed or hung */
static gboolean
probe_model (gpointer m, GError **error)
{
  int timeout = cfg_probe_timeout >= 0 ? cfg_probe_timeout : PROBE_TIMEOUT;
  const gchar *path, *voice = NULL;
  gchar *stamp, *file, *cached = NULL, *cached_stamp = NULL, *result = NULL;
  gchar **envp, *argv[3];
  GKeyFile *kf;
  GPid pid;
  GError *e = NULL;
  gint64 t0, end;
  int status = 0;
  gboolean ok = TRUE, done = FALSE, got_status = FALSE;

  if (timeout == 0 || E->model_path == NULL || conf_given == NULL || module_base == NULL
      || (path = E->model_path (m)) == NULL || (stamp = model_stamp (path)) == NULL)
    return TRUE;

  file = probe_cache_path ();
  kf = g_key_file_new ();
  if (g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, NULL))
    {
      cached_stamp = g_key_file_get_string (kf, path, "stamp", NULL);
      cached = g_key_file_get_string (kf, path, "result", NULL);
    }
  g_key_file_free (kf);
  g_free (file);
  if (cached != NULL && g_strcmp0 (cached_stamp, stamp) == 0)
    {
      ok = strcmp (cached, "ok") == 0;
      if (!ok)
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s (%s; remembered "
                     "until its files change)", cached, path);
      goto out;
    }

  g_mutex_lock (&m_lock);
  for (guint i = 0; voices != NULL && i < voices->len; i++)
    if (((TtsVoice *) g_ptr_array_index (voices, i))->model == m)
      {
        voice = ((TtsVoice *) g_ptr_array_index (voices, i))->name;
        break;
      }
  g_mutex_unlock (&m_lock);
  if (voice == NULL)
    goto out;

  argv[0] = (gchar *) "/proc/self/exe";
  argv[1] = conf_given;
  argv[2] = NULL;
  envp = g_environ_setenv (g_get_environ (), "LINUX_TTS_PROBE", voice, TRUE);
  /* stdin and stdout /dev/null: the child must not talk to the server */
  if (!g_spawn_async (NULL, argv, envp, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                      probe_setup, NULL, &pid, &e))
    {
      TTS_ERR ("no test load of %s: %s", voice, e->message);
      g_clear_error (&e);
      g_strfreev (envp);
      goto out;
    }
  g_strfreev (envp);
  probe_pid = pid;
  TTS_DBG ("test load of %s (%s) in process %d", voice, path, (int) pid);
  t0 = g_get_monotonic_time ();
  end = t0 + (gint64) timeout * G_TIME_SPAN_SECOND;
  while (!done)
    {
      pid_t r = waitpid (pid, &status, WNOHANG);

      if (r == pid)
        done = got_status = TRUE;
      else if (r < 0 && errno != EINTR)
        done = TRUE;   /* no status: no verdict, the loader tries itself */
      else if (g_get_monotonic_time () >= end)
        {
          kill (pid, SIGKILL);
          waitpid (pid, &status, 0);
          probe_pid = 0;
          TTS_ERR ("test load of %s did not finish in %d s", voice, timeout);
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "the test load of %s did not finish in %d s", voice, timeout);
          ok = FALSE;
          goto out;      /* not remembered: the machine may just be busy */
        }
      else
        g_usleep (100 * 1000);
    }
  probe_pid = 0;
  TTS_DBG ("test load of %s: status %d in %.1f s", voice, status,
           (g_get_monotonic_time () - t0) / 1e6);
  if (!got_status)
    TTS_ERR ("test load of %s: no exit status (%s)", voice, g_strerror (errno));
  else if (WIFSIGNALED (status) && crash_signal (WTERMSIG (status)))
    result = g_strdup_printf ("the model crashed the engine (signal %d, %s)", WTERMSIG (status),
                              g_strsignal (WTERMSIG (status)));
  else if (WIFSIGNALED (status))
    {
      /* killed from outside (the OOM killer, the end of the session): says
       * nothing about the model, so not remembered; tried again later */
      TTS_ERR ("test load of %s killed by signal %d", voice, WTERMSIG (status));
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "the test load of %s was killed (signal %d, %s)", voice, WTERMSIG (status),
                   g_strsignal (WTERMSIG (status)));
      ok = FALSE;
    }
  else if (WEXITSTATUS (status) == 0)
    result = g_strdup ("ok");
  else if (WEXITSTATUS (status) != 2 && WEXITSTATUS (status) != 3)
    /* the engine gave up and exited itself (sherpa-onnx: _Exit(-1) on bad
     * model metadata): it would take the module with it */
    result = g_strdup_printf ("the model made the engine exit (code %d)", WEXITSTATUS (status));
  /* exit 2 or 3: the load fails normally here too; nothing to remember */
  if (result != NULL)
    {
      probe_cache_put (path, stamp, result);
      ok = strcmp (result, "ok") == 0;
      if (!ok)
        {
          TTS_ERR ("test load of %s: %s", voice, result);
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s", result);
        }
    }
out:
  g_free (result);
  g_free (cached);
  g_free (cached_stamp);
  g_free (stamp);
  return ok;
}

static gpointer
loader_main (gpointer data)
{
  g_mutex_lock (&m_lock);
  for (;;)
    {
      gpointer m, old = NULL;
      GError *error = NULL;
      gint64 t0;
      gboolean ok;

      while (!loader_quit && !unload_all && !needs_load_locked (speak_wanted)
             && !needs_load_locked (wanted))
        g_cond_wait (&m_cond, &m_lock);
      if (loader_quit)
        break;

      if (unload_all)
        {
          GList *keep = NULL;
          guint gen = wanted_gen;

          unload_all = FALSE;
          while (!g_queue_is_empty (&loaded))
            {
              gpointer x = g_queue_pop_head (&loaded);
              if (x == in_use)
                {
                  keep = g_list_append (keep, x);
                  continue;
                }
              g_mutex_unlock (&m_lock);
              E->unload (x);
              g_mutex_lock (&m_lock);
            }
          for (GList *k = keep; k != NULL; k = k->next)
            g_queue_push_tail (&loaded, k->data);
          g_list_free (keep);
          /* the old request would load its model again at once; one that
           * came while the models were unloaded stays */
          if (wanted != in_use && wanted_gen == gen)
            wanted = NULL;
          g_mutex_unlock (&m_lock);
          malloc_trim (0);
          write_state ("idle\n");
          TTS_DBG ("models unloaded after %d s without speech", idle_timeout ());
          g_mutex_lock (&m_lock);
          continue;
        }

      m = needs_load_locked (speak_wanted) ? speak_wanted : wanted;
      /* make room: the least recently used model that is not in use */
      if ((int) g_queue_get_length (&loaded) >= max_models ())
        for (GList *l = g_queue_peek_tail_link (&loaded); l != NULL; l = l->prev)
          if (l->data != in_use)
            {
              old = l->data;
              g_queue_delete_link (&loaded, l);
              break;
            }
      loading = m;
      g_mutex_unlock (&m_lock);

      if (old != NULL)
        {
          E->unload (old);
          malloc_trim (0);
        }
      write_state ("loading\n");
      t0 = g_get_monotonic_time ();
      ok = probe_model (m, &error) && E->load (m, &error);
      if (ok)
        {
          TTS_DBG ("model loaded in %.1f s", (g_get_monotonic_time () - t0) / 1e6);
          write_state ("ready\n");
        }
      else
        {
          TTS_ERR ("cannot load the model: %s", error != NULL ? error->message : "?");
          write_state ("failed %s\n", error != NULL ? error->message : "?");
          g_clear_error (&error);
        }
      /* the idle time counts from the end of a load too: a model loaded
       * for a request nobody speaks with goes after IdleTimeout, even if
       * the request was withdrawn while it loaded */
      g_mutex_lock (&q_lock);
      idle_since = g_get_monotonic_time ();
      idle_done = FALSE;
      g_cond_broadcast (&q_cond);
      g_mutex_unlock (&q_lock);

      g_mutex_lock (&m_lock);
      loading = NULL;
      if (ok)
        {
          g_queue_push_head (&loaded, m);
          if (failed != NULL)
            g_hash_table_remove (failed, m);
        }
      else
        {
          gint64 *t = g_new (gint64, 1);
          *t = g_get_monotonic_time ();
          if (failed == NULL)
            failed = g_hash_table_new_full (NULL, NULL, NULL, g_free);
          g_hash_table_replace (failed, m, t);
        }
      g_cond_broadcast (&m_cond);
    }
  g_mutex_unlock (&m_lock);
  return NULL;
}

static void
wake_loader_locked (void)
{
  if (loader == NULL)
    loader = g_thread_new ("tts-loader", loader_main, NULL);
  g_cond_broadcast (&m_cond);
}

static void
request_model (gpointer m)
{
  if (m == NULL || E->load == NULL)
    return;
  g_mutex_lock (&m_lock);
  wanted = m;
  wanted_gen++;
  wake_loader_locked ();
  g_mutex_unlock (&m_lock);
}

/* worker: waits until m is loaded; FALSE if it failed or the message was
 * stopped meanwhile */
static gboolean
wait_model (gpointer m)
{
  gboolean ok;

  if (m == NULL || E->load == NULL)
    return TRUE;
  g_mutex_lock (&m_lock);
  in_use = m;
  wanted = m;          /* replaces an older request, which would push it out */
  speak_wanted = m;    /* and survives a newer one until it is loaded */
  wake_loader_locked ();
  while (needs_load_locked (m) && !tts_stopped ())
    {
      gint64 until = g_get_monotonic_time () + 50 * G_TIME_SPAN_MILLISECOND;
      g_cond_wait_until (&m_cond, &m_lock, until);
    }
  speak_wanted = NULL;
  ok = g_queue_find (&loaded, m) != NULL;
  if (ok && g_queue_peek_head (&loaded) != m)
    {
      g_queue_remove (&loaded, m);
      g_queue_push_head (&loaded, m);
    }
  g_mutex_unlock (&m_lock);
  return ok;
}

static void
release_model (void)
{
  g_mutex_lock (&m_lock);
  in_use = NULL;
  g_mutex_unlock (&m_lock);
}

static gboolean
model_failed (gpointer m)
{
  gboolean f;

  if (m == NULL || E->load == NULL)
    return FALSE;
  g_mutex_lock (&m_lock);
  f = failed_recently_locked (m);
  g_mutex_unlock (&m_lock);
  return f;
}

/* ------------------------------------------------------------------ text */

static gchar *
attr_value (const char *tag, const char *end, const char *attr)
{
  gsize n = strlen (attr);

  for (const char *p = tag; p + n + 2 < end; p++)
    if ((p == tag || isspace ((guchar) p[-1])) && strncmp (p, attr, n) == 0)
      {
        const char *q = p + n;
        char quote;
        const char *close;

        while (q < end && isspace ((guchar) *q))
          q++;
        if (q >= end || *q != '=')
          continue;
        q++;
        while (q < end && isspace ((guchar) *q))
          q++;
        if (q >= end || (*q != '"' && *q != '\''))
          continue;
        quote = *q++;
        close = memchr (q, quote, end - q);
        if (close == NULL)
          return NULL;
        return g_strndup (q, close - q);
      }
  return NULL;
}

#define MARK_MAX 256

static gboolean
mark_ok (const gchar *name)
{
  if (strlen (name) > MARK_MAX || !g_utf8_validate (name, -1, NULL))
    return FALSE;
  for (const gchar *c = name; *c != '\0'; c++)
    if ((guchar) *c < 0x20 || *c == 0x7f)
      return FALSE;
  return TRUE;
}

static void
segment_add (GArray *out, GString *text, gchar *mark)
{
  TtsSegment s;

  s.text = g_strdup (g_strstrip (text->str));
  s.mark = mark;
  g_array_append_val (out, s);
  g_string_truncate (text, 0);
}

GArray *
tts_ssml_parse (const char *data, gsize bytes)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (TtsSegment));
  GString *text = g_string_sized_new (bytes);
  const char *p = data, *end = data + bytes;

  while (p < end && *p != '\0')
    {
      if (*p == '<')
        {
          const char *close = memchr (p, '>', end - p);
          if (close == NULL)
            break;
          if (strncmp (p, "<mark", 5) == 0 && (isspace ((guchar) p[5]) || p[5] == '/'))
            {
              gchar *name = attr_value (p + 5, close, "name");
              if (name != NULL && !mark_ok (name))
                {
                  /* "700-<name>" is one protocol line: a newline in the
                   * name could fake "702 END" to the server */
                  TTS_ERR ("a mark name that cannot be sent, dropped");
                  g_clear_pointer (&name, g_free);
                }
              if (name != NULL)
                segment_add (out, text, name);
            }
          else
            g_string_append_c (text, ' ');
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
                g_string_append_c (text, ents[i].ch);
                p += strlen (ents[i].ent);
                done = TRUE;
                break;
              }
          if (!done)
            g_string_append_c (text, *p++);
        }
      else
        g_string_append_c (text, *p++);
    }
  segment_add (out, text, NULL);
  g_string_free (text, TRUE);
  return out;
}

void
tts_segments_free (GArray *segments)
{
  for (guint i = 0; i < segments->len; i++)
    {
      TtsSegment *s = &g_array_index (segments, TtsSegment, i);
      g_free (s->text);
      g_free (s->mark);
    }
  g_array_free (segments, TRUE);
}

GPtrArray *
tts_split_sentences (const gchar *text, int first)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);

  gchar *valid = NULL;

  /* the text from the server is UTF-8; anything else is split as well,
   * not given to the engine as one piece */
  if (!g_utf8_validate (text, -1, NULL))
    text = valid = g_utf8_make_valid (text, -1);
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
                      > (out->len == 0 && first > 0 ? first : 200))
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
  g_free (valid);
  return out;
}

glong
tts_cut_point (const gchar *text, glong limit)
{
  glong space = -1, mark = -1, i = 0;

  for (const gchar *p = text; *p != '\0' && i < limit; p = g_utf8_next_char (p), i++)
    if (*p == ' ')
      {
        space = i;
        if (i > 0)
          {
            gunichar prev = g_utf8_get_char (g_utf8_prev_char (p));
            if (prev == ',' || prev == ':' || prev == ';' || prev == 0x2014)
              mark = i;
          }
      }
  if (mark > limit / 2)
    return mark;
  return space > 0 ? space : MAX (limit, 1);
}

/* ----------------------------------------------------------------- audio */

gboolean
tts_stopped (void)
{
  Msg *m = current;

  return m != NULL && g_atomic_int_get (&m->state) != RUN;
}

/* 705: the samples HDLC-escaped (newline and 0x7d prefixed with 0x7d and
 * their bit 5 inverted), as libspeechd_module does; that function also
 * reads the server's commands, which only the main thread may do */
static gboolean
send_chunk (const gint16 *s, gsize n, int sample_rate)
{
  const unsigned char *p = (const unsigned char *) s, *end = p + n * 2;

  pthread_mutex_lock (&module_stdout_mutex);
  if (current == NULL || g_atomic_int_get (&current->state) != RUN)
    {
      pthread_mutex_unlock (&module_stdout_mutex);
      return FALSE;
    }
  printf ("705-bits=16\n705-num_channels=1\n705-sample_rate=%d\n705-num_samples=%zu\n"
          "705-big_endian=%d\n705-AUDIO", sample_rate, n, G_BYTE_ORDER == G_BIG_ENDIAN);
  putc (0, stdout);
  for (; p < end; p++)
    if (*p == '\n' || *p == 0x7d)
      {
        putc (0x7d, stdout);
        putc (*p ^ 0x20, stdout);
      }
    else
      putc (*p, stdout);
  fputs ("\n705 AUDIO\n", stdout);
  fflush (stdout);
  pthread_mutex_unlock (&module_stdout_mutex);
  return TRUE;
}

/* worker: an event of the current message unless it was stopped */
static void
send_mark (const gchar *mark)
{
  pthread_mutex_lock (&module_stdout_mutex);
  if (g_atomic_int_get (&current->state) == RUN)
    {
      printf ("700-%s\n700 INDEX MARK\n", mark);
      fflush (stdout);
    }
  pthread_mutex_unlock (&module_stdout_mutex);
}

/* the final event of m, once; the caller holds module_stdout_mutex */
static void
finish_locked (Msg *m)
{
  if (m->finished)
    return;
  m->finished = TRUE;
  switch (g_atomic_int_get (&m->state))
    {
    case STOP:
      fputs ("703 STOP\n", stdout);
      break;
    case PAUSE:
      fputs ("704 PAUSE\n", stdout);
      break;
    default:
      fputs ("702 END\n", stdout);
    }
  fflush (stdout);
}

/* rate -100..0 -> 0.5..1 times, 0..100 -> 1..MaxRate times; a screen reader
 * user listens at 3-4 times */
static double
total_speed (int rate)
{
  return rate <= 0 ? pow (2.0, rate / 100.0) : pow (cfg_max_rate, rate / 100.0);
}

double
tts_model_speed (const TtsSettings *s)
{
  double v = total_speed (s->rate);

#ifdef HAVE_SONIC
  return MIN (v, cfg_model_max_speed);
#else
  return v;
#endif
}

#ifdef HAVE_SONIC
/* the audio factor times faster at the same pitch (sonic, made for speech) */
static gint16 *
speed_up (const gint16 *in, gsize n, int sample_rate, double factor, gsize *out_n)
{
  sonicStream st = sonicCreateStream (sample_rate, 1);
  gsize cap = (gsize) (n / factor) + 4096, len = 0;
  gint16 *out;
  int got;

  if (st == NULL)
    return NULL;
  sonicSetSpeed (st, (float) factor);
  for (gsize i = 0; i < n; i += 1 << 20)
    sonicWriteShortToStream (st, (short *) in + i, (int) MIN (n - i, (gsize) 1 << 20));
  sonicFlushStream (st);
  out = g_new (gint16, cap);
  while ((got = sonicReadShortFromStream (st, out + len, (int) MIN (cap - len, (gsize) G_MAXINT))) > 0)
    {
      len += got;
      if (len == cap)
        out = g_renew (gint16, out, cap *= 2);
    }
  sonicDestroyStream (st);
  *out_n = len;
  return out;
}
#endif

/* volume above 0: up to 2 times louder; the peaks are compressed softly
 * instead of clipped, the models make them near full scale already */
static gint16
louder (gint16 x, double gain)
{
  double v = x * gain / 32768.0, a = fabs (v);

  if (a > 0.6)
    a = 0.6 + 0.4 * tanh ((a - 0.6) / 0.4);
  return (gint16) CLAMP ((v < 0 ? -a : a) * 32767.0, -32768.0, 32767.0);
}

/* the speed the model synthesized the current piece at, if not
 * tts_model_speed (): tts_audio_float_at (); 0 when unset */
static double piece_model_speed = 0;

gboolean
tts_audio_s16 (const gint16 *samples, gsize n, int sample_rate)
{
  /* small enough to stop quickly */
  const gsize chunk = 5000;
  int volume = current != NULL ? current->s.volume : 0;
  gint16 *scaled = NULL;

#ifdef HAVE_SONIC
  if (current != NULL && n > 0)
    {
      double done = piece_model_speed > 0 ? piece_model_speed : tts_model_speed (&current->s);
      double factor = total_speed (current->s.rate) / done;
      gsize m;
      if (factor > 1.01 && (scaled = speed_up (samples, n, sample_rate, factor, &m)) != NULL)
        samples = scaled, n = m;
    }
#endif
  if (volume != 0)
    {
      /* below 0 quieter, down to silence */
      double gain = volume < 0 ? CLAMP ((volume + 100) / 100.0, 0.0, 1.0) : 1.0 + volume / 100.0;
      gint16 *v = scaled != NULL ? scaled : g_new (gint16, n);
      for (gsize i = 0; i < n; i++)
        v[i] = volume < 0 ? (gint16) (samples[i] * gain) : louder (samples[i], gain);
      samples = scaled = v;
    }
  for (gsize i = 0; i < n; i += chunk)
    {
      gsize k = MIN (chunk, n - i);
      if (!send_chunk (samples + i, k, sample_rate))
        {
          g_free (scaled);
          return FALSE;
        }
      tts_copy_audio (copy, samples + i, k, sample_rate, 1);
    }
  g_free (scaled);
  return !tts_stopped ();
}

gboolean
tts_audio_float (const float *f, gsize n, int sample_rate)
{
  gint16 *s;
  gboolean ok;

  if (n == 0)
    return !tts_stopped ();
  s = g_new (gint16, n);
  for (gsize i = 0; i < n; i++)
    s[i] = (gint16) (CLAMP (f[i], -1.0f, 1.0f) * 32767.0f);
  ok = tts_audio_s16 (s, n, sample_rate);
  g_free (s);
  return ok;
}

gboolean
tts_audio_float_at (const float *f, gsize n, int sample_rate, double model_speed)
{
  gboolean ok;

  piece_model_speed = model_speed;
  ok = tts_audio_float (f, n, sample_rate);
  piece_model_speed = 0;
  return ok;
}

/* ---------------------------------------------------------------- worker */

static gboolean
norm_cancel (void)
{
  return tts_stopped ();
}

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

static void
speak_one (Msg *m, const gchar *text, gboolean *first)
{
  TtsPiece piece = { text, m->voice, &m->s, m->type, *first };
  GError *error = NULL;
  gint64 t0 = g_get_monotonic_time ();

  tts_copy_text (copy, text);
  if (!E->speak (&piece, &error))
    {
      TTS_ERR ("synthesis failed: %s: %s", error != NULL ? error->message : "?", text);
      g_clear_error (&error);
    }
  else
    TTS_DBG ("%.2f s: %s", (g_get_monotonic_time () - t0) / 1e6, text);
  *first = FALSE;
}

static gboolean
longer_than (const gchar *text, glong n)
{
  for (; *text != '\0'; text = g_utf8_next_char (text))
    if (n-- == 0)
      return TRUE;
  return FALSE;
}

/* a text without sentence marks (a table, a log, base64) would go to the
 * engine in one call: memory and time grow with it, and STOP waits for the
 * call to return */
static void
speak_piece (Msg *m, const gchar *text, gboolean *first)
{
  glong max = E->max_piece > 0 ? E->max_piece : TTS_MAX_PIECE;
  gchar *rest = NULL;

  while (longer_than (text, max) && !tts_stopped ())
    {
      const gchar *cut = g_utf8_offset_to_pointer (text, tts_cut_point (text, max));
      gchar *part = g_strstrip (g_strndup (text, cut - text));
      gchar *next = g_strstrip (g_strdup (cut));

      if (*part != '\0')
        speak_one (m, part, first);
      g_free (part);
      g_free (rest);
      text = rest = next;
    }
  if (*text != '\0' && !tts_stopped ())
    speak_one (m, text, first);
  g_free (rest);
}

/* the stages after the normalizer, as the voice's profile asks: a new
 * string */
static gchar *
profile_text (const TtsTextProfile *prof, const gchar *text, const gchar *lang)
{
  gchar *s, *low;

  if (prof->stress && stress_cfg.enabled)
    {
      if (stress == NULL)
        stress = tts_stress_new (&stress_cfg);
      s = tts_stress_apply_mark (stress, text, lang, prof->mark);
    }
  else
    s = g_strdup (text);
  if (!prof->lowercase)
    return s;
  low = g_utf8_strdown (s, -1);
  g_free (s);
  return low;
}

static void
speak_text (Msg *m)
{
  GArray *segs = tts_ssml_parse (m->data, m->bytes);
  const gchar *lang = E->text_language != NULL ? E->text_language (&m->s, m->voice)
                                              : m->voice->language;
  gboolean first = TRUE;
  TtsTextProfile prof = { TRUE, FALSE, RU_STRESS_NONE, FALSE };

  /* the helper starts while the model loads */
  tts_norm_start (get_norm ());
  if (!wait_model (m->voice->model))
    {
      tts_segments_free (segs);
      return;
    }
  if (E->text_profile != NULL)
    E->text_profile (m->voice, &prof);
  for (guint i = 0; i < segs->len && !tts_stopped (); i++)
    {
      TtsSegment *seg = &g_array_index (segs, TtsSegment, i);
      gchar *text = g_strdup (seg->text);
      gboolean whole = *text == '\0' || !prof.normalize
                       || tts_norm_message (norm, &text, lang);
      GPtrArray *pieces = tts_split_sentences (text, first ? first_chunk () : 0);

      for (guint j = 0; j < pieces->len && !tts_stopped (); j++)
        {
          const gchar *piece = g_ptr_array_index (pieces, j);
          gchar *normed = whole ? NULL : tts_norm_text (norm, piece, lang);
          gchar *ready = profile_text (&prof, normed != NULL ? normed : piece, lang);

          if (!tts_stopped ())
            speak_piece (m, ready, &first);
          g_free (ready);
          g_free (normed);
        }
      g_ptr_array_free (pieces, TRUE);
      g_free (text);
      if (seg->mark != NULL && !tts_stopped ())
        send_mark (seg->mark);
    }
  tts_segments_free (segs);
}

static void
speak_message (Msg *m)
{
  pthread_mutex_lock (&module_stdout_mutex);
  fputs ("701 BEGIN\n", stdout);
  fflush (stdout);
  m->began = TRUE;
  pthread_mutex_unlock (&module_stdout_mutex);
  take_config ();
  if (!copy_made)
    copy = tts_copy_new (&copy_cfg), copy_made = TRUE;

  if (g_atomic_int_get (&m->state) == RUN)
    {
      TTS_DBG ("speaking with %s: %.*s", m->voice->name, (int) MIN (m->bytes, 200), m->data);
      if (m->type == SPD_MSGTYPE_CHAR || m->type == SPD_MSGTYPE_KEY)
        {
          const gchar *lang = E->text_language != NULL
                              ? E->text_language (&m->s, m->voice) : m->voice->language;
          gchar *text = tts_key_text (m->data, m->type, lang);
          gboolean first = TRUE;
          if (wait_model (m->voice->model))
            speak_piece (m, text, &first);
          g_free (text);
        }
      else if (m->type != SPD_MSGTYPE_SOUND_ICON)
        speak_text (m);
      release_model ();
    }
  tts_copy_end (copy);

  pthread_mutex_lock (&module_stdout_mutex);
  finish_locked (m);
  pthread_mutex_unlock (&module_stdout_mutex);
}

static void
msg_free (Msg *m)
{
  g_free (m->data);
  g_free ((gchar *) m->s.voice_name);
  g_free ((gchar *) m->s.language);
  g_free (m);
}

static gpointer
worker_main (gpointer data)
{
  g_mutex_lock (&q_lock);
  for (;;)
    {
      Msg *m;

      /* idle: unload when the time is up; the time and the idle timeout
       * may change meanwhile (a load request, a new configuration), and
       * whoever changes them wakes us up to look again */
      while (g_queue_is_empty (&queue) && !quitting)
        {
          int idle = idle_timeout ();
          gint64 until = idle_since + (gint64) idle * G_TIME_SPAN_SECOND;

          if (idle <= 0 || idle_done)
            g_cond_wait (&q_cond, &q_lock);
          else if (g_get_monotonic_time () < until)
            g_cond_wait_until (&q_cond, &q_lock, until);
          else
            {
              idle_done = TRUE;
              g_mutex_unlock (&q_lock);
              tts_norm_stop (norm);
              if (E->load != NULL)
                {
                  g_mutex_lock (&m_lock);
                  unload_all = TRUE;
                  g_cond_broadcast (&m_cond);
                  g_mutex_unlock (&m_lock);
                }
              g_mutex_lock (&q_lock);
            }
        }
      if (g_queue_is_empty (&queue))
        break;
      m = g_queue_pop_head (&queue);
      current = m;
      worker_idle = FALSE;
      g_mutex_unlock (&q_lock);

      speak_message (m);

      g_mutex_lock (&q_lock);
      current = NULL;
      worker_idle = g_queue_is_empty (&queue);
      g_cond_broadcast (&q_cond);
      g_mutex_unlock (&q_lock);
      msg_free (m);
      g_mutex_lock (&q_lock);
      idle_since = g_get_monotonic_time ();
      idle_done = FALSE;
    }
  worker_idle = TRUE;
  g_cond_broadcast (&q_cond);
  g_mutex_unlock (&q_lock);
  return NULL;
}

/* marks the current and the queued messages; PAUSE does not override STOP */
static void
stop_all (int how)
{
  g_mutex_lock (&q_lock);
  if (current != NULL)
    {
      if (how == STOP)
        g_atomic_int_set (&current->state, STOP);
      else
        g_atomic_int_compare_and_exchange (&current->state, RUN, how);
      pthread_mutex_lock (&module_stdout_mutex);
      if (current->began)
        finish_locked (current);
      pthread_mutex_unlock (&module_stdout_mutex);
    }
  for (GList *l = queue.head; l != NULL; l = l->next)
    {
      Msg *m = l->data;
      if (how == STOP)
        g_atomic_int_set (&m->state, STOP);
      else
        g_atomic_int_compare_and_exchange (&m->state, RUN, how);
    }
  g_mutex_unlock (&q_lock);
}

/* ------------------------------------------------------- module interface */

int
module_init (char **msg)
{
  gchar *emsg = NULL;

  /* glibc gives every thread that allocates its own 64 MB arena (up to 8 per
   * core), and the inference threads leave hundreds of MB in them that
   * malloc_trim does not get back after the model is unloaded.  Two are
   * enough: the engines allocate big blocks, mostly from one thread. */
  if (g_getenv ("MALLOC_ARENA_MAX") == NULL)
    mallopt (M_ARENA_MAX, 2);

  if (E->init != NULL && !E->init (&emsg))
    {
      *msg = emsg != NULL ? emsg : g_strdup ("cannot initialize");
      return -1;
    }
  g_free (emsg);
  if (tts_voices ()->len == 0)
    {
      *msg = g_strdup ("no voices");
      return -1;
    }
  spd_voices = g_new0 (SPDVoice *, voices->len + 1);
  for (guint i = 0; i < voices->len; i++)
    {
      TtsVoice *v = g_ptr_array_index (voices, i);
      spd_voices[i] = g_new0 (SPDVoice, 1);
      spd_voices[i]->name = v->name;
      spd_voices[i]->language = v->language;
      spd_voices[i]->variant = v->gender;
    }
  module_audio_set_server ();
  *msg = g_strdup_printf ("%s ready, %u voices", E->name, voices->len);
  return 0;
}

SPDVoice **
module_list_voices (void)
{
  return spd_voices;
}

static gchar *
dup_value (const char *val)
{
  return (val != NULL && strcmp (val, "NULL") != 0 && *val != '\0') ? g_strdup (val) : NULL;
}

int
module_set (const char *var, const char *val)
{
  if (val == NULL)
    val = "";
  if (strcmp (var, "synthesis_voice") == 0)
    {
      g_free (set_voice_name);
      set_voice_name = dup_value (val);
    }
  else if (strcmp (var, "language") == 0)
    {
      g_free (set_language);
      set_language = dup_value (val);
    }
  else if (strcmp (var, "voice") == 0 || strcmp (var, "voice_type") == 0)
    {
      /* male1..3, female1..3, child_male, child_female */
      const gchar *t = val;
      set_gender = NULL;
      set_type_index = 0;
      if (g_ascii_strncasecmp (t, "child_", 6) == 0)
        t += 6;
      if (g_ascii_strncasecmp (t, "female", 6) == 0)
        set_gender = "female", t += 6;
      else if (g_ascii_strncasecmp (t, "male", 4) == 0)
        set_gender = "male", t += 4;
      if (g_ascii_isdigit (*t) && atoi (t) > 0)
        set_type_index = MIN (atoi (t), 100) - 1;
    }
  else if (strcmp (var, "rate") == 0)
    set_rate = CLAMP (atoi (val), -100, 100);
  else if (strcmp (var, "pitch") == 0)
    set_pitch = CLAMP (atoi (val), -100, 100);
  else if (strcmp (var, "volume") == 0)
    set_volume = CLAMP (atoi (val), -100, 100);
  else if (strcmp (var, "punctuation_mode") == 0)
    {
      static const char *const modes[] = { "none", "some", "all", "most" };
      set_punctuation = 0;
      for (int i = 0; i < 4; i++)
        if (strcmp (val, modes[i]) == 0)
          set_punctuation = i;
      if (g_ascii_isdigit (*val))
        set_punctuation = CLAMP (atoi (val), 0, 3);
    }
  else if (strcmp (var, "spelling_mode") == 0)
    set_spelling = strcmp (val, "on") == 0 || strcmp (val, "1") == 0;
  return 0;
}

int
module_audio_set (const char *var, const char *val)
{
  if (strcmp (var, "audio_output_method") == 0)
    return val != NULL && strcmp (val, "server") == 0 ? 0 : -1;
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
  tts_debug = enable;
  return 0;
}

/* main thread, with module_stdout_mutex held: queue only, never print */
/* ------------------------------------------------- configuration changes */

/* the worker's idle clock starts again at @since (0: long ago, so the
 * models go now); main thread */
static void
idle_restart (gint64 since)
{
  g_mutex_lock (&q_lock);
  idle_since = since;
  idle_done = FALSE;
  if (worker == NULL && !quitting)
    worker = g_thread_new ("tts-worker", worker_main, NULL);
  g_cond_broadcast (&q_cond);
  g_mutex_unlock (&q_lock);
}

/* loads @name ("1", or the voice whose model to load); @why for the log */
static void
load_voice (const gchar *name,
            const gchar *why)
{
  TtsVoice *v;

  if (name == NULL || voices == NULL || voices->len == 0 || E->load == NULL)
    return;
  v = voice_by_name (name);
  if (v == NULL && !tts_parse_bool (name))
    TTS_ERR ("%s: no voice %s, loading the default one", why, name);
  if (v == NULL)
    v = default_voice ();
  if (v->model == NULL)
    return;
  /* not "loaded forever": the idle time counts from now */
  idle_restart (g_get_monotonic_time ());
  request_model (v->model);
}

/* <Prefix>Preload 1, or the voice whose model to load */
static void
preload (void)
{
  gchar *why = g_strdup_printf ("%sPreload", E->prefix);

  load_voice (cfg_preload, why);
  g_free (why);
}

/* the dialog asks to load a voice, or (the file gone) is done with us */
static void
load_request_check (void)
{
  Stamp st = stamp_of (load_path);
  gchar *contents = NULL;

  if (load_path == NULL || stamp_same (&st, &stamp_load))
    return;
  stamp_load = st;
  if (!st.exists)
    {
      TTS_DBG ("load request withdrawn");
      idle_restart (0);
      return;
    }
  if (g_file_get_contents (load_path, &contents, NULL, NULL))
    {
      g_strstrip (contents);
      TTS_DBG ("load request: %s", contents);
      load_voice (*contents != '\0' ? contents : "1", load_path);
    }
  g_free (contents);
}

/* the worker owns the normalizer and the copy: it takes their new settings
 * before its next message (under q_lock) */
static TtsNormConfig *pending_norm = NULL;
static TtsStressConfig *pending_stress = NULL;
static TtsCopyConfig *pending_copy = NULL;

static void
take_config (void)
{
  TtsNormConfig *n;
  TtsStressConfig *st;
  TtsCopyConfig *c;

  g_mutex_lock (&q_lock);
  n = pending_norm;
  st = pending_stress;
  c = pending_copy;
  pending_norm = NULL;
  pending_stress = NULL;
  pending_copy = NULL;
  g_mutex_unlock (&q_lock);
  if (st != NULL)
    {
      tts_stress_free (stress);
      stress = NULL;
      tts_stress_config_clear (&stress_cfg);
      stress_cfg = *st;
      g_free (st);
    }
  if (n != NULL)
    {
      tts_norm_free (norm);
      norm = NULL;
      tts_norm_config_clear (&norm_cfg);
      norm_cfg = *n;
      g_free (n);
    }
  if (c != NULL)
    {
      tts_copy_free (copy);
      copy = NULL;
      copy_made = FALSE;
      tts_copy_config_clear (&copy_cfg);
      copy_cfg = *c;
      g_free (c);
    }
}

/* The settings dialog changes the user's file and no longer restarts the
 * server for it: the frame's own keys (voices per language, Preload, idle
 * time, rate scale, normalizer, copy) are read again when a file changes.
 * The engine's keys still need a restart.  Main thread. */
static void
config_check (void)
{
  Stamp u, sy;
  Parsed c = { NULL };
  gchar *contents = NULL, *old_preload, *sig;

  if (conf_given == NULL)
    return;
  u = stamp_of (conf_user);
  sy = stamp_of (conf_sys);
  if (stamp_same (&u, &stamp_user) && stamp_same (&sy, &stamp_sys))
    return;
  stamp_user = u;
  stamp_sys = sy;
  TTS_DBG ("configuration changed, reading it again");

  tts_norm_config_init (&c.norm);
  tts_stress_config_init (&c.stress, TRUE, RU_STRESS_NONE);
  tts_copy_config_init (&c.copy);
  old_preload = g_strdup (cfg_preload);
  reset_scalars ();
  if (tts_config_read (conf_given, &contents))
    parse_config (contents, &c, FALSE);
  g_free (contents);
  if (cfg_lang_voice != NULL)
    g_hash_table_unref (cfg_lang_voice);
  cfg_lang_voice = c.lang_voice;

  g_mutex_lock (&q_lock);
  sig = norm_signature (&c.norm);
  if (strcmp (sig, norm_sig) != 0)
    {
      if (pending_norm != NULL)
        tts_norm_config_clear (pending_norm);
      g_free (pending_norm);
      pending_norm = g_memdup2 (&c.norm, sizeof c.norm);
      g_free (norm_sig);
      norm_sig = sig;
    }
  else
    {
      tts_norm_config_clear (&c.norm);
      g_free (sig);
    }
  sig = stress_signature (&c.stress);
  if (strcmp (sig, stress_sig) != 0)
    {
      if (pending_stress != NULL)
        tts_stress_config_clear (pending_stress);
      g_free (pending_stress);
      pending_stress = g_memdup2 (&c.stress, sizeof c.stress);
      g_free (stress_sig);
      stress_sig = sig;
    }
  else
    {
      tts_stress_config_clear (&c.stress);
      g_free (sig);
    }
  sig = copy_signature (&c.copy);
  if (strcmp (sig, copy_sig) != 0)
    {
      if (pending_copy != NULL)
        tts_copy_config_clear (pending_copy);
      g_free (pending_copy);
      pending_copy = g_memdup2 (&c.copy, sizeof c.copy);
      g_free (copy_sig);
      copy_sig = sig;
    }
  else
    {
      tts_copy_config_clear (&c.copy);
      g_free (sig);
    }
  g_mutex_unlock (&q_lock);

  if (cfg_preload != NULL && g_strcmp0 (old_preload, cfg_preload) != 0)
    preload ();
  g_free (old_preload);

  /* the idle timeout may have changed: the worker looks again */
  g_mutex_lock (&q_lock);
  g_cond_broadcast (&q_cond);
  g_mutex_unlock (&q_lock);
}

int
module_speak (char *data, size_t bytes, SPDMessageType msgtype)
{
  Msg *m;

  config_check ();

  if (closed || voices == NULL || voices->len == 0)
    return 0;
  m = g_new0 (Msg, 1);
  m->data = g_strndup (data, bytes);
  m->bytes = strlen (m->data);
  m->type = msgtype;
  m->s.voice_name = g_strdup (set_voice_name);
  m->s.language = g_strdup (set_language);
  m->s.gender = set_gender;
  m->s.type_index = set_type_index;
  m->s.rate = set_rate;
  m->s.pitch = set_pitch;
  m->s.volume = set_volume;
  m->s.punctuation = set_punctuation;
  m->s.spelling = set_spelling;
  m->voice = pick_voice (&m->s);
  if (model_failed (m->voice->model))
    {
      /* do not pretend: the server tells the client at once */
      msg_free (m);
      return 0;
    }

  g_mutex_lock (&q_lock);
  g_queue_push_tail (&queue, m);
  worker_idle = FALSE;
  if (worker == NULL)
    worker = g_thread_new ("tts-worker", worker_main, NULL);
  g_cond_broadcast (&q_cond);
  g_mutex_unlock (&q_lock);
  return 1;
}

int
module_stop (void)
{
  stop_all (STOP);
  return 0;
}

size_t
module_pause (void)
{
  stop_all (PAUSE);
  return 0;
}

/* quit without waiting for the engine (a model load cannot be interrupted;
 * speech-dispatcher waits for the reply to QUIT meanwhile) */
static void
exit_now (gboolean reply)
{
  TTS_DBG ("quitting while the engine is busy");
  if (probe_pid > 0)
    kill (probe_pid, SIGKILL);
  if (reply)
    module_send ("210 OK QUIT\n");
  fflush (stdout);
  _exit (0);
}

static void
close_module (gboolean reply)
{
  gint64 until = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  gboolean busy;

  if (closed)
    return;
  closed = TRUE;
  stop_all (STOP);

  g_mutex_lock (&q_lock);
  quitting = TRUE;
  g_cond_broadcast (&q_cond);
  while (!worker_idle)
    if (!g_cond_wait_until (&q_cond, &q_lock, until))
      break;
  busy = !worker_idle;
  g_mutex_unlock (&q_lock);
  if (busy)
    exit_now (reply);
  if (worker != NULL)
    g_thread_join (worker);
  worker = NULL;

  g_mutex_lock (&m_lock);
  busy = loading != NULL;
  loader_quit = TRUE;
  g_cond_broadcast (&m_cond);
  g_mutex_unlock (&m_lock);
  if (busy)
    exit_now (reply);
  if (loader != NULL)
    g_thread_join (loader);
  loader = NULL;
  while (!g_queue_is_empty (&loaded))
    E->unload (g_queue_pop_head (&loaded));

  if (E->close != NULL)
    E->close ();
  tts_norm_free (norm);
  norm = NULL;
  tts_stress_free (stress);
  stress = NULL;
  tts_copy_free (copy);
  copy = NULL;
}

int
module_close (void)
{
  /* called by the library for QUIT, which then prints 210 itself */
  close_module (TRUE);
  return 0;
}

int
module_loop (void)
{
  preload ();

  for (;;)
    {
      struct pollfd fd = { STDIN_FILENO, POLLIN, 0 };
      /* wakes up to see the configuration change: three stat() calls */
      int ret = poll (&fd, 1, CONFIG_CHECK_MS);

      if (ret == 0)
        {
          config_check ();
          load_request_check ();
          continue;
        }

      if (ret < 0)
        {
          if (errno == EINTR)
            continue;
          close_module (FALSE);
          return -1;
        }
      if ((fd.revents & POLLIN) && module_process (STDIN_FILENO, 0) == 0)
        return 0;       /* QUIT: module_close ran and 210 was sent */
      if (fd.revents & (POLLHUP | POLLERR | POLLNVAL))
        {
          /* the server is gone */
          close_module (FALSE);
          return -1;
        }
    }
}
