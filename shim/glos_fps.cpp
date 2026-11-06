// Optional FPS overlay for goodluckOS (not part of the original game):
//   RE3_FPS_COUNTER=1  show FPS in the top-left corner
//   RE3_DRAW_STATS=1   show FPS, draw calls per frame, and per-frame CPU time vs
//                      time blocked in the buffer swap waiting for the GPU
//   RE3_FPS_LOG=1      also log both to stdout twice a second
#include "common.h"
#include "Font.h"

int glosDrawCalls;	// incremented by librw at every glDraw*
int glosTexBinds, glosTexBindsRedundant;	// librw bindTexture calls / re-binds
extern double glosSwapMs;	// gl3fbdev.cpp: time blocked in eglSwapBuffers
extern double glosFlipWaitMs;	// gl3fbdev.cpp: part of it waiting for the previous flip
static double spritePixels;	// screen area covered by buffered sprites
static int sprites;

// Called by CSprite's buffered sprite functions (quad centred on x,y with
// half-extents w,h), clipped to the screen.
void
glosCountSprite(float x, float y, float w, float h)
{
	float x0 = Max(x - w, 0.0f), x1 = Min(x + w, SCREEN_WIDTH);
	float y0 = Max(y - h, 0.0f), y1 = Min(y + h, SCREEN_HEIGHT);
	if(x1 > x0 && y1 > y0)
		spritePixels += (x1 - x0) * (y1 - y0);
	sprites++;
}

void
GlosDrawFPS(void)
{
	static bool installed;
	static bool show, showDraws, logToStdout;
	static double windowStart;
	static int frames;
	static int drawsAtStart;
	static double swapAtStart, flipWaitAtStart, spritePixelsAtStart;
	static int spritesAtStart, bindsAtStart, redundantAtStart;
	static char text[160] = "FPS: --";

	if(!installed){
		showDraws = getenv("RE3_DRAW_STATS") != nullptr;
		const char *fps = getenv("RE3_FPS_COUNTER");
		show = showDraws || (fps && strcmp(fps, "0") != 0);
		logToStdout = getenv("RE3_FPS_LOG") != nullptr;
		windowStart = RsTimer();
		installed = true;
	}
	if(!show && !logToStdout)
		return;

	frames++;
	double now = RsTimer();
	if(now - windowStart >= 500.0){
		if(showDraws || logToStdout){
			double frameMs = (now - windowStart) / frames;
			double swapMs = (glosSwapMs - swapAtStart) / frames;
			sprintf(text, "FPS: %.1f  draws: %d  binds %d (%d same)  cpu %.1fms gpu-wait %.1fms (flip %.1fms)  sprites %d overdraw %.1fx",
			        frames * 1000.0 / (now - windowStart), (glosDrawCalls - drawsAtStart) / frames,
			        (glosTexBinds - bindsAtStart) / frames, (glosTexBindsRedundant - redundantAtStart) / frames,
			        frameMs - swapMs, swapMs, (glosFlipWaitMs - flipWaitAtStart) / frames, (sprites - spritesAtStart) / frames,
			        (spritePixels - spritePixelsAtStart) / frames / (SCREEN_WIDTH * SCREEN_HEIGHT));
		}
		else
			sprintf(text, "FPS: %.1f", frames * 1000.0 / (now - windowStart));
		if(logToStdout)
			printf("glos: %s\n", text);
		frames = 0;
		drawsAtStart = glosDrawCalls;
		swapAtStart = glosSwapMs;
		flipWaitAtStart = glosFlipWaitMs;
		spritesAtStart = sprites;
		bindsAtStart = glosTexBinds;
		redundantAtStart = glosTexBindsRedundant;
		spritePixelsAtStart = spritePixels;
		windowStart = now;
	}
	if(!show)
		return;

	wchar wtext[160];
	AsciiToUnicode(text, wtext);
	CFont::SetBackgroundOff();
	CFont::SetScale(SCREEN_SCALE_X(0.5f), SCREEN_SCALE_Y(0.9f));
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetJustifyOff();
	CFont::SetPropOn();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_BANK);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
	CFont::SetColor(CRGBA(255, 255, 255, 255));
	CFont::PrintString(SCREEN_SCALE_X(6.0f), SCREEN_SCALE_Y(6.0f), wtext);
	CFont::SetDropShadowPosition(0);
	CFont::DrawFonts();
}
