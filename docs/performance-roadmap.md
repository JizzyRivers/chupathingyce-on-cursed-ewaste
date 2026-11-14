# Performance roadmap: Halo on the 2015 Steam Link

Working document. Update the tables as measurements come in; say what was
measured, on which map, and with which build.

## Goal

"Playable for gamers" means, with performance mode on (the default):

| Target | Now (0.9.1 + this branch) | Goal |
| --- | --- | --- |
| Multiplayer maps, standing at the spawn | 32 to 62 ms a frame | 33.3 ms (30 fps) on every stock map |
| Campaign gameplay | d40 about 56 ms; a30, b30, c10 not yet measured past their opening cutscenes | 50 ms (20 fps) or better, no crashes |
| Campaign cutscenes | 170 to 290 ms a frame | 100 ms or better |
| Start to main menu, second start onward | about 60 s (26 s of it shaders, was 63 s) | 25 s |
| Peak memory (box has 263 MB, no swap) | 141 to 192 MB | under 170 MB on every map |

## What a frame costs (measured)

The frame is bound by Vivante's driver, not by the game's code:

- about 17 ms fixed (tick, sound, Valve's media process, present), plus
- about 0.45 ms for every draw call, whatever it draws, plus
- the final blit to the screen surface, which scales with the surface's size.

So the levers are: fewer draws, a smaller screen surface, and nothing the
driver has to redo. Game-side CPU work is not the bottleneck: removing the
debug asserts, turning frame interpolation off and a faster sound limiter
each changed the frame by 1 ms or less.

Baseline, 0.9.1 with program binaries, 1280x720 surface, 120 s at the
spawn, no perf log (the layer's "frames N to M took" lines):

| Map | Draws | ms a frame | Peak MB |
| --- | --- | --- | --- |
| hangemhigh | 40 | 35.3 | 143 |
| longest | 57 | 39.1 | 161 |
| bloodgulch | 57 | 42.5 | 146 |
| sidewinder | 61 | 44.6 | 162 |
| chillout | 99 | 46.8 | 161 |
| damnation | 102 | 62.1 | 150 |
| d40 (The Maw), gameplay | 53 to 62 | 55 to 58 | 192 |
| a30, b30, c10 (opening cutscene) | about 300 | 190 to 290 | 169 to 184 |

With an 848x480 screen surface (phase 1): bloodgulch 37.7, damnation 57.3,
hangemhigh 31.8. With 640x360: bloodgulch 37.1.

Measured 2026-11-14, 848x480 (set through `EGL_MVGFX_W` and `EGL_MVGFX_H` in
the environment, since run.sh does not set them yet; 120 s a map at the
spawn, mean of the last three samples; same build as above):

| Map | Draws | ms a frame | Peak MB |
| --- | --- | --- | --- |
| beavercreek | 51 | 31.3 | 150 |
| hangemhigh | 40 | 31.5 | 151 |
| wizard | 64 | 32.8 | 149 |
| longest | 57 | 34.3 | 151 |
| putput | 53 | 35.6 | 147 |
| prisoner | 78 | 37.8 (one sample 49.0) | 152 |
| ratrace | 89 | 37.8 | 153 |
| bloodgulch | 57 | 38.9 | 141 |
| sidewinder | 61 | 40.1 | 152 |
| chillout | 99 | 41.6 | 151 |
| damnation | 102 | 57.8 | 150 |
| boardingaction | 141 | 59.6 | 150 |
| carousel | 171 | 70.9 to 80.6 (only two samples) | 151 |

Ten of thirteen are 42 ms or better; the goal of 33.3 ms is met on
beavercreek, hangemhigh and wizard only. Carousel, boardingaction and
damnation are far off; the first two were not measured before.

Campaign, 848x480, 300 s a map from the start of the map. "Gameplay" cannot
be told from the log alone: a drop in draws and time is what to look for.
Last five "frames N to M" lines of each run (b30 has three: the game was
still shutting down after the kill and the log was copied then):

| Map | Peak MB | Frames N to M: ms each, draws |
| --- | --- | --- |
| a30 | 167 | 30-330: 343, 25; 330-630: 170, 331; 630-930: 211, 393; 930-1230: 181, 340 |
| b30 | 180 | 30-330: 333, 69; 330-630: 187, 275; 630-930: 279, 397 |
| c10 | 176 | 330-630: 181, 267; 630-930: 128, 174; 930-1230: 103, 162; 1230-1530: 89, 148; 1530-1830: 89, 146 |
| a50 | 194 | 330-630: 143, 230; 630-930: 120, 144; 930-1230: 108, 121; 1230-1530: 105, 133; 1530-1830: 108, 135 |
| b40 | 195 | 1230-1530: 69, 103; 1530-1830: 90, 107; 1830-2130: 61, 93; 2130-2430: 88, 112; 2430-2730: 67, 94 |
| c20 | 173 | 1230-1530: 78, 128; 1530-1830: 77, 129; 1830-2130: 77, 127; 2130-2430: 78, 132; 2430-2730: 78, 129 |
| c40 | 164 | 330-630: 106, 162; 630-930: 115, 175; 930-1230: 110, 162; 1230-1530: 110, 146; 1530-1830: 158, 249 |
| d20 | 174 | 330-630: 126, 94; 630-930: 159, 200; 930-1230: 92, 165; 1230-1530: 97, 176; 1530-1830: 96, 177 |

Readings (no screenshots were taken in the campaign, so these are guesses):
a30 and b30 stayed in the opening cutscene for the whole 300 s (340 to 400
draws, 170 to 280 ms). b40 and c20 look like gameplay (about 100 to 130
draws, 60 to 90 ms, steady frame counts of 2700 in 300 s). c10, a50, c40 and
d20 are at 90 to 110 ms with 120 to 175 draws: either gameplay at those
costs or a long cutscene; c40 jumps to 249 draws at the end. Nothing
crashed. Peak memory is over 170 MB on a50, b40 (195), b30, c10, d20 and c20.

## Phases, in order

### Phase 0. Done this session (committed, not released)

- Program binaries (`GL_OES_get_program_binary`) in the GLES 2 layer: linked
  programs are kept in `save/gles2_programs`, and shaders are compiled only
  when a link finds no binary. Start-up shader step 63 s to 26 s; peak
  memory about 15 MB lower (the compiler no longer runs).
- `HALO_MEM_TRACE=<seconds>`: live allocations by caller, in the log.
- The layer's periodic log line now has heap use and client-array draws.
- `tools/box/`: the box test scripts (below).

### Phase 1. Smaller screen surface (expected 4 to 5 ms; measured)

`EGL_MVGFX_W` and `EGL_MVGFX_H` (Marvell's EGL) set the surface's size, and
the box's display hardware scales it to the television. 848x480 holds the
game's 640x480 interface pixel for pixel, and the 320x240 performance
picture at exactly twice its size.

Measured 2026-11-14 (ms a frame, mean of the last three samples after the
first load; draws and peak MB were the same at all three sizes, peak 141 to
152 MB):

| Map | Draws | 1280x720 | 848x480 | 640x360 | 848 minus 640 |
| --- | --- | --- | --- | --- | --- |
| bloodgulch | 57 | 43.9 | 38.0 | 36.4 | 1.6 |
| damnation | 102 | 62.2 | 57.7 | 56.0 | 1.7 |
| hangemhigh | 40 | 35.6 | 31.6 | 31.0 | 0.6 |
| chillout | 99 | 45.5 | 42.2 | 40.2 | 2.0 |

Going from 1280x720 to 848x480 gains 3.3 to 5.9 ms (4.5 on average); going
on to 640x360 gains another 0.6 to 2.0 (1.5 on average).

Screenshots (`docs/shots/`): bloodgulch in play at 848x480 (`game848.png`,
848x480 window dump) and 640x360 (`game640.png`): the 4:3 picture sits in
the middle with black bars left and right, nothing is cut off, the shield and
health bars, the reticle and the weapon are all there, and there is no
garbage at the edges. The main menu dumps (`menu848_frame01800.png`,
`menu640_frame01800.png`) are the game's own 320x240 frame, which is the
same at both sizes: the logo, CAMPAIGN and MULTIPLAYER read clearly and the
lower entries are faint. A window screenshot of the main menu was not
obtained (the timed screenshot is only saved in a map), so the menu as it
reaches the 848 or 640 surface, and the ring screens, Online Games and the
Back + d-pad right screenshot, are not checked.

Decision: the rule was that 848x480 becomes the default if it is no more
than 1 ms slower than 640x360 and the picture is right. The picture is
right, but 848 is more than 1 ms slower on three of four maps, so run.sh is
left as it was (1280x720). The owner decides between taking the 4.5 ms of
848x480 with the interface pixel for pixel, and the extra 1.5 ms of 640x360.

### Phase 2. Start-up time (expected 26 s to under 10 s)

Of the 26 s: 11 s is the driver loading 219 binaries (55 ms each), 6 s is
the layer translating 277 shader texts, the rest is upstream's cache.

- Translate lazily: key a binary by the untranslated texts, and translate a
  shader only when its program has no binary or something needs its text.
- Load lazily: at the start-up warm, defer a program's binary load to its
  first use (or to its map's load), so a session pays only for what it draws.
- Free translated texts once their programs are linked (4.3 MB).

### Phase 3. Fewer draws in gameplay (expected 5 to 12 ms a map)

Ideas taken from the other ports, to be measured one at a time:

- HUD: 13 draws, about 6 ms, every frame. Draw it into a texture and redraw
  that only every third frame or when it changes (the PS2 port keeps a
  "resident HUD"; the 3DS port separates UI from world resolution).
- Per-map view distance (the 3DS port: 60 units on Halo, Silent Cartographer
  and 343 Guilty Spark, 80 elsewhere, longer in cutscenes). Ours is one
  value, 60, everywhere.
- Adaptive quality with hysteresis (3DS and PS3 ports): shorten the view
  distance, then lower model detail, only under sustained load, and recover
  slowly. Never both trials at once.
- Merge consecutive draws that change nothing but vertices (measured
  earlier: at most a quarter of draws qualify).
- Damnation and Chill Out draw about 100 calls with 36 programs a frame:
  find which passes those are (`HALO_PERF_LOG`, per-part draw counts) and
  whether performance mode can leave any out indoors.

  Measured 2026-11-14 with `HALO_PERF_LOG=1` (draws a frame by part; the
  timings of those runs are not comparable with the others):

  | Map | World | Objects | Transparent | HUD | Total |
  | --- | --- | --- | --- | --- | --- |
  | damnation | 61 | 26 (24 to 26) | 2 to 3 | 13 | 102 |
  | chillout | 51 | 10 | 25 | 13 | 99 |

  Typical lines (damnation, then chill out; the first line of each run is
  the load and is left out):

  - `perf: 59.7 ms a frame; ... world 23.8 (61 draws) objects 14.7 (26 draws) transparent 1.5 (2 draws) effects 0.0 hud 6.1 (13 draws)`
  - `perf: 59.6 ms a frame; ... world 22.9 (61 draws) objects 14.9 (26 draws) transparent 2.2 (3 draws) effects 0.1 hud 6.1 (13 draws)`
  - `perf: 43.7 ms a frame; ... world 19.8 (51 draws) objects 3.6 (10 draws) transparent 5.7 (25 draws) effects 0.0 hud 6.6 (13 draws)`
  - `perf: 41.2 ms a frame; ... world 19.0 (51 draws) objects 3.3 (10 draws) transparent 5.2 (25 draws) effects 0.0 hud 6.3 (13 draws)`

  So indoors the world is half or more of the draws (61 and 51) and
  damnation has 26 object draws; Chill Out has 25 transparent draws, which
  the other map does not. The HUD is 13 again. Which of the world's draws
  are lightmap, detail or specular passes is not yet known.

### Phase 4. Campaign

- Measure gameplay, not the opening cutscene, on a30, b30, c10, a50, b40,
  c20, c40, d20 (skip the cutscene, or wait it out).
- Performance mode leaves cutscenes alone on purpose. At 200 to 290 ms they
  need their own, milder, profile: decals, dynamic lights and specular off,
  full view distance kept.
- Peak memory: d40 reaches 192 MB.

### Phase 5. Memory (target: under 170 MB everywhere)

From `HALO_MEM_TRACE` in d40 (80 MB of allocations 4 KB and larger):
33 MB the game's own pools (Xbox kernel allocator), 15 MB inside the
driver, 9 MB the layer's buffer copies, 8 MB the map browser's index
document while it refreshes, 4 MB the high-res text atlas, 4.3 MB
translated shader text, 2.7 MB the ring screens' pictures (never freed),
2.2 MB frame interpolation's objects.

- Free the ring screens' pictures after start-up (ours, 2.7 MB).
- Free translated shader text (phase 2).
- Ask upstream about `HALO_CONSOLE_LIMITS` for this port: the desktop's
  game state is 20 MB against the consoles' 16, and its object pool 8 MB
  against 1. It changes saved games, so decide before testers have saves.

### Phase 6. Ship

- Adopt `--console-no-debug-asserts` in `release.py` (every other console
  port ships it; about 1 ms here).
- Release build, update test from 0.9.1 on a local server, then publish.

## Do not retry (measured, no gain or worse)

- `VIV_NO_MT=1`: frames went from 42 to 69 ms and the game would not quit.
- Frame interpolation off; no-asserts build; faster sound limiter: under 1 ms.
- Vertices are already drawn from GPU buffers (0 client-array draws a frame).
- Unpacking packed normals in the shader (shaders stop linking) or on the
  CPU (5 ms a frame).
- Skipping repeated shader constants; grouping objects by definition; lower
  model detail; swap interval 0; `EGL_MVGFX_MULTI_BUFFER=3`; larger audio
  buffer; LTO and `-mcpu` tuning (3 to 5% in a10 only).
- The perf log itself costs several ms in 0.9.1: never compare timings
  taken with `HALO_PERF_LOG` set against ones without.

## Known correctness problems (not performance)

- The first-person assault rifle is posed wrong (its rear is cut off).
- One skinned model shader (constants c_14 to c_27 with c_bones[108]) does
  not link.
- Stubbs mode (upstream's, experimental) corrupts the heap at its main
  menu on the box; `patches/chupathingyce/0001-transparent-sort-record.patch`
  fixes one cause found on the way (a stale sort record in
  `_rasterizer_model_transparent_geometry_submit`), and should go upstream.

## How to measure

Everything runs from this PC against the box over ssh (`tools/box/`):

```sh
tools/box/deploy.sh                      # strip and install the built game, upload the scripts
tools/box/box.sh run 'sh /tmp/trial/sweep.sh 120 levels/test/bloodgulch/bloodgulch levels/test/damnation/damnation'
tools/box/box.sh run 'sh /tmp/trial/ab4.sh "EGL_MVGFX_W=848 EGL_MVGFX_H=480" levels/test/hangemhigh/hangemhigh'
tools/box/box.sh run 'sh /tmp/trial/mem.sh 200 levels/d40/d40'
```

`sweep.sh <seconds> <map>...` starts the app the way the Steam Link does,
loads each map through `init.txt`, and prints frame times, draws and peak
memory; each run's log is kept as `/mnt/scratch/sweep-<map>.log`.
`ab4.sh "<VAR=value ...>" <map>...` does the same with environment
variables set for that run ("-" for none). A map's first load after a new
build is slower (its programs are linked from text once): run it twice and
use the second.

Rules for testing on the box: check `pidof halo` first and never kill a
game you did not start; nothing is published or pushed from a test; leave
`config.toml` and the saves on the stick alone; after a hang, `kill -9`
the game and unmount `/mnt/chupathingyce-usb`.
