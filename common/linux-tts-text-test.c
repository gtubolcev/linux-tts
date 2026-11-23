/* linux-tts-text: the SSML walk and the escapes, with normalization off (no
 * data needed).  GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "linux-tts-text.h"

static int fails;

static void
check (LinuxTtsText *t, const char *in, int ssml, const char *want)
{
  char *got = linux_tts_text_prepare (t, in, -1, "ru", ssml, "+");
  const char *g = got != NULL ? got : "(null)";

  if (strcmp (g, want) != 0)
    printf ("FAIL %s\n  got  %s\n  want %s\n", in, g, want), fails++;
  linux_tts_text_string_free (got);
}

int
main (void)
{
  gchar *dir = g_dir_make_tmp ("ltt-XXXXXX", NULL);
  gchar *conf = g_build_filename (dir, "x.conf", NULL);
  LinuxTtsText *t;

  /* normalization and stress off: the walk must give the input back */
  g_file_set_contents (conf, "TextNormalization no\nTextStress \"no\"\n", -1, NULL);
  g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
  t = linux_tts_text_new (conf);
  check (t, "<speak>a &amp; b &lt;c&gt; <mark name=\"x>y\"/>d</speak>", 1, "(null)");
  check (t, "plain", 0, "(null)");
  check (t, "<speak>&#1092;&#x44B;</speak>", 1, "<speak>фы</speak>");
  check (t, "<speak>a &bogus; b</speak>", 1, "<speak>a &amp;bogus; b</speak>");
  check (t, "\xff\xfe", 0, "(null)");
  check (t, "", 0, "(null)");
  linux_tts_text_free (t);
  linux_tts_text_free (NULL);
  g_unlink (conf);
  g_rmdir (dir);
  if (fails == 0)
    printf ("ok\n");
  return fails != 0;
}
