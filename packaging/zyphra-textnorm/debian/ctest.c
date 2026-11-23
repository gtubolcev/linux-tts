/* smoke test of the C ABI: a few languages, a known answer each */
#include <stdio.h>
#include <string.h>
#include "zyphra_textnorm.h"

int
main (void)
{
  static const char *t[][3] = {
    { "en", "It costs $5.32.", "It costs five dollars thirty two cents." },
    { "de", "Am 3. Oktober kostete es 5,32 €.", "Am dritten Oktober kostete es fünf Euro zweiunddreißig." },
  };
  int bad = 0;
  zyphra_textnorm *tn = zyphra_textnorm_new (NULL, 0);

  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    {
      char *out = zyphra_textnorm_normalize (tn, t[i][1], strlen (t[i][1]), t[i][0], 2);
      if (strcmp (out, t[i][2]) != 0)
        printf ("FAIL %s: %s -> %s\n", t[i][0], t[i][1], out), bad = 1;
      zyphra_textnorm_free (out);
    }
  if (!zyphra_textnorm_supported ("cs", 2) || zyphra_textnorm_supported ("xx", 2))
    printf ("FAIL supported()\n"), bad = 1;
  zyphra_textnorm_delete (tn);
  return bad;
}
