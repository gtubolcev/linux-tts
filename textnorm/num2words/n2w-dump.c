/* n2w-dump < numbers > c.txt: n2w-ru over all option combos; see pyn2w-dump.py */
#include <stdio.h>
#include <string.h>

#include "n2w-ru.h"

int main(void)
{
	char line[256], out[1024];
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		for (int ord = 0; ord < 2; ord++)
			for (int c = 0; c < 6; c++)
				for (int g = 0; g < 3; g++)
					for (int pl = 0; pl < 2; pl++)
						for (int an = 1; an >= 0; an--) {
							n2w_opts o = {(enum n2w_case)c, (enum n2w_gender)g, pl, an};
							int n = ord ? n2w_ru_ordinal(line, o, out, sizeof out)
							            : n2w_ru_cardinal(line, o, out, sizeof out);
							if (n < 0 || (size_t)n >= sizeof out)
								puts("!");
							else
								puts(out);
						}
	}
	return 0;
}
