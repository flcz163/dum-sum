#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/limits.h>

int rm_main(int argc, char *argv[])
{
	int c;
	char *p;
	struct stat sb;

	if (optind == argc) {
		fprintf(stderr, "Usage: %s [-f] source dest\n", argv[0]);
		return 1;
	}

	for (c = 1; c < argc; c++) {
		char target[PATH_MAX];

		p = strrchr(argv[c], '/');
		p++;

		if (S_ISDIR(sb.st_mode))
			snprintf(target, PATH_MAX, "%s/%s", argv[argc - 1], p);
		else
			snprintf(target, PATH_MAX, "%s", argv[argc - 1]);

		unlink(target);
	}

	return 0;
}
