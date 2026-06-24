/* glacier — leveled logging to stderr. */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void log_msg(const char *level, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "[%s] ", level);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
