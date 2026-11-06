// Minimal GLFW 3.3 API on top of SDL2, just enough for re3's skel/glfw.
// See glfw_sdl2_shim.h. Joystick ids map to SDL device indices.
#include "glfw_sdl2_shim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOYS (GLFW_JOYSTICK_LAST + 1)

// On ARM's driver, librw draws through EGL on /dev/fb0 and the "window" is a
// placeholder, so window queries go to the framebuffer instead of SDL.
namespace rw { namespace gl3 {
bool fbdevActive(void);
bool fbdevGetSize(int *w, int *h);
} }
using rw::gl3::fbdevActive;
using rw::gl3::fbdevGetSize;

static bool shouldClose;
static unsigned char keyState[GLFW_KEY_LAST + 1];
static double cursorX, cursorY;

static GLFWkeyfun keyCB;
static GLFWframebuffersizefun fbSizeCB;
static GLFWscrollfun scrollCB;
static GLFWcursorposfun cursorPosCB;
static GLFWcursorenterfun cursorEnterCB;
static GLFWwindowiconifyfun iconifyCB;
static GLFWwindowfocusfun focusCB;
static GLFWjoystickfun joystickCB;

static struct {
	SDL_Joystick *joy;
	SDL_GameController *pad;
	float axes[16];
	unsigned char buttons[32];
} joys[MAX_JOYS];

static void
initInput(void)
{
	static bool done;
	if(done)
		return;
	done = true;
	SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
	SDL_GameControllerEventState(SDL_ENABLE);
	SDL_JoystickEventState(SDL_ENABLE);
}

static void
openJoy(int jid)
{
	if(jid < 0 || jid >= MAX_JOYS || joys[jid].joy)
		return;
	if(SDL_IsGameController(jid))
		joys[jid].pad = SDL_GameControllerOpen(jid);
	joys[jid].joy = joys[jid].pad ? SDL_GameControllerGetJoystick(joys[jid].pad) : SDL_JoystickOpen(jid);
}

static void
closeJoy(int jid)
{
	if(joys[jid].pad)
		SDL_GameControllerClose(joys[jid].pad);
	else if(joys[jid].joy)
		SDL_JoystickClose(joys[jid].joy);
	joys[jid].pad = nullptr;
	joys[jid].joy = nullptr;
}

static int
jidFromInstance(SDL_JoystickID id)
{
	for(int i = 0; i < MAX_JOYS; i++)
		if(joys[i].joy && SDL_JoystickInstanceID(joys[i].joy) == id)
			return i;
	return -1;
}

static int
mapKey(SDL_Keycode k)
{
	if(k >= SDLK_a && k <= SDLK_z) return GLFW_KEY_A + (k - SDLK_a);
	if(k >= SDLK_0 && k <= SDLK_9) return GLFW_KEY_0 + (k - SDLK_0);
	if(k >= SDLK_F1 && k <= SDLK_F12) return GLFW_KEY_F1 + (k - SDLK_F1);
	switch(k){
	case SDLK_SPACE: return GLFW_KEY_SPACE;
	case SDLK_RETURN: return GLFW_KEY_ENTER;
	case SDLK_ESCAPE: return GLFW_KEY_ESCAPE;
	case SDLK_TAB: return GLFW_KEY_TAB;
	case SDLK_BACKSPACE: return GLFW_KEY_BACKSPACE;
	case SDLK_UP: return GLFW_KEY_UP;
	case SDLK_DOWN: return GLFW_KEY_DOWN;
	case SDLK_LEFT: return GLFW_KEY_LEFT;
	case SDLK_RIGHT: return GLFW_KEY_RIGHT;
	case SDLK_LSHIFT: return GLFW_KEY_LEFT_SHIFT;
	case SDLK_RSHIFT: return GLFW_KEY_RIGHT_SHIFT;
	case SDLK_LCTRL: return GLFW_KEY_LEFT_CONTROL;
	case SDLK_RCTRL: return GLFW_KEY_RIGHT_CONTROL;
	case SDLK_LALT: return GLFW_KEY_LEFT_ALT;
	case SDLK_RALT: return GLFW_KEY_RIGHT_ALT;
	case SDLK_INSERT: return GLFW_KEY_INSERT;
	case SDLK_DELETE: return GLFW_KEY_DELETE;
	case SDLK_HOME: return GLFW_KEY_HOME;
	case SDLK_END: return GLFW_KEY_END;
	case SDLK_PAGEUP: return GLFW_KEY_PAGE_UP;
	case SDLK_PAGEDOWN: return GLFW_KEY_PAGE_DOWN;
	case SDLK_MINUS: return GLFW_KEY_MINUS;
	case SDLK_EQUALS: return GLFW_KEY_EQUAL;
	case SDLK_COMMA: return GLFW_KEY_COMMA;
	case SDLK_PERIOD: return GLFW_KEY_PERIOD;
	case SDLK_SLASH: return GLFW_KEY_SLASH;
	case SDLK_SEMICOLON: return GLFW_KEY_SEMICOLON;
	case SDLK_QUOTE: return GLFW_KEY_APOSTROPHE;
	case SDLK_BACKQUOTE: return GLFW_KEY_GRAVE_ACCENT;
	case SDLK_LEFTBRACKET: return GLFW_KEY_LEFT_BRACKET;
	case SDLK_RIGHTBRACKET: return GLFW_KEY_RIGHT_BRACKET;
	case SDLK_BACKSLASH: return GLFW_KEY_BACKSLASH;
	}
	return -1;
}

void GlosProfilePoll(void);

void
glfwPollEvents(void)
{
	GlosProfilePoll();
	initInput();
	SDL_Event e;
	while(SDL_PollEvent(&e)){
		SDL_Window *win = SDL_GL_GetCurrentWindow();
		switch(e.type){
		case SDL_QUIT:
			shouldClose = true;
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			int key = mapKey(e.key.keysym.sym);
			if(key < 0)
				break;
			int action = e.type == SDL_KEYUP ? GLFW_RELEASE : e.key.repeat ? GLFW_REPEAT : GLFW_PRESS;
			keyState[key] = action != GLFW_RELEASE;
			if(keyCB)
				keyCB(win, key, e.key.keysym.scancode, action, 0);
			break;
		}
		case SDL_MOUSEMOTION:
			cursorX = e.motion.x;
			cursorY = e.motion.y;
			if(cursorPosCB)
				cursorPosCB(win, cursorX, cursorY);
			break;
		case SDL_MOUSEWHEEL:
			if(scrollCB)
				scrollCB(win, e.wheel.x, e.wheel.y);
			break;
		case SDL_WINDOWEVENT:
			switch(e.window.event){
			case SDL_WINDOWEVENT_SIZE_CHANGED:
				if(fbSizeCB) fbSizeCB(win, e.window.data1, e.window.data2);
				break;
			case SDL_WINDOWEVENT_ENTER:
			case SDL_WINDOWEVENT_LEAVE:
				if(cursorEnterCB) cursorEnterCB(win, e.window.event == SDL_WINDOWEVENT_ENTER);
				break;
			case SDL_WINDOWEVENT_MINIMIZED:
			case SDL_WINDOWEVENT_RESTORED:
				if(iconifyCB) iconifyCB(win, e.window.event == SDL_WINDOWEVENT_MINIMIZED);
				break;
			case SDL_WINDOWEVENT_FOCUS_GAINED:
			case SDL_WINDOWEVENT_FOCUS_LOST:
				if(focusCB) focusCB(win, e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
				break;
			}
			break;
		case SDL_JOYDEVICEADDED:
			openJoy(e.jdevice.which);
			if(joystickCB && e.jdevice.which < MAX_JOYS)
				joystickCB(e.jdevice.which, GLFW_CONNECTED);
			break;
		case SDL_JOYDEVICEREMOVED: {
			int jid = jidFromInstance(e.jdevice.which);
			if(jid >= 0){
				closeJoy(jid);
				if(joystickCB)
					joystickCB(jid, GLFW_DISCONNECTED);
			}
			break;
		}
		}
	}
}

int glfwWindowShouldClose(GLFWwindow *) { return shouldClose; }

int
glfwGetKey(GLFWwindow *, int key)
{
	if(key < 0 || key > GLFW_KEY_LAST)
		return GLFW_RELEASE;
	return keyState[key] ? GLFW_PRESS : GLFW_RELEASE;
}

int
glfwGetMouseButton(GLFWwindow *, int button)
{
	static const int map[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2 };
	if(button < 0 || button > 4)
		return GLFW_RELEASE;
	return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(map[button])) ? GLFW_PRESS : GLFW_RELEASE;
}

void
glfwGetCursorPos(GLFWwindow *, double *x, double *y)
{
	if(x) *x = cursorX;
	if(y) *y = cursorY;
}

void
glfwSetCursorPos(GLFWwindow *w, double x, double y)
{
	cursorX = x;
	cursorY = y;
	if(w && !fbdevActive())
		SDL_WarpMouseInWindow(w, (int)x, (int)y);
}

void
glfwSetInputMode(GLFWwindow *, int mode, int value)
{
	if(mode == GLFW_CURSOR)
		SDL_ShowCursor(value == GLFW_CURSOR_NORMAL ? SDL_ENABLE : SDL_DISABLE);
}

void
glfwGetWindowSize(GLFWwindow *w, int *width, int *height)
{
	int tw = 0, th = 0;
	if(fbdevActive())
		fbdevGetSize(&tw, &th);
	else if(w)
		SDL_GetWindowSize(w, &tw, &th);
	if(width) *width = tw;
	if(height) *height = th;
}

void glfwSetWindowSize(GLFWwindow *w, int width, int height) { if(w && !fbdevActive()) SDL_SetWindowSize(w, width, height); }
void glfwSetWindowPos(GLFWwindow *w, int x, int y) { if(w && !fbdevActive()) SDL_SetWindowPosition(w, x, y); }

GLFWmonitor *
glfwGetPrimaryMonitor(void)
{
	static int dummy;
	return (GLFWmonitor *)&dummy;
}

const GLFWvidmode *
glfwGetVideoMode(GLFWmonitor *)
{
	static GLFWvidmode mode;
	SDL_DisplayMode dm;
	if(fbdevActive()){
		fbdevGetSize(&mode.width, &mode.height);
		mode.refreshRate = 60;
		mode.redBits = mode.greenBits = mode.blueBits = 8;
	}else if(SDL_GetDesktopDisplayMode(0, &dm) == 0){
		mode.width = dm.w;
		mode.height = dm.h;
		mode.refreshRate = dm.refresh_rate;
		mode.redBits = mode.greenBits = mode.blueBits = 8;
	}
	return &mode;
}

#define SETCB(type, var) type old = var; var = cb; return old;
GLFWkeyfun glfwSetKeyCallback(GLFWwindow *, GLFWkeyfun cb) { SETCB(GLFWkeyfun, keyCB) }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow *, GLFWframebuffersizefun cb) { SETCB(GLFWframebuffersizefun, fbSizeCB) }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *, GLFWscrollfun cb) { SETCB(GLFWscrollfun, scrollCB) }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow *, GLFWcursorposfun cb) { SETCB(GLFWcursorposfun, cursorPosCB) }
GLFWcursorenterfun glfwSetCursorEnterCallback(GLFWwindow *, GLFWcursorenterfun cb) { SETCB(GLFWcursorenterfun, cursorEnterCB) }
GLFWwindowiconifyfun glfwSetWindowIconifyCallback(GLFWwindow *, GLFWwindowiconifyfun cb) { SETCB(GLFWwindowiconifyfun, iconifyCB) }
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow *, GLFWwindowfocusfun cb) { SETCB(GLFWwindowfocusfun, focusCB) }
GLFWjoystickfun glfwSetJoystickCallback(GLFWjoystickfun cb) { SETCB(GLFWjoystickfun, joystickCB) }
#undef SETCB

int
glfwJoystickPresent(int jid)
{
	initInput();
	if(jid < 0 || jid >= MAX_JOYS || jid >= SDL_NumJoysticks())
		return GLFW_FALSE;
	openJoy(jid);
	return joys[jid].joy != nullptr;
}

int
glfwJoystickIsGamepad(int jid)
{
	return glfwJoystickPresent(jid) && joys[jid].pad != nullptr;
}

const char *
glfwGetJoystickName(int jid)
{
	if(!glfwJoystickPresent(jid))
		return nullptr;
	return SDL_JoystickName(joys[jid].joy);
}

// RE3_STICK_SCALE (default 1) multiplies a stick's deflection: the GA36-MB's
// sticks reach only about two thirds of what its gamepad driver declares
// (43-213 of 0-255), so its start.sh sets 1.5. RE3_STICK_DEADZONE (default 0,
// i.e. the game's own handling) is a radial dead zone as a fraction of full
// deflection, applied to a stick's two axes together so a small push keeps
// its angle. Read by the game only.
static float
stickSetting(const char *name, float fallback)
{
	const char *text = getenv(name);
	if(text == nullptr || *text == 0)
		return fallback;
	char *end = nullptr;
	float value = strtof(text, &end);
	return end != text && value >= 0.0f && value < 10.0f ? value : fallback;
}

static void
stickShape(float *x, float *y)
{
	static float scale = stickSetting("RE3_STICK_SCALE", 1.0f);
	static float deadZone = fminf(stickSetting("RE3_STICK_DEADZONE", 0.0f), 0.95f);
	float sx = *x * scale, sy = *y * scale;
	float deflection = sqrtf(sx * sx + sy * sy);
	if(deflection <= deadZone){
		*x = *y = 0.0f;
		return;
	}
	float k = (deadZone > 0.0f ? fminf(1.0f, (deflection - deadZone) / (1.0f - deadZone)) : fminf(1.0f, deflection)) / deflection;
	*x = sx * k;
	*y = sy * k;
}

const float *
glfwGetJoystickAxes(int jid, int *count)
{
	*count = 0;
	if(!glfwJoystickPresent(jid))
		return nullptr;
	int n = SDL_JoystickNumAxes(joys[jid].joy);
	if(n > 16) n = 16;
	for(int i = 0; i < n; i++)
		joys[jid].axes[i] = SDL_JoystickGetAxis(joys[jid].joy, i) / 32767.0f;
	for(int i = 0; i + 1 < n && i < 4; i += 2)
		stickShape(&joys[jid].axes[i], &joys[jid].axes[i + 1]);
	*count = n;
	return joys[jid].axes;
}

const unsigned char *
glfwGetJoystickButtons(int jid, int *count)
{
	*count = 0;
	if(!glfwJoystickPresent(jid))
		return nullptr;
	int n = SDL_JoystickNumButtons(joys[jid].joy);
	if(n > 32) n = 32;
	for(int i = 0; i < n; i++)
		joys[jid].buttons[i] = SDL_JoystickGetButton(joys[jid].joy, i);
	*count = n;
	return joys[jid].buttons;
}

int
glfwGetGamepadState(int jid, GLFWgamepadstate *state)
{
	if(!glfwJoystickIsGamepad(jid))
		return GLFW_FALSE;
	SDL_GameController *p = joys[jid].pad;
	static const struct { int glfw; SDL_GameControllerButton sdl; } buttons[] = {
		{ GLFW_GAMEPAD_BUTTON_A, SDL_CONTROLLER_BUTTON_A },
		{ GLFW_GAMEPAD_BUTTON_B, SDL_CONTROLLER_BUTTON_B },
		{ GLFW_GAMEPAD_BUTTON_X, SDL_CONTROLLER_BUTTON_X },
		{ GLFW_GAMEPAD_BUTTON_Y, SDL_CONTROLLER_BUTTON_Y },
		{ GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
		{ GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
		{ GLFW_GAMEPAD_BUTTON_BACK, SDL_CONTROLLER_BUTTON_BACK },
		{ GLFW_GAMEPAD_BUTTON_START, SDL_CONTROLLER_BUTTON_START },
		{ GLFW_GAMEPAD_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_GUIDE },
		{ GLFW_GAMEPAD_BUTTON_LEFT_THUMB, SDL_CONTROLLER_BUTTON_LEFTSTICK },
		{ GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
		{ GLFW_GAMEPAD_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP },
		{ GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
		{ GLFW_GAMEPAD_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
		{ GLFW_GAMEPAD_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
	};
	memset(state, 0, sizeof(*state));
	for(auto &b : buttons)
		state->buttons[b.glfw] = SDL_GameControllerGetButton(p, b.sdl) ? GLFW_PRESS : GLFW_RELEASE;
	state->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
	state->axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
	state->axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
	state->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
	stickShape(&state->axes[GLFW_GAMEPAD_AXIS_LEFT_X], &state->axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
	stickShape(&state->axes[GLFW_GAMEPAD_AXIS_RIGHT_X], &state->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
	// GLFW triggers rest at -1 and go to 1; SDL's go from 0 to 32767.
	state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 16383.5f - 1.0f;
	state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 16383.5f - 1.0f;
	return GLFW_TRUE;
}

int
glfwUpdateGamepadMappings(const char *string)
{
	initInput();
	SDL_RWops *rw = SDL_RWFromConstMem(string, (int)strlen(string));
	return SDL_GameControllerAddMappingsFromRW(rw, 1) >= 0;
}
