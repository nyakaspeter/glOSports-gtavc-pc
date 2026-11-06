// EGL-on-fbdev window system for librw, for ARM's closed Mali-400 driver
// (r6p2 "fbdev" blob), which has no GBM/KMS support. Used whenever /dev/mali
// exists, i.e. while ARM's driver is loaded (glos-gpu run mali).
// EGL is loaded at runtime so the normal (Mesa/KMSDRM) path doesn't depend on it.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

double glosSwapMs;	// total time blocked in eglSwapBuffers, for the FPS overlay
double glosFlipWaitMs;	// part of it spent waiting for the previous flip (Vsync on)

// Tear-free flips. ARM's fbdev EGL returns from eglSwapBuffers at once and
// flips (FBIOPAN_DISPLAY, which returns at the vblank) on a thread of its own.
// With two buffers the GPU would then draw the next frame into the one still
// on screen: tearing, and a flicker between two frames. With Vsync on, each
// frame waits before its flush until the previous frame's flip has landed.
// The flips are counted here: the binary exports ioctl, so libMali's calls
// land in this one. From glOSports-maxpayne.
static pthread_mutex_t flipLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flipCond = PTHREAD_COND_INITIALIZER;
static unsigned flipsDone, swapsDone;

extern "C" int
ioctl(int fd, unsigned long request, ...)
{
	static int (*real)(int, unsigned long, void *);
	if(real == nullptr)
		real = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	int r = real(fd, request, arg);
	if(request == FBIOPAN_DISPLAY){
		int e = errno;
		pthread_mutex_lock(&flipLock);
		flipsDone++;
		pthread_cond_broadcast(&flipCond);
		pthread_mutex_unlock(&flipLock);
		errno = e;
	}
	return r;
}

static void
waitPreviousFlip(void)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_nsec += 100 * 1000000;	// never hang on a flip that doesn't come
	if(t.tv_nsec >= 1000000000){
		t.tv_sec++;
		t.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&flipLock);
	// a flip that comes late stays owed: counting it as done would let every
	// later frame tear
	while((int)(flipsDone - swapsDone) < 0)
		if(pthread_cond_timedwait(&flipCond, &flipLock, &t) == ETIMEDOUT)
			break;
	pthread_mutex_unlock(&flipLock);
}

namespace rw {
namespace gl3 {

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned int EGLBoolean;

// ARM's fbdev native window type (EGL/fbdev_window.h).
struct fbdev_window {
	unsigned short width;
	unsigned short height;
};

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

static struct {
	EGLDisplay (*GetDisplay)(void *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
	EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
	EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
	EGLBoolean (*Terminate)(EGLDisplay);
	EGLint (*GetError)(void);
	void *(*GetProcAddress)(const char *);
} egl;

static void *eglLib, *glesLib;
static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;
static fbdev_window nativeWindow;
static int lastInterval = -1;

bool
fbdevActive(void)
{
	static int active = -1;
	if(active < 0)
		active = access("/dev/mali", F_OK) == 0;
	return active;
}

bool
fbdevGetSize(int *w, int *h)
{
	static int cw, ch;
	if(cw == 0){
		int fd = open("/dev/fb0", O_RDONLY);
		struct fb_var_screeninfo vi;
		if(fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0){
			if(fd >= 0) close(fd);
			printf("librw: fbdev: can't read /dev/fb0\n");
			return false;
		}
		close(fd);
		cw = vi.xres;
		ch = vi.yres;
		printf("librw: fbdev %dx%d (virtual %dx%d, %d bpp)\n", vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual, vi.bits_per_pixel);
	}
	*w = cw;
	*h = ch;
	return true;
}

void *
fbdevGetProcAddress(const char *name)
{
	void *p = glesLib ? dlsym(glesLib, name) : nullptr;
	if(p == nullptr && egl.GetProcAddress)
		p = egl.GetProcAddress(name);
	return p;
}

bool
fbdevStart(int w, int h)
{
	eglLib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	glesLib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	if(eglLib == nullptr || glesLib == nullptr){
		printf("librw: fbdev: can't load EGL/GLES: %s\n", dlerror());
		return false;
	}
#define LOAD(f) *(void**)&egl.f = dlsym(eglLib, "egl" #f); if(egl.f == nullptr){ printf("librw: fbdev: missing egl" #f "\n"); return false; }
	LOAD(GetDisplay) LOAD(Initialize) LOAD(ChooseConfig) LOAD(CreateWindowSurface)
	LOAD(CreateContext) LOAD(MakeCurrent) LOAD(SwapBuffers) LOAD(SwapInterval)
	LOAD(DestroySurface) LOAD(DestroyContext) LOAD(Terminate) LOAD(GetError)
	LOAD(GetProcAddress)
#undef LOAD

	display = egl.GetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major, minor;
	if(display == nullptr || !egl.Initialize(display, &major, &minor)){
		printf("librw: fbdev: eglInitialize failed 0x%x\n", egl.GetError());
		return false;
	}
	static const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint n = 0;
	if(!egl.ChooseConfig(display, attribs, &config, 1, &n) || n < 1){
		printf("librw: fbdev: no EGL config 0x%x\n", egl.GetError());
		return false;
	}
	nativeWindow.width = w;
	nativeWindow.height = h;
	surface = egl.CreateWindowSurface(display, config, &nativeWindow, nullptr);
	static const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	context = egl.CreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
	if(surface == nullptr || context == nullptr || !egl.MakeCurrent(display, surface, surface, context)){
		printf("librw: fbdev: surface/context failed 0x%x\n", egl.GetError());
		return false;
	}
	printf("librw: fbdev: EGL %d.%d up, %dx%d\n", major, minor, w, h);
	return true;
}

void
fbdevSwap(bool vsync)
{
	if(lastInterval != vsync){
		egl.SwapInterval(display, vsync);
		lastInterval = vsync;
	}
	struct timespec t0, tw, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	static bool first = true;
	if(first){
		// the driver flips once when the surface is made: count from here
		pthread_mutex_lock(&flipLock);
		flipsDone = swapsDone = 0;
		pthread_mutex_unlock(&flipLock);
		first = false;
	}else if(vsync)
		waitPreviousFlip();
	clock_gettime(CLOCK_MONOTONIC, &tw);
	egl.SwapBuffers(display, surface);
	pthread_mutex_lock(&flipLock);
	swapsDone++;
	pthread_mutex_unlock(&flipLock);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	glosSwapMs += (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	glosFlipWaitMs += (tw.tv_sec - t0.tv_sec) * 1000.0 + (tw.tv_nsec - t0.tv_nsec) / 1e6;
}

void
fbdevStop(void)
{
	egl.MakeCurrent(display, nullptr, nullptr, EGL_NO_CONTEXT);
	egl.DestroyContext(display, context);
	egl.DestroySurface(display, surface);
	egl.Terminate(display);
}

}
}
