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

Decision (2026-11-14): 848x480 is run.sh's default. 640x360 is 1.5 ms
faster on average, but it scales the 640x480 interface down, and menus and
text are what a smaller surface hurts; `launch.env` can still set another
size. Still to check by eye on the television: the main menu, the ring
screens and Online Games at 848x480.

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


  Measured 2026-11-14 (round 2): `display.performance_view_distance` is
  60.0 on the box (the default; the stick's `config.toml` has the line
  commented out). The value was set for each run with the environment
  variable `HALO_PERFORMANCE_VIEW_DISTANCE` in launch.env, not by editing
  `config.toml`, so the file was not touched (its md5 was
  85637f02974dc3bedf29fba6270e96b9 before and after; that the variable
  overrides the file was not checked directly, but the draws moved with it:
  sidewinder 58 draws at 30 and 45, 61 at 60 and 90). 848x480, no
  `HALO_PERF_LOG`, 110 s a map at the spawn, one run per cell; ms a frame is
  the mean of the last three "frames N to M" samples after the first (a 110
  s run has only two to three: n is given where it is 2), draws are the
  layer's count:

  | Map | 30 | 45 | 60 (default) | 90 |
  |---|---|---|---|---|
  | carousel | 71.8 (n=2), 171 draws | 71.5 (n=2), 171 | 72.0 (n=2), 171 | 76.1 (n=2; 80.0 and 72.2), 171 |
  | boardingaction | 64.9 (71.0 and 58.8; n=2), 141 | 59.0 (n=2), 141 | 59.0 (n=3), 141 | 59.2 (n=2), 141 |
  | damnation | 58.1 (n=2), 102 to 103 | 58.6 (n=2), 102 | 59.3 (n=2), 102 | 59.5 (n=2), 102 |
  | sidewinder | 40.1 (n=3), 58 | 40.5 (n=3), 58 | 42.9 (47.6, 40.6, 40.6; n=3), 61 | 40.4 (n=3), 61 |

  Peak memory 148 to 159 MB in all sixteen runs. The view distance changes
  nothing measurable at these spawns: the draws are the same from 30 to 90
  on carousel, boardingaction and damnation (damnation 103 in one window at
  30), and sidewinder loses 3 draws at 30 and 45, with its ms inside the
  noise (one slow sample of 47.6 and 72.2 to 80.0 are single windows, not
  a trend). These three are interiors and the sight lines at the spawn are
  short; the maps where it was expected to pay (the outdoor ones: bloodgulch,
  longest, beavercreek, wizard) were not run. Screenshots (the game's frame
  at frame 900, one a distance): sidewinder at 30, 45, 60 and 90 look the
  same (the spawn faces a wall; only the motion tracker ring at the bottom left differs, absent at 30 and 45 and faint at 60 and 90, which is the tracker's own state, not the distance);
  carousel reached frame 900 in the 110 s only at 90, so no comparison
  pictures for it. The visual cost of 30 was therefore not judged on a
  view that has a distance. `config.toml` was never changed, so no backup or
  restore was needed.
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

  Round 2, measured 2026-11-14, 848x480, 120 s a map from the start of the
  map, `HALO_PERF_LOG=1` (so these timings are not comparable with the
  tables above; the phases of one map can be compared with each other).
  Each column is one `perf:` line, the third from the last of the run
  (a 30-frame window; every phase the line prints, nothing trimmed; "-"
  means the line did not print that phase; the first line of each run is
  the load and is left out). The draws are the ones the layer counted in
  that part of the frame; they add up to the layer's own total for the map
  (hangemhigh 40, chillout 99, damnation 102, carousel 171, boardingaction
  141, bloodgulch 57):

  | Phase (ms a frame) | hangemhigh | chillout | damnation | carousel | boardingaction | bloodgulch |
  |---|---|---|---|---|---|---|
  | total, ms a frame | 32.1 | 40.7 | 64.1 | 71.8 | 61.5 | 40.2 |
  | tick | 1.1 | 1.5 | 3.8 | 4.0 | 2.9 | 4.6 |
  | render | 17.9 | 34.0 | 51.5 | 61.8 | 52.7 | 26.5 |
  | world | 5.4 (12 draws) | 18.8 (51 draws) | 25.1 (61 draws) | 25.2 (90 draws) | 31.5 (99 draws) | 9.8 (21 draws) |
  | objects | 6.1 (15 draws) | 3.5 (10 draws) | 16.1 (26 draws) | 17.8 (36 draws) | 12.0 (25 draws) | 9.0 (22 draws) |
  | transparent | 0.0 | 5.1 (25 draws) | 2.1 (2 draws) | 11.9 (32 draws) | 1.9 (4 draws) | 0.6 (1 draws) |
  | effects | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | hud | 5.8 (13 draws) | 5.9 (13 draws) | 6.8 (13 draws) | 6.0 (13 draws) | 6.4 (13 draws) | 6.4 (13 draws) |
  | sound | 0.2 | 0.5 | 1.2 | 1.0 | 1.0 | 0.5 |
  | network | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | present | 11.7 | 3.6 | 6.7 | 3.8 | 4.0 | 7.2 |
  | vsync | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.2 |
  | begin | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | setup | 0.1 | 0.1 | 0.1 | 0.0 | 0.1 | 0.1 |
  | flares | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | end | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 |
  | render sound | 0.2 | 0.4 | 1.1 | 0.7 | 0.7 | 0.3 |
  | units | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 |
  | ai | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.0 |
  | tick effects | 0.0 | 0.0 | 0.3 | 0.0 | 0.0 | 0.0 |
  | game engine | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | tick objects | 0.8 | 1.2 | 2.9 | 3.6 | 2.3 | 4.2 |
  | collision | 0.1 | 0.1 | 0.2 | 0.2 | 0.2 | 0.3 |
  | netcode | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | bipeds | 0.4 | 0.4 | 0.4 | 0.5 | 0.4 | 0.3 |
  | weapons | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.1 |
  | other objects | 0.0 | 0.0 | 0.2 | 0.2 | 0.1 | 0.1 |
  | damage | 0.0 | 0.1 | 0.3 | 0.2 | 0.1 | 0.2 |
  | nodes | 0.1 | 0.2 | 0.6 | 0.7 | 0.6 | 1.1 |
  | functions | 0.1 | 0.1 | 0.4 | 0.6 | 0.3 | 0.4 |
  | object lights | 0.0 | 0.1 | 0.2 | 0.4 | 0.2 | 0.8 |
  | activation | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
  | garbage | 0.0 | 0.0 | 0.1 | 0.0 | 0.0 | 0.0 |
  | vehicles | - | - | - | - | - | 0.4 |

  The tick's parts (units to garbage) are inside "tick"; "render sound" is
  inside "render"; world, objects, transparent, effects, hud, begin,
  setup, flares and end are render's parts. One window is noisy (the same
  map's windows move by 1 to 3 ms), so the mean of the steady windows (the
  9th to the last: 65, 57, 39, 25, 32 and 55 windows of 30 frames) is given
  too:

  | Mean of the steady windows (ms) | hangemhigh | chillout | damnation | carousel | boardingaction | bloodgulch |
  |---|---|---|---|---|---|---|
  | total | 32.49 | 42.42 | 58.86 | 73.92 | 62.94 | 39.02 |
  | tick | 1.23 | 1.60 | 3.45 | 4.31 | 2.87 | 4.22 |
  | render | 18.65 | 35.46 | 47.03 | 63.88 | 52.89 | 25.73 |
  | world | 5.10 | 19.43 | 23.28 | 25.73 | 31.65 | 9.58 |
  | objects | 6.48 | 3.60 | 14.52 | 18.44 | 11.76 | 8.78 |
  | transparent | 0.00 | 5.62 | 1.94 | 12.49 | 1.93 | 0.57 |
  | hud | 6.51 | 6.11 | 6.10 | 6.26 | 6.39 | 6.23 |
  | sound | 0.29 | 0.58 | 0.99 | 0.89 | 1.15 | 0.50 |
  | render sound | 0.26 | 0.45 | 0.85 | 0.71 | 0.88 | 0.28 |
  | present | 11.17 | 3.79 | 6.60 | 3.78 | 3.79 | 7.28 |
  | tick objects | 0.90 | 1.21 | 2.89 | 3.78 | 2.37 | 3.85 |
  | nodes | 0.16 | 0.21 | 0.68 | 0.82 | 0.56 | 0.96 |
  | functions | 0.10 | 0.11 | 0.47 | 0.54 | 0.32 | 0.43 |
  | object lights | 0.04 | 0.12 | 0.21 | 0.38 | 0.28 | 0.84 |

  Draws a frame by part, and the layer's own counts for the same runs (its
  line of the third 300-frame period; programs, texture binds, uniforms,
  attribute pointers and sampler states are a frame's calls; "GPU-indexed"
  are indexed draws from GPU buffers):

  | | hangemhigh | chillout | damnation | carousel | boardingaction | bloodgulch |
  |---|---|---|---|---|---|---|
  | world / objects / transparent / hud | 12 / 15 / 0 / 13 | 51 / 10 / 25 / 13 | 61 / 26 / 2 / 13 | 90 / 36 / 32 / 13 | 99 / 25 / 4 / 13 | 21 / 22 / 1 / 13 |
  | draws | 40 | 99 | 102 | 171 | 141 | 57 |
  | programs | 12 | 24 | 36 | 38 | 28 | 15 |
  | texture binds | 55 | 107 | 169 | 194 | 157 | 89 |
  | sampler states | 24 | 26 | 37 | 72 | 26 | 18 |
  | uniforms | 31 | 63 | 93 | 104 | 74 | 54 |
  | attribute pointers | 284 | 590 | 654 | 983 | 877 | 389 |
  | GPU-indexed draws | 15 | 10 | 26 | 36 | 25 | 22 |
  | indices | 6513 | 4420 | 15265 | 14541 | 14044 | 15202 |
  | draws from client arrays | 0 | 0 | 0 | 0 | 0 | 0 |

  Not verified: the HUD showed 19 to 23 draws in the first windows after
  the load on hangemhigh (4 windows at 23), bloodgulch (about 10 windows
  at 15 to 23) and boardingaction (3 windows at 21 to 23), and 13 for the
  rest of every run; the extra ten are something temporary on the screen
  at the start (a message; a guess), and in those windows the parts' draws
  add up to 6 to 10 more than the layer's total, which is not explained.
  Those windows are left out above. One run of each map; the layer's line
  per 300 frames was steady within 1 ms between periods.

  What the numbers say:

  1. Hangemhigh's fixed part. Of its 32.5 ms (mean), the three parts that
     draw (world 5.1, objects 6.5, hud 6.5) are 18.1 ms; the other 14.4 ms
     is: present 11.2 (78% of it), tick 1.2 (tick objects 0.9), 1.2 ms no
     phase accounts for (the total minus tick, render, sound and present),
     render's own rest 0.6 (render sound 0.3, end, setup, flares) and sound
     0.3. Ranked: present 11.2, tick 1.2, unaccounted 1.2, render rest 0.6,
     sound 0.3. Present is the big one and it is not a constant: 3.8 ms on
     chillout, carousel and boardingaction, 6.6 on damnation, 7.3 on
     bloodgulch, 11.2 here; present plus render is 29.8 here against 39.3
     (chillout), 53.6 (damnation), 67.7 (carousel), 56.7 (boardingaction)
     and 33.0 (bloodgulch): the smaller the CPU's drawing part, the longer
     present takes. That fits present being where the CPU waits for the
     GPU to finish the frame (a guess; not measured with
     `HALO_GLES2_FRAME_SPLIT`), which would make the "17 ms fixed" of the
     cost model partly GPU work that the bigger maps hide behind their
     draw submission.
  2. Damnation against Chill Out. Steady means 58.86 against 42.42, 16.44
     ms. By phase (damnation minus chillout): objects +10.92, world +3.85,
     present +2.81, tick +1.85 (tick objects +1.68, nodes +0.47, functions
     +0.36), sound +0.41, render sound +0.40, hud -0.01, transparent
     -3.68. The objects part holds two thirds of it: 26 draws against 10,
     14.52 ms against 3.60, 0.56 ms a draw against 0.36. The 16 extra
     draws at Chill Out's 0.36 explain 5.8 of the 10.9 ms; the other 5.1
     ms is the dearer draws. The world costs the same per draw in both
     (23.28/61 = 0.38 and 19.43/51 = 0.38): its +3.85 is its 10 more draws.
     Chill Out's 25 transparent draws are cheap (0.22 each) and take 3.7
     ms off the difference. The layer's fields (damnation / chillout, a
     frame, per draw in brackets): programs 36 / 24 (0.35 / 0.24), texture
     binds 169 / 107 (1.66 / 1.08), uniforms 93 / 63 (0.91 / 0.64),
     attribute pointers 654 / 590 (6.4 / 6.0), sampler states 37 / 26,
     framebuffers 6 / 6, client-array draws 0 / 0, GPU-indexed draws 26 /
     10 (equal to each map's object draws; the objects are the indexed
     draws, a guess from the match), indices 15265 / 4420. So damnation's
     draws change more state (12 more programs, 62 more texture binds, 30
     more uniforms a frame), its frame has 3.5 times the indices, and its
     object draws cost 56% more each while its binds per draw are 54% up
     and its uniforms 42% up: consistent with state changes (or the
     indices) making the draws dear, not proven; the layer cannot split
     them by part. Objects elsewhere: hangemhigh 0.43 a draw, bloodgulch
     0.40, boardingaction 0.47, carousel 0.51. Damnation also has more to
     tick (3.45 against 1.60 ms) and a longer present (6.6 against 3.8).
  3. Carousel and Boarding Action by part, draws (ms): carousel world 90
     (25.7), objects 36 (18.4), transparent 32 (12.5), hud 13 (6.3), 171
     in all; boarding action world 99 (31.7), objects 25 (11.8),
     transparent 4 (1.9), hud 13 (6.4), 141 in all. Per draw: carousel
     world 0.29, objects 0.51, transparent 0.39, hud 0.48; boarding action
     world 0.32, objects 0.47, transparent 0.48 (4 draws), hud 0.49.
     Carousel's cost is its 90 world draws and 32 transparent draws;
     Boarding Action's is its 99 world draws.
  4. Per draw on these six maps: world 0.29 to 0.38 ms, objects 0.36 to
     0.56, HUD 0.45 to 0.5 (13 draws; a HUD draw costs more than a world
     draw).

  Phase 3, task 2: what the 13 HUD draws are. No existing variable can log
  per-draw state, so nothing was run for it and no code was added.
  `HALO_GLES2_TRACE=n` logs the mode, count and program of each of the
  first n draws since the process started (and framebuffer and texture
  upload calls), with a `glFinish` after each, no texture, no blend, no
  element, and n would have to reach past the load; `HALO_GLES2_SLOW=ms`
  (2 or more) logs only draws that take that many ms, which no HUD draw
  does; `HALO_GLES2_MERGE_STATS` counts a frame's changes as totals,
  without blend state; `HALO_GLES2_DUMP` only adds program texts to the
  others; `HALO_PERF_LOG` gives the HUD part's count (13). From the source
  (`hud_draw.c`, `hud_unit.c`, `hud_weapon.c`, `hud.c`, `motion_sensor.c`,
  `steamlink_performance.c`):

  | Element | Draw calls it makes (from the source, not measured) |
  |---|---|
  | static elements: frames, backgrounds (the shield and health meters' backgrounds, the motion sensor's, weapon icons; `hud_draw_static_element`) | 1 quad each, but none in performance mode: `console_performance_hud_frames()` is false (`display.performance_hud_frames`, default false) and the function returns at once outside cutscenes |
  | shield meter (`hud_draw_meter`) | 1 quad, plus 1 for each overcharge level above 100% |
  | health meter (`hud_draw_meter`) | 1 quad |
  | weapon meters (heat, battery; `hud_draw_meter` in hud_weapon.c) | 1 quad each, if the weapon's interface has them |
  | numbers: ammo, grenades (`hud_draw_numbers`) | 1 quad (`hud_draw_bitmap_direct`) for each digit drawn, so a two digit number is 2 |
  | reticle (the weapon interface's crosshair items, `hud_draw_bitmap`) and the weapon overlays (`hud_draw_weapon_overlays`) | 1 quad for each item in the weapon's tag that is on in the weapon's state |
  | motion tracker (`motion_sensor_draw_screen`: `rasterizer_hud_motion_sensor_blip_begin`, one `..._blip_draw` for each blip, `..._blip_end`) | 1 for each blip; what begin and end draw was not read |
  | text: messages, names, chat (`rasterizer_draw_string`) | not read: the number of draws for a string is the text renderer's, not the HUD's |
  | nav points, damage indicators, timers | 1 quad each while on |

  Every quad goes through `rasterizer_psuedo_dynamic_screen_quad_draw`
  (from `hud_draw_bitmap_internal`) with the framebuffer blend function
  alpha-multiply-add and the element's bitmap as its texture. Whether
  that is one GL draw per quad in the layer was not verified (the HUD
  part's steady 13 at the spawn fits). Which of the 13 are which, and
  which consecutive ones share program, texture and blend state, cannot be
  said from the source. The guess: the digits of one number are sprites of
  one bitmap and the items of one overlay of another, so each number and
  each overlay could be one draw; the meters use the meter shader and
  would not merge with them. To get the real list, the layer would need a
  log of program, texture 0 and blend state for each draw inside the HUD
  part (a few lines in `gles2_layer.c`; not added).

### Phase 4. Campaign

- Measure gameplay, not the opening cutscene, on a30, b30, c10, a50, b40,
  c20, c40, d20 (skip the cutscene, or wait it out).
- Performance mode leaves cutscenes alone on purpose. At 200 to 290 ms they
  need their own, milder, profile: decals, dynamic lights and specular off,
  full view distance kept.
- Peak memory: d40 reaches 192 MB.


Measured 2026-11-14 (round 2), 848x480, no `HALO_PERF_LOG`, from the start
of the map, with `HALO_SCREENSHOT_EVERY=250`: the game's own frame (320x240)
saved every 250 frames as `frameNNNNN.bmp` in `HALO_SCREENSHOT_DIR`
(`d3d8_gl.c`, `D3DDevice_Present`; run.sh sets the folder, launch.env here
set another so the stick's old shots were left alone), converted with
`tools/box/bmp2png.py` and looked at. A picture is a cutscene when it has
the 30-pixel black bars at top and bottom and no HUD (checked for every
picture by whether the top 22 and bottom 22 rows are black), gameplay when
the shield and ammo HUD shows. a30 and b30 ran 480 s, the others 300 s.
The frame number of a picture is matched to the layer's "frames N to M took"
lines by taking the two frame counts as the same (not verified; the changes
of draws in the log fall where the pictures change). The seconds below are
counted from the layer's frame 30, so the map's own load before it is not
in them. Pictures: `docs/shots/campaign/` (two or three a map, 320x240).

| Map | Peak MB | Cutscene | Gameplay starts | Gameplay windows ("frames N to M": ms a frame, draws) |
|---|---|---|---|---|
| a30 | 173 | frames 250 and 500 (to about 120 s) | between frame 500 and 750 (about 120 to 170 s) | 930-1230: 184.1, 340; 1230-1530: 179.0, 340; 1530-1830: 178.6, 340; 1830-2130: 181.7, 340 |
| b30 | 185 | frames 250 and 500 (to about 130 s) | between frame 500 and 750 (130 to 190 s) | none whole: 630-930 (279.5, 399) holds the end of the cutscene and the start of play; the game crashed (below) |
| c10 | 182 | frames 250 and 500 (to about 140 s) | between frame 500 and 750 (140 to 180 s) | 930-1230: 102.1, 160; 1230-1530: 90.0, 148; 1530-1830: 89.7, 145 |
| a50 | 198 | frames 250 and 500 (to about 120 s) | between frame 500 and 750 (120 to 155 s) | 930-1230: 104.8, 120; 1230-1530: 105.5, 131; 1530-1830: 109.0, 137 (the last two include pictures with no HUD, below) |
| c40 | 165 | all of the run: frames 250 to 1750 (8 pictures) have the bars and no HUD | does not start in 300 s | none; the cutscene's windows: 630-930: 112.9, 176; 930-1230: 107.9, 161; 1230-1530: 105.3, 149; 1530-1830: 147.1, 235 |
| d20 | 175 | frames 250, 500 and 750 (to about 150 s) | between frame 750 and 1000 (150 to 190 s) | 930-1230: 87.4, 151 (starts in it); 1230-1530: 95.2, 175; 1530-1830: 94.9, 178 |

The earlier windows of each run (30-330, 330-630) are the load and the
opening cutscene (a30: 313.5 ms with 16 draws, then 165.5 ms with 323;
the same two windows on the others are in the round 1 table above).

What the pictures show:

- a30: black (frame 0), two cutscene pictures (the ridge and the Pelican's
  cockpit), then the player in a corridor of the ship with the HUD
  (shield bar, ammo, grenades, reticle), the same view standing still from
  frame 750 to 2250 (7 pictures): gameplay was reached and the player was
  not moved, so what is measured is a still view of one place: 340 draws,
  179 to 184 ms, 61 programs, 527 texture binds, 127840 indices a frame
  (the last period). Round 1's reading "a30 stayed in the cutscene" was
  wrong.
- b30: the same shape (two bars pictures, then gameplay with HUD at frame
  750 on a beach with smoke). It crashed: `gles2: bus error at 0xbec83d35,
  pc 00ec3844 lr 010ca268` at 335 s (the sweep reported ENDED at 335 s),
  after frame 930 and before 1230. `llvm-symbolizer` on the unstripped
  build gives pc in `actor_move_try_evasion_vector` (`actor_moving.c`:
  1232) and lr in `collision_surface_project_point2d` (`collision_bsp.c`:
  829): an AI evasion step in a collision test; the faulting address is
  odd (an unaligned access: a guess at the cause, not investigated). The
  layer's last period before it: 399 draws, 99 programs, 848 texture
  binds, 100529 indices, 11 client-array draws. Not repeated, so whether
  it recurs is not known; b30 has no gameplay number.
- c10: two bars pictures, then play in the jungle with the HUD from frame
  750 (5 pictures to 1750), the camera turning (leaves in view in the
  later ones): 160, 148 and 145 draws, 102 to 90 ms.
- a50: two bars pictures (the second with the caption "Truth and
  Reconciliation"), then play with the HUD at frames 750 to 1250 (the
  objective "Board the Covenant Battle Cruiser" shown), and at frames
  1500 and 1750 a close picture of a character with no HUD meters and no
  bars (the second with the hint "Use [button] when zoomed in to activate
  night vision"): a body or a first-person death view; a guess, it was
  not checked further. Draws and ms are about the same across those
  windows (120 to 137, 105 to 109 ms).
- c40: every picture is a cutscene (a dark approach, a pale platform hall,
  a green character and a floating sphere); 300 s were not enough to reach gameplay. Round 1's
  guess that c40 was gameplay was wrong.
- d20: three dark bars pictures (rock face, sparks), then gameplay with
  the HUD from frame 1000 ("Find a way into the control room"), the player
  standing still with projectiles in view.

Gameplay, in short: a30 about 180 ms and 340 draws (the worst, standing
in the ship's interior), a50 105 to 109 ms and 120 to 137 draws, d20 87 to
95 ms and 151 to 178 draws, c10 90 ms and 145 to 148 draws. The goal of
50 ms is far off on all four (1.8 to 3.6 times); only the d40 map from
round 1 (56 ms) is near it. The three campaign maps not reached or
measured (b30 crash, c40 cutscene) still need a run: c40 needs more than
300 s, b30 needs a repeat. b40 and c20 were not run again.

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

## Decisions after round 2 (2026-11-14)

What the round's numbers say, and what follows from them:

- A draw is not one price. World draws cost 0.29 to 0.38 ms, object draws
  0.36 to 0.56, HUD quads 0.48, Chill Out's transparent draws 0.22. Objects
  and the HUD are the dear ones; count them first.
- `present` is 3.8 ms when the frame is busy and up to 11 ms when it is not
  (Hang 'Em High). The likeliest reading is that the CPU waits there for
  the GPU, so the light maps are GPU-bound and the heavy ones driver-bound.
  To prove: `HALO_GLES2_FRAME_SPLIT` on hangemhigh and carousel. If true,
  cutting draws on a map already near 33 ms buys less than its draw count
  says, and those maps want fewer pixels or cheaper shaders instead.
- View distance below 60 does nothing at the four indoor spawns. Per-map
  view distance is dropped from phase 3 unless the outdoor run (bloodgulch,
  sidewinder away from the wall, a30) shows otherwise.
- Order of building, by ms a map for the work:
  1. HUD in one pass (6.2 ms on every map, 13 quads). First merge quads
     that share bitmap and blend (digits, overlay items), which needs no
     new render target. Only if that leaves more than 3 ms, the resident
     HUD: drawn into an RGBA texture whose alpha holds what the HUD lets
     through (`glBlendFuncSeparate` with alpha ZERO / the colour's own
     destination factor, cleared to alpha 1), put on the frame with
     ONE / SRC_ALPHA, redrawn every third frame. That is exact for the
     HUD's alpha and additive blends.
  2. Object draws on damnation, carousel and boardingaction (12 to 18 ms):
     find what the 25 to 36 are (weapons and pickups at rest are the
     guess) and whether the bone constants are sent for unskinned ones.
  3. Carousel's 32 transparent draws (12.5 ms).
- Campaign is far from 20 fps: gameplay is 90 ms on c10 and d20, 105 on
  a50 and 180 on a30 (340 draws, 127840 indices a frame). No single item
  above closes that; the goal for campaign in this release is "runs and
  does not crash", and its frame rate is a later roadmap.
- b30 crashed (bus error) at 335 s: `actions.c` kept a
  `path_collision_result` in a `byte` array, which the ARM compiler may
  put at an odd address, and reading its reals from there faults.
  `patches/chupathingyce/0002-aligned-path-collision-result.patch` aligns
  the three of them. Upstream should have it; other byte-array records in
  the decompiled sources are the same risk on this CPU.

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
