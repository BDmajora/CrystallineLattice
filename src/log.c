/* glacier — leveled logging to stderr AND a per-user log file.
 *
 * The file copy exists because glacier is normally launched by the snowfall
 * greeter, which owns/redirects the session's stderr; when the session dies
 * the on-screen output is gone. A durable log at $HOME/glacier.log (fallback
 * /tmp/glacier.log) survives, so a failed boot can be diagnosed offline with:
 *     sudo cat /mnt/home/<user>/glacier.log        (image mounted at /mnt)
 */
#define _GNU_SOURCE
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static FILE *g_logfile;
static int   g_logfile_tried;

static FILE *logfile(void)
{
	if (g_logfile || g_logfile_tried)
		return g_logfile;
	g_logfile_tried = 1;

	const char *home = getenv("HOME");
	char path[512];
	if (home && *home)
		snprintf(path, sizeof(path), "%s/glacier.log", home);
	else
		snprintf(path, sizeof(path), "/tmp/glacier.log");

	g_logfile = fopen(path, "a");
	if (!g_logfile)
		return NULL;
	/* Line-buffered so the last line before a crash/exit reaches disk. */
	setvbuf(g_logfile, NULL, _IOLBF, 0);

	time_t t = time(NULL);
	struct tm tm;
	char ts[32] = "?";
	if (localtime_r(&t, &tm))
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(g_logfile, "\n=== glacier session start: %s (pid %d) ===\n",
	        ts, (int)getpid());
	return g_logfile;
}

void log_msg(const char *level, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "[%s] ", level);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	FILE *f = logfile();
	if (f) {
		fprintf(f, "[%s] ", level);
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
	}
}
