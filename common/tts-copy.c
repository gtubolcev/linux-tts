/*
 * tts-copy - a copy of what a module speaks, in files, for debugging
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "tts-copy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>

#define WAV_HEADER 44
/* a RIFF size is 32 bits: start a new file well before that */
#define MAX_DATA ((guint64) 0xF0000000)
/* a text file stops growing here; the audio does not depend on it */
#define MAX_TEXT (4 << 20)

struct TtsCopy
{
  gchar *dir;
  guint next;           /* the next number to try; 0 until the dir is scanned */
  GString *text;        /* the text of the message so far */
  FILE *wav;            /* open while the message has audio */
  gchar *wav_path;
  guint number;         /* of the open file */
  gint rate, channels;
  guint64 data_bytes;
  gboolean failed;      /* this message: logged once, no more tries */
};

void
tts_copy_config_init (TtsCopyConfig *c)
{
  c->dir = NULL;
}

void
tts_copy_config_clear (TtsCopyConfig *c)
{
  g_clear_pointer (&c->dir, g_free);
}

gboolean
tts_copy_config_parse (TtsCopyConfig *c, const gchar *key, const gchar *value)
{
  if (g_ascii_strcasecmp (key, "AudioCopyDir") != 0)
    return FALSE;
  g_free (c->dir);
  c->dir = g_strdup (value);
  return TRUE;
}

/* "~/x" and "$HOME/x" against the home; anything not absolute is refused,
 * as the module's working directory is not the user's */
static gchar *
expand_dir (const gchar *dir)
{
  if (g_str_has_prefix (dir, "~/") || g_strcmp0 (dir, "~") == 0)
    return g_build_filename (g_get_home_dir (), dir + 1, NULL);
  if (g_str_has_prefix (dir, "$HOME/") || g_strcmp0 (dir, "$HOME") == 0)
    return g_build_filename (g_get_home_dir (), dir + 5, NULL);
  if (g_path_is_absolute (dir))
    return g_strdup (dir);
  return NULL;
}

TtsCopy *
tts_copy_new (const TtsCopyConfig *c)
{
  TtsCopy *t;
  gchar *dir;

  if (c == NULL || c->dir == NULL || *c->dir == '\0')
    return NULL;
  dir = expand_dir (c->dir);
  if (dir == NULL)
    {
      g_printerr ("tts-copy: AudioCopyDir \"%s\" is not an absolute path; no copy\n", c->dir);
      return NULL;
    }
  t = g_new0 (TtsCopy, 1);
  t->dir = dir;
  t->text = g_string_new (NULL);
  return t;
}

/* the highest N of outputN.wav or outputN.txt in the directory, plus one */
static guint
scan_next (const gchar *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;
  guint64 max = 0;

  if (d == NULL)
    return 1;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      const gchar *num = name + strlen ("output");
      gchar *end;
      guint64 n;

      if (!g_str_has_prefix (name, "output") || !g_ascii_isdigit (*num))
        continue;
      n = g_ascii_strtoull (num, &end, 10);
      if ((g_strcmp0 (end, ".wav") == 0 || g_strcmp0 (end, ".txt") == 0) && n > max
          && n < G_MAXUINT - 1)
        max = n;
    }
  g_dir_close (d);
  return (guint) max + 1;
}

static void
put_le32 (guint8 *p, guint32 v)
{
  p[0] = v & 0xff, p[1] = (v >> 8) & 0xff, p[2] = (v >> 16) & 0xff, p[3] = v >> 24;
}

static void
put_le16 (guint8 *p, guint16 v)
{
  p[0] = v & 0xff, p[1] = v >> 8;
}

static void
wav_header (guint8 *h, gint rate, gint channels, guint64 data_bytes)
{
  guint32 data = (guint32) MIN (data_bytes, MAX_DATA);

  memcpy (h, "RIFF", 4);
  put_le32 (h + 4, 36 + data);
  memcpy (h + 8, "WAVEfmt ", 8);
  put_le32 (h + 16, 16);
  put_le16 (h + 20, 1);         /* PCM */
  put_le16 (h + 22, channels);
  put_le32 (h + 24, rate);
  put_le32 (h + 28, rate * channels * 2);
  put_le16 (h + 32, channels * 2);
  put_le16 (h + 34, 16);
  memcpy (h + 36, "data", 4);
  put_le32 (h + 40, data);
}

static void
fail (TtsCopy *t, const gchar *what, const gchar *path, int err)
{
  if (!t->failed)
    g_printerr ("tts-copy: %s %s: %s; no copy of this message\n", what, path,
                g_strerror (err));
  t->failed = TRUE;
}

/* writes the text of the open file's number; the text so far stays for a
 * following file of the same message (a new rate) */
static void
write_text (TtsCopy *t)
{
  gchar *path, *name;
  GError *error = NULL;

  if (t->text->len == 0)
    return;
  name = g_strdup_printf ("output%u.txt", t->number);
  path = g_build_filename (t->dir, name, NULL);
  if (!g_file_set_contents_full (path, t->text->str, t->text->len,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    {
      g_printerr ("tts-copy: %s\n", error->message);
      g_error_free (error);
    }
  g_free (path);
  g_free (name);
}

static void
close_wav (TtsCopy *t)
{
  guint8 h[WAV_HEADER];
  gboolean ok;

  if (t->wav == NULL)
    return;
  wav_header (h, t->rate, t->channels, t->data_bytes);
  ok = fseek (t->wav, 0, SEEK_SET) == 0 && fwrite (h, 1, sizeof h, t->wav) == sizeof h;
  if (fclose (t->wav) != 0)
    ok = FALSE;
  t->wav = NULL;
  if (!ok)
    fail (t, "cannot finish", t->wav_path, errno);
  write_text (t);
  g_clear_pointer (&t->wav_path, g_free);
}

static gboolean
open_wav (TtsCopy *t, gint rate, gint channels)
{
  guint8 h[WAV_HEADER];

  if (g_mkdir_with_parents (t->dir, 0700) != 0)
    {
      fail (t, "cannot create", t->dir, errno);
      return FALSE;
    }
  if (t->next == 0)
    t->next = scan_next (t->dir);
  /* another module may write to the same directory: never overwrite */
  for (guint tries = 0; tries < 1000 && t->next < G_MAXUINT; tries++, t->next++)
    {
      gchar *name = g_strdup_printf ("output%u.wav", t->next);
      gchar *path = g_build_filename (t->dir, name, NULL);
      int fd = g_open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

      g_free (name);
      if (fd < 0 && errno == EEXIST)
        {
          g_free (path);
          continue;
        }
      if (fd < 0)
        {
          fail (t, "cannot create", path, errno);
          g_free (path);
          return FALSE;
        }
      t->wav = fdopen (fd, "wb");
      if (t->wav == NULL)
        {
          fail (t, "cannot open", path, errno);
          close (fd);
          g_free (path);
          return FALSE;
        }
      t->wav_path = path;
      t->number = t->next++;
      t->rate = rate;
      t->channels = channels;
      t->data_bytes = 0;
      wav_header (h, rate, channels, 0);
      if (fwrite (h, 1, sizeof h, t->wav) != sizeof h)
        {
          fail (t, "cannot write", path, errno);
          close_wav (t);
          return FALSE;
        }
      return TRUE;
    }
  fail (t, "no free outputN.wav name in", t->dir, EEXIST);
  return FALSE;
}

void
tts_copy_text (TtsCopy *t, const gchar *text)
{
  if (t == NULL || text == NULL || t->text->len >= MAX_TEXT)
    return;
  g_string_append (t->text, text);
  g_string_append_c (t->text, '\n');
}

void
tts_copy_audio (TtsCopy *t, const gint16 *samples, gsize num_samples,
                gint sample_rate, gint channels)
{
  gsize bytes;

  if (t == NULL || t->failed || samples == NULL || num_samples == 0
      || sample_rate <= 0 || channels <= 0 || channels > 8)
    return;
  bytes = num_samples * channels * 2;
  if (t->wav != NULL
      && (sample_rate != t->rate || channels != t->channels || t->data_bytes + bytes > MAX_DATA))
    close_wav (t);
  if (t->wav == NULL && (t->failed || !open_wav (t, sample_rate, channels)))
    return;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  if (fwrite (samples, 1, bytes, t->wav) != bytes)
#else
  for (gsize i = 0; i < num_samples * channels; i++)
    {
      gint16 s = GINT16_TO_LE (samples[i]);

      if (fwrite (&s, 2, 1, t->wav) != 1)
        break;
    }
  if (ferror (t->wav))
#endif
    {
      fail (t, "cannot write", t->wav_path, errno);
      close_wav (t);
      return;
    }
  t->data_bytes += bytes;
}

void
tts_copy_end (TtsCopy *t)
{
  if (t == NULL)
    return;
  close_wav (t);
  g_string_truncate (t->text, 0);
  t->failed = FALSE;
}

void
tts_copy_free (TtsCopy *t)
{
  if (t == NULL)
    return;
  tts_copy_end (t);
  g_string_free (t->text, TRUE);
  g_free (t->dir);
  g_free (t);
}
