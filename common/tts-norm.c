/*
 * tts-norm - text normalization for the speech-dispatcher modules
 *
 * One helper per user serves all modules: it listens on
 * $XDG_RUNTIME_DIR/tts-normalize-<hash of python and helper>.sock, and the
 * first module that finds nobody there starts it (detached; it exits by
 * itself when no module has been connected for TextNormalizationIdleTimeout
 * seconds).  It talks lines over that socket (a socket, not a pipe: a write
 * to a dead helper must not kill the module with SIGPIPE):
 *   helper -> module   "READY" once loaded, or "ERROR<TAB>message" and exit
 *   module -> helper   "<id><TAB><lang><TAB><text>"
 *   helper -> module   "<id><TAB><text>"
 * The text has no tabs or line breaks; the module replaces them with
 * spaces.  An answer with another id is the late answer to a request that
 * timed out, and is skipped.
 *
 * The c engine (rn) needs none of this: it runs in the module, and a text
 * it cannot handle comes back NULL and passes through unnormalized.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tts-norm.h"
#include "user-stress.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef HAVE_RN
#include "rn.h"
#endif

#ifndef TTS_NORM_HELPER
#define TTS_NORM_HELPER "/usr/libexec/speech-dispatcher-modules/tts-normalize"
#endif
#ifndef TTS_NORM_DATA
#define TTS_NORM_DATA "/usr/share/speech-dispatcher-modules/textnorm"
#endif

/* after a second failure in a row, a helper that did not start, ... */
#define RETRY_AFTER_US (60 * G_TIME_SPAN_SECOND)
/* after ERROR from the helper (ru_normalizr missing or broken): each try
 * costs a Python start and seconds of CPU */
#define RETRY_BROKEN_US (10 * 60 * G_TIME_SPAN_SECOND)
#define MAX_TEXT 16384          /* bytes per request; longer passes through */
/* the answer may take timeout_ms plus this per byte (ru_normalizr does
 * ~100 bytes/ms; a long sentence must not look like a hung helper) */
#define US_PER_BYTE 100

struct TtsNorm
{
  TtsNormConfig cfg;
  gboolean active;              /* connecting or connected */
  int fd;                       /* -1 until connected */
  gboolean spawned;             /* we started the helper for this connection */
  gboolean ready;
  gint64 started;               /* monotonic, when we began to connect */
  gint64 retry_after;           /* monotonic; 0: may start */
  guint64 next_id;
  GString *in;                  /* bytes read, not yet a whole line */
  TtsNormCancel cancel;
  gchar *sock;                  /* the helper's socket */
  guint requests, timeouts;     /* for the log: how often the text went unnormalized */
  guint strikes;                /* requests failed in a row */
  int c_state;                  /* the c engine: 0 not tried yet, 1 in use, -1 not available */
};

void
tts_norm_set_cancel (TtsNorm *n, TtsNormCancel cancel)
{
  if (n != NULL)
    n->cancel = cancel;
}

void
tts_norm_config_init (TtsNormConfig *c)
{
  c->enabled = TRUE;
  c->python_only = FALSE;
  c->data = g_strdup (TTS_NORM_DATA);
  c->python = g_strdup ("python3");
  c->helper = g_strdup (TTS_NORM_HELPER);
  c->timeout_ms = 1500;
  c->start_ms = 8000;
  c->idle_s = 60;
}

void
tts_norm_config_clear (TtsNormConfig *c)
{
  g_clear_pointer (&c->python, g_free);
  g_clear_pointer (&c->helper, g_free);
  g_clear_pointer (&c->data, g_free);
}

static gboolean
parse_bool (const gchar *v)
{
  return g_ascii_strcasecmp (v, "1") == 0 || g_ascii_strcasecmp (v, "yes") == 0
         || g_ascii_strcasecmp (v, "true") == 0 || g_ascii_strcasecmp (v, "on") == 0;
}

gboolean
tts_norm_config_parse (TtsNormConfig *c, const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "TextNormalization") == 0)
    c->enabled = parse_bool (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationEngine") == 0)
    c->python_only = g_ascii_strcasecmp (value, "python") == 0;
  else if (g_ascii_strcasecmp (key, "TextNormalizationData") == 0 && *value != '\0')
    g_free (c->data), c->data = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationPython") == 0 && *value != '\0')
    g_free (c->python), c->python = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationHelper") == 0 && *value != '\0')
    g_free (c->helper), c->helper = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationTimeout") == 0)
    c->timeout_ms = CLAMP (atoi (value), 50, 60000);
  else if (g_ascii_strcasecmp (key, "TextNormalizationStartTimeout") == 0)
    c->start_ms = CLAMP (atoi (value), 100, 120000);
  else if (g_ascii_strcasecmp (key, "TextNormalizationIdleTimeout") == 0)
    c->idle_s = CLAMP (atoi (value), 1, 86400);
  else
    return FALSE;
  return TRUE;
}

TtsNorm *
tts_norm_new (const TtsNormConfig *c)
{
  TtsNorm *n;

  if (c == NULL || !c->enabled)
    return NULL;
  n = g_new0 (TtsNorm, 1);
  n->cfg = *c;
  n->cfg.python = g_strdup (c->python);
  n->cfg.helper = g_strdup (c->helper);
  n->cfg.data = g_strdup (c->data);
  n->fd = -1;
  n->in = g_string_new (NULL);
  {
    /* modules configured with another helper get their own */
    gchar *key = g_strconcat (c->python, "\n", c->helper, NULL);
    gchar *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
    hash[12] = '\0';
    n->sock = g_strdup_printf ("%s/tts-normalize-%s.sock", g_get_user_runtime_dir (), hash);
    g_free (hash);
    g_free (key);
  }
  return n;
}

void
tts_norm_free (TtsNorm *n)
{
  if (n == NULL)
    return;
  tts_norm_stop (n);
  tts_norm_config_clear (&n->cfg);
  g_string_free (n->in, TRUE);
  g_free (n->sock);
  g_free (n);
}

void
tts_norm_stop (TtsNorm *n)
{
  if (n == NULL || !n->active)
    return;
  /* the helper stays for the other modules and exits when idle */
  if (n->fd >= 0)
    close (n->fd);
  n->fd = -1;
  n->active = FALSE;
  n->ready = FALSE;
  g_string_truncate (n->in, 0);
}



/* Let go of the helper.  A first failure (a late answer, a helper that
 * went away) only costs this text: the next one tries again.  A second
 * one in a row, or a helper that cannot start (PAUSE_US), and the text
 * goes unnormalized for a while. */
static void
fail_for (TtsNorm *n, const gchar *why, gint64 pause_us)
{
  n->timeouts++;
  n->strikes++;
  tts_norm_stop (n);
  if (pause_us == 0 && n->strikes >= 2)
    pause_us = RETRY_AFTER_US;
  if (pause_us == 0)
    g_printerr ("tts-norm: %s; this text goes unnormalized (failure %u; %u requests so far)\n",
                why, n->timeouts, n->requests);
  else
    g_printerr ("tts-norm: %s; text goes to the engine unnormalized for %d s"
                " (failure %u; %u requests so far)\n",
                why, (int) (pause_us / G_TIME_SPAN_SECOND), n->timeouts, n->requests);
  if (pause_us != 0)
    n->retry_after = g_get_monotonic_time () + pause_us;
}

static void
fail (TtsNorm *n, const gchar *why)
{
  fail_for (n, why, RETRY_AFTER_US);
}

static void
fail_once (TtsNorm *n, const gchar *why)
{
  fail_for (n, why, 0);
}

static int
connect_helper (TtsNorm *n)
{
  struct sockaddr_un addr = { 0 };
  int fd;

  addr.sun_family = AF_UNIX;
  if (g_strlcpy (addr.sun_path, n->sock, sizeof (addr.sun_path)) >= sizeof (addr.sun_path))
    return -1;
  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) != 0)
    {
      close (fd);
      return -1;
    }
  return fd;
}



/* starts the helper, detached: it outlives this module and serves the others */
static gboolean
spawn_helper (TtsNorm *n)
{
  GError *error = NULL;
  gchar idle[16];
  gchar *argv[] = { n->cfg.python, "-I", n->cfg.helper, "--socket", n->sock, "--idle", idle, NULL };

  g_snprintf (idle, sizeof (idle), "%d", n->cfg.idle_s);
  /* without G_SPAWN_DO_NOT_REAP_CHILD glib forks twice: init reaps it */
  /* not this module's stderr: it serves the other modules too, and what
   * it has to say comes as ERROR over the socket */
  if (!g_spawn_async (NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL
                      | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, &error))
    {
      fail (n, error->message);
      g_error_free (error);
      return FALSE;
    }
  return TRUE;
}



/* the c engine: loads rn once per process (all TtsNorm share it); FALSE
 * leaves the python engine */
static gboolean
c_start (TtsNorm *n)
{
#ifdef HAVE_RN
  static GMutex lock;
  static int opened;            /* 0 not tried, 1 open, -1 failed (for the data it was given) */
  static gchar *opened_data;

  if (n->c_state != 0)
    return n->c_state > 0;
  n->c_state = -1;
  if (n->cfg.python_only)
    return FALSE;
  g_mutex_lock (&lock);
  if (opened == 0 || (opened < 0 && g_strcmp0 (opened_data, n->cfg.data) != 0))
    {
      gchar *morph = g_build_filename (n->cfg.data, "cmorphy3-ru.bin", NULL);
      gchar *ipa = g_build_filename (n->cfg.data, "en-ipa.tsv", NULL);
      char err[512];

      if (!g_file_test (morph, G_FILE_TEST_EXISTS) || !g_file_test (ipa, G_FILE_TEST_EXISTS))
        {
          g_printerr ("tts-norm: no rn data in %s; using ru_normalizr (python)\n", n->cfg.data);
          opened = -1;
        }
      else if (rn_open (morph, ipa, RN_FIXES_ALL, RN_N2W_FIXES_ALL, err, sizeof err) < 0)
        {
          g_printerr ("tts-norm: rn: %s; using ru_normalizr (python)\n", err);
          opened = -1;
        }
      else
        opened = 1;
      g_free (opened_data);
      opened_data = g_strdup (n->cfg.data);
      g_free (morph);
      g_free (ipa);
    }
  if (opened > 0)
    n->c_state = 1;
  g_mutex_unlock (&lock);
  return n->c_state > 0;
#else
  n->c_state = -1;
  return FALSE;
#endif
}

void
tts_norm_start (TtsNorm *n)
{
  if (n == NULL || n->active || c_start (n))
    return;
  if (n->retry_after != 0 && g_get_monotonic_time () < n->retry_after)
    return;
  n->retry_after = 0;
  if (!g_file_test (n->cfg.helper, G_FILE_TEST_EXISTS))
    {
      n->retry_after = g_get_monotonic_time () + RETRY_AFTER_US;
      return;
    }
  if (strlen (n->sock) >= sizeof (((struct sockaddr_un *) 0)->sun_path))
    {
      fail (n, "the path of $XDG_RUNTIME_DIR is too long for a socket");
      return;
    }
  n->fd = connect_helper (n);
  n->spawned = FALSE;
  if (n->fd < 0)
    {
      if (!spawn_helper (n))
        return;
      n->spawned = TRUE;
    }
  n->active = TRUE;
  n->ready = FALSE;
  n->started = g_get_monotonic_time ();
  g_string_truncate (n->in, 0);
}



/* until the helper we started listens; FALSE at the deadline or when the
 * message is stopped (*cancelled) */
static gboolean
wait_connected (TtsNorm *n, gint64 deadline, gboolean *cancelled)
{
  *cancelled = FALSE;
  while (n->fd < 0)
    {
      n->fd = connect_helper (n);
      if (n->fd >= 0)
        break;
      if (g_get_monotonic_time () >= deadline)
        return FALSE;
      if (n->cancel != NULL && n->cancel ())
        {
          *cancelled = TRUE;
          return FALSE;
        }
      g_usleep (50000);
    }
  return TRUE;
}



/* One line without the "\n", or NULL at the deadline, when the message
 * is stopped (*cancelled) or when the helper is gone (*gone). */
static gchar *
read_line (TtsNorm *n, gint64 deadline, gboolean *gone, gboolean *cancelled)
{
  *gone = FALSE;
  *cancelled = FALSE;
  for (;;)
    {
      gchar *nl = memchr (n->in->str, '\n', n->in->len);
      if (nl != NULL)
        {
          gchar *line = g_strndup (n->in->str, nl - n->in->str);
          g_string_erase (n->in, 0, nl - n->in->str + 1);
          return line;
        }
      if (n->in->len > 4 * MAX_TEXT)
        {
          *gone = TRUE;         /* no line end in sight: not our helper */
          return NULL;
        }

      gint64 left = deadline - g_get_monotonic_time ();
      if (left <= 0)
        return NULL;
      struct pollfd p = { n->fd, POLLIN, 0 };
      int r = poll (&p, 1, (int) MIN ((left + 999) / 1000, 100));
      if (r < 0 && errno == EINTR)
        continue;
      if (r == 0)
        {
          if (n->cancel != NULL && n->cancel ())
            {
              *cancelled = TRUE;
              return NULL;
            }
          continue;
        }
      if (r < 0)
        {
          *gone = TRUE;
          return NULL;
        }
      gchar buf[8192];
      ssize_t got = recv (n->fd, buf, sizeof buf, MSG_DONTWAIT);
      if (got < 0 && (errno == EAGAIN || errno == EINTR))
        continue;
      if (got <= 0)
        {
          *gone = TRUE;
          return NULL;
        }
      g_string_append_len (n->in, buf, got);
    }
}

typedef enum { WAIT_OK, WAIT_FAILED, WAIT_GONE } WaitResult;

/* waits for READY up to the start timeout; a stopped message leaves the
 * helper loading for the next one.  WAIT_GONE: a helper we did not start
 * went away as we connected (it was exiting, idle or updated). */
static WaitResult
wait_ready (TtsNorm *n)
{
  gint64 deadline = n->started + (gint64) n->cfg.start_ms * 1000;
  gboolean gone, cancelled;

  if (!wait_connected (n, deadline, &cancelled))
    {
      if (!cancelled)
        fail (n, "the normalizer helper did not start in time");
      return WAIT_FAILED;
    }
  while (!n->ready)
    {
      gchar *line = read_line (n, deadline, &gone, &cancelled);
      if (line == NULL)
        {
          if (gone && !n->spawned)
            {
              tts_norm_stop (n);
              return WAIT_GONE;
            }
          if (gone)
            fail (n, "the normalizer helper exited while loading");
          else if (!cancelled)
            fail (n, "the normalizer helper did not load in time");
          return WAIT_FAILED;
        }
      if (g_str_equal (line, "READY"))
        n->ready = TRUE;
      else if (g_str_has_prefix (line, "ERROR\t"))
        {
          gchar *why = g_strdup_printf ("normalizer: %s", line + 6);
          fail_for (n, why, RETRY_BROKEN_US);
          g_free (why);
          g_free (line);
          return WAIT_FAILED;
        }
      g_free (line);
    }
  return WAIT_OK;
}

static gboolean
is_russian (const gchar *lang)
{
  return lang != NULL && g_ascii_strncasecmp (lang, "ru", 2) == 0
         && (lang[2] == '\0' || lang[2] == '-' || lang[2] == '_');
}

/* the text as one request line: valid UTF-8, no control characters */
static gchar *
clean_text (const gchar *text)
{
  gchar *s = g_utf8_make_valid (text, -1);

  for (gchar *p = s; *p != '\0'; p++)
    if ((guchar) *p < 0x20 || *p == 0x7f)
      *p = ' ';
  return s;
}

static gboolean
send_all (int fd, const gchar *buf, gsize len)
{
  while (len > 0)
    {
      ssize_t w = send (fd, buf, len, MSG_NOSIGNAL);
      if (w < 0 && errno == EINTR)
        continue;
      if (w <= 0)
        return FALSE;
      buf += w;
      len -= w;
    }
  return TRUE;
}

/* the normalized text, or NULL: *gone when the helper went away (the
 * caller may try once more), else the failure is dealt with here */
static gchar *
ask (TtsNorm *n, const gchar *text, const gchar *lang, gboolean *gone_out)
{
  gchar *clean = clean_text (text);
  guint64 id = ++n->next_id;
  gchar *req = g_strdup_printf ("%" G_GUINT64_FORMAT "\t%s\t%s\n", id, lang, clean);
  gchar *prefix = g_strdup_printf ("%" G_GUINT64_FORMAT "\t", id);
  gint64 deadline = g_get_monotonic_time () + (gint64) n->cfg.timeout_ms * 1000
                    + (gint64) strlen (clean) * US_PER_BYTE;
  gchar *result = NULL;
  gboolean gone = FALSE, cancelled = FALSE;

  n->requests++;
  if (!send_all (n->fd, req, strlen (req)))
    gone = TRUE;
  while (!gone && !cancelled && result == NULL)
    {
      gchar *line = read_line (n, deadline, &gone, &cancelled);
      if (line == NULL)
        break;
      if (g_str_has_prefix (line, prefix))
        result = g_strdup (line + strlen (prefix));
      g_free (line);
    }
  /* a stopped message: the late answer is skipped by its id */
  *gone_out = gone && !cancelled;
  if (result == NULL)
    {
      if (!cancelled && !gone)
        fail_once (n, "the normalizer did not answer in time");
    }
  else if (!g_utf8_validate (result, -1, NULL)
           || (*result == '\0' && *g_strstrip (clean) != '\0'))
    {
      /* lost text is worse than unnormalized text */
      g_free (result);
      result = NULL;
    }
  g_free (prefix);
  g_free (req);
  g_free (clean);
  return result;
}

static gchar *
norm_plain (TtsNorm *n, const gchar *text, const gchar *lang)
{
  gchar *out = NULL;

  if (n == NULL || text == NULL || !is_russian (lang) || strlen (text) > MAX_TEXT)
    return g_strdup (text);
  for (const gchar *p = text; ; p++)
    {
      if (*p == '\0')
        return g_strdup (text);       /* blank */
      if (!g_ascii_isspace (*p))
        break;
    }
#ifdef HAVE_RN
  if (c_start (n))
    {
      gchar *clean = clean_text (text);

      n->requests++;
      out = rn_normalize (clean, strlen (clean));
      if (out != NULL && *out == '\0' && *g_strstrip (clean) != '\0')
        g_clear_pointer (&out, free);   /* lost text is worse than unnormalized text */
      g_free (clean);
      if (out == NULL)
        {
          n->timeouts++;
          g_printerr ("tts-norm: rn could not normalize a text; it goes unnormalized"
                      " (failure %u; %u requests so far)\n", n->timeouts, n->requests);
          return g_strdup (text);
        }
      for (char *p = out; *p != '\0'; p++)
        if (*p == '\t' || *p == '\r' || *p == '\n')
          *p = ' ';
      {
        gchar *res = g_strdup (out);    /* g_free-able for the caller */
        free (out);
        return res;
      }
    }
#endif
  /* a helper that went away (crashed, exiting when idle, updated) is
   * started again at once, once */
  for (int attempt = 0; attempt < 2 && out == NULL; attempt++)
    {
      gboolean gone = FALSE;
      WaitResult r;

      tts_norm_start (n);
      if (!n->active)
        break;
      r = wait_ready (n);
      if (r == WAIT_GONE && attempt == 0)
        continue;
      if (r != WAIT_OK)
        {
          if (r == WAIT_GONE)
            fail_once (n, "the normalizer helper went away twice");
          break;
        }
      out = ask (n, text, "ru", &gone);
      if (out != NULL)
        n->strikes = 0;
      if (!gone)
        break;
      if (attempt == 0)
        tts_norm_stop (n);
      else
        fail_once (n, "the normalizer helper exited");
    }
  return out != NULL ? out : g_strdup (text);
}

gchar *
tts_norm_text (TtsNorm *n, const gchar *text, const gchar *lang)
{
  gchar *plain, *norm, *out;
  UserStress *u;

  if (n == NULL || text == NULL || !is_russian (lang) || strlen (text) > MAX_TEXT)
    return g_strdup (text);
  /* the user's stress marks break the normalizer's words: out and back */
  u = user_stress_take (text, &plain);
  if (u == NULL)
    return norm_plain (n, text, lang);
  norm = norm_plain (n, plain, lang);
  out = user_stress_put (u, norm);
  g_free (norm);
  g_free (plain);
  user_stress_free (u);
  return out;
}

gboolean
tts_norm_message (TtsNorm *n, gchar **text, const gchar *lang)
{
  gchar *norm;

  if (n == NULL || !is_russian (lang))
    return TRUE;                /* nothing to do per sentence either */
  if (strlen (*text) > TTS_NORM_WHOLE_MAX)
    return FALSE;
  norm = tts_norm_text (n, *text, lang);
  g_free (*text);
  *text = norm;
  return TRUE;
}
