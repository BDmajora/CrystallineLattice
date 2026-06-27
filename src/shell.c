/* glacier — Wine desktop-shell launcher (see shell.h).
 *
 * Ported from frostedglass src/fg_wine.c. The setup sequence is the same —
 * wineboot --init, synthesize event sounds, import registry prefs — but the
 * client transport changes: instead of $WAYLAND_DISPLAY we export
 * $GLACIER_SOCKET and force the Wine graphics driver to "drm" (winedrm.drv,
 * Path β), so the shell renders through CrystallineLattice rather than the
 * Wayland frontend. The shell programs (explorer's taskbar/Start, the wallpaper
 * painter) come up as separate top-levels, each tagged with a role by the
 * driver; glacier draws the wallpaper itself, so a desktop.exe is optional. */
#define _GNU_SOURCE
#include "shell.h"
#include "protocol.h"   /* CL_SOCKET_ENV */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Run a command to completion (child context, before exec). Returns the exit
 * status, or -1 on spawn failure. SIGCHLD must be SIG_DFL here (we reset it in
 * the launcher child) so waitpid() can reap. */
static int run_wait(const char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Create the Wine prefix on first run (no ~/.wine/system.reg yet). */
static void ensure_wine_prefix(void)
{
	const char *home = getenv("HOME");
	char probe[512];
	if (!home)
		return;
	snprintf(probe, sizeof(probe), "%s/.wine/system.reg", home);
	if (access(probe, F_OK) == 0)
		return;
	fprintf(stderr, "glacier-shell: initializing Wine prefix (wineboot --init)\n");
	run_wait((const char *[]){ "wineboot", "--init", NULL });
}

/* Synthesize the system event sounds into C:\windows\Media (idempotent). */
static void ensure_prefix_media(void)
{
	const char *home = getenv("HOME");
	char media_dir[512], probe[600];
	if (!home)
		return;
	snprintf(media_dir, sizeof(media_dir),
	         "%s/.wine/drive_c/windows/Media", home);
	snprintf(probe, sizeof(probe), "%s/ding.wav", media_dir);
	if (access(probe, F_OK) == 0)
		return;
	run_wait((const char *[]){ "python3", "/usr/lib/yetios/gen-media.py",
	                           media_dir, NULL });
}

/* Force the Wine graphics driver to winedrm.drv and apply optional user prefs,
 * then kill wineserver so the changes are flushed before explorer starts. */
static void configure_registry(void)
{
	const char *home = getenv("HOME");
	char reg_path[512];

	/* HKCU\Software\Wine\Drivers "Graphics" = "drm" — overrides any stale
	 * "wayland"/"x11" value so the shell always takes Path β. */
	run_wait((const char *[]){ "wine", "reg", "add",
	                           "HKCU\\Software\\Wine\\Drivers",
	                           "/v", "Graphics", "/t", "REG_SZ",
	                           "/d", "drm", "/f", NULL });

	/* Optional appearance/sound prefs, if the user dropped a file in place. */
	if (home) {
		snprintf(reg_path, sizeof(reg_path), "%s/.glacier_prefs.reg", home);
		if (access(reg_path, R_OK) != 0)
			snprintf(reg_path, sizeof(reg_path),
			         "%s/.frostedglass_prefs.reg", home);
		if (access(reg_path, R_OK) == 0) {
			fprintf(stderr, "glacier-shell: importing %s\n", reg_path);
			run_wait((const char *[]){ "wine", "regedit", "/s", reg_path, NULL });
		}
	}

	/* Flush the registry to disk; explorer starts a fresh wineserver. */
	run_wait((const char *[]){ "wineserver", "--kill", NULL });
}

/* Fork a fire-and-forget daemon (no wait; the server's SIG_IGN SIGCHLD reaps
 * it), output to a log file. Returns the pid or -1. */
static pid_t spawn_daemon(const char *bin, const char *logpath)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		setsid();
		int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		execlp(bin, bin, (char *)NULL);
		_exit(127);
	}
	return pid;
}

/* Bring up the PipeWire audio stack so Wine's winepipewire.drv finds a live
 * daemon when mmdevapi probes (DESIGN.md keeps PipeWire as-is; this is just the
 * session bring-up frostedglass's fg_audio.c used to do). Must run before the
 * Wine shell, since the driver probes exactly once at explorer startup. */
void shell_start_audio(void)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	const char *home = getenv("HOME");
	const char *base = (home && home[0]) ? home : "/tmp";
	char sock[512], log[512];
	bool ready = false;

	snprintf(sock, sizeof(sock), "%s/pipewire-0", xdg ? xdg : "/tmp");
	if (access(sock, F_OK) == 0) {
		LOG_INFO("audio: PipeWire already running (%s)", sock);
		return;
	}

	snprintf(log, sizeof(log), "%s/glacier-pipewire.log", base);
	if (spawn_daemon("pipewire", log) <= 0) {
		LOG_WARN("audio: could not spawn pipewire; Wine audio will be silent");
		return;
	}
	LOG_INFO("audio: pipewire launched");

	/* The session manager and Wine both need the client socket present. */
	for (int i = 0; i < 60 && !ready; i++) {
		if (access(sock, F_OK) == 0)
			ready = true;
		else {
			struct timespec ts = { .tv_nsec = 50 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	if (!ready) {
		LOG_WARN("audio: pipewire socket %s never appeared (see %s)", sock, log);
		return;
	}

	snprintf(log, sizeof(log), "%s/glacier-wireplumber.log", base);
	spawn_daemon("wireplumber", log);     /* builds the session / device graph */
	snprintf(log, sizeof(log), "%s/glacier-pipewire-pulse.log", base);
	spawn_daemon("pipewire-pulse", log);  /* PulseAudio shim for pulse-API apps */
	LOG_INFO("audio: wireplumber + pipewire-pulse launched");
}

void shell_launch(int width, int height, const char *cl_socket_path)
{
	pid_t pid = fork();
	if (pid < 0) {
		LOG_WARN("shell: fork failed; no Wine desktop");
		return;
	}
	if (pid > 0) {
		LOG_INFO("shell: Wine desktop launching (pid %d, %dx%d) via %s",
		         pid, width, height, cl_socket_path ? cl_socket_path : "(default)");
		return;   /* server_run sets SIGCHLD=SIG_IGN, so the child auto-reaps */
	}

	/* ---- child ---- */
	setsid();
	/* Restore default SIGCHLD so the setup steps' waitpid() works (the server
	 * left it SIG_IGN for fire-and-forget reaping). */
	signal(SIGCHLD, SIG_DFL);

	/* Speak Path β to glacier; make sure nothing lures Wine onto X or the
	 * Wayland frontend instead of winedrm.drv. */
	if (cl_socket_path && cl_socket_path[0])
		setenv(CL_SOCKET_ENV, cl_socket_path, 1);
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("DISPLAY");

	/* Capture all Wine output for diagnosis. */
	const char *home = getenv("HOME");
	char logpath[512];
	snprintf(logpath, sizeof(logpath), "%s/glacier-wine.log",
	         (home && home[0]) ? home : "/tmp");
	int lfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (lfd >= 0) {
		dup2(lfd, 1);
		dup2(lfd, 2);
		if (lfd > 2)
			close(lfd);
	}

	ensure_wine_prefix();
	ensure_prefix_media();
	configure_registry();

	char desktop_arg[64];
	snprintf(desktop_arg, sizeof(desktop_arg), "/desktop=shell,%dx%d",
	         width, height);
	execlp("wine", "wine", "explorer", desktop_arg, (char *)NULL);

	fprintf(stderr, "glacier-shell: failed to exec wine explorer\n");
	_exit(127);
}
