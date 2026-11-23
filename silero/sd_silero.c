/*
 * sd_silero - speech-dispatcher output module for Silero TTS (Russian)
 *
 * The model is run by silero-server (ONNX Runtime on the CPU), a resident
 * process on a Unix socket in $XDG_RUNTIME_DIR, started the first time the module has to
 * speak and stopped after SileroIdleTimeout seconds without speech.  The
 * server outlives the module: speech-dispatcher restarts its modules on every
 * reload, and a new module adopts the running server (state file in
 * $XDG_RUNTIME_DIR) instead of loading the model again.  A small watcher
 * process started with the server stops it when no living module has owned it
 * for SileroIdleTimeout seconds (also when the module was killed).
 * SileroPreload starts it with the module instead of with the first message.
 * For tts-module the server is the one model: loading it starts or adopts
 * the server, the idle unload stops it, QUIT leaves it to the next module.
 * The model and the runtime are not free software (CC BY-NC-SA 4.0), so they
 * are not part of the module: they are installed, with their licences, into
 * SileroDir (default ~/.local/share/silero-tts/<version>).  The voices are
 * the speakers of the model version, offered as "silero-ru-<speaker>".
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tts-module.h"

#define DBG(...) TTS_DBG (__VA_ARGS__)
#define ERR(...) TTS_ERR (__VA_ARGS__)

/* configuration */
static gchar *cfg_server = NULL;
static gchar *cfg_dir = NULL;
static gchar *cfg_version = NULL;
static int cfg_threads = 0;
static int cfg_load_timeout = 60;

/* the speakers of the model versions */
static const struct { const char *version, *language, *speaker, *gender; } speakers[] = {
  { "v5_6_ru", "ru", "aidar", "male" },
  { "v5_6_ru", "ru", "baya", "female" },
  { "v5_6_ru", "ru", "eugene", "male" },
  { "v5_6_ru", "ru", "kseniya", "female" },
  { "v5_6_ru", "ru", "xenia", "female" },
  { "v5_ru", "ru", "aidar", "male" },
  { "v5_ru", "ru", "baya", "female" },
  { "v5_ru", "ru", "eugene", "male" },
  { "v5_ru", "ru", "kseniya", "female" },
  { "v5_ru", "ru", "xenia", "female" },
};

/* the one model of all voices: the server */
static int server_token;

/* server; used by the loader, and by the worker only while it is loaded */
static pid_t server_pid = 0;
static long long server_serial = 0;   /* names its socket */
static gboolean server_ready = FALSE;
static pid_t server_child = 0;   /* server_pid when we forked it */



static int
wanted_threads (void)
{
  /* more than 4 decoder threads made it slower on a 4-core/8-thread laptop */
  return cfg_threads > 0 ? cfg_threads : MIN ((int) g_get_num_processors (), 4);
}



static gchar *
state_path (void)
{
  return g_build_filename (g_get_user_runtime_dir (), "sd_silero.state", NULL);
}



/* $XDG_RUNTIME_DIR/sd_silero-<serial>.sock: a new name for every server, so
 * that a module never talks to a dying one; the directory is the user's (0700) */
static void
sock_path (long long serial, char *buf, size_t size)
{
  g_snprintf (buf, size, "%s/sd_silero-%lld.sock", g_get_user_runtime_dir (), serial);
}



/* "<version> <dir>": what the server was started with */
static gchar *
server_model (void)
{
  return g_strdup_printf ("%s %s", cfg_version, cfg_dir);
}



/* pid serial threads owner released, then the model (server_model) on the second line;
 * owner 0 = no module uses the server, released = when the last one let go. */
static void
state_write (long owner, long released)
{
  gchar *path = state_path ();
  gchar *model = server_model ();
  gchar *text = g_strdup_printf ("%d %lld %d %ld %ld\n%s\n", server_pid, server_serial,
                                 wanted_threads (), owner, released, model);

  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
    ERR ("can't write %s", path);
  g_free (text);
  g_free (model);
  g_free (path);
}



typedef struct
{
  pid_t pid;
  long long serial;
  int threads;
  long owner, released;
  gchar model[1024];
} State;

static gboolean
state_read (State *st)
{
  gchar *path = state_path ();
  gchar *text = NULL;
  gboolean ok = FALSE;

  if (g_file_get_contents (path, &text, NULL, NULL))
    {
      int pid;
      ok = sscanf (text, "%d %lld %d %ld %ld\n%1023[^\n]", &pid, &st->serial, &st->threads,
                   &st->owner, &st->released, st->model) == 6;
      st->pid = pid;
    }
  g_free (text);
  g_free (path);
  return ok;
}



static void
state_remove (void)
{
  gchar *path = state_path ();
  g_unlink (path);
  g_free (path);
}



/* The server may be our child or one adopted from an earlier module. */
static gboolean
pid_is_server (pid_t pid)
{
  gchar *path = g_strdup_printf ("/proc/%d/comm", (int) pid);
  gchar *comm = NULL;
  gboolean ok;

  if (pid > 0 && pid == server_child)
    {
      /* our child: right after fork its comm is still ours */
      g_free (path);
      if (waitpid (pid, NULL, WNOHANG) == 0)
        return TRUE;
      server_child = 0;
      return FALSE;
    }
  ok = pid > 0 && kill (pid, 0) == 0
       && g_file_get_contents (path, &comm, NULL, NULL)
       && g_str_has_prefix (comm, "silero-server");
  g_free (comm);
  g_free (path);
  return ok;
}



static void
kill_server (pid_t pid, long long serial)
{
  char sock[512];

  kill (pid, SIGTERM);
  for (int i = 0; i < 50 && pid_is_server (pid); i++)
    g_usleep (100000);
  if (pid_is_server (pid))
    kill (pid, SIGKILL);
  waitpid (pid, NULL, WNOHANG);
  /* it removes the socket itself, unless killed */
  sock_path (serial, sock, sizeof (sock));
  unlink (sock);
}



static void
server_stop (void)
{
  if (server_pid <= 0)
    return;
  DBG ("stopping silero-server (pid %d)", server_pid);
  kill_server (server_pid, server_serial);
  server_pid = 0;
  server_ready = FALSE;
  state_remove ();
}



static gboolean
server_alive (void)
{
  if (server_pid <= 0)
    return FALSE;
  if (!pid_is_server (server_pid))
    {
      ERR ("silero-server exited");
      server_pid = 0;
      server_ready = FALSE;
      state_remove ();
      return FALSE;
    }
  return TRUE;
}



/* Takes over a server left behind by an earlier instance of the module. */
static gboolean
server_adopt (void)
{
  State st;
  gchar *model;
  gboolean same;

  if (!state_read (&st))
    return FALSE;
  if (!pid_is_server (st.pid))
    {
      state_remove ();
      return FALSE;
    }
  model = server_model ();
  same = st.threads == wanted_threads () && strcmp (st.model, model) == 0;
  g_free (model);
  if (same)
    {
      /* a server of an older version listened on a TCP port */
      char sock[512];
      sock_path (st.serial, sock, sizeof (sock));
      same = g_file_test (sock, G_FILE_TEST_EXISTS);
    }
  if (!same)
    {
      DBG ("silero-server %d runs with other settings, stopping it", st.pid);
      kill_server (st.pid, st.serial);
      state_remove ();
      return FALSE;
    }
  server_pid = st.pid;
  server_serial = st.serial;
  server_ready = FALSE;   /* it may still be loading */
  state_write (getpid (), 0);
  DBG ("adopted silero-server pid %d (sd_silero-%lld.sock)", server_pid, server_serial);
  return TRUE;
}



/* Is the module that owns the server still running? */
static gboolean
owner_alive (long owner)
{
  gchar *path = g_strdup_printf ("/proc/%ld/comm", owner);
  gchar *comm = NULL;
  gboolean ok = owner > 0 && g_file_get_contents (path, &comm, NULL, NULL)
                && g_str_has_prefix (comm, "sd_silero");

  g_free (comm);
  g_free (path);
  return ok;
}



/* A watcher process lives as long as the server and stops it when no module
 * has owned it for SileroIdleTimeout seconds: after the last module released
 * it, or after the owner died without doing so (killed by speech-dispatcher).
 * It is this program again (SD_SILERO_WATCH="<pid> <serial> <idle>"): after
 * fork in a threaded process only async-signal-safe calls are allowed, and
 * the watcher reads files and allocates. */
static void
watch_loop (const gchar *arg)
{
  int pid = 0, idle = 0;
  long long serial = 0;
  time_t dead_since = 0;

  if (sscanf (arg, "%d %lld %d", &pid, &serial, &idle) != 3 || pid <= 0)
    _exit (2);
  prctl (PR_SET_NAME, "silero-watch", 0, 0, 0);
  for (;;)
    {
      State st;
      time_t since;

      sleep (5);
      if (!state_read (&st) || st.pid != pid)
        _exit (0);   /* stopped by a module */
      if (!pid_is_server (pid))
        {
          state_remove ();
          _exit (0);
        }
      if (st.owner != 0 && owner_alive (st.owner))
        {
          dead_since = 0;
          continue;
        }
      if (st.owner == 0)
        since = st.released, dead_since = 0;
      else
        since = dead_since != 0 ? dead_since : (dead_since = time (NULL));
      if (time (NULL) - since >= idle)
        {
          kill_server (pid, serial);
          state_remove ();
          _exit (0);
        }
    }
}



static void
server_watch (void)
{
  gchar *arg = g_strdup_printf ("%d %lld %d", (int) server_pid, server_serial,
                                tts_idle_timeout ());
  gchar **envp = g_environ_setenv (g_get_environ (), "SD_SILERO_WATCH", arg, TRUE);
  char *const argv[] = { (char *) "sd_silero", NULL };
  pid_t child;

  /* forked twice, so that init reaps it and not the module */
  child = fork ();
  if (child == 0)
    {
      /* a threaded process: only async-signal-safe calls up to exec */
      int null;

      if (fork () != 0)
        _exit (0);
      /* no ties to speech-dispatcher; the server is not our child */
      setsid ();
      null = open ("/dev/null", O_RDWR);
      dup2 (null, STDIN_FILENO);
      dup2 (null, STDOUT_FILENO);
      dup2 (null, STDERR_FILENO);
      closefrom (3);
      execve ("/proc/self/exe", argv, envp);
      _exit (127);
    }
  if (child > 0)
    waitpid (child, NULL, 0);
  g_strfreev (envp);
  g_free (arg);
}



/* Called when the module goes away: the server stays for the next module,
 * the watcher stops it if none comes within the idle timeout. */
static void
server_release (void)
{
  if (!server_alive ())
    return;
  if (tts_idle_timeout () <= 0)
    server_stop ();
  else
    state_write (0, (long) time (NULL));
}



static int
server_connect (void)
{
  struct sockaddr_un addr = { 0 };
  int fd;

  addr.sun_family = AF_UNIX;
  sock_path (server_serial, addr.sun_path, sizeof (addr.sun_path));
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



static gboolean
server_health (void)
{
  static const char req[] = "GET /v2/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  char buf[64] = "";
  int fd = server_connect ();
  gboolean ok = FALSE;

  /* a server busy with a long sentence or hung: not ready */
  struct timeval tv = { 2, 0 };

  if (fd < 0)
    return FALSE;
  setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
  setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
  if (send (fd, req, sizeof (req) - 1, MSG_NOSIGNAL) == (ssize_t) sizeof (req) - 1
      && read (fd, buf, sizeof (buf) - 1) > 12)
    ok = strncmp (buf + 9, "200", 3) == 0;
  close (fd);
  return ok;
}



static gboolean
server_spawn (void)
{
  gchar threads[16];
  char sock[sizeof (((struct sockaddr_un *) 0)->sun_path)];
  gchar *log = tts_debug ? g_build_filename (g_get_user_runtime_dir (), "silero-server.log", NULL)
                         : NULL;

  server_serial = g_get_real_time ();
  sock_path (server_serial, sock, sizeof (sock));
  if (strlen (sock) + 1 >= sizeof (sock))
    {
      g_free (log);
      ERR ("the path of the socket is too long: %s", sock);
      return FALSE;
    }
  unlink (sock);
  g_snprintf (threads, sizeof (threads), "%d", wanted_threads ());

  server_pid = fork ();
  if (server_pid < 0)
    {
      server_pid = 0;
      g_free (log);
      return FALSE;
    }
  if (server_pid == 0)
    {
      /* a threaded process: only async-signal-safe calls up to exec */
      int out;

      /* its own session, so it outlives the module (see server_release) */
      setsid ();
      out = open ("/dev/null", O_RDWR);
      dup2 (out, STDIN_FILENO);
      if (log != NULL)
        out = open (log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      dup2 (out, STDOUT_FILENO);
      dup2 (out, STDERR_FILENO);
      closefrom (3);
      execl (cfg_server, cfg_server, "--dir", cfg_dir, "--version", cfg_version,
             "--socket", sock, "--threads", threads, (char *) NULL);
      _exit (127);
    }

  g_free (log);
  server_child = server_pid;
  server_ready = FALSE;
  state_write (getpid (), 0);
  server_watch ();
  DBG ("started silero-server pid %d on %s", server_pid, sock);
  return TRUE;
}



/* stoppable: in the worker, where STOP ends the wait */
static gboolean
server_start (gboolean stoppable)
{
  time_t start;

  if (!server_alive () && !server_adopt () && !server_spawn ())
    return FALSE;
  if (server_ready)
    return TRUE;

  start = time (NULL);
  while (time (NULL) - start < cfg_load_timeout)
    {
      if (!server_alive ())
        return FALSE;   /* see silero-server.log with Debug 1 */
      if (stoppable && tts_stopped ())
        return FALSE;
      if (server_health ())
        {
          DBG ("silero-server ready after %ld s", (long) (time (NULL) - start));
          server_ready = TRUE;
          return TRUE;
        }
      g_usleep (200000);
    }
  ERR ("silero-server did not start in %d s", cfg_load_timeout);
  server_stop ();
  return FALSE;
}



/* ------------------------------------------------------------------ engine */

static void
silero_setup (void)
{
  const gchar *watch = g_getenv ("SD_SILERO_WATCH");

  if (watch != NULL)
    watch_loop (watch);   /* never returns */
  cfg_server = g_strdup ("/usr/bin/silero-server");
  cfg_version = g_strdup ("v5_6_ru");
}



static gboolean
silero_config (const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "SileroServer") == 0)
    g_free (cfg_server), cfg_server = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "SileroDir") == 0)
    g_free (cfg_dir), cfg_dir = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "SileroVersion") == 0)
    g_free (cfg_version), cfg_version = g_strdup (value);
  else if (g_ascii_strcasecmp (key, "SileroThreads") == 0)
    cfg_threads = atoi (value);
  else if (g_ascii_strcasecmp (key, "SileroLoadTimeout") == 0)
    cfg_load_timeout = MAX (atoi (value), 1);
  else
    return FALSE;
  return TRUE;
}



static gboolean
silero_init (gchar **msg)
{
  gchar *models;
  guint n = 0;

  if (cfg_dir == NULL || *cfg_dir == '\0')
    {
      g_free (cfg_dir);
      cfg_dir = g_build_filename (g_get_user_data_dir (), "silero-tts", cfg_version, NULL);
    }
  else if (cfg_dir[0] == '~' && cfg_dir[1] == '/')
    {
      gchar *d = g_build_filename (g_get_home_dir (), cfg_dir + 2, NULL);
      g_free (cfg_dir);
      cfg_dir = d;
    }
  if (!g_file_test (cfg_server, G_FILE_TEST_IS_EXECUTABLE))
    {
      *msg = g_strdup_printf ("SileroServer %s is missing", cfg_server);
      return FALSE;
    }
  models = g_build_filename (cfg_dir, "models", NULL);
  if (!g_file_test (models, G_FILE_TEST_IS_DIR))
    {
      *msg = g_strdup_printf ("the Silero model is not installed (no %s)", models);
      g_free (models);
      return FALSE;
    }
  g_free (models);
  for (guint i = 0; i < G_N_ELEMENTS (speakers); i++)
    if (strcmp (speakers[i].version, cfg_version) == 0)
      {
        gchar *name = g_strdup_printf ("silero-%s-%s", speakers[i].language, speakers[i].speaker);
        tts_voice_add (name, speakers[i].language, speakers[i].gender, &server_token,
                       (gpointer) speakers[i].speaker);
        g_free (name);
        n++;
      }
  if (n == 0)
    {
      *msg = g_strdup_printf ("unknown SileroVersion %s", cfg_version);
      return FALSE;
    }
  *msg = g_strdup_printf ("Silero ready, %u voices", n);
  return TRUE;
}



/* in the loader thread: starts the server (or adopts a running one) and
 * waits until it has loaded the model */
static gboolean
silero_load (gpointer model, GError **error)
{
  if (server_start (FALSE))
    return TRUE;
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
               "silero-server did not start (see silero-server.log with Debug 1)");
  return FALSE;
}



/* idle: stop the server; QUIT: leave it to the next module */
static void
silero_unload (gpointer model)
{
  if (tts_closing ())
    server_release ();
  else
    server_stop ();
}



static gchar *
json_escape (const gchar *s)
{
  GString *out = g_string_new (NULL);

  for (; *s != '\0'; s++)
    {
      guchar c = (guchar) *s;
      if (c == '"' || c == '\\')
        g_string_append_printf (out, "\\%c", c);
      else if (c < 0x20)
        g_string_append_printf (out, "\\u%04x", c);
      else
        g_string_append_c (out, c);
    }
  return g_string_free (out, FALSE);
}



/* ------------------------------------------------------------------- HTTP */

static gboolean
write_all (int fd, const char *buf, size_t len)
{
  while (len > 0)
    {
      /* the server gone: an error, not SIGPIPE (speech-dispatcher ignores
       * it for its modules, a module run by hand does not) */
      ssize_t n = send (fd, buf, len, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return FALSE;
      buf += n;
      len -= n;
    }
  return TRUE;
}



typedef struct
{
  int fd;
  char buf[65536];
  size_t start, end;
} Reader;



/* Reads more data; gives up when the message is stopped. 1 = data, 0 = EOF, -1 = error or stop. */
static int
reader_fill (Reader *r)
{
  if (r->start > 0)
    {
      memmove (r->buf, r->buf + r->start, r->end - r->start);
      r->end -= r->start;
      r->start = 0;
    }
  if (r->end == sizeof (r->buf))
    return -1;
  for (;;)
    {
      struct pollfd fds[1] = { { r->fd, POLLIN, 0 } };
      ssize_t n;

      if (poll (fds, 1, 200) < 0 && errno != EINTR)
        return -1;
      if (tts_stopped ())
        return -1;
      if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      n = read (r->fd, r->buf + r->end, sizeof (r->buf) - r->end);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        return -1;
      if (n == 0)
        return 0;
      r->end += n;
      return 1;
    }
}



/* One line (without CRLF) into line, at most size - 1 bytes */
static gboolean
reader_line (Reader *r, char *line, size_t size)
{
  for (;;)
    {
      char *nl = memchr (r->buf + r->start, '\n', r->end - r->start);
      if (nl != NULL)
        {
          size_t len = nl - (r->buf + r->start);
          size_t copy = MIN (len, size - 1);
          memcpy (line, r->buf + r->start, copy);
          line[copy] = '\0';
          if (copy > 0 && line[copy - 1] == '\r')
            line[copy - 1] = '\0';
          r->start += len + 1;
          return TRUE;
        }
      if (reader_fill (r) <= 0)
        return FALSE;
    }
}



static gboolean
reader_bytes (Reader *r, GByteArray *out, size_t len)
{
  while (len > 0)
    {
      size_t have = r->end - r->start;
      if (have == 0)
        {
          if (reader_fill (r) <= 0)
            return FALSE;
          continue;
        }
      have = MIN (have, len);
      if (out != NULL)
        g_byte_array_append (out, (const guint8 *) r->buf + r->start, have);
      r->start += have;
      len -= have;
    }
  return TRUE;
}



/* Synthesizes one sentence into float samples. */
static gboolean
generate (const gchar *text, const TtsPiece *p, GByteArray *pcm, int *sample_rate)
{
  gchar *esc, *body, *req;
  Reader *r;
  char line[1024];
  gboolean ok = FALSE, chunked = FALSE;
  long content_length = -1;
  int status = 0;

  esc = json_escape (text);
  {
    gchar rate[G_ASCII_DTOSTR_BUF_SIZE], pitch[G_ASCII_DTOSTR_BUF_SIZE];

    /* rate: the model's part of the speed; pitch -100..100 -> 0.75..1.25 */
    g_ascii_formatd (rate, sizeof (rate), "%.3f", tts_model_speed (p->settings));
    g_ascii_formatd (pitch, sizeof (pitch), "%.3f", 1.0 + p->settings->pitch / 400.0);
    body = g_strdup_printf ("{\"text\": \"%s\", \"speaker\": \"%s\", \"rate\": %s, \"pitch\": %s}",
                            esc, (const gchar *) p->voice->data, rate, pitch);
  }
  req = g_strdup_printf ("POST /v2/generate HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                         "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n", strlen (body));
  g_free (esc);

  r = g_new0 (Reader, 1);
  r->fd = server_connect ();
  if (r->fd < 0)
    goto out;
  if (!write_all (r->fd, req, strlen (req)) || !write_all (r->fd, body, strlen (body)))
    goto out;

  if (!reader_line (r, line, sizeof (line)) || sscanf (line, "HTTP/%*s %d", &status) != 1)
    goto out;
  *sample_rate = 48000;
  while (reader_line (r, line, sizeof (line)) && *line != '\0')
    {
      if (g_ascii_strncasecmp (line, "X-Audio-Sample-Rate:", 20) == 0)
        *sample_rate = atoi (line + 20);
      else if (g_ascii_strncasecmp (line, "Content-Length:", 15) == 0)
        content_length = atol (line + 15);
      else if (g_ascii_strncasecmp (line, "Transfer-Encoding:", 18) == 0 && strstr (line, "chunked"))
        chunked = TRUE;
    }

  if (status != 200)
    {
      GByteArray *err = g_byte_array_new ();
      if (content_length > 0 && content_length < 65536)
        reader_bytes (r, err, content_length);
      g_byte_array_append (err, (const guint8 *) "", 1);
      ERR ("silero-server: HTTP %d %s", status, (const char *) err->data);
      g_byte_array_free (err, TRUE);
      goto out;
    }

  if (chunked)
    {
      for (;;)
        {
          long size;
          if (!reader_line (r, line, sizeof (line)))
            goto out;
          size = strtol (line, NULL, 16);
          if (size == 0)
            break;
          if (!reader_bytes (r, pcm, size) || !reader_line (r, line, sizeof (line)))
            goto out;
        }
    }
  else if (content_length >= 0)
    {
      if (!reader_bytes (r, pcm, content_length))
        goto out;
    }
  else
    while (reader_fill (r) > 0)
      ;
  ok = TRUE;

out:
  if (r->fd >= 0)
    close (r->fd);
  g_free (r);
  g_free (req);
  g_free (body);
  return ok && !tts_stopped ();
}



/* in the worker */
static gboolean
silero_speak (const TtsPiece *p, GError **error)
{
  GByteArray *pcm;
  int rate = 48000;
  gboolean ok;

  /* it died since it was loaded: start it again */
  if ((!server_alive () || !server_ready) && !server_start (TRUE))
    {
      if (tts_stopped ())
        return TRUE;
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "silero-server is gone");
      return FALSE;
    }
  pcm = g_byte_array_new ();
  ok = generate (p->text, p, pcm, &rate);
  if (ok)
    tts_audio_float ((const float *) pcm->data, pcm->len / sizeof (float), rate);
  else if (!tts_stopped ())
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "silero-server failed");
  g_byte_array_free (pcm, TRUE);
  return ok || tts_stopped ();
}



/* Silero reads "+" before the stressed vowel */
static void
silero_text_profile (const TtsVoice *voice, TtsTextProfile *p)
{
  p->stress = TRUE;
  p->mark = RU_STRESS_PLUS;
}



const TtsEngine tts_engine = {
  .name = "sd_silero",
  .prefix = "Silero",
  .max_models = 1,
  .idle_timeout = 300,
  .first_chunk = 0,
  .setup = silero_setup,
  .config = silero_config,
  .init = silero_init,
  .load = silero_load,
  .unload = silero_unload,
  .speak = silero_speak,
  .text_profile = silero_text_profile,
};
