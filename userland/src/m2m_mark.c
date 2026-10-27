/*
 * m2m_mark: one record in mali2metal's channel from a script (m2m.h), so a run's records can be told
 * apart by what the phone was doing: "I MARK step 2/6 Settings".
 *
 *   m2m_mark [-s I|W|E] [-c CAT] <text...>
 */
#include <stdlib.h>
#include <string.h>

#include "m2m.h"

int
main(int argc, char **argv)
{
	char sev = 'I', text[M2M_RECORD_MAX] = "";
	const char *cat = "MARK";
	int i = 1;

	for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
		if (strcmp(argv[i], "-s") == 0) {
			sev = argv[i + 1][0];
		} else if (strcmp(argv[i], "-c") == 0) {
			cat = argv[i + 1];
		} else {
			break;
		}
	}
	for (; i < argc; i++) {
		strlcat(text, argv[i], sizeof(text));
		if (i + 1 < argc) {
			strlcat(text, " ", sizeof(text));
		}
	}
	m2m_log(sev, cat, "%s", text);
	return 0;
}
