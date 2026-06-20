/* glacier — Phase 0 driver.
 * One binary, one subcommand per P0.x sub-phase (strict dependency order).
 *
 *   glacier-phase0 <enum|firstlight|flip|gl|seatd|hotplug> [/dev/dri/cardN]
 *
 * enum/hotplug take no DRM master (safe under a live session); the rest
 * modeset and must run from a bare VT (see README). */
#include "phases.h"

#include <stdio.h>
#include <string.h>

struct cmd {
	const char *name;
	int (*run)(int, char **);
	const char *desc;
};

static const struct cmd cmds[] = {
	{ "enum",       p0_1_enum_run,       "P0.1  resource enumeration (no master)" },
	{ "firstlight", p0_2_firstlight_run, "P0.2  dumb-buffer atomic modeset, solid color" },
	{ "flip",       p0_3_flip_run,       "P0.3  double-buffer page-flip loop (vsync)" },
	{ "gl",         p0_4_gl_run,         "P0.4  GBM/EGL/GLES triangle, scanned out" },
	{ "seatd",      p0_5_seatd_run,      "P0.5  seatd session + VT switching" },
	{ "hotplug",    p0_6_hotplug_run,    "P0.6  udev hotplug monitor (no master)" },
};

static int usage(const char *prog)
{
	fprintf(stderr, "usage: %s <command> [/dev/dri/cardN]\n\n", prog);
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		fprintf(stderr, "  %-11s %s\n", cmds[i].name, cmds[i].desc);
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage(argv[0]);

	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (strcmp(argv[1], cmds[i].name) == 0)
			/* Shift past the program name so the phase sees
			 * argv[0]=command, argv[1]=optional device path. */
			return cmds[i].run(argc - 1, argv + 1);

	fprintf(stderr, "unknown command: %s\n\n", argv[1]);
	return usage(argv[0]);
}
