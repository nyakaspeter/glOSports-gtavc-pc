// Render scale for goodluckOS (not part of the original game):
//   RE3_RENDER_SCALE=75   draw the 3D world at 75% of the screen size (25-99)
// The world goes into the corner of an offscreen target and is stretched to
// the screen with bilinear filtering; HUD, menus and fades are drawn
// afterwards at full resolution. The Mali-400 is fill-rate bound, so fewer
// pixels is the main lever left.
#include "common.h"
#include "main.h"

rw::Raster *glosScaleTarget;	// librw's beginUpdate uses glosScaleW/H as its viewport
int glosScaleW, glosScaleH;
namespace rw { namespace gl3 { int glosFramebufferComplete(Raster *raster); } }	// librw gl3device.cpp

static RwRaster *depth, *savedRaster, *savedZRaster;
static bool active, unsupported;

static int
renderScale(void)
{
	static int pct = -1;
	if(pct < 0){
		const char *env = getenv("RE3_RENDER_SCALE");
		pct = env ? atoi(env) : 100;
		if(pct < 25 || pct > 100)
			pct = 100;
	}
	return pct;
}

static bool
createTarget(RwRaster *screen)
{
	int w = RwRasterGetWidth(screen), h = RwRasterGetHeight(screen);
	glosScaleTarget = RwRasterCreate(w, h, 0, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT888);
	depth = RwRasterCreate(w, h, 0, rwRASTERTYPEZBUFFER);
	if(glosScaleTarget && depth){
		// Clearing through the camera attaches the depth buffer to the target.
		RwRaster *screenZ = RwCameraGetZRaster(Scene.camera);
		RwRGBA black = { 0, 0, 0, 255 };
		RwCameraSetRaster(Scene.camera, glosScaleTarget);
		RwCameraSetZRaster(Scene.camera, depth);
		RwCameraClear(Scene.camera, &black, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
		RwCameraSetRaster(Scene.camera, screen);
		RwCameraSetZRaster(Scene.camera, screenZ);
		if(rw::gl3::glosFramebufferComplete(glosScaleTarget)){
			printf("glos: render scale target %dx%d\n", w, h);
			return true;
		}
	}
	printf("glos: render scale unavailable, GPU can't render to the offscreen target\n");
	return false;
}

// Start of the 3D frame (after CameraSize): redirect the camera to the target.
void
GlosScaleBegin(void)
{
	int pct = renderScale();
	if(pct >= 100 || active || unsupported)
		return;
	RwRaster *screen = RwCameraGetRaster(Scene.camera);
	if(glosScaleTarget == nil && !createTarget(screen)){
		unsupported = true;
		return;
	}
	if(RwRasterGetWidth(screen) != RwRasterGetWidth(glosScaleTarget) ||
	   RwRasterGetHeight(screen) != RwRasterGetHeight(glosScaleTarget))
		return;
	glosScaleW = (RwRasterGetWidth(screen) * pct + 50) / 100;
	glosScaleH = (RwRasterGetHeight(screen) * pct + 50) / 100;
	savedRaster = screen;
	savedZRaster = RwCameraGetZRaster(Scene.camera);
	RwCameraSetRaster(Scene.camera, glosScaleTarget);
	RwCameraSetZRaster(Scene.camera, depth);
	active = true;
}

static void
setVertex(RwIm2DVertex *v, float x, float y, float u, float tv, float z, float recipZ)
{
	RwIm2DVertexSetScreenX(v, x);
	RwIm2DVertexSetScreenY(v, y);
	RwIm2DVertexSetScreenZ(v, z);
	RwIm2DVertexSetCameraZ(v, 1.0f / recipZ);
	RwIm2DVertexSetRecipCameraZ(v, recipZ);
	RwIm2DVertexSetU(v, u, recipZ);
	RwIm2DVertexSetV(v, tv, recipZ);
	RwIm2DVertexSetIntRGBA(v, 255, 255, 255, 255);
}

// End of the 3D part of the frame: back to the screen, stretch the target onto it.
void
GlosScaleEnd(void)
{
	if(!active)
		return;
	active = false;
	RwCameraEndUpdate(Scene.camera);
	RwCameraSetRaster(Scene.camera, savedRaster);
	RwCameraSetZRaster(Scene.camera, savedZRaster);
	RwRGBA black = { 0, 0, 0, 255 };
	RwCameraClear(Scene.camera, &black, rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
	RwCameraBeginUpdate(Scene.camera);

	// On the Mali blob the 3D image ends up in the top glosScaleH rows of the
	// texture, upright when sampled top-down (v = 1 - scale at the image's top
	// edge, 1 at its bottom). Sample texel centres so the edges don't bleed.
	float tw = RwRasterGetWidth(glosScaleTarget), th = RwRasterGetHeight(glosScaleTarget);
	float u0 = 0.5f / tw, u1 = (glosScaleW - 0.5f) / tw;
	float vTop = (th - glosScaleH + 0.5f) / th, vBottom = (th - 0.5f) / th;
	float w = RwRasterGetWidth(savedRaster), h = RwRasterGetHeight(savedRaster);
	float z = RwIm2DGetFarScreenZ();	// stay behind the radar's depth mask
	float recipZ = 1.0f / RwCameraGetNearClipPlane(Scene.camera);
	RwIm2DVertex v[4];
	setVertex(&v[0], 0.0f, 0.0f, u0, vTop, z, recipZ);
	setVertex(&v[1], 0.0f, h, u0, vBottom, z, recipZ);
	setVertex(&v[2], w, h, u1, vBottom, z, recipZ);
	setVertex(&v[3], w, 0.0f, u1, vTop, z, recipZ);

	static const RwRenderState states[] = {
		rwRENDERSTATETEXTUREFILTER, rwRENDERSTATETEXTUREADDRESS, rwRENDERSTATEZTESTENABLE,
		rwRENDERSTATEZWRITEENABLE, rwRENDERSTATEVERTEXALPHAENABLE, rwRENDERSTATEFOGENABLE,
		rwRENDERSTATECULLMODE
	};
	static const uint32 values[] = {
		rwFILTERLINEAR, rwTEXTUREADDRESSCLAMP, FALSE, FALSE, FALSE, FALSE, rwCULLMODECULLNONE
	};
	uint32 saved[ARRAY_SIZE(states)];
	void *savedTexture;
	RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &savedTexture);
	for(int i = 0; i < (int)ARRAY_SIZE(states); i++){
		RwRenderStateGet(states[i], &saved[i]);
		RwRenderStateSet(states[i], (void*)(uintptr)values[i]);
	}
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, glosScaleTarget);
	RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, v, 4);
	// The HUD's radar masks its map with the depth buffer: hand it a clean one.
	RwCameraClear(Scene.camera, &black, rwCAMERACLEARZ);
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, savedTexture);
	for(int i = 0; i < (int)ARRAY_SIZE(states); i++)
		RwRenderStateSet(states[i], (void*)(uintptr)saved[i]);
}
