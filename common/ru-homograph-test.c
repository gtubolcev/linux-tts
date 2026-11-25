/*
 * ru-homograph-test - checks ru-homograph and hf-tok against a dump of
 * RUAccent itself (tests/ruaccent-dump.py: per sentence the model
 * texts, variants, token ids and choices; then raw tokenizer pairs)
 *
 *   ru-homograph-test DUMP.jsonl tiny|turbo RUACCENT_DIR [-v]
 *   ru-homograph-test --check-rules RUACCENT_DIR
 *   ru-homograph-test --yo YODUMP.jsonl RUACCENT_DIR [-v]
 *
 * The rules (ru-homograph-rules) are off here: the model alone is compared;
 * --check-rules checks each rule gives a variant of omographs.json.gz.
 * --yo checks the ё stage against tests/ruaccent-yo-dump.py: tokens with
 * offsets, and the YO/NO_YO of every word of yo_homographs.json.gz.
 *
 * Words RUAccent scores but we keep on purpose (a capital inside the
 * sentence) are counted apart, not as errors.
 *
 * Copyright (C) 2026 Grigory Tuboltsev
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <json-glib/json-glib.h>
#include <string.h>

#include "hf-tok.h"
#include "ru-homograph.h"

typedef struct
{
  gchar *text, *chosen;
  gchar **variants;
  GPtrArray *ids;               /* GArray of gint64 per variant */
} Entry;

static GPtrArray *got;
static gboolean verbose;

static void
entry_free (gpointer p)
{
  Entry *e = p;
  g_free (e->text);
  g_free (e->chosen);
  g_strfreev (e->variants);
  g_ptr_array_unref (e->ids);
  g_free (e);
}

static void
trace (const gchar *text, gchar **variants, GArray **ids, const float *probs, int chosen,
       gpointer data)
{
  Entry *e = g_new0 (Entry, 1);

  e->text = g_strdup (text);
  e->variants = g_strdupv (variants);
  e->chosen = g_strdup (variants[chosen]);
  e->ids = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref);
  for (guint i = 0; variants[i] != NULL; i++)
    g_ptr_array_add (e->ids, g_array_ref (ids[i]));
  g_ptr_array_add (got, e);
}

static gboolean
ids_equal (GArray *a, JsonArray *b)
{
  if (a->len != json_array_get_length (b))
    return FALSE;
  for (guint i = 0; i < a->len; i++)
    if (g_array_index (a, gint64, i) != json_array_get_int_element (b, i))
      return FALSE;
  return TRUE;
}

static gchar *
ids_str (GArray *a)
{
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < a->len; i++)
    g_string_append_printf (s, "%s%" G_GINT64_FORMAT, i ? "," : "", g_array_index (a, gint64, i));
  return g_string_free (s, FALSE);
}

static gchar *
json_ids_str (JsonArray *b)
{
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < json_array_get_length (b); i++)
    g_string_append_printf (s, "%s%" G_GINT64_FORMAT, i ? "," : "", json_array_get_int_element (b, i));
  return g_string_free (s, FALSE);
}

typedef struct
{
  gchar *text, *word;
  glong start;
  gboolean yo;
} YoEntry;

static void
yo_entry_free (gpointer p)
{
  YoEntry *e = p;
  g_free (e->text);
  g_free (e->word);
  g_free (e);
}

static void
yo_trace (const gchar *text, glong start, const gchar *word, gboolean yo, gpointer data)
{
  YoEntry *e = g_new0 (YoEntry, 1);

  e->text = g_strdup (text);
  e->word = g_strdup (word);
  e->start = start;
  e->yo = yo;
  g_ptr_array_add (got, e);
}

/* ids and offsets of the tokenizer against the reference */
static gboolean
spans_equal (HfTok *tok, const gchar *text, JsonObject *o)
{
  GArray *spans, *ids = hf_tok_encode_spans (tok, text, 512, &spans);
  JsonArray *rids = json_object_get_array_member (o, "ids");
  JsonArray *roff = json_object_get_array_member (o, "offsets");
  gboolean ok = ids != NULL && ids_equal (ids, rids) && json_array_get_length (roff) == spans->len;

  for (guint i = 0; ok && i < spans->len; i++)
    {
      JsonArray *r = json_array_get_array_element (roff, i);
      HfSpan *sp = &g_array_index (spans, HfSpan, i);

      ok = json_array_get_int_element (r, 0) == sp->start && json_array_get_int_element (r, 1) == sp->end;
    }
  if (!ok && verbose)
    {
      gchar *a = ids ? ids_str (ids) : g_strdup ("NULL"), *b = json_ids_str (rids);
      g_print ("TOK   [%s]\n  ours: %s\n  ref:  %s\n", text, a, b);
      for (guint i = 0; ids && i < spans->len; i++)
        g_print (" %ld-%ld%s", g_array_index (spans, HfSpan, i).start, g_array_index (spans, HfSpan, i).end,
                 g_array_index (spans, HfSpan, i).subword ? "#" : "");
      g_print ("\n");
      g_free (a);
      g_free (b);
    }
  if (ids != NULL)
    {
      g_array_free (ids, TRUE);
      g_array_free (spans, TRUE);
    }
  return ok;
}

static int
yo_main (const gchar *dump, const gchar *dir)
{
  RuHomograph *h = ru_homograph_new (dir, "tiny", 2, 0);
  gchar *text, **lines, *mdir = g_build_filename (dir, "nn", "nn_yo_homograph_resolver", NULL);
  HfTok *tok;
  GError *error = NULL;

  /* the installed layout: dir/tiny/yo */
  if (!g_file_test (mdir, G_FILE_TEST_IS_DIR))
    {
      g_free (mdir);
      mdir = g_build_filename (dir, "tiny", "yo", NULL);
    }
  int sent = 0, n_ref = 0, n_ok = 0, missing = 0, differ = 0, extra = 0, tok_n = 0, tok_bad = 0;

  ru_homograph_set_rules (h, FALSE);
  ru_homograph_set_yo_trace (h, yo_trace, NULL);
  if ((tok = hf_tok_load (mdir, &error)) == NULL || !g_file_get_contents (dump, &text, NULL, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  lines = g_strsplit (text, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      JsonParser *p;
      JsonObject *o;

      if (**l == '\0')
        continue;
      p = json_parser_new ();
      if (!json_parser_load_from_data (p, *l, -1, NULL))
        {
          g_object_unref (p);
          continue;
        }
      o = json_node_get_object (json_parser_get_root (p));
      if (json_object_has_member (o, "yo"))
        {
          const gchar *s = json_object_get_string_member (o, "yo");
          const gchar *rt = json_object_get_string_member (o, "text");
          JsonArray *ref = json_object_get_array_member (o, "words");
          gchar *res;
          guint used = 0;

          tok_n++;
          if (!spans_equal (tok, rt, o))
            tok_bad++;
          got = g_ptr_array_new_with_free_func (yo_entry_free);
          res = ru_homograph_resolve (h, s, s, RU_STRESS_PLUS);
          sent++;
          for (guint i = 0; i < json_array_get_length (ref); i++)
            {
              JsonArray *r = json_array_get_array_element (ref, i);
              glong start = json_array_get_int_element (r, 0);
              const gchar *w = json_array_get_string_element (r, 1);
              gboolean ryo = strcmp (json_array_get_string_element (r, 2), "YO") == 0;
              YoEntry *e = NULL;

              n_ref++;
              for (guint k = 0; k < got->len; k++)
                {
                  YoEntry *x = got->pdata[k];
                  if (x->start == start && strcmp (x->text, rt) == 0)
                    e = x;
                }
              if (e == NULL)
                {
                  missing++;
                  if (verbose || missing <= 10)
                    g_print ("MISS  %s @%ld in %s\n", w, start, s);
                  continue;
                }
              used++;
              if (e->yo != ryo)
                {
                  differ++;
                  g_print ("YO    %s @%ld: ours %d, ref %d in %s\n", w, start, e->yo, ryo, s);
                  continue;
                }
              n_ok++;
            }
          extra += got->len - used;
          if (verbose && res != NULL)
            g_print ("OUT   %s\n", res);
          g_free (res);
          g_ptr_array_unref (got);
        }
      else if (json_object_has_member (o, "tok"))
        {
          tok_n++;
          if (!spans_equal (tok, json_object_get_string_member (o, "tok"), o))
            {
              tok_bad++;
              if (!verbose && tok_bad <= 5)
                g_print ("TOK   bad: %s\n", json_object_get_string_member (o, "tok"));
            }
        }
      g_object_unref (p);
    }
  g_print ("yo: %d sentences, %d words: %d same, %d differ, %d missing, %d extra; "
           "tokenizer %d/%d same\n", sent, n_ref, n_ok, differ, missing, extra, tok_n - tok_bad, tok_n);
  g_strfreev (lines);
  g_free (text);
  g_free (mdir);
  hf_tok_free (tok);
  ru_homograph_free (h);
  return differ + missing + tok_bad > 0;
}

int
main (int argc, char **argv)
{
  RuHomograph *h;
  gchar *text, **lines, *mdir;
  const gchar *size;
  HfTok *tok;
  GError *error = NULL;
  int sent = 0, n_ref = 0, n_ok = 0, bad_text = 0, bad_ids = 0, bad_choice = 0, skipped = 0,
      extra = 0, tok_n = 0, tok_bad = 0;
  gint64 t_us = 0;

  if (argc == 3 && strcmp (argv[1], "--check-rules") == 0)
    {
      int bad;

      h = ru_homograph_new (argv[2], "tiny", 1, 0);
      bad = ru_homograph_check_rules (h);
      g_print ("rules: %d bad\n", bad);
      ru_homograph_free (h);
      return bad != 0;
    }
  if (argc >= 4 && strcmp (argv[1], "--yo") == 0)
    {
      verbose = argc > 4 && strcmp (argv[4], "-v") == 0;
      return yo_main (argv[2], argv[3]);
    }
  if (argc < 4)
    {
      g_printerr ("usage: %s DUMP.jsonl tiny|turbo RUACCENT_DIR [-v]\n"
                  "       %s --check-rules RUACCENT_DIR\n"
                  "       %s --yo YODUMP.jsonl RUACCENT_DIR [-v]\n", argv[0], argv[0], argv[0]);
      return 2;
    }
  verbose = argc > 4 && strcmp (argv[4], "-v") == 0;
  size = strcmp (argv[2], "turbo") == 0 ? "turbo" : "tiny";
  h = ru_homograph_new (argv[3], size, 2, 0);
  ru_homograph_set_rules (h, FALSE);
  ru_homograph_set_yo (h, FALSE);     /* the dump has the ё stage off */
  ru_homograph_set_trace (h, trace, NULL);
  mdir = g_build_filename (argv[3], "nn", "nn_omograph", strcmp (size, "turbo") == 0 ? "turbo3.1" : "tiny2.1", NULL);
  if ((tok = hf_tok_load (mdir, &error)) == NULL)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  if (!g_file_get_contents (argv[1], &text, NULL, &error))
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  lines = g_strsplit (text, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      JsonParser *p;
      JsonObject *o;

      if (**l == '\0')
        continue;
      p = json_parser_new ();
      if (!json_parser_load_from_data (p, *l, -1, NULL))
        {
          g_object_unref (p);
          continue;
        }
      o = json_node_get_object (json_parser_get_root (p));
      if (json_object_has_member (o, "s"))
        {
          const gchar *s = json_object_get_string_member (o, "s");
          JsonArray *ref = json_object_get_array_member (o, size);
          gchar *res;
          gint64 t0;
          guint used = 0;

          got = g_ptr_array_new_with_free_func (entry_free);
          t0 = g_get_monotonic_time ();
          res = ru_homograph_resolve (h, s, s, RU_STRESS_PLUS);
          t_us += g_get_monotonic_time () - t0;
          sent++;
          for (guint i = 0; i < json_array_get_length (ref); i++)
            {
              JsonObject *r = json_array_get_object_element (ref, i);
              const gchar *rt = json_object_get_string_member (r, "text");
              JsonArray *rids = json_object_get_array_member (r, "ids");
              Entry *e = NULL;
              gboolean ok = TRUE;

              n_ref++;
              for (guint k = 0; k < got->len; k++)
                if (strcmp (((Entry *) got->pdata[k])->text, rt) == 0)
                  {
                    e = got->pdata[k];
                    break;
                  }
              if (e == NULL)
                {
                  /* the word wrapped: kept on purpose when it has a capital
                   * (RUAccent lower-cases it) */
                  const gchar *w = strstr (rt, "<w>"), *we = w ? strstr (w, "</w>") : NULL;
                  gchar *cap = NULL;

                  if (w != NULL && we != NULL)
                    {
                      gchar *low = g_strndup (w + 3, we - w - 3);
                      gunichar c = g_unichar_toupper (g_utf8_get_char (low));
                      gchar buf[8] = { 0 };

                      g_unichar_to_utf8 (c, buf);
                      cap = g_strconcat (buf, g_utf8_next_char (low), NULL);
                      g_free (low);
                    }
                  if (cap != NULL && strstr (s, cap) != NULL && !g_str_has_prefix (s, cap))
                    skipped++;
                  else
                    {
                      bad_text++;
                      if (verbose || bad_text <= 10)
                        g_print ("TEXT  %s\n  ref: %s\n", s, rt);
                    }
                  g_free (cap);
                  continue;
                }
              used++;
              for (guint k = 0; k < e->ids->len && k < json_array_get_length (rids); k++)
                if (!ids_equal (e->ids->pdata[k], json_array_get_array_element (rids, k)))
                  {
                    if (verbose || bad_ids < 10)
                      {
                        gchar *a = ids_str (e->ids->pdata[k]);
                        gchar *b = json_ids_str (json_array_get_array_element (rids, k));
                        g_print ("IDS   %s | %s\n  ours: %s\n  ref:  %s\n", rt, e->variants[k], a, b);
                        g_free (a);
                        g_free (b);
                      }
                    ok = FALSE;
                  }
              if (e->ids->len != json_array_get_length (rids))
                ok = FALSE;
              if (!ok)
                {
                  bad_ids++;
                  continue;
                }
              if (strcmp (e->chosen, json_object_get_string_member (r, "chosen")) != 0)
                {
                  bad_choice++;
                  g_print ("CHOICE %s: ours %s, ref %s\n", rt, e->chosen,
                           json_object_get_string_member (r, "chosen"));
                  continue;
                }
              n_ok++;
            }
          extra += got->len - used;
          if (verbose && res != NULL)
            g_print ("OUT   %s\n", res);
          g_free (res);
          g_ptr_array_unref (got);
        }
      else if (json_object_has_member (o, "tok"))
        {
          JsonArray *pairs = json_object_get_array_member (json_object_get_object_member (o, "tok"),
                                                           strcmp (size, "turbo") == 0 ? "turbo3.1" : "tiny2.1");
          for (guint i = 0; i < json_array_get_length (pairs); i++)
            {
              JsonArray *t = json_array_get_array_element (pairs, i);
              GArray *ids = hf_tok_encode_pair (tok, json_array_get_string_element (t, 0),
                                                json_array_get_string_element (t, 1), 512);
              tok_n++;
              if (!ids_equal (ids, json_array_get_array_element (t, 2)))
                {
                  tok_bad++;
                  if (verbose || tok_bad <= 5)
                    {
                      gchar *a = ids_str (ids), *b = json_ids_str (json_array_get_array_element (t, 2));
                      g_print ("TOK   [%s] [%s]\n  ours: %s\n  ref:  %s\n", json_array_get_string_element (t, 0),
                               json_array_get_string_element (t, 1), a, b);
                      g_free (a);
                      g_free (b);
                    }
                }
              g_array_unref (ids);
            }
        }
      g_object_unref (p);
    }
  g_print ("%s: %d sentences, %d homographs scored by RUAccent: %d same, %d text, %d ids, "
           "%d choice differ; %d kept (capital), %d extra; tokenizer %d/%d same; %.1f ms/sentence\n",
           size, sent, n_ref, n_ok, bad_text, bad_ids, bad_choice, skipped, extra, tok_n - tok_bad,
           tok_n, sent ? t_us / 1000.0 / sent : 0.0);
  g_strfreev (lines);
  g_free (text);
  g_free (mdir);
  hf_tok_free (tok);
  ru_homograph_free (h);
  return bad_text + bad_ids + bad_choice + tok_bad > 0;
}
