# glOSports-gtavc-pc — GTA Vice City (reVC) on the GA36-MB

GTA Vice City running natively through the [re3](https://github.com/Cai1Hsu/re3)
reverse-engineered engine (fork `Cai1Hsu/re3`, branch `miami` at `e19f406` (reVC), with `main`'s librw, so every patch of the GTA III port applies), built for
goodluckOS on the GA36-MB (Allwinner A33, 4× Cortex-A7 at 1.2 GHz, Mali-400
MP2 on ARM's closed driver), as a self-contained game folder. Your PC game
files go into the folder.

Vice City specifics: its cutscene face rigs need 64 bones, so the bone palette
is sent as 3 rows per bone (the Mali-400's uniform budget is small); the
texture-conversion fix below is what made the cutscene faces show faces
instead of jeans. `REVC_*` variables instead of `RE3_*` (the build renames
them).
| Setup | FPS while driving |
|---|---|
| on Mesa lima | 7–9 |
| on ARM's Mali driver | 18–20 |
| + no per-draw buffer reallocation | ~28 (30 with Vsync, the game's own cap) |
| + light caching, lighter audio mixing, less input polling | 27–30 with Vsync; main thread ~83% busy |
| + particle overdraw budget, re3 `SQUEEZE_PERFORMANCE` | smoke: 15–22 → 27–30 |
| + GL command thread, release config, no replay, NEON maths, LTO+PGO | main thread ~50% busy; the limit is GPU fill |

Recommended in-game settings: **Vsync on, Frame Limiter on** (steady 30, dips
to 20 in the heaviest spots), Trails off, Draw Distance to taste. Radio: **L1**
in a car (controller "Setup 1").

Controls: the build defaults to the **Classic** control method (the PS2 layout:
the stick turns the player). "Standard" is the keyboard-and-mouse layout, in
which left and right make the player strafe sideways; it is what the engine
chooses on its own, and a `Method = 0` saved in the ini brings it back
(Controller Setup in the menu, or `Method = 1`).
**Free Camera** is on by default too (Display Setup): the right stick orbits
the camera on foot and follows the car San Andreas-style; off, the right
stick is the PS2's first-person look. Targets: hold R1 to lock on, L2 / R2
(the lower shoulder buttons) cycle them.

## Game files

From the PC game's folder, copied whole into `/home/player/ports/gtavc-pc/`:

| Folder | Needed |
|---|---|
| `Audio/` | yes (sound effects, radio, cutscene audio) |
| `anim/` | yes |
| `data/` | yes (the port's own `data/freeroam_miami.scm` is already there: don't overwrite it) |
| `models/` | yes (the port's own `.txd` files in `models/` are already there: don't overwrite them) |
| `txd/` | yes (loading screens) |
| `skins/` | optional (your own player skins) |
| `TEXT/` | no: the port ships its own `TEXT/` |
| `movies/`, `mss/`, `gta-vc.exe`, the `.dll`s | no (the game runs without them) |

## The game folder on the console (`/home/player/ports/gtavc-pc/`)

| | |
|---|---|
| `reVC` | the game (RUNPATH `$ORIGIN/lib`) |
| `lib/` | OpenAL, mpg123, sndio, bsd, md: what goodluckOS lacks |
| `alsoft.conf` | OpenAL reads it from the game folder by itself |
| `glos/` | the ARM driver set from `goodluckOS-mali-driver` |
| `start.sh` | controller mapping and tuning, then `exec ./glos/glos-gpu run mali ./reVC` |
| `icon.png`, `~/.local/share/applications/gtavc-pc.puppy` | the launcher entry |
| `models/txd.img` + `txd.dir` | the texture cache the game builds on its first start ("converting textures"); delete both to rebuild it |
| `userfiles/` | saves (never touched by tooling); the rest is your game data (above) |

## Build, package, deploy

```sh
docker buildx build --output type=local,dest=./out . && ./package.sh
```
`out/ports/gtavc-pc/` is then the complete folder (the game data excluded):
copy it over the console's, and `out/.local/share/applications/gtavc-pc.puppy` next
to the other launcher entries. To update only the binary:
`python3 ../goodluckOS-tools/mtp_send.py / out/ports/gtavc-pc/reVC`, then over the console
`appctl kill-application; mv /home/player/reVC /home/player/ports/gtavc-pc/reVC`.

The default build is LTO + profile-guided (profile in `pgo/`) with
`-fno-strict-aliasing` (without it the optimiser exposed type-punning in the
game code: Claude's head jumping into the air now and then). Retrain with
`tools/pgo.sh` (instrumented build, play, collect) after large code changes.
Plain build: `--build-arg LTO=0 --build-arg PGO=off`.

| Path | What |
|---|---|
| `Dockerfile` | clones the fork, applies `patch-re3.sh` and `shim/`, cross-compiles, zips `ports/gtavc-pc/` |
| `patch-re3.sh` | every change to the engine and librw, as text substitutions applied at build time |
| `shim/` | `glfw_sdl2_shim.cpp` (the GLFW subset re3 uses, on SDL2: the fork only has a GLFW front end and GLFW can't do KMS/DRM), `gl3fbdev.cpp` (EGL on `/dev/fb0` through ARM's libraries, opened by name), `gl3thread.cpp` (the GL command thread), `glos_fps.cpp`, `glos_prof.cpp`, `glos_scale.cpp` |
| `start.sh`, `gtavc-pc.puppy`, `icon.png`, `alsoft.conf` | as shipped |
| `pgo/` | the training profile |
| `tools/` | `bench.sh` (relaunch with FPS logging, measure), `profile.sh` (sampling profiler, hottest functions per library), `pgo.sh`, the GL-thread generator |
| `glos/testing/` | console-side tools, copied over only when needed (`../goodluckOS-tools/testing-sync.sh gtavc-pc`) |

## Tuning (`start.sh`; everything is off in the binary itself)

| Variable | start.sh | Effect |
|---|---|---|
| `REVC_GL_THREAD=1` | 1 | GL command thread: driver work runs on another core |
| `REVC_PARTICLE_BUDGET=<screens>` | 5 | large-particle overdraw budget (smoke up close); unset or 0 = off |
| `REVC_FAST_PARTICLES=1` | 1 | lean fragment shader for blended particles |
| `REVC_FPS_COUNTER=1` | 1 | FPS overlay in the top-left corner |
| `REVC_RENDER_SCALE=<percent>` | 75 | draw the 3D world at 25–99% of 640×480 and stretch it (HUD stays sharp); fewer pixels for the fill-bound GPU |
| `REVC_STICK_SCALE=<factor>` | 1.5 | the GA36-MB's sticks reach about 43–213 of the 0–255 its gamepad driver declares |
| `REVC_STICK_DEADZONE=<fraction>` | – | radial dead zone on a stick's two axes together (0 = the game's own handling) |
| `REVC_DRAW_STATS=1` | – | overlay plus draws, CPU vs swap-wait ms, sprites and sprite overdraw |
| `REVC_FPS_LOG=1` | – | FPS and draw stats to `/dev/shm/reVC.log` twice a second |
| `REVC_PROFILE=1` | – | sampling profiler; `kill -USR2 <pid>` dumps `/dev/shm/re3prof.txt` |
| `REVC_NO_PARTICLES=1` | – | skip particles entirely (diagnostics) |

Buttons are mapped by position (Xbox-style A = bottom) in `start.sh`.

## What had to change, and why

- librw's SDL2 backend never implemented display "subsystems": re3 saw 0
  displays. Fixed in the shim.
- Mali-400: GLES 2.0 only; 512-instruction vertex shaders with no real loops
  (lights cut 8 → 4); a small uniform budget (re3's extra "Neo" pipelines
  off).
- GLES 2.0 texture rules: unsized formats (`GL_RGBA`, not `GL_RGBA8`), no
  `GL_TEXTURE_MAX_LEVEL`, an incomplete mip chain samples black: every
  texture was black at first.
- The texture conversion (the "converting textures" cache) reads textures
  back from the GPU, and GLES 2 can only read mip level 0; librw cut the
  smaller levels out of it, so every distant texture used a cropped corner
  of the full image. The smaller levels are now box-filtered from level 0.
  A cache built before this fix must be deleted.
- 2D at exactly z = −1 is clipped by Mali: menus and HUD were invisible. Fixed
  by scaling z (clamping broke the radar's depth mask). SDL's default 16-bit
  depth gave shadow z-fighting: 24-bit now.
- ARM's driver turns every `glBufferData(NULL)` orphan into a GPU
  allocation, and librw did that for every HUD and particle draw: client-side
  arrays on this path (20 → 28 fps).
- Smoke up close was 13–16 screens of blended overdraw: the particle budget
  keeps it near 5. ARM's driver blocks *inside draw calls* when the GPU is
  behind, so swap time doesn't show GPU load; a drop in CPU samples per
  second while FPS falls does.

Last profile (driving): re3 32%, libMali 32% (per-draw setup: vertex streams,
uniforms, texture binds), libc 25%, OpenAL 5%, SDL 4%. The main thread is
the bottleneck; the other three cores are mostly idle.

### Tear-free flips (Vsync on)

ARM's fbdev EGL returns from `eglSwapBuffers` at once and flips
(`FBIOPAN_DISPLAY`, which returns at the vblank) on a thread of its own, so
with two buffers the GPU could draw the next frame into the one still on
screen: tearing. With the game's Vsync on, `gl3fbdev.cpp` now waits before
each flush until the previous frame's flip has landed; the binary exports
`ioctl` (`-Wl,--export-dynamic-symbol=ioctl`) so libMali's flips are counted
there. Measured in Vice City with Vsync and the Frame Limiter on: 28–29 fps
outdoors, ~12 ms a frame in the wait (the limiter's slack). The draw-stats
line shows it as `flip`. Carried over from glOSports-maxpayne.

## Benchmark

`tools/bench.sh start` relaunches with FPS logging and the overlay, then with
Vsync and the Frame Limiter off and a save loaded, `tools/bench.sh measure`
(20 s). Standing still the scene is partly GPU-bound (~41 fps), so also
compare the "main thread busy" figure, which tracks CPU cost per frame.
