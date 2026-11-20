/* tts-config-test: the user's module file goes on top of the system one */
#include <glib.h>
#include <glib/gstdio.h>
#include "tts-config.h"

int
main (void)
{
  gchar *tmp = g_dir_make_tmp ("tts-config-XXXXXX", NULL);
  gchar *sys = g_build_filename (tmp, "sys", NULL);
  gchar *usr = g_build_filename (tmp, "usr", NULL);
  gchar *sysf = g_build_filename (sys, "m.conf", NULL);
  gchar *usrf = g_build_filename (usr, "m.conf", NULL);
  gchar *other = g_build_filename (usr, "other.conf", NULL);
  gchar *c = NULL;
  int fail = 0;

  g_mkdir (sys, 0700);
  g_mkdir (usr, 0700);
  g_file_set_contents (sysf, "A 1\nB 2\n", -1, NULL);
  g_file_set_contents (usrf, "B 3\n", -1, NULL);
  g_file_set_contents (other, "C 4\n", -1, NULL);
  g_setenv ("TTS_SYS_CONF_DIR", sys, TRUE);

#define CHECK(cond, what) do { if (!(cond)) { g_printerr ("FAIL %s\n", what); fail = 1; } } while (0)
  CHECK (tts_config_read (usrf, &c), "user read");
  CHECK (c && g_str_has_prefix (c, "A 1\nB 2\n") && g_str_has_suffix (c, "\nB 3\n"), "user on top");
  g_free (c);
  CHECK (tts_config_read (sysf, &c), "system read");
  CHECK (g_strcmp0 (c, "A 1\nB 2\n") == 0, "system file alone");
  g_free (c);
  CHECK (tts_config_read (other, &c), "no system file");
  CHECK (g_strcmp0 (c, "C 4\n") == 0, "user file alone");
  g_free (c);
  CHECK (!tts_config_read (NULL, &c) && c == NULL, "NULL path");
  CHECK (!tts_config_read ("/nonexistent/x.conf", &c) && c == NULL, "missing file");

  g_unlink (sysf); g_unlink (usrf); g_unlink (other);
  g_rmdir (sys); g_rmdir (usr); g_rmdir (tmp);
  g_free (sysf); g_free (usrf); g_free (other);
  g_free (sys); g_free (usr); g_free (tmp);
  if (!fail)
    g_print ("ok\n");
  return fail;
}
