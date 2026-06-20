/* P0.4 — GBM + EGL + GLES (the GPU path).
 * gbm_device -> gbm_surface -> EGL(GBM) -> GLES2 context; renders a
 * clearing background + triangle, locks the front bo, wraps it as a DRM
 * FB (with modifier when available), scans it out via the P0.3 flip loop.
 * The dumb-buffer CPU path in common.c remains the pixman fallback.
 * Run on a bare VT as root. */
#define _GNU_SOURCE
#include "common.h"
#include "phases.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <gbm.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

/* ---- DRM FB wrapper cached on the gbm_bo ----------------------------- */
struct fb_wrap { int fd; uint32_t fb_id; };
static void bo_destroy(struct gbm_bo *bo, void *data)
{
	(void)bo;
	struct fb_wrap *w = data;
	if (w->fb_id)
		drmModeRmFB(w->fd, w->fb_id);
	free(w);
}
static uint32_t fb_for_bo(int fd, struct gbm_bo *bo)
{
	struct fb_wrap *w = gbm_bo_get_user_data(bo);
	if (w)
		return w->fb_id;
	w = calloc(1, sizeof(*w));
	w->fd = fd;

	uint32_t width = gbm_bo_get_width(bo), height = gbm_bo_get_height(bo);
	uint32_t fmt = gbm_bo_get_format(bo);
	uint64_t mod = gbm_bo_get_modifier(bo);
	int planes = gbm_bo_get_plane_count(bo);
	uint32_t handles[4] = { 0 }, strides[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 };
	for (int i = 0; i < planes; i++) {
		handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		offsets[i] = gbm_bo_get_offset(bo, i);
		mods[i] = mod;
	}

	int ret = -1;
	if (mod != DRM_FORMAT_MOD_INVALID)
		ret = drmModeAddFB2WithModifiers(fd, width, height, fmt, handles,
		                                 strides, offsets, mods, &w->fb_id,
		                                 DRM_MODE_FB_MODIFIERS);
	if (ret != 0) /* fallback: no modifiers (virtio/CPU paths) */
		ret = drmModeAddFB2(fd, width, height, fmt, handles, strides,
		                    offsets, &w->fb_id, 0);
	if (ret != 0) {
		LOG_ERR("AddFB for bo: %s", strerror(errno));
		free(w);
		return 0;
	}
	gbm_bo_set_user_data(bo, w, bo_destroy);
	return w->fb_id;
}

/* ---- GLES helpers ---------------------------------------------------- */
static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		LOG_ERR("shader compile: %s", log);
	}
	return s;
}
static const char *VS =
        "attribute vec2 pos;\n"
        "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
        "precision mediump float;\n"
        "uniform vec3 tint;\n"
        "void main(){ gl_FragColor = vec4(tint, 1.0); }\n";

struct flip_ctx { int pending; };
static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec,
                    unsigned crtc, void *d)
{
	(void)fd; (void)seq; (void)sec; (void)usec; (void)crtc;
	((struct flip_ctx *)d)->pending = 0;
}

static EGLConfig pick_config(EGLDisplay dpy, uint32_t gbm_format)
{
	EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
	                  EGL_ALPHA_SIZE, 0, EGL_NONE };
	EGLint n = 0;
	eglChooseConfig(dpy, attr, NULL, 0, &n);
	if (n <= 0)
		return NULL;
	EGLConfig *cfgs = calloc(n, sizeof(EGLConfig));
	eglChooseConfig(dpy, attr, cfgs, n, &n);
	EGLConfig chosen = cfgs[0];
	for (EGLint i = 0; i < n; i++) {
		EGLint vid = 0;
		eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
		if ((uint32_t)vid == gbm_format) {
			chosen = cfgs[i];
			break;
		}
	}
	free(cfgs);
	return chosen;
}

int p0_4_gl_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);

	int fd = drm_open(argc > 1 ? argv[1] : NULL);
	if (fd < 0)
		return 1;
	if (drmSetMaster(fd) != 0)
		LOG_WARN("drmSetMaster: %s (need a bare VT as root)", strerror(errno));

	struct kms k;
	if (kms_setup(fd, &k) != 0)
		return 1;

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		LOG_ERR("gbm_create_device failed");
		return 1;
	}
	uint32_t gbm_fmt = GBM_FORMAT_XRGB8888;
	struct gbm_surface *gs = gbm_surface_create(
	        gbm, k.mode.hdisplay, k.mode.vdisplay, gbm_fmt,
	        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gs) {
		LOG_ERR("gbm_surface_create failed (no scanout-capable buffers?)");
		return 1;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
	        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_dpy
	        ? get_dpy(EGL_PLATFORM_GBM_KHR, gbm, NULL)
	        : eglGetDisplay((EGLNativeDisplayType)gbm);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		LOG_ERR("eglInitialize failed");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);

	EGLConfig cfg = pick_config(dpy, gbm_fmt);
	if (!cfg) {
		LOG_ERR("no matching EGL config");
		return 1;
	}
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg,
	                                         (EGLNativeWindowType)gs, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		LOG_ERR("EGL context/surface setup failed");
		return 1;
	}
	LOG_INFO("GL_RENDERER: %s", (const char *)glGetString(GL_RENDERER));

	GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint u_tint = glGetUniformLocation(prog, "tint");
	const GLfloat tri[] = { 0.0f, 0.6f, -0.6f, -0.5f, 0.6f, -0.5f };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, k.mode.hdisplay, k.mode.vdisplay);

	drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
	struct flip_ctx flip = { 0 };
	struct gbm_bo *prev_bo = NULL;
	int first = 1;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (running) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		float t = (now.tv_sec - t0.tv_sec) +
		          (now.tv_nsec - t0.tv_nsec) / 1e9f;
		glClearColor(0.06f, 0.10f, 0.16f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glUniform3f(u_tint, 0.5f + 0.5f * sinf(t),
		            0.5f + 0.5f * sinf(t + 2.0f),
		            0.5f + 0.5f * sinf(t + 4.0f));
		glDrawArrays(GL_TRIANGLES, 0, 3);
		eglSwapBuffers(dpy, surf);

		struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
		if (!bo) {
			LOG_ERR("lock_front_buffer failed");
			break;
		}
		uint32_t fb_id = fb_for_bo(fd, bo);
		if (!fb_id) {
			gbm_surface_release_buffer(gs, bo);
			break;
		}

		if (first) {
			drmModeAtomicReq *req = drmModeAtomicAlloc();
			kms_atomic_modeset(req, &k);
			kms_atomic_plane(req, &k, fb_id);
			if (drmModeAtomicCommit(fd, req,
			                        DRM_MODE_ATOMIC_ALLOW_MODESET,
			                        NULL) != 0) {
				LOG_ERR("initial modeset: %s", strerror(errno));
				drmModeAtomicFree(req);
				break;
			}
			drmModeAtomicFree(req);
			first = 0;
			prev_bo = bo;
		} else {
			drmModeAtomicReq *req = drmModeAtomicAlloc();
			atomic_add(req, k.plane_id, &k.plane_props, "FB_ID", fb_id);
			flip.pending = 1;
			if (drmModeAtomicCommit(fd, req,
			                        DRM_MODE_ATOMIC_NONBLOCK |
			                        DRM_MODE_PAGE_FLIP_EVENT,
			                        &flip) != 0) {
				LOG_ERR("flip commit: %s", strerror(errno));
				drmModeAtomicFree(req);
				gbm_surface_release_buffer(gs, bo);
				break;
			}
			drmModeAtomicFree(req);
			while (flip.pending && running) {
				struct pollfd pfd = { .fd = fd, .events = POLLIN };
				if (poll(&pfd, 1, 1000) <= 0)
					continue;
				drmHandleEvent(fd, &ev);
			}
			if (prev_bo)
				gbm_surface_release_buffer(gs, prev_bo);
			prev_bo = bo;
		}

		if (t >= 15.0f)
			running = 0; /* auto-exit after 15s */
	}

	if (prev_bo)
		gbm_surface_release_buffer(gs, prev_bo);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(dpy, surf);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	gbm_surface_destroy(gs);
	gbm_device_destroy(gbm);
	kms_finish(&k);
	drmDropMaster(fd);
	close(fd);
	return 0;
}
