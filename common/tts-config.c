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

void
tts_config_paths (const char *configfile, gchar **user, gchar **sys)
{
  const char *dir = g_getenv ("TTS_SYS_CONF_DIR");
  gchar *base;

  if (dir == NULL || *dir == '\0')
    dir = TTS_SYS_CONF_DIR;
  base = g_path_get_basename (configfile);
  *sys = g_build_filename (dir, base, NULL);
  /* speech-dispatcher gives the system file when the user had none at its
   * start; the user's file may come later (the settings dialog) */
  if (same_file (configfile, *sys))
    *user = g_build_filename (g_get_user_config_dir (), "speech-dispatcher", "modules", base, NULL);
  else
    *user = g_strdup (configfile);
  g_free (base);
}

gboolean
tts_config_read (const char *configfile, gchar **contents)
{
  gchar *user = NULL, *sys = NULL, *upath, *spath;

  *contents = NULL;
  if (configfile == NULL)
    return FALSE;
  tts_config_paths (configfile, &upath, &spath);
  if (!g_file_get_contents (upath, &user, NULL, NULL))
    user = NULL;
  if (!g_file_get_contents (spath, &sys, NULL, NULL))
    sys = NULL;
  if (sys != NULL && user != NULL)
    *contents = g_strconcat (sys, "\n# ---- ", upath, "\n", user, NULL);
  else
    *contents = g_strdup (sys != NULL ? sys : user);
  g_free (sys);
  g_free (user);
  g_free (upath);
  g_free (spath);
  return *contents != NULL;
}
