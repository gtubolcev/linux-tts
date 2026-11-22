/* user-stress-test - take/put of the user's stress marks */
#include "user-stress.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void
check (const gchar *text, const gchar *want_plain, const gchar *normalized,
       const gchar *want)
{
  gchar *plain = NULL, *got;
  UserStress *u = user_stress_take (text, &plain);

  if (g_strcmp0 (plain ? plain : text, want_plain) != 0)
    {
      printf ("FAIL take %s: %s, want %s\n", text, plain ? plain : text, want_plain);
      fails++;
    }
  got = user_stress_put (u, normalized);
  if (strcmp (got, want) != 0)
    {
      printf ("FAIL put %s: %s, want %s\n", text, got, want);
      fails++;
    }
  g_free (got);
  g_free (plain);
  user_stress_free (u);
}

int
main (void)
{
  check ("нет ударений 5", "нет ударений 5", "нет ударений пять", "нет ударений пять");
  check ("за́мок 5 г.", "замок 5 г.", "замок пять граммов", "за́мок пять граммов");
  check ("зам+ок", "замок", "замок", "зам+ок");
  check ("+она и он+а", "она и она", "она и она", "+она и он+а");
  check ("ЗА́МОК", "ЗАМОК", "ЗАМОК", "ЗА́МОК");
  check ("За́мок", "Замок", "замок", "за́мок");     /* case of the output */
  check ("2+она", "2+она", "два плюс она", "два плюс она");   /* a sum */
  check ("x+он", "x+он", "x плюс он", "x плюс он");
  check ("́а", "́а", "́а", "́а");          /* a stray acute stays */
  check ("кни́га 21", "книга 21", "книга двадцать один", "кни́га двадцать один");
  /* a word the normalizer changed loses its mark, the next keeps it */
  check ("кни́га и до́м", "книга и дом", "книги и дом", "книги и до́м");
  /* the same word twice, marked once: the marked one */
  check ("дом до́м", "дом дом", "дом дом", "дом до́м");
  check ("дом, дом и до́м", "дом, дом и дом", "дом, дом и дом", "дом, дом и до́м");
  /* mixed forms: an acute makes all acute */
  check ("мн+огосло́жный", "многосложный", "многосложный", "мно́госло́жный");
  return fails != 0;
}
