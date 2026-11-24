/*
 * tts-norm-test - normalizes stdin line by line through the helper
 *
 *   tts-norm-test [--engine c|python] [--data DIR] [--helper PATH] [--python PATH] [--zt LIB]
 *                 [--timeout MS] [--idle S] [--lang ru]
 *                 [--stress DICT] [--mark plus|acute] [--cancel-after MS]
 *
 * Prints "<ms>\t<normalized line>" for each line, so the timeouts and the
 * fallbacks of tts-norm.c can be tried with broken helpers (--engine python).
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "tts-norm.h"
#include "tts-stress.h"

/* --cancel-after: the line counts as stopped this long after it started */
static gint64 line_start, cancel_after_us = -1;

static gboolean
cancel_line (void)
{
  return g_get_monotonic_time () - line_start >= cancel_after_us;
}

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
      if (strcmp (argv[i], "--engine") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationEngine", argv[i + 1]);
      else if (strcmp (argv[i], "--data") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationData", argv[i + 1]);
      else if (strcmp (argv[i], "--helper") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationHelper", argv[i + 1]);
      else if (strcmp (argv[i], "--zt") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationLibrary", argv[i + 1]);
      else if (strcmp (argv[i], "--python") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationPython", argv[i + 1]);
      else if (strcmp (argv[i], "--timeout") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationTimeout", argv[i + 1]);
      else if (strcmp (argv[i], "--idle") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationIdleTimeout", argv[i + 1]);
      else if (strcmp (argv[i], "--start-timeout") == 0)
        tts_norm_config_parse (&cfg, "TextNormalizationStartTimeout", argv[i + 1]);
      else if (strcmp (argv[i], "--stress") == 0)
        {
          tts_stress_config_parse (&scfg, "TextStress", "yes");
          tts_stress_config_parse (&scfg, "TextStressDictionary", argv[i + 1]);
        }
      else if (strcmp (argv[i], "--mark") == 0)
        tts_stress_config_parse (&scfg, "TextStressMark", argv[i + 1]);
      else if (strcmp (argv[i], "--homographs") == 0)
        tts_stress_config_parse (&scfg, "TextStressHomographs", argv[i + 1]);
      else if (strcmp (argv[i], "--homographs-dir") == 0)
        tts_stress_config_parse (&scfg, "TextStressHomographsDir", argv[i + 1]);
      else if (strcmp (argv[i], "--cancel-after") == 0)
        cancel_after_us = g_ascii_strtoll (argv[i + 1], NULL, 10) * 1000;
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
  if (n != NULL && cancel_after_us >= 0)
    tts_norm_set_cancel (n, cancel_line);
  while (fgets (line, sizeof line, stdin) != NULL)
    {
      gint64 t0 = line_start = g_get_monotonic_time ();
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
