/*
 * teratts-test - TeraTTS from the command line, writes raw float32 samples
 *
 *   teratts-test [options] MODEL-DIR VOICE TEXT OUT.f32
 *     -r        TEXT is model text already (tags, "+", no digits)
 *     -l LANG   language of text without letters (ru)
 *     -d DICT   sorted Vosk dictionary for the stress marks
 *     -n FILE   initial noise (float32, from the Python reference)
 *     -s SCALE  duration scale (1.0)
 *     -t N      threads (4)
 *     -p        print the model text and exit
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: MIT
 */

#include "teratts.h"
#include "ru-stress.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static gchar *
stress_cb (const gchar *text, gpointer data)
{
  return ru_stress_apply (data, text, RU_STRESS_PLUS);
}

int
main (int argc, char **argv)
{
  const char *lang = "ru", *dict = NULL, *noise_path = NULL;
  gboolean raw = FALSE, print_only = FALSE;
  double scale = 1.0;
  int threads = 4, opt;
  GError *error = NULL;
  RuStress *rs = NULL;
  TeraTts *t;
  TeraVoice *v;
  gchar *text, *noise = NULL;
  gsize n_noise = 0, n = 0;
  gfloat *wav;
  gint64 t0;

  while ((opt = getopt (argc, argv, "rl:d:n:s:t:p")) != -1)
    switch (opt)
      {
      case 'r': raw = TRUE; break;
      case 'l': lang = optarg; break;
      case 'd': dict = optarg; break;
      case 'n': noise_path = optarg; break;
      case 's': scale = g_ascii_strtod (optarg, NULL); break;
      case 't': threads = atoi (optarg); break;
      case 'p': print_only = TRUE; break;
      default: return 2;
      }
  if (argc - optind != 4)
    {
      fprintf (stderr, "usage: %s [-r] [-l LANG] [-d DICT] [-n NOISE] [-s SCALE] [-t N] [-p]"
               " MODEL-DIR VOICE TEXT OUT.f32\n", argv[0]);
      return 2;
    }
  if (dict != NULL && (rs = ru_stress_new (dict, &error)) == NULL)
    goto fail;
  t0 = g_get_monotonic_time ();
  if ((t = teratts_new (argv[optind], threads, &error)) == NULL)
    goto fail;
  fprintf (stderr, "load %.2f s\n", (g_get_monotonic_time () - t0) / 1e6);
  text = raw ? g_utf8_normalize (argv[optind + 2], -1, G_NORMALIZE_NFKD)
             : teratts_prepare (t, argv[optind + 2], lang, rs != NULL ? stress_cb : NULL, rs);
  if (text == NULL)
    {
      fprintf (stderr, "nothing to say\n");
      return 1;
    }
  {
    gchar *nfc = g_utf8_normalize (text, -1, G_NORMALIZE_NFC);
    printf ("%s\n", nfc);
    g_free (nfc);
  }
  if (print_only)
    return 0;
  if ((v = teratts_voice_load (argv[optind], argv[optind + 1], &error)) == NULL)
    goto fail;
  if (noise_path != NULL && !g_file_get_contents (noise_path, &noise, &n_noise, &error))
    goto fail;
  t0 = g_get_monotonic_time ();
  wav = teratts_synth (t, v, text, scale, 1234, (gfloat *) noise, n_noise / sizeof (gfloat),
                       &n, &error);
  if (wav == NULL)
    goto fail;
  fprintf (stderr, "%zu samples (%.2f s audio) in %.2f s\n", n, (double) n / TERATTS_SAMPLE_RATE,
           (g_get_monotonic_time () - t0) / 1e6);
  if (!g_file_set_contents (argv[optind + 3], (gchar *) wav, n * sizeof (gfloat), &error))
    goto fail;
  g_free (wav);
  g_free (noise);
  g_free (text);
  teratts_voice_free (v);
  teratts_free (t);
  ru_stress_free (rs);
  return 0;
fail:
  fprintf (stderr, "%s\n", error->message);
  return 1;
}
