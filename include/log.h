/* glacier — leveled logging to stderr (no platform deps). */
#ifndef GLACIER_LOG_H
#define GLACIER_LOG_H

void log_msg(const char *level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define LOG_INFO(...) log_msg("INFO", __VA_ARGS__)
#define LOG_WARN(...) log_msg("WARN", __VA_ARGS__)
#define LOG_ERR(...)  log_msg("ERR ", __VA_ARGS__)

#endif /* GLACIER_LOG_H */
