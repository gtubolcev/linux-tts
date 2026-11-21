/* phone-check < numbers: rn_phone_international per line, as phone-check.py */
#include <stdio.h>
#include <string.h>

#include "rn-phone.h"

int main(void)
{
	char line[256], g[256];
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\n")] = 0;
		int cc, r = rn_phone_international(line, &cc, g, sizeof g);
		if (r < 0)
			printf("%s\tERR\n", line);
		else if (r)
			printf("%s\tOK %s\n", line, g);
		else
			printf("%s\tNO\n", line);
	}
	return 0;
}
