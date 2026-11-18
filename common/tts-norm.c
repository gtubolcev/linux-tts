/*
 * tts-norm - text normalization for the speech-dispatcher modules
 *
 * The helper talks lines over a socket (a socket, not a pipe: a write to
 * a dead helper must not kill the module with SIGPIPE):
 *   helper -> module   "READY" once loaded, or "ERROR<TAB>message" and exit
 *   module -> helper   "<id><TAB><lang><TAB><text>"
 *   helper -> module   "<id><TAB><text>"
 * The text has no tabs or line breaks; the module replaces them with
 * spaces.  An answer with another id is the late answer to a request that
 * timed out, and is skipped.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "tts-norm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TTS_NORM_HELPER
#define TTS_NORM_HELPER "/usr/libexec/speech-dispatcher-modules/tts-normalize"
#endif

#define RETRY_AFTER_US (60 * G_TIME_SPAN_SECOND)
#define MAX_TEXT 16384          /* bytes per request; longer passes through */
/* the answer may take timeout_ms plus this per byte (ru_normalizr does
 * ~100 bytes/ms; a long sentence must not look like a hung helper) */
#define US_PER_BYTE 100

struct TtsNorm
{
  TtsNormConfig cfg;
  GPid pid;                     /* 0: not running */
  int fd;
  gboolean ready;
  gint64 started;               /* monotonic, when spawned */
  gint64 retry_after;           /* monotonic; 0: may start */
  guint64 next_id;
  GString *in;                  /* bytes read, not yet a whole line */
  TtsNormCancel cancel;
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
  c->python = g_strdup ("python3");
  c->helper = g_strdup (TTS_NORM_HELPER);
  c->timeout_ms = 1500;
  c->start_ms = 8000;
}

void
tts_norm_config_clear (TtsNormConfig *c)
{
  g_clear_pointer (&c->python, g_free);
  g_clear_pointer (&c->helper, g_free);
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
  else if (g_ascii_strcasecmp (key, "TextNormalizationPython") == 0 && *value != '\0')
    g_free (c->python), c->python = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationHelper") == 0 && *value != '\0')
    g_free (c->helper), c->helper = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "TextNormalizationTimeout") == 0)
    c->timeout_ms = CLAMP (atoi (value), 50, 60000);
  else if (g_ascii_strcasecmp (key, "TextNormalizationStartTimeout") == 0)
    c->start_ms = CLAMP (atoi (value), 100, 120000);
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
  n->fd = -1;
  n->in = g_string_new (NULL);
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
  g_free (n);
}

static void
child_setup (gpointer data)
{
  /* the helper dies with the module, even if the module is killed */
  prctl (PR_SET_PDEATHSIG, SIGTERM);
}

void
tts_norm_stop (TtsNorm *n)
{
  if (n == NULL || n->pid == 0)
    return;
  /* closing the socket ends the helper's loop; TERM in case it is busy */
  close (n->fd);
  n->fd = -1;
  kill (n->pid, SIGTERM);
  for (int i = 0; i < 50; i++)
    {
      pid_t r = waitpid (n->pid, NULL, WNOHANG);
      if (r == n->pid || (r < 0 && errno == ECHILD))
        goto reaped;
      g_usleep (10000);
    }
  kill (n->pid, SIGKILL);
  waitpid (n->pid, NULL, 0);
reaped:
  n->pid = 0;
  n->ready = FALSE;
  g_string_truncate (n->in, 0);
}

/* stop and do not start again for a while */
static void
fail (TtsNorm *n, const gchar *why)
{
  g_printerr ("tts-norm: %s; text goes to the engine unnormalized for %d s\n",
              why, (int) (RETRY_AFTER_US / G_TIME_SPAN_SECOND));
  tts_norm_stop (n);
  n->retry_after = g_get_monotonic_time () + RETRY_AFTER_US;
}

void
tts_norm_start (TtsNorm *n)
{
  GError *error = NULL;
  int sv[2];
  gchar *argv[] = { n != NULL ? n->cfg.python : NULL, "-I", n != NULL ? n->cfg.helper : NULL, NULL };

  if (n == NULL || n->pid != 0)
    return;
  if (n->retry_after != 0 && g_get_monotonic_time () < n->retry_after)
    return;
  n->retry_after = 0;
  if (!g_file_test (n->cfg.helper, G_FILE_TEST_EXISTS))
    {
      n->retry_after = g_get_monotonic_time () + RETRY_AFTER_US;
      return;
    }
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    {
      fail (n, g_strerror (errno));
      return;
    }
  if (!g_spawn_async_with_pipes_and_fds (NULL, (const gchar * const *) argv, NULL,
                                         G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                         child_setup, NULL, sv[1], sv[1], -1, NULL, NULL, 0,
                                         &n->pid, NULL, NULL, NULL, &error))
    {
      close (sv[0]);
      close (sv[1]);
      n->pid = 0;
      fail (n, error->message);
      g_error_free (error);
      return;
    }
  close (sv[1]);
  n->fd = sv[0];
  n->ready = FALSE;
  n->started = g_get_monotonic_time ();
  g_string_truncate (n->in, 0);
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

/* waits for READY up to the start timeout; a stopped message leaves the
 * helper loading for the next one */
static gboolean
wait_ready (TtsNorm *n)
{
  gint64 deadline = n->started + (gint64) n->cfg.start_ms * 1000;
  gboolean gone, cancelled;

  while (!n->ready)
    {
      gchar *line = read_line (n, deadline, &gone, &cancelled);
      if (line == NULL)
        {
          if (gone)
            fail (n, "the normalizer helper exited while loading");
          else if (!cancelled)
            fail (n, "the normalizer helper did not load in time");
          return FALSE;
        }
      if (g_str_equal (line, "READY"))
        n->ready = TRUE;
      else if (g_str_has_prefix (line, "ERROR\t"))
        {
          gchar *why = g_strdup_printf ("normalizer: %s", line + 6);
          fail (n, why);
          g_free (why);
          g_free (line);
          return FALSE;
        }
      g_free (line);
    }
  return TRUE;
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

static gchar *
ask (TtsNorm *n, const gchar *text, const gchar *lang)
{
  gchar *clean = clean_text (text);
  guint64 id = ++n->next_id;
  gchar *req = g_strdup_printf ("%" G_GUINT64_FORMAT "\t%s\t%s\n", id, lang, clean);
  gchar *prefix = g_strdup_printf ("%" G_GUINT64_FORMAT "\t", id);
  gint64 deadline = g_get_monotonic_time () + (gint64) n->cfg.timeout_ms * 1000
                    + (gint64) strlen (clean) * US_PER_BYTE;
  gchar *result = NULL;
  gboolean gone = FALSE, cancelled = FALSE;

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
  if (result == NULL && !cancelled)
    fail (n, gone ? "the normalizer helper exited" : "the normalizer did not answer in time");
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

gchar *
tts_norm_text (TtsNorm *n, const gchar *text, const gchar *lang)
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
  tts_norm_start (n);
  if (n->pid != 0 && wait_ready (n))
    out = ask (n, text, "ru");
  return out != NULL ? out : g_strdup (text);
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
