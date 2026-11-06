#!/bin/sh
# Adapt re3 (GTA III branch) to goodluckOS: SDL2 + KMSDRM + OpenGL ES 2.0.
# Run from the re3 source root.
set -eu
SHIM_DIR=$1

# re3's GLFW skeleton drives everything; run it on SDL2 through the shim.
cp "$SHIM_DIR"/glfw_sdl2_shim.h "$SHIM_DIR"/glfw_sdl2_shim.cpp src/skel/glfw/
# Every "GLFW, not SDL2" code path in re3 should be taken, since the shim is GLFW.
sed -i 's/ && !defined LIBRW_SDL2//' src/skel/glfw/glfw.cpp src/core/config.h src/core/MenuScreensCustom.cpp
! grep -rq 'LIBRW_SDL2' src
# ...except fetching keyboard input from X11, which goodluckOS doesn't have.
sed -i 's/^#if !defined _WIN32 && !defined __APPLE__ && !defined __SWITCH__/& \&\& !defined LIBRW_SDL2/' src/skel/glfw/glfw.cpp
cat >> src/CMakeLists.txt <<'EOF'
if(LIBRW_GL3_GFXLIB STREQUAL "SDL2")
    target_compile_options(${EXECUTABLE} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/skel/glfw/glfw_sdl2_shim.h)
endif()
EOF

# Say why RenderWare failed to start instead of exiting silently.
perl -0pi -e 's/(if\( rsEVENTERROR == RsEventHandler\(rsRWINITIALIZE, &openParams\) \)\n\t\{\n)/$1\t\tfprintf(stderr, "re3: RenderWare init failed: %s\\n", SDL_GetError());\n/' src/skel/glfw/glfw.cpp
grep -q 'RenderWare init failed' src/skel/glfw/glfw.cpp
# Line-buffer stdout so the log is readable while the game runs.
perl -0pi -e 's/(main\(int argc, char \*argv\[\]\)\n\{\n#endif\n\tRwV2d pos;\n\tRwInt32 i;\n)/$1\tsetvbuf(stdout, NULL, _IOLBF, 0);\n/' src/skel/glfw/glfw.cpp
grep -q 'setvbuf(stdout' src/skel/glfw/glfw.cpp

# librw's SDL2 backend leaves display "subsystems" unimplemented, so re3 sees
# zero displays and refuses to start. Report SDL's displays instead.
perl -0pi -e 's|\t// TODO: implement subsystems\n|\tcase DEVICEGETNUMSUBSYSTEMS:\n\t\treturn SDL_GetNumVideoDisplays();\n\tcase DEVICEGETCURRENTSUBSYSTEM:\n\t\treturn 0;\n\tcase DEVICESETSUBSYSTEM:\n\t\tif(n >= SDL_GetNumVideoDisplays())\n\t\t\treturn 0;\n\t\tmakeVideoModeList(n);\n\t\treturn 1;\n\tcase DEVICEGETSUBSSYSTEMINFO:\n\t\tif(n >= SDL_GetNumVideoDisplays())\n\t\t\treturn 0;\n\t\tstrncpy(((SubSystemInfo*)arg)->name, SDL_GetDisplayName(n) ? SDL_GetDisplayName(n) : "Display", sizeof(SubSystemInfo::name));\n\t\treturn 1;\n|' vendor/librw/src/gl/gl3device.cpp
grep -q 'return SDL_GetNumVideoDisplays();' vendor/librw/src/gl/gl3device.cpp

# Mali-400 has a small vertex uniform budget: drop re3's extra shader effects
# (not in the original game) and size the skinning palette for GTA III's peds.
for d in EXTENDED_COLOURFILTER EXTENDED_PIPELINES SCREEN_DROPLETS NEW_RENDERER; do
    sed -i "s|^#define $d\b|//#define $d|" src/core/config.h
    ! grep -q "^#define $d\b" src/core/config.h
done
# GPU time savers: PS2-style alpha test draws alpha geometry twice, and the
# "Trails" motion blur copies the whole framebuffer every frame (a full tile
# flush on Mali). Trails stays available in the Display menu.
sed -i 's|^#define PS2_ALPHA_TEST\b|//#define PS2_ALPHA_TEST|' src/core/config.h
! grep -q '^#define PS2_ALPHA_TEST' src/core/config.h
sed -i 's/^\tBlurOn = true;$/\tBlurOn = false;/' src/render/MBlur.cpp
grep -qP '^\tBlurOn = false;$' src/render/MBlur.cpp
sed -i 's/^\tCMBlur::BlurOn = true;$/\tCMBlur::BlurOn = false;/' src/core/Frontend.cpp
grep -qP '^\tCMBlur::BlurOn = false;$' src/core/Frontend.cpp
# 64 bone mat4s (256 vectors) overflow Mali-400's vertex uniforms, but Vice
# City's cutscene characters use up to 64 bones (face rigs). Send each bone
# as 3 rows instead (the 4th is always 0 0 0 1): 192 vectors, and only as
# many bones as the model has.
for f in vendor/librw/src/gl/shaders/skin_gl.inc vendor/librw/src/gl/shaders/skin.vert; do
    perl -0pi -e 's/uniform mat4 u_boneMatrices\[64\];/uniform vec4 u_boneMatrices[192];/' $f
    perl -0pi -e 's/SkinVertex \+= \(u_boneMatrices\[int\(in_indices\[i\]\)\] \* vec4\(in_pos, 1\.0\)\)\.xyz \* in_weights\[i\];/int b = int(in_indices[i]) * 3; vec4 p = vec4(in_pos, 1.0); SkinVertex += vec3(dot(u_boneMatrices[b], p), dot(u_boneMatrices[b+1], p), dot(u_boneMatrices[b+2], p)) * in_weights[i];/' $f
    perl -0pi -e 's/SkinNormal \+= \(mat3\(u_boneMatrices\[int\(in_indices\[i\]\)\]\) \* in_normal\) \* in_weights\[i\];/SkinNormal += vec3(dot(u_boneMatrices[b].xyz, in_normal), dot(u_boneMatrices[b+1].xyz, in_normal), dot(u_boneMatrices[b+2].xyz, in_normal)) * in_weights[i];/' $f
    grep -q 'u_boneMatrices\[192\]' $f
    grep -q 'dot(u_boneMatrices\[b+2\].xyz, in_normal)' $f
done
perl -0pi -e 's/\tglUniformMatrix4fv\(U\(u_boneMatrices\), 64, GL_FALSE,\n\s+\(GLfloat\*\)skinMatrices\);/\t{\n\t\tstatic float rows[64*12];\n\t\tint n = skin->numBones < 64 ? skin->numBones : 64;\n\t\tm = (Matrix*)skinMatrices;\n\t\tfor(i = 0; i < n; i++, m++){\n\t\t\tfloat *r = &rows[i*12];\n\t\t\tr[0] = m->right.x; r[1] = m->up.x; r[2] = m->at.x; r[3] = m->pos.x;\n\t\t\tr[4] = m->right.y; r[5] = m->up.y; r[6] = m->at.y; r[7] = m->pos.y;\n\t\t\tr[8] = m->right.z; r[9] = m->up.z; r[10] = m->at.z; r[11] = m->pos.z;\n\t\t}\n\t\tglUniform4fv(U(u_boneMatrices), n*3, rows);\n\t}/' vendor/librw/src/gl/gl3skin.cpp
grep -q 'glUniform4fv(U(u_boneMatrices), n\*3, rows);' vendor/librw/src/gl/gl3skin.cpp

# Ask for a 24-bit depth buffer (SDL defaults to 16, which makes shadows and
# decals z-fight), and log what SDL and EGL actually gave us.
perl -0pi -e 's/(\tSDL_GL_SetAttribute\(SDL_GL_MULTISAMPLESAMPLES, glGlobals.numSamples\);\n)/$1\tSDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);\n\tSDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);\n/' vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(\tglGlobals.window = win;\n\tglGlobals.glcontext = ctx;\n)/$1\t{ int ww, wh, depth = 0, stencil = 0; SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth); SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);\n\t  printf("librw: mode %dx%d, window %dx%d, depth %d stencil %d, renderer %s\\n", mode->mode.w, mode->mode.h, ww, wh, depth, stencil, glGetString(GL_RENDERER)); }\n/' vendor/librw/src/gl/gl3device.cpp
grep -q 'SDL_GL_DEPTH_SIZE, 24' vendor/librw/src/gl/gl3device.cpp
grep -q 'librw: mode' vendor/librw/src/gl/gl3device.cpp

# Optional FPS overlay (RE3_FPS_COUNTER / RE3_DRAW_STATS / RE3_FPS_LOG, see shim/glos_fps.cpp).
cp "$SHIM_DIR"/glos_fps.cpp "$SHIM_DIR"/glos_prof.cpp src/extras/
perl -0pi -e 's/(\n\tDoRWStuffEndOfFrame\(\);\n\n\tPOP_MEMID\(\);\t\/\/ MEMID_RENDER\n)/\n\tGlosDrawFPS();\n$1/' src/core/main.cpp
perl -0pi -e 's/(\nvoid\nRender2dStuffAfterFade\(void\))/\nvoid GlosDrawFPS(void);\n$1/' src/core/main.cpp
[ "$(grep -c 'GlosDrawFPS' src/core/main.cpp)" -eq 2 ]
# Count GL draw calls for the overlay (optimisation aid).
for f in vendor/librw/src/gl/gl3render.cpp vendor/librw/src/gl/gl3immed.cpp; do
    sed -i '1i extern int glosDrawCalls;' "$f"
    sed -i 's/^\tgl\(DrawElements\|DrawArrays\)(/\tglosDrawCalls++; gl\1(/' "$f"
done
[ "$(grep -c 'glosDrawCalls++' vendor/librw/src/gl/gl3render.cpp vendor/librw/src/gl/gl3immed.cpp | awk -F: '{s+=$2} END {print s}')" -eq 5 ]

# OpenGL ES 2.0 texture rules: the internal format must be unsized (GL_RGBA, not
# GL_RGBA8), GL_TEXTURE_MAX_LEVEL doesn't exist, and a texture with an
# incomplete mip chain samples as black. Otherwise every texture is black.
R=vendor/librw/src/gl/gl3raster.cpp
perl -0pi -e 's/(\nstatic Raster\*\nrasterCreateTexture\(Raster \*raster\)\n)/\n#define ES2_ONLY (gl3Caps.gles \&\& gl3Caps.glversion < 30)\nstatic GLint esFmt(GLint internal, GLenum format) { return ES2_ONLY ? (GLint)format : internal; }\n$1/' $R
sed -i 's/glTexImage2D(GL_TEXTURE_2D, \(0\|level\), natras->internalFormat,/glTexImage2D(GL_TEXTURE_2D, \1, esFmt(natras->internalFormat, natras->format),/' $R
sed -i 's/^\(\s*\)glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, natras->numLevels-1);/\1if(!ES2_ONLY) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, natras->numLevels-1);/' $R
# Mipmaps: ES 2.0 can only mipmap power-of-two textures, and only with a
# complete chain (TXDs often stop early). Generate the chain after level 0 and
# fall back to plain filtering for NPOT textures.
perl -0pi -e 's/(\t\t\tif\(level == 0 && natras->autogenMipmap\))/\t\t\tif(level == 0 \&\& !natras->autogenMipmap \&\& ES2_ONLY \&\& natras->numLevels > 1 \&\&\n\t\t\t   !(raster->width \& (raster->width-1)) \&\& !(raster->height \& (raster->height-1)))\n\t\t\t\tglGenerateMipmap(GL_TEXTURE_2D);\n$1/' $R
D=vendor/librw/src/gl/gl3device.cpp
sed -i 's/GL_TEXTURE_MAG_FILTER, filterConvMap_MIP\[filter\]/GL_TEXTURE_MAG_FILTER, filterConvMap_NoMIP[filter]/' $D
perl -0pi -e 's/(\nstatic void\nsetFilterMode\()/\nstatic bool\nnoMipES2(Raster *r)\n{\n\treturn gl3Caps.gles \&\& gl3Caps.glversion < 30 \&\&\n\t       ((r->width \& (r->width-1)) || (r->height \& (r->height-1)));\n}\n$1/' $D
sed -i 's/GL_TEXTURE_MIN_FILTER, filterConvMap_MIP\[filter\]/GL_TEXTURE_MIN_FILTER, (noMipES2(raster) ? filterConvMap_NoMIP : filterConvMap_MIP)[filter]/' $D
[ "$(grep -c 'esFmt(natras->internalFormat' $R)" -ge 3 ]
grep -q 'if(!ES2_ONLY) glTexParameteri' $R
grep -q 'ES2_ONLY && natras->numLevels > 1' $R
[ "$(grep -c 'noMipES2(raster)' $D)" -eq 2 ]

# 2D vertices sit exactly on the near plane (z = -1), which Mali-400 clips.
# Scale z a hair inward; scaling (not clamping) keeps the radar's depth mask
# in front of the map.
sed -i 's|^"\tv_fog = DoFog(gl_Position.w);\\n"$|&\n"\tgl_Position.z *= 0.999;\\n"|' vendor/librw/src/gl/shaders/im2d_gl.inc
sed -i 's|^\tv_fog = DoFog(gl_Position.w);$|&\n\tgl_Position.z *= 0.999;|' vendor/librw/src/gl/shaders/im2d.vert
grep -q 'gl_Position.z \*= 0.999' vendor/librw/src/gl/shaders/im2d_gl.inc

# Mali-400's vertex unit (gpir) caps shaders at 512 instructions and has no
# real loops, so the 8-light loop is unrolled. Four lights fit. (MAX_LIGHTS
# build arg overrides, for experiments.)
L=${MAX_LIGHTS:-4}
sed -i "s/#define MAX_LIGHTS 8/#define MAX_LIGHTS $L/" vendor/librw/src/gl/shaders/header_vs.inc vendor/librw/src/gl/shaders/header.vert vendor/librw/src/gl/gl3device.cpp
[ "$(grep -l "define MAX_LIGHTS $L" vendor/librw/src/gl/shaders/header_vs.inc vendor/librw/src/gl/gl3device.cpp | wc -l)" -eq 2 ]

# EGL-on-fbdev window system for ARM's closed Mali driver (when /dev/mali exists); see
# shim/gl3fbdev.cpp. SDL keeps handling input and audio.
cp "$SHIM_DIR"/gl3fbdev.cpp vendor/librw/src/gl/
sed -i 's|^\(\s*\)gl/gl3device.cpp$|&\n\1gl/gl3fbdev.cpp|' vendor/librw/src/CMakeLists.txt
grep -q 'gl/gl3fbdev.cpp' vendor/librw/src/CMakeLists.txt
D=vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(namespace gl3 \{\n)/$1\nbool fbdevActive(void);\nbool fbdevGetSize(int *w, int *h);\nbool fbdevStart(int w, int h);\nvoid fbdevSwap(bool vsync);\nvoid fbdevStop(void);\nvoid *fbdevGetProcAddress(const char *name);\n/' $D
perl -0pi -e 's/(\tmemset\(&gl3Caps, 0, sizeof\(gl3Caps\)\);\n)/$1\n\tif(fbdevActive()){\n\t\tint w, h;\n\t\tif(!fbdevGetSize(&w, &h))\n\t\t\treturn 0;\n\t\trwFree(glGlobals.modes);\n\t\tglGlobals.modes = rwNewT(DisplayMode, 2, ID_DRIVER | MEMDUR_EVENT);\n\t\tmemset(glGlobals.modes, 0, 2*sizeof(DisplayMode));\n\t\tfor(int i = 0; i < 2; i++){\n\t\t\tglGlobals.modes[i].mode.w = w;\n\t\t\tglGlobals.modes[i].mode.h = h;\n\t\t\tglGlobals.modes[i].mode.format = SDL_PIXELFORMAT_ARGB8888;\n\t\t\tglGlobals.modes[i].depth = 32;\n\t\t}\n\t\tglGlobals.modes[1].flags = VIDEOMODEEXCLUSIVE;\n\t\tglGlobals.numModes = 2;\n\t\treturn 1;\n\t}\n/' $D
perl -0pi -e 's/(\tmode = &glGlobals.modes\[glGlobals.currentMode\];\n)/$1\n\tif(fbdevActive()){\n\t\tstatic int fakeWindow;\n\t\tif(!fbdevStart(mode->mode.w, mode->mode.h))\n\t\t\treturn 0;\n\t\tgl3Caps.gles = 1;\n\t\tgl3Caps.glversion = 20;\n\t\tif(!gladLoadGLES2Loader((GLADloadproc)fbdevGetProcAddress, 20)){\n\t\t\tRWERROR((ERR_GENERAL, "gladLoadGLES2Loader failed"));\n\t\t\treturn 0;\n\t\t}\n\t\tprintf("OpenGL version: %s, renderer %s (fbdev)\\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));\n\t\tglGlobals.window = (SDL_Window*)&fakeWindow;\n\t\tglGlobals.glcontext = nil;\n\t\t*glGlobals.pWindow = glGlobals.window;\n\t\tglGlobals.presentWidth = 0;\n\t\tglGlobals.presentHeight = 0;\n\t\tglGlobals.presentOffX = 0;\n\t\tglGlobals.presentOffY = 0;\n\t\treturn 1;\n\t}\n/' $D
perl -0pi -e 's/\t\tSDL_GetWindowSize\(glGlobals.window, &w, &h\);\n#else\n\t\tglfwGetFramebufferSize/\t\tif(fbdevActive())\n\t\t\tfbdevGetSize(&w, &h);\n\t\telse\n\t\t\tSDL_GetWindowSize(glGlobals.window, &w, &h);\n#else\n\t\tglfwGetFramebufferSize/' $D
perl -0pi -e 's/(\tif\(flags & Raster::FLIPWAITVSYNCH\)\n\t\tSDL_GL_SetSwapInterval\(1\);\n\telse\n\t\tSDL_GL_SetSwapInterval\(0\);\n\tSDL_GL_SwapWindow\(glGlobals.window\);\n)/\tif(fbdevActive()){\n\t\tfbdevSwap(flags & Raster::FLIPWAITVSYNCH);\n\t\treturn;\n\t}\n$1/' $D
perl -0pi -e 's/(\nstatic int\nstopSDL2\(void\)\n\{\n)/$1\tif(fbdevActive()){\n\t\tfbdevStop();\n\t\treturn 1;\n\t}\n/' $D
sed -i 's/SDL_GetNumVideoDisplays()/(fbdevActive() ? 1 : SDL_GetNumVideoDisplays())/g; s/SDL_GetDisplayName(n) ? SDL_GetDisplayName(n) : "Display"/fbdevActive() ? "fbdev" : SDL_GetDisplayName(n) ? SDL_GetDisplayName(n) : "Display"/; s/^\t\tmakeVideoModeList(n);$/\t\tif(!fbdevActive())\n\t\t\tmakeVideoModeList(n);/' $D
[ "$(grep -c 'fbdevActive()' $D)" -ge 9 ]

# ARM's blob does a full GPU memory allocation for every glBufferData orphan,
# and Im2D/Im3D orphan their buffers on every draw (HUD, radar, particles).
# On the fbdev/ARM path, feed those draws from client-side arrays instead,
# which the blob copies into its own streaming memory.
I=vendor/librw/src/gl/gl3immed.cpp
perl -0pi -e 's/(namespace gl3 \{\n)/$1\nbool fbdevActive(void);\n\nstatic void\nsetAttribPointersClient(AttribDesc *attribDescs, int32 numAttribs, void *base)\n{\n\tfor(AttribDesc *a = attribDescs; a != &attribDescs[numAttribs]; a++){\n\t\tglEnableVertexAttribArray(a->index);\n\t\tglVertexAttribPointer(a->index, a->size, a->type, a->normalized,\n\t\t                      a->stride, (uint8*)base + a->offset);\n\t}\n}\n/' $I
perl -0pi -e 's/(\nim2DRenderPrimitive\(PrimitiveType primType, void \*vertices, int32 numVertices\)\n\{\n)/$1\tif(fbdevActive()){\n\t\tglBindBuffer(GL_ARRAY_BUFFER, 0);\n\t\tif(im2dOverrideShader)\n\t\t\tim2dOverrideShader->use();\n\t\telse\n\t\t\tim2dShader->use();\n\t\tsetAttribPointersClient(im2dattribDesc, 3, vertices);\n\t\tim2DSetXform();\n\t\tflushCache();\n\t\tglosDrawCalls++; glDrawArrays(primTypeMap[primType], 0, numVertices);\n\t\tdisableAttribPointers(im2dattribDesc, 3);\n\t\treturn;\n\t}\n/' $I
perl -0pi -e 's/(\nim2DRenderIndexedPrimitive\(PrimitiveType primType,\n[^{]*\{\n)/$1\tif(fbdevActive()){\n\t\tglBindBuffer(GL_ARRAY_BUFFER, 0);\n\t\tglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);\n\t\tif(im2dOverrideShader)\n\t\t\tim2dOverrideShader->use();\n\t\telse\n\t\t\tim2dShader->use();\n\t\tsetAttribPointersClient(im2dattribDesc, 3, vertices);\n\t\tim2DSetXform();\n\t\tflushCache();\n\t\tglosDrawCalls++; glDrawElements(primTypeMap[primType], numIndices, GL_UNSIGNED_SHORT, indices);\n\t\tdisableAttribPointers(im2dattribDesc, 3);\n\t\treturn;\n\t}\n/' $I
perl -0pi -e 's/(\tglBindBuffer\(GL_ARRAY_BUFFER, im3DVbo\);\n\tglBufferData\(GL_ARRAY_BUFFER, STARTVERTICES\*sizeof\(Im3DVertex\), nil, GL_STREAM_DRAW\);\n\tglBufferSubData\(GL_ARRAY_BUFFER, 0, numVertices\*sizeof\(Im3DVertex\), vertices\);\n#ifndef RW_GL_USE_VAOS\n\tsetAttribPointers\(im3dattribDesc, 3\);\n#endif\n)/\tif(fbdevActive()){\n\t\tglBindBuffer(GL_ARRAY_BUFFER, 0);\n\t\tsetAttribPointersClient(im3dattribDesc, 3, vertices);\n\t\tnum3DVertices = numVertices;\n\t\treturn;\n\t}\n$1/' $I
perl -0pi -e 's/(\nim3DRenderIndexedPrimitive\(PrimitiveType primType, void \*indices, int32 numIndices\)\n\{\n)/$1\tif(fbdevActive()){\n\t\tglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);\n\t\tflushCache();\n\t\tglosDrawCalls++; glDrawElements(primTypeMap[primType], numIndices, GL_UNSIGNED_SHORT, indices);\n\t\treturn;\n\t}\n/' $I
[ "$(grep -c 'if(fbdevActive())' $I)" -eq 4 ]
grep -q 'setAttribPointersClient(im3dattribDesc' $I

# GL command thread on the fbdev/ARM path (shim/gl3thread.cpp): the game thread
# records GL calls, a render thread that owns the EGL context replays them.
# Off unless RE3_GL_THREAD=1 (start.sh sets it).
cp "$SHIM_DIR"/gl3thread.cpp "$SHIM_DIR"/gl3thread_gen.inc vendor/librw/src/gl/
sed -i 's|^\(\s*\)gl/gl3fbdev.cpp$|&\n\1gl/gl3thread.cpp|' vendor/librw/src/CMakeLists.txt
grep -q 'gl/gl3thread.cpp' vendor/librw/src/CMakeLists.txt
D=vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(void \*fbdevGetProcAddress\(const char \*name\);\n)/$1bool glthreadEnabled(void);\nbool glthreadStart(int w, int h);\nbool fbdevStartGL(int w, int h);\nvoid glosSwap(bool vsync);\nvoid glosStop(void);\n/' $D
perl -0pi -e 's/\t\tif\(!fbdevStart\(mode->mode.w, mode->mode.h\)\)\n\t\t\treturn 0;\n\t\tgl3Caps.gles = 1;\n\t\tgl3Caps.glversion = 20;\n\t\tif\(!gladLoadGLES2Loader\(\(GLADloadproc\)fbdevGetProcAddress, 20\)\)\{\n\t\t\tRWERROR\(\(ERR_GENERAL, "gladLoadGLES2Loader failed"\)\);\n\t\t\treturn 0;\n\t\t\}\n/\t\tgl3Caps.gles = 1;\n\t\tgl3Caps.glversion = 20;\n\t\tif(!(glthreadEnabled() ? glthreadStart(mode->mode.w, mode->mode.h)\n\t\t                       : fbdevStartGL(mode->mode.w, mode->mode.h))){\n\t\t\tRWERROR((ERR_GENERAL, "fbdev GL start failed"));\n\t\t\treturn 0;\n\t\t}\n/' $D
sed -i 's/\t\tfbdevSwap(flags & Raster::FLIPWAITVSYNCH);/\t\tglosSwap(flags \& Raster::FLIPWAITVSYNCH);/; s/^\t\tfbdevStop();$/\t\tglosStop();/' $D
grep -q 'glthreadStart(mode->mode.w' $D
grep -q 'glosSwap(flags' $D
grep -q '^\s*glosStop();' $D
! grep -q 'fbdevSwap(flags\|^\s*fbdevStop();' $D
I=vendor/librw/src/gl/gl3immed.cpp
perl -0pi -e 's/\nstatic void\nsetAttribPointersClient\(AttribDesc \*attribDescs, int32 numAttribs, void \*base\)\n\{\n/\nvoid *glthreadPersist(const void *src, size_t n);\n\nstatic void\nsetAttribPointersClient(AttribDesc *attribDescs, int32 numAttribs, void *base, size_t bytes)\n{\n\tbase = glthreadPersist(base, bytes);	\/\/ outlives the call when the GL thread is on\n/' $I
sed -i 's/setAttribPointersClient(im2dattribDesc, 3, vertices);/setAttribPointersClient(im2dattribDesc, 3, vertices, numVertices*sizeof(Im2DVertex));/; s/setAttribPointersClient(im3dattribDesc, 3, vertices);/setAttribPointersClient(im3dattribDesc, 3, vertices, numVertices*sizeof(Im3DVertex));/' $I
[ "$(grep -c 'sizeof(Im2DVertex));' $I)" -eq 2 ]
grep -q 'sizeof(Im3DVertex));' $I
grep -q 'base = glthreadPersist(base, bytes);' $I

# Particle overdraw budget. Up close, smoke covers the screen 13-14 times per
# frame and the Mali-400 can't shade that many blended pixels. Each frame we add
# up the screen area of all particles; above RE3_PARTICLE_BUDGET screens
# (off unless set; start.sh uses 5) a matching share of the *large* particles is skipped
# next frame. A per-particle hash picks which, so the same puffs stay hidden
# instead of flickering; small particles (sparks, debris) are always drawn.
# RE3_NO_PARTICLES=1 skips particles entirely (diagnostics).
P=src/render/Particle.cpp
perl -0pi -e 's/(\nvoid CParticle::Render\(\)\n\{\n)/\nstatic float glosParticleArea, glosDropFrac;\n\nstatic bool\nglosKeepParticle(const void *p, float w, float h)\n{\n\tfloat screen = SCREEN_WIDTH * SCREEN_HEIGHT;\n\tfloat area = Min(4.0f * w * h, screen);\n\tglosParticleArea += area;\n\tif(glosDropFrac <= 0.0f || area < screen * 0.01f)\n\t\treturn true;\n\tuint32 hash = (uint32)((uintptr_t)p >> 3) * 2654435761u;\n\treturn (hash >> 16) \/ 65536.0f >= glosDropFrac;\n}\n$1\tstatic int noParticles = -1;\n\tstatic float budget;\n\tif(noParticles < 0){\n\t\tnoParticles = getenv("RE3_NO_PARTICLES") != nil;\n\t\tconst char *b = getenv("RE3_PARTICLE_BUDGET");\n\t\tbudget = b ? atof(b) : 0.0f;\n\t}\n\tif(noParticles)\n\t\treturn;\n\t{\n\t\tfloat overdraw = glosParticleArea \/ (SCREEN_WIDTH * SCREEN_HEIGHT);\n\t\tfloat target = budget > 0.0f \&\& overdraw > budget ? Min(1.0f - budget \/ overdraw, 0.9f) : 0.0f;\n\t\tglosDropFrac = glosDropFrac * 0.8f + target * 0.2f;\n\t\tstatic int logFrames;\n\t\tstatic bool log = getenv("RE3_FPS_LOG") != nil;\n\t\tif(log \&\& ++logFrames % 15 == 0)\n\t\t\tprintf("glos: particles overdraw %.1fx drop %.2f\\n", overdraw, glosDropFrac);\n\t\tglosParticleArea = 0.0f;\n\t}\n/' $P
sed -i 's/^\t\t\t\tif ( CSprite::CalcScreenCoors(particle->m_vecPosition, \&coors, \&w, \&h, true) )$/\t\t\t\tif ( CSprite::CalcScreenCoors(particle->m_vecPosition, \&coors, \&w, \&h, true) \&\&\n\t\t\t\t     glosKeepParticle(particle, particle->m_fSize * w, particle->m_fSize * h) )/' $P
grep -q 'getenv("RE3_PARTICLE_BUDGET")' $P
grep -q 'glosKeepParticle(particle, particle->m_fSize' $P
# re3's own low-end mode: the PC version's particle limits (PC_PARTICLE), VC's
# cheaper ped collisions, fewer rain particles, per-island loading.
sed -i 's|^//#define SQUEEZE_PERFORMANCE$|#define SQUEEZE_PERFORMANCE|' src/core/config.h
grep -q '^#define SQUEEZE_PERFORMANCE$' src/core/config.h

# Count screen area covered by buffered sprites (particles, coronas) for the
# RE3_DRAW_STATS overlay: overdraw in "screens per frame".
S=src/render/Sprite.cpp
perl -0pi -e 's/(\n[^\n]*\nCSprite::RenderBufferedOneXLUSprite)/\nvoid glosCountSprite(float x, float y, float w, float h);\n$1/; s/^(\tm_bFlushSpriteBufferSwitchZTest = [01];\n)/$1\tglosCountSprite(x, y, w, h);\n/mg' $S
[ "$(grep -c 'glosCountSprite(x, y, w, h);' $S)" -eq 6 ]

# Release-style build: no debug menu, debug timers (several by-name lookups
# per frame), loading chatter or script debug output. Not FINAL, which would
# also move the save files. Replay recording (every frame) is compiled out;
# Replay.h has stubs for that.
C=src/core/config.h
perl -0pi -e 's/(\n#ifdef MASTER\n\t\/\/ only in master builds)/\n#define MASTER\n$1/' $C
perl -0pi -e 's/(\n#define FIX_BUGS\t)/\n#undef TIMEBARS\n#undef CHATTYSPLASH\n$1/' $C
sed -i 's|^#\tdefine GTA_REPLAY$|//#\tdefine GTA_REPLAY\t// goodluckOS: no per-frame replay recording|' $C
grep -q '^#define MASTER$' $C
grep -q '^#undef TIMEBARS$' $C
! grep -q '^#\s*define GTA_REPLAY' $C

# NEON for the two hottest matrix routines (librw Matrix::mult_, re3
# CMatrix * CVector): whole-row multiply-accumulates instead of 9-12 scalar
# expressions.
B=vendor/librw/src/base.cpp
perl -0pi -e 's/(#include <math.h>\n)/$1#ifdef __ARM_NEON\n#include <arm_neon.h>\n#endif\n/' $B
perl -0pi -e 's/(\nMatrix::mult_\(Matrix \*dst, const Matrix \*src1, const Matrix \*src2\)\n\{\n)(.*?)(\n\}\n)/$1#ifdef __ARM_NEON\n\t\/\/ Each dst row = a.x*b.right + a.y*b.up + a.z*b.at (+ b.pos for pos);\n\t\/\/ all loads happen before the stores, so dst may alias a source.\n\tconst float32 *a = \&src1->right.x, *b = \&src2->right.x;\n\tfloat32 *d = \&dst->right.x;\n\tfloat32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b+4), b2 = vld1q_f32(b+8), b3 = vld1q_f32(b+12);\n\tfloat32x4_t r[4];\n\tfor(int i = 0; i < 4; i++)\n\t\tr[i] = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(b0, a[4*i]), b1, a[4*i+1]), b2, a[4*i+2]);\n\tr[3] = vaddq_f32(r[3], b3);\n\tfor(int i = 0; i < 4; i++){\n\t\tvst1_f32(d+4*i, vget_low_f32(r[i]));\n\t\tvst1q_lane_f32(d+4*i+2, r[i], 2);\t\/\/ leave flags\/padding alone\n\t}\n#else\n$2\n#endif$3/s' $B
grep -q 'vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(b0' $B
V=src/math/Vector.cpp
perl -0pi -e 's/(#include "common.h"\n)/$1#ifdef __ARM_NEON\n#include <arm_neon.h>\n#endif\n/' $V
perl -0pi -e 's/(operator\*\(const CMatrix &mat, const CVector &vec\)\n\{\n)\t\/\/ TODO: VU0 code\n/$1#ifdef __ARM_NEON\n\tfloat32x4_t v = vld1q_f32(\&mat.px);\n\tv = vmlaq_n_f32(v, vld1q_f32(\&mat.rx), vec.x);\n\tv = vmlaq_n_f32(v, vld1q_f32(\&mat.fx), vec.y);\n\tv = vmlaq_n_f32(v, vld1q_f32(\&mat.ux), vec.z);\n\treturn CVector(vgetq_lane_f32(v, 0), vgetq_lane_f32(v, 1), vgetq_lane_f32(v, 2));\n#endif\n/' $V
grep -q 'vld1q_f32(&mat.rx)' $V

# Count texture binds for the RE3_DRAW_STATS overlay (total, and how many re-bind
# the texture that's already bound).
D=vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(\n#define PLUGIN_ID 0\n)/\nextern int glosTexBinds, glosTexBindsRedundant;	\/\/ glos_fps.cpp\n$1/' $D
perl -0pi -e 's/(\nbindTexture\(uint32 texid\)\n\{\n\tuint32 prev = boundTexture\[activeTexture\];\n)/$1\tglosTexBinds++;\n\tif(prev == texid)\n\t\tglosTexBindsRedundant++;\n/' $D
grep -q 'extern int glosTexBinds' $D
grep -q 'glosTexBindsRedundant++;' $D

# Particles (smoke etc.) are Im3D draws with fog off, no depth writes and
# blending on; up close they cover the screen many layers deep and the Mali-400
# runs out of pixel throughput. For exactly that state use a fragment shader
# that is just colour x texture: no fog mix, no alpha-test discard (pointless
# when blending without depth writes, and costly on Mali). RE3_FAST_PARTICLES=1
# turns it on (start.sh sets it).
I=vendor/librw/src/gl/gl3immed.cpp
perl -0pi -e 's/(static Shader \*im3dShader;\n)/$1static Shader *im3dFastShader;\n/' $I
perl -0pi -e 's/(\tim3dShader = Shader::create\(vs, fs\);\n\tassert\(im3dShader\);\n)/$1\tstatic const char *fast_frag_src =\n\t\t"uniform sampler2D tex0;\\n"\n\t\t"FSIN vec4 v_color;\\n"\n\t\t"FSIN vec2 v_tex0;\\n"\n\t\t"FSIN float v_fog;\\n"\n\t\t"void\\nmain(void)\\n{\\n"\n\t\t"\tFRAGCOLOR(v_color*texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y)));\\n"\n\t\t"}\\n";\n\tconst char *ffs[] = { shaderDecl, header_frag_src, fast_frag_src, nil };\n\tim3dFastShader = Shader::create(vs, ffs);\n\tassert(im3dFastShader);\n/' $I
perl -0pi -e 's/(\tim3dShader->destroy\(\);\n\tim3dShader = nil;\n)/$1\tim3dFastShader->destroy();\n\tim3dFastShader = nil;\n/' $I
perl -0pi -e 's/\tsetWorldMatrix\(world\);\n\tim3dShader->use\(\);\n/\tsetWorldMatrix(world);\n\tstatic int fastParticles = -1;\n\tif(fastParticles < 0){\n\t\tconst char *s = getenv("RE3_FAST_PARTICLES");\n\t\tfastParticles = s \&\& strcmp(s, "1") == 0;\n\t}\n\tif(fastParticles \&\& !rw::GetRenderState(rw::ZWRITEENABLE) \&\&\n\t   !rw::GetRenderState(rw::FOGENABLE) \&\& rw::GetRenderState(rw::VERTEXALPHA))\n\t\tim3dFastShader->use();\n\telse\n\t\tim3dShader->use();\n/' $I
grep -q 'im3dFastShader = Shader::create' $I
grep -q 'im3dFastShader->use();' $I
grep -q 'im3dFastShader->destroy();' $I

# librw re-sends ambient + all light arrays with every object's world matrix,
# though GTA's lighting rarely changes between objects. Only send lights when
# they differ from what the current shader last got.
D=vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(static bool32 objectDirty = 1;\n)/$1static bool32 lightsDirty = 1;\nstatic uint8 lastLights[sizeof(UniformObject) - offsetof(UniformObject, ambLight)];\n/' $D
perl -0pi -e 's/out:\n\tobjectDirty = 1;\n\treturn bits;/out:\n\tif(memcmp(lastLights, &uniformObject.ambLight, sizeof(lastLights)) != 0){\n\t\tmemcpy(lastLights, &uniformObject.ambLight, sizeof(lastLights));\n\t\tlightsDirty = 1;\n\t}\n\treturn bits;/' $D
perl -0pi -e 's/(\t\tlastShaderUploaded = currentShader;\n\t\tobjectDirty = 1;\n)/$1\t\tlightsDirty = 1;\n/' $D
perl -0pi -e 's/(\t\tglUniformMatrix4fv\(U\(u_world\), 1, 0, \(float\*\)&uniformObject.world\);\n)(\t\tglUniform4fv\(U\(u_ambLight\).*?\n\t\tglUniform4fv\(U\(u_lightColor\)[^\n]*\n)(\t\tobjectDirty = 0;\n\t\}\n)/$1$3\n\tif(lightsDirty){\n$2\t\tlightsDirty = 0;\n\t}\n/s' $D
grep -q 'if(lightsDirty){' $D
[ "$(grep -c 'lightsDirty = 1' $D)" -eq 3 ]

# Texture conversion (the first-launch "converting textures" step, which
# writes models/txd.img) reads textures back from the GPU. GLES 2 can only
# read mip level 0, and librw read the smaller levels' sizes out of it, so
# every converted mip level was a corner crop of the full image: textures went
# wrong at a distance (cutscene faces showed jeans or shirt). Read level 0 and
# box-filter it down instead. Delete models/txd.img + txd.dir to reconvert.
R=vendor/librw/src/gl/gl3raster.cpp
perl -0pi -e 's/(\nrasterLock\(Raster \*raster, int32 level, int32 lockMode\)\n\{\n#ifdef RW_OPENGL\n\tGl3Raster \*natras GETGL3RASTEREXT\(raster\);\n)/$1\tint32 w0 = raster->width, h0 = raster->height;\n/' $R
perl -0pi -e 's/\t\t\t\tglReadPixels\(0, 0, raster->width, raster->height, natras->format, natras->type, px\);\n/\t\t\t\tif(level == 0)\n\t\t\t\t\tglReadPixels(0, 0, raster->width, raster->height, natras->format, natras->type, px);\n\t\t\t\telse{\n\t\t\t\t\t\/\/ only level 0 can be attached on GLES 2: read it and box-filter it down\n\t\t\t\t\tuint8 *full = (uint8*)rwMalloc(w0*h0*4, MEMDUR_EVENT | ID_DRIVER);\n\t\t\t\t\tglReadPixels(0, 0, w0, h0, natras->format, natras->type, full);\n\t\t\t\t\tint32 fx = w0\/raster->width, fy = h0\/raster->height;\n\t\t\t\t\tfor(int32 y = 0; y < raster->height; y++)\n\t\t\t\t\tfor(int32 x = 0; x < raster->width; x++)\n\t\t\t\t\tfor(int32 c = 0; c < 4; c++){\n\t\t\t\t\t\tuint32 sum = 0;\n\t\t\t\t\t\tfor(int32 j = 0; j < fy; j++)\n\t\t\t\t\t\tfor(int32 k = 0; k < fx; k++)\n\t\t\t\t\t\t\tsum += full[((y*fy+j)*w0 + x*fx+k)*4 + c];\n\t\t\t\t\t\tpx[(y*raster->width + x)*4 + c] = sum\/(fx*fy);\n\t\t\t\t\t}\n\t\t\t\t\trwFree(full);\n\t\t\t\t}\n/' $R
grep -q 'int32 w0 = raster->width, h0 = raster->height;' $R
grep -q 'only level 0 can be attached on GLES 2' $R

# Mali-400 only does OpenGL ES 2.0, so skip the desktop and ES 3.1 attempts.
sed -i '/{ SDL_GL_CONTEXT_PROFILE_CORE, /d; /{ SDL_GL_CONTEXT_PROFILE_ES, 3, 1 },/d' vendor/librw/src/gl/gl3device.cpp

# GL entry points come from glad via SDL_GL_GetProcAddress; goodluckOS has no libGL.
sed -i '/find_package(OpenGL REQUIRED)/d; /OpenGL::GL$/d' vendor/librw/src/CMakeLists.txt

# RE3_RENDER_SCALE=<percent>: draw the 3D world into the corner of an
# offscreen target and stretch it to the screen before the HUD (shim/glos_scale.cpp).
cp "$SHIM_DIR"/glos_scale.cpp src/extras/
M=src/core/main.cpp
perl -0pi -e 's/(\nbool\nDoRWStuffStartOfFrame\(int16)/\nvoid GlosScaleBegin(void);	\/\/ glos_scale.cpp\nvoid GlosScaleEnd(void);\n$1/' $M
perl -0pi -e 's/(\nDoRWStuffStartOfFrame_Horizon\(int16[^\n]*\n.*?\n\tCVisibilityPlugins::SetRenderWareCamera\(Scene.camera\);\n)/$1\tGlosScaleBegin();\n/s' $M
perl -0pi -e 's/(\n\t\ttbStartTimer\(0, "Render2dStuff"\);\n)/\n\t\tGlosScaleEnd();$1/' $M
perl -0pi -e 's/(\tFlushObrsPrintfs\(\);\n)(\tRwCameraEndUpdate\(Scene.camera\);\n\tRsCameraShowRaster)/$1\tGlosScaleEnd();	\/\/ in case the 3D frame was cut short\n$2/' $M
[ "$(grep -c 'GlosScaleBegin' $M)" -eq 2 ]
[ "$(grep -c 'GlosScaleEnd' $M)" -eq 3 ]
D=vendor/librw/src/gl/gl3device.cpp
perl -0pi -e 's/(\nextern int glosTexBinds, glosTexBindsRedundant;[^\n]*\n)/$1extern rw::Raster *glosScaleTarget;	\/\/ glos_scale.cpp\nextern int glosScaleW, glosScaleH;\n/' $D
perl -0pi -e 's/(\t\tw = fb->width;\n\t\th = fb->height;\n\t\}\n)/$1\tif(fb == glosScaleTarget){\n\t\tw = glosScaleW;\n\t\th = glosScaleH;\n\t}\n/' $D
perl -0pi -e 's/(\nstatic void\nsetFrameBuffer\(Camera \*cam\))/\nint\nglosFramebufferComplete(Raster *raster)\n{\n\tGl3Raster *natras = PLUGINOFFSET(Gl3Raster, raster, nativeRasterOffset);\n\tbindFramebuffer(natras->fbo);\n\tGLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);\n\tbindFramebuffer(0);\n\treturn status == GL_FRAMEBUFFER_COMPLETE;\n}\n$1/' $D
# GLES 2 has no combined GL_DEPTH_STENCIL_ATTACHMENT: attach the packed buffer twice.
perl -0pi -e 's/\t\t\t\t\tglFramebufferRenderbuffer\(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, natzb->texid\);\n/\t\t\t\t\t{\n\t\t\t\t\t\tglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, natzb->texid);\n\t\t\t\t\t\tglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, natzb->texid);\n\t\t\t\t\t}\n/' $D
[ "$(grep -c 'glosScaleW' $D)" -eq 2 ]
grep -q '^glosFramebufferComplete' $D
grep -q 'GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, natzb->texid' $D

grep -q 'defined RW_GL3$' src/skel/glfw/glfw.cpp
grep -q '__SWITCH__ && !defined LIBRW_SDL2' src/skel/glfw/glfw.cpp
! grep -q 'SDL_GL_CONTEXT_PROFILE_CORE, ' vendor/librw/src/gl/gl3device.cpp
echo "re3 patched"

# Control method "Classic" by default (the PS2 layout: the stick turns the
# player; "Standard" is the keyboard+mouse layout, where left/right strafe).
# A saved Method in re3.ini / reVC.ini still wins.
perl -0pi -e 's/\nint32 CMenuManager::m_ControlMethod;/\nint32 CMenuManager::m_ControlMethod = CONTROL_CLASSIC;/' src/core/Frontend.cpp
grep -q 'm_ControlMethod = CONTROL_CLASSIC;' src/core/Frontend.cpp

# Free Camera on by default: the right stick orbits the camera on foot and
# gives the follow camera in vehicles (off, the PS2 behaviour: first-person
# look). A saved FreeCam in the ini still wins.
perl -0pi -e 's/\nbool CCamera::bFreeCam = false;/\nbool CCamera::bFreeCam = true;/' src/core/Cam.cpp
grep -q 'bFreeCam = true;' src/core/Cam.cpp
