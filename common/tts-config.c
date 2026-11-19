/* tts-config: the configuration file of a module, the system file with the
 * user's file on top.  MIT. */
#include "tts-config.h"

#ifndef TTS_SYS_CONF_DIR
#define TTS_SYS_CONF_DIR "/etc/speech-dispatcher/modules"
#endif

static gboolean
same_file (const char *a, const char *b)
{
  gchar *ra = g_canonicalize_filename (a, NULL);
  gchar *rb = g_canonicalize_filename (b, NULL);
  gboolean same = g_strcmp0 (ra, rb) == 0;

  g_free (ra);
  g_free (rb);
  return same;
}

gboolean
tts_config_read (const char *configfile, gchar **contents)
{
  const char *dir = g_getenv ("TTS_SYS_CONF_DIR");
  gchar *user = NULL, *sys = NULL, *base, *syspath;

  *contents = NULL;
  if (configfile == NULL || !g_file_get_contents (configfile, &user, NULL, NULL))
    return FALSE;

  if (dir == NULL || *dir == '\0')
    dir = TTS_SYS_CONF_DIR;
  base = g_path_get_basename (configfile);
  syspath = g_build_filename (dir, base, NULL);
  if (!same_file (configfile, syspath)
      && g_file_get_contents (syspath, &sys, NULL, NULL))
    {
      *contents = g_strconcat (sys, "\n# ---- ", configfile, "\n", user, NULL);
      g_free (sys);
      g_free (user);
    }
  else
    *contents = user;
  g_free (syspath);
  g_free (base);
  return TRUE;
}
