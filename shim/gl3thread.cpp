// GL command thread for librw on ARM's Mali blob (fbdev path).
//
// The game thread no longer calls the GL driver: glad's function pointers are
// replaced with stubs that record each call into a single-producer /
// single-consumer ring, and a render thread (which owns the EGL context)
// replays them. Calls that return data run synchronously on the render
// thread; glGen* names come from pre-generated pools. Data passed by pointer
// is copied, except client vertex arrays, which librw hands over through
// glthreadPersist() so they live until their frame has been drawn.
//
// Enabled with RE3_GL_THREAD=1 on the fbdev path (start.sh sets it): it moves
// roughly half of the main thread's work, the driver calls, to another core.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#ifdef RW_OPENGL
#include "rwgl3.h"
#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

bool fbdevStart(int w, int h);
void fbdevSwap(bool vsync);
void fbdevStop(void);
void *fbdevGetProcAddress(const char *name);

struct Cmd {
	void (*exec)(Cmd *c);
	uint32 a[10];
	void *heap;		// payload too big for inl, freed after exec
	uint8 inl[64];
};

static const uint32 RING_SIZE = 1 << 15;	// commands
static Cmd ring[RING_SIZE];
static uint32 head;				// producer-private
static std::atomic<uint32> headPub(0), tailPub(0);
static std::atomic<int> consumerSleeping(0);
static std::atomic<bool> quitting(false);
static std::atomic<long> heapBytes(0);	// outstanding payload copies
static sem_t wakeSem, frameSem, readySem;
static pthread_t thread;
static bool threaded, stopped, startOk;
static int startW, startH;
static Cmd scratch;				// sink for calls after shutdown

static void
semWait(sem_t *s)
{
	while(sem_wait(s) == -1 && errno == EINTR)
		;
}

static void
wakeConsumer(void)
{
	if(consumerSleeping.load() && consumerSleeping.exchange(0))
		sem_post(&wakeSem);
}

static Cmd*
cmdAlloc(void)
{
	if(stopped)
		return &scratch;
	while(head - tailPub.load(std::memory_order_acquire) >= RING_SIZE){
		wakeConsumer();
		sched_yield();
	}
	Cmd *c = &ring[head & (RING_SIZE-1)];
	c->heap = nullptr;
	return c;
}

static void
cmdCommit(void)
{
	if(stopped)
		return;
	head++;
	headPub.store(head);
	wakeConsumer();
}

// Copy n bytes of call data into the command (inline when small).
static void
setPayload(Cmd *c, const void *src, size_t n)
{
	if(n <= sizeof(c->inl)){
		memcpy(c->inl, src, n);
		return;
	}
	while(heapBytes.load(std::memory_order_relaxed) > (64<<20))
		sched_yield();	// let the GL thread catch up on uploads
	c->heap = malloc(n);
	memcpy(c->heap, src, n);
	heapBytes += n;
	c->a[9] = n;
}

static const void*
payload(Cmd *c)
{
	return c->heap ? c->heap : c->inl;
}

static void
freePayload(Cmd *c)
{
	if(c->heap){
		free(c->heap);
		heapBytes -= c->a[9];
	}
}

struct SyncCtx {
	void (*fn)(void *arg);
	void *arg;
	sem_t done;
};

static void
exec_sync(Cmd *c)
{
	SyncCtx *s;
	memcpy(&s, &c->a[0], sizeof(s));
	s->fn(s->arg);
	sem_post(&s->done);
}

// Run f on the GL thread after everything queued so far, and wait for it.
template<class F> static void
syncRun(F f)
{
	if(stopped)
		return;
	SyncCtx s;
	s.fn = [](void *arg){ (*(F*)arg)(); };
	s.arg = &f;
	sem_init(&s.done, 0, 0);
	SyncCtx *sp = &s;
	Cmd *c = cmdAlloc();
	c->exec = exec_sync;
	memcpy(&c->a[0], &sp, sizeof(sp));
	cmdCommit();
	semWait(&s.done);
	sem_destroy(&s.done);
}

#define ARG(T, n, i) T n; memcpy(&n, &c->a[i], sizeof(n))
#define PUT(i, v) do { c->a[i] = 0; memcpy(&c->a[i], &(v), sizeof(v)); } while(0)

#include "gl3thread_gen.inc"

// --- binding tracking (needed to tell offsets from client pointers) ---

static GLuint boundArrayBuffer, boundElementBuffer;
static GLint unpackAlignment = 4;

static void exec_BindBuffer(Cmd *c) { ARG(GLenum, t, 0); ARG(GLuint, b, 1); real.BindBuffer(t, b); }
static void APIENTRY
stub_BindBuffer(GLenum target, GLuint buffer)
{
	if(target == GL_ARRAY_BUFFER) boundArrayBuffer = buffer;
	else if(target == GL_ELEMENT_ARRAY_BUFFER) boundElementBuffer = buffer;
	Cmd *c = cmdAlloc();
	c->exec = exec_BindBuffer;
	PUT(0, target); PUT(1, buffer);
	cmdCommit();
}

static void exec_PixelStorei(Cmd *c) { ARG(GLenum, p, 0); ARG(GLint, v, 1); real.PixelStorei(p, v); }
static void APIENTRY
stub_PixelStorei(GLenum pname, GLint param)
{
	if(pname == GL_UNPACK_ALIGNMENT) unpackAlignment = param;
	Cmd *c = cmdAlloc();
	c->exec = exec_PixelStorei;
	PUT(0, pname); PUT(1, param);
	cmdCommit();
}

// --- calls with data ---

static void
exec_BufferData(Cmd *c)
{
	ARG(GLenum, t, 0); ARG(GLsizeiptr, n, 1); ARG(GLenum, u, 2); ARG(uint32, has, 3);
	real.BufferData(t, n, has ? payload(c) : nullptr, u);
	freePayload(c);
}
static void APIENTRY
stub_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_BufferData;
	uint32 has = data != nullptr;
	PUT(0, target); PUT(1, size); PUT(2, usage); PUT(3, has);
	if(has) setPayload(c, data, size);
	cmdCommit();
}

static void
exec_BufferSubData(Cmd *c)
{
	ARG(GLenum, t, 0); ARG(GLintptr, o, 1); ARG(GLsizeiptr, n, 2);
	real.BufferSubData(t, o, n, payload(c));
	freePayload(c);
}
static void APIENTRY
stub_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_BufferSubData;
	PUT(0, target); PUT(1, offset); PUT(2, size);
	setPayload(c, data, size);
	cmdCommit();
}

static size_t
texBytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
	int bpp;
	switch(type){
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT:
		bpp = 2; break;
	case GL_UNSIGNED_INT:
	case 0x84FA:	// GL_UNSIGNED_INT_24_8
		bpp = 4; break;
	case GL_FLOAT:
		bpp = 4 * (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1); break;
	default:	// GL_UNSIGNED_BYTE
		bpp = format == GL_RGBA || format == 0x80E1 /* BGRA */ ? 4 :
		      format == GL_RGB ? 3 :
		      format == GL_LUMINANCE_ALPHA ? 2 : 1;
	}
	if(w <= 0 || h <= 0)
		return 0;
	size_t row = (size_t)w * bpp;
	size_t stride = (row + unpackAlignment - 1) / unpackAlignment * unpackAlignment;
	return stride * (h - 1) + row;
}

static void
exec_TexImage2D(Cmd *c)
{
	ARG(GLenum, t, 0); ARG(GLint, l, 1); ARG(GLint, i, 2); ARG(GLsizei, w, 3); ARG(GLsizei, h, 4);
	ARG(GLint, b, 5); ARG(GLenum, f, 6); ARG(GLenum, ty, 7); ARG(uint32, has, 8);
	real.TexImage2D(t, l, i, w, h, b, f, ty, has ? payload(c) : nullptr);
	freePayload(c);
}
static void APIENTRY
stub_TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLint border, GLenum format, GLenum type, const void *pixels)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_TexImage2D;
	uint32 has = pixels != nullptr;
	PUT(0, target); PUT(1, level); PUT(2, internalformat); PUT(3, width); PUT(4, height);
	PUT(5, border); PUT(6, format); PUT(7, type); PUT(8, has);
	if(has) setPayload(c, pixels, texBytes(width, height, format, type));
	cmdCommit();
}

static void
exec_CompressedTexImage2D(Cmd *c)
{
	ARG(GLenum, t, 0); ARG(GLint, l, 1); ARG(GLenum, i, 2); ARG(GLsizei, w, 3); ARG(GLsizei, h, 4);
	ARG(GLint, b, 5); ARG(GLsizei, n, 6);
	real.CompressedTexImage2D(t, l, i, w, h, b, n, payload(c));
	freePayload(c);
}
static void APIENTRY
stub_CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                          GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_CompressedTexImage2D;
	PUT(0, target); PUT(1, level); PUT(2, internalformat); PUT(3, width); PUT(4, height);
	PUT(5, border); PUT(6, imageSize);
	setPayload(c, data, imageSize);
	cmdCommit();
}

#define DELETE_STUB(Name, track)						\
static void exec_##Name(Cmd *c)							\
{										\
	ARG(GLsizei, n, 0);							\
	real.Name(n, (const GLuint*)payload(c));				\
	freePayload(c);								\
}										\
static void APIENTRY								\
stub_##Name(GLsizei n, const GLuint *names)					\
{										\
	track;									\
	Cmd *c = cmdAlloc();							\
	c->exec = exec_##Name;							\
	PUT(0, n);								\
	setPayload(c, names, n * sizeof(GLuint));				\
	cmdCommit();								\
}
DELETE_STUB(DeleteBuffers,
	for(GLsizei i = 0; i < n; i++){
		if(names[i] == boundArrayBuffer) boundArrayBuffer = 0;
		if(names[i] == boundElementBuffer) boundElementBuffer = 0;
	})
DELETE_STUB(DeleteTextures, )
DELETE_STUB(DeleteFramebuffers, )
DELETE_STUB(DeleteRenderbuffers, )

#define UNIFORMV_STUB(Name, floats)						\
static void exec_##Name(Cmd *c)							\
{										\
	ARG(GLint, l, 0); ARG(GLsizei, n, 1);					\
	real.Name(l, n, (const GLfloat*)payload(c));				\
	freePayload(c);								\
}										\
static void APIENTRY								\
stub_##Name(GLint location, GLsizei count, const GLfloat *value)		\
{										\
	Cmd *c = cmdAlloc();							\
	c->exec = exec_##Name;							\
	PUT(0, location); PUT(1, count);					\
	setPayload(c, value, count * (floats) * sizeof(GLfloat));		\
	cmdCommit();								\
}
UNIFORMV_STUB(Uniform2fv, 2)
UNIFORMV_STUB(Uniform4fv, 4)

static void
exec_UniformMatrix4fv(Cmd *c)
{
	ARG(GLint, l, 0); ARG(GLsizei, n, 1); ARG(GLboolean, t, 2);
	real.UniformMatrix4fv(l, n, t, (const GLfloat*)payload(c));
	freePayload(c);
}
static void APIENTRY
stub_UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_UniformMatrix4fv;
	PUT(0, location); PUT(1, count); PUT(2, transpose);
	setPayload(c, value, count * 16 * sizeof(GLfloat));
	cmdCommit();
}

static void
exec_ShaderSource(Cmd *c)
{
	ARG(GLuint, s, 0);
	const GLchar *src = (const GLchar*)payload(c);
	real.ShaderSource(s, 1, &src, nullptr);
	freePayload(c);
}
static void APIENTRY
stub_ShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
	size_t total = 1;
	for(GLsizei i = 0; i < count; i++)
		total += length && length[i] >= 0 ? length[i] : strlen(string[i]);
	char *all = (char*)malloc(total), *p = all;
	for(GLsizei i = 0; i < count; i++){
		size_t n = length && length[i] >= 0 ? length[i] : strlen(string[i]);
		memcpy(p, string[i], n);
		p += n;
	}
	*p = '\0';
	Cmd *c = cmdAlloc();
	c->exec = exec_ShaderSource;
	PUT(0, shader);
	setPayload(c, all, total);
	cmdCommit();
	free(all);
}

static void
exec_BindAttribLocation(Cmd *c)
{
	ARG(GLuint, p, 0); ARG(GLuint, i, 1);
	real.BindAttribLocation(p, i, (const GLchar*)payload(c));
	freePayload(c);
}
static void APIENTRY
stub_BindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_BindAttribLocation;
	PUT(0, program); PUT(1, index);
	setPayload(c, name, strlen(name) + 1);
	cmdCommit();
}

static void
exec_PushDebugGroup(Cmd *c)
{
	ARG(GLenum, s, 0); ARG(GLuint, id, 1);
	real.PushDebugGroup(s, id, -1, (const GLchar*)payload(c));
	freePayload(c);
}
static void APIENTRY
stub_PushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar *message)
{
	size_t n = length >= 0 ? (size_t)length : strlen(message);
	char *tmp = (char*)malloc(n + 1);
	memcpy(tmp, message, n);
	tmp[n] = '\0';
	Cmd *c = cmdAlloc();
	c->exec = exec_PushDebugGroup;
	PUT(0, source); PUT(1, id);
	setPayload(c, tmp, n + 1);
	cmdCommit();
	free(tmp);
}

// Index data without a bound element buffer is a client pointer: copy it.
static void
exec_DrawElements(Cmd *c)
{
	ARG(GLenum, m, 0); ARG(GLsizei, n, 1); ARG(GLenum, t, 2); ARG(const void*, off, 3); ARG(uint32, copied, 4);
	real.DrawElements(m, n, t, copied ? payload(c) : off);
	freePayload(c);
}
static void APIENTRY
stub_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	Cmd *c = cmdAlloc();
	c->exec = exec_DrawElements;
	uint32 copied = boundElementBuffer == 0 && indices != nullptr;
	PUT(0, mode); PUT(1, count); PUT(2, type); PUT(3, indices); PUT(4, copied);
	if(copied)
		setPayload(c, indices, count * (type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4));
	cmdCommit();
}

// --- object names: pre-generated pools, so streaming doesn't sync ---

struct NamePool {
	GLuint names[256];
	int count;
};
#define GEN_STUB(Name)								\
static NamePool pool_##Name;							\
static void APIENTRY								\
stub_##Name(GLsizei n, GLuint *out)						\
{										\
	for(GLsizei i = 0; i < n; i++){						\
		if(pool_##Name.count == 0){					\
			syncRun([&]{ real.Name(256, pool_##Name.names); });	\
			pool_##Name.count = 256;				\
		}								\
		out[i] = pool_##Name.names[--pool_##Name.count];		\
	}									\
}
GEN_STUB(GenBuffers)
GEN_STUB(GenTextures)
GEN_STUB(GenFramebuffers)
GEN_STUB(GenRenderbuffers)
GEN_STUB(GenVertexArrays)

// --- client vertex arrays: per-frame arenas ---

static const size_t ARENA_SIZE = 4 << 20;
static uint8 *arena[3];
static size_t arenaUsed[3];
static std::vector<void*> arenaOverflow[3];
static uint32 framesQueued, arenaFrame = ~0u;
static std::atomic<uint32> framesDone(0);

void*
glthreadPersist(const void *src, size_t n)
{
	if(!threaded)
		return (void*)src;
	uint32 i = framesQueued % 3;
	if(arenaFrame != framesQueued){
		// Frame framesQueued-3 used this arena; the swap throttle below
		// guarantees it has been drawn.
		arenaFrame = framesQueued;
		arenaUsed[i] = 0;
		for(void *p : arenaOverflow[i])
			free(p);
		arenaOverflow[i].clear();
	}
	size_t off = (arenaUsed[i] + 7) & ~(size_t)7;
	void *dst;
	if(off + n <= ARENA_SIZE){
		dst = arena[i] + off;
		arenaUsed[i] = off + n;
	}else{
		dst = malloc(n);
		arenaOverflow[i].push_back(dst);
	}
	memcpy(dst, src, n);
	return dst;
}

// --- thread, swap, start/stop ---

static void
exec_swap(Cmd *c)
{
	fbdevSwap(c->a[0] != 0);
	framesDone.fetch_add(1, std::memory_order_release);
	sem_post(&frameSem);
}

static void
exec_quit(Cmd *c)
{
	quitting = true;
}

static void
consumerLoop(void)
{
	uint32 tail = 0;
	while(!quitting){
		uint32 h = headPub.load(std::memory_order_acquire);
		if(tail == h){
			tailPub.store(tail, std::memory_order_release);
			bool more = false;
			for(int i = 0; i < 20000 && !more; i++)
				more = headPub.load(std::memory_order_acquire) != tail;
			if(!more){
				consumerSleeping.store(1);
				if(headPub.load() == tail)
					semWait(&wakeSem);
				consumerSleeping.store(0);
			}
			continue;
		}
		while(tail != h && !quitting){
			Cmd *c = &ring[tail & (RING_SIZE-1)];
			c->exec(c);
			tail++;
			if((tail & 31) == 0)
				tailPub.store(tail, std::memory_order_release);
		}
	}
	tailPub.store(tail, std::memory_order_release);
}

static bool
loadGL(void)
{
	return gladLoadGLES2Loader((GLADloadproc)fbdevGetProcAddress, 20) != 0;
}

static void*
threadMain(void *)
{
	startOk = fbdevStart(startW, startH) && loadGL();
	if(startOk)
		captureReal();
	sem_post(&readySem);
	if(startOk)
		consumerLoop();
	return nullptr;
}

bool
glthreadEnabled(void)
{
	static int enabled = -1;
	if(enabled < 0){
		const char *s = getenv("RE3_GL_THREAD");
		enabled = s && strcmp(s, "1") == 0;
	}
	return enabled;
}

// Create the EGL context and load GL on the calling thread (no GL thread).
bool
fbdevStartGL(int w, int h)
{
	return fbdevStart(w, h) && loadGL();
}

bool
glthreadStart(int w, int h)
{
	sem_init(&wakeSem, 0, 0);
	sem_init(&frameSem, 0, 0);
	sem_init(&readySem, 0, 0);
	for(int i = 0; i < 3; i++)
		arena[i] = (uint8*)malloc(ARENA_SIZE);
	startW = w;
	startH = h;
	if(pthread_create(&thread, nullptr, threadMain, nullptr) != 0)
		return false;
	pthread_setname_np(thread, "re3-gl");
	semWait(&readySem);
	if(!startOk){
		pthread_join(thread, nullptr);
		return false;
	}
	installStubs();
	threaded = true;
	printf("librw: GL command thread running\n");
	return true;
}

void
glosSwap(bool vsync)
{
	if(!threaded){
		fbdevSwap(vsync);
		return;
	}
	Cmd *c = cmdAlloc();
	c->exec = exec_swap;
	c->a[0] = vsync;
	cmdCommit();
	framesQueued++;
	// Record at most one frame ahead of the one being drawn.
	while(framesQueued - framesDone.load(std::memory_order_acquire) > 1)
		semWait(&frameSem);
}

void
glosStop(void)
{
	if(!threaded){
		fbdevStop();
		return;
	}
	syncRun([]{ fbdevStop(); });
	Cmd *c = cmdAlloc();
	c->exec = exec_quit;
	cmdCommit();
	pthread_join(thread, nullptr);
	threaded = false;
	stopped = true;	// later GL calls (teardown) are dropped
}

}
}
#endif
