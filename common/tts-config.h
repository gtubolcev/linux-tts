/* tts-config: the configuration file of a module, the system file with the
 * user's file on top.  MIT. */
#pragma once

#include <glib.h>

/* Reads CONFIGFILE, the path speech-dispatcher gives the module.  When it is
 * the user's file (~/.config/speech-dispatcher/modules/NAME.conf) and the
 * system file of the same name exists, the result is the system file followed
 * by the user's one: the modules take the last value of a key, so the user's
 * file only needs the keys it changes, and options added to the system file
 * later still reach the user.  FALSE when CONFIGFILE cannot be read. */
gboolean tts_config_read (const char *configfile, gchar **contents);
