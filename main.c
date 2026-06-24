/* glacier — display server + platform diagnostics, one binary.
 *
 *   glacier wm [/dev/dri/cardN]        the display server (Phase 2)
 *   glacier <enum|firstlight|flip|gl|seatd|hotplug> [/dev/dri/cardN]
 *                                      platform bring-up diagnostics
 *
 * enum/hotplug take no DRM master (safe under a live session); the modeset
 * diagnostics and `wm` must run from a bare VT (see README). */
#include "diagnostics.h"
#include "server.h"

#include <stdio.h>
#include <string.h>

struct cmd {
	const char *name;
	int (*run)(int, char **);
	const char *desc;
};

static const struct cmd cmds[] = {
	{ "wm",         server_run,      "display server: WM + CPU compositor (Phase 2)" },
	{ "enum",       diag_enumerate,  "diag: resource enumeration (no master)" },
	{ "firstlight", diag_firstlight, "diag: dumb-buffer atomic modeset, solid color" },
	{ "flip",       diag_pageflip,   "diag: double-buffer page-flip loop (vsync)" },
	{ "gl",         diag_gl,         "diag: GBM/EGL/GLES triangle, scanned out" },
	{ "seatd",      diag_seat,       "diag: seatd session + VT switching" },
	{ "hotplug",    diag_hotplug,    "diag: udev DRM hotplug monitor (no master)" },
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
