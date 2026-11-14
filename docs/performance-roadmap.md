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

To do: set them in `port/steamlink/app/run.sh`; check every screen still
looks right (the ring screens of first run, install and update; the main
menu; Online Games; the HUD; a screenshot with Back + d-pad right); compare
640x360, 848x480 and 1280x720 on three maps; pick the default.

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
