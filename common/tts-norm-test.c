/*
 * tts-norm-test - normalizes stdin line by line through the helper
 *
 *   tts-norm-test [--helper PATH] [--python PATH] [--timeout MS] [--lang ru]
 *                 [--stress DICT] [--mark plus|acute]
 *
 * Prints "<ms>\t<normalized line>" for each line, so the timeouts and the
 * fallbacks of tts-norm.c can be tried with broken helpers.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "tts-norm.h"
#include "tts-stress.h"

int
main (int argc, char **argv)
{
  TtsNormConfig cfg;
  TtsNorm *n;
  TtsStressConfig scfg;
  TtsStress *st;
  const gchar *lang = "ru";
  gchar line[1 << 16];

  tts_norm_config_init (&cfg);
  tts_stress_config_init (&scfg, FALSE, RU_STRESS_PLUS);
  for (int i = 1; i + 1 < argc; i += 2)
    {
      if (strcmp (argv[i], "--helper") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationHelper", argv[i + 1]);
      else if (strcmp (argv[i], "--python") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationPython", argv[i + 1]);
      else if (strcmp (argv[i], "--timeout") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationTimeout", argv[i + 1]);
      else if (strcmp (argv[i], "--start-timeout") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationStartTimeout", argv[i + 1]);
      else if (strcmp (argv[i], "--stress") == 0)
        {
          tts_stress_config_parse (&scfg, "TextStress", "yes");
          tts_stress_config_parse (&scfg, "TextStressDictionary", argv[i + 1]);
        }
      else if (strcmp (argv[i], "--mark") == 0)
        tts_stress_config_parse (&scfg, "TextStressMark", argv[i + 1]);
      else if (strcmp (argv[i], "--lang") == 0)
        lang = argv[i + 1];
      else
        {
          g_printerr ("unknown option %s\n", argv[i]);
          return 2;
        }
    }
  n = tts_norm_new (&cfg);
  st = tts_stress_new (&scfg);
  while (fgets (line, sizeof line, stdin) != NULL)
    {
      gint64 t0 = g_get_monotonic_time ();
      gchar *out;

      line[strcspn (line, "\n")] = '\0';
      out = tts_norm_text (n, line, lang);
      {
        gchar *marked = tts_stress_apply (st, out, lang);
        g_free (out);
        out = marked;
      }
      printf ("%d\t%s\n", (int) ((g_get_monotonic_time () - t0) / 1000), out);
      fflush (stdout);
      g_free (out);
    }
  tts_norm_free (n);
  tts_stress_free (st);
  tts_stress_config_clear (&scfg);
  tts_norm_config_clear (&cfg);
  return 0;
}
