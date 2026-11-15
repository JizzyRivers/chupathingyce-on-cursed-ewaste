# Round 4: crash/speed sweep and view distance (0.9.1a-dev, Steam Link box)

Build: `ChupathingyCE 0.9.1a-dev (dev, release config, commit 2e5eac00, built 2026-11-14) Linux arm`
(run.sh already had EGL_MVGFX_W=848, so 848x480 internal). No env vars in step 1.
ms a frame = mean of the last 3 steady 300-frame windows the layer printed
(fewer where the run had fewer windows: ratrace/sidewinder/chillout/damnation/boardingaction/carousel
had 3, 3, 3, 2, 2, 1 windows after the load window). "ran" = still running at the time limit.
No `bus error`, `crash`, `Assertion` or `halt` line in any of the 16 logs.

## Step 1: sweep, no env vars

| map | seconds | end | peak MB | ms a frame | draws |
|---|---|---|---|---|---|
| beavercreek | 100 | ran | 151 | 32.0 | 51 |
| hangemhigh | 100 | ran | 159 | 31.6 | 40 |
| wizard | 100 | ran | 156 | 32.7 | 64 |
| longest | 100 | ran | 158 | 34.1 | 57 |
| putput | 100 | ran | 155 | 35.3 | 53 |
| prisoner | 100 | ran | 159 | 37.5 | 78 |
| ratrace | 100 | ran | 161 | 38.1 | 89 |
| bloodgulch | 100 | ran | 158 | 37.7 | 57 |
| sidewinder | 100 | ran | 160 | 40.7 | 61 |
| chillout | 100 | ran | 159 | 43.4 (42.1, 47.0, 41.2) | 99 |
| damnation | 100 | ran | 158 | 59.4 (2 windows) | 102 |
| boardingaction | 100 | ran | 158 | 59.9 (2 windows) | 141 |
| carousel | 100 | ran | 158 | 76.4 (1 window) | 135 |
| b30 | 480 | ran | 187 | 121.3 (125.4, 122.0, 116.4, still falling) | 62 |
| a10 | 240 | ran | 176 | 88.2 (106.2, 84.6, 73.9, not steady) | 96 |
| d40 | 240 | ran | 173 | 62.6 | 122 |

Nothing crashed; every run was still alive at its limit. a10 and b30 are not steady
(a10 window sequence 79 18 106 85 74; b30 186 134 125 122 116), so their means are rough.

## Step 2: HALO_PERFORMANCE_VIEW_DISTANCE

Each run also had HALO_SCREENSHOT_EVERY=250 and a HALO_SCREENSHOT_DIR, so these
numbers are only comparable with each other, not with step 1 (bloodgulch at 60 reads
46 here against 37.7 in step 1; the screenshots are the likely difference, a guess).
Bloodgulch and sidewinder 100 s, a30 300 s.

| map | distance | ms a frame | draws |
|---|---|---|---|
| bloodgulch | 30 | 42.3 | 49 |
| bloodgulch | 45 | 43.6 | 49 |
| bloodgulch | 60 | 46.3 | 53 |
| sidewinder | 30 | 40.8 | 65 |
| sidewinder | 45 | 41.8 | 69 |
| sidewinder | 60 | 46.5 | 81 |
| a30 | 30 | 175.1 | 325 |
| a30 | 45 | 172.2 | 291 |
| a30 | 60 | 194.1 (217.4, 181.5, 183.5) | 334 |

a30's windows were noisy (30: 305 176 190 160; 45: 218 216 171 174 171; 60: 217 217 182 184);
the a30 differences are not clear. Bloodgulch and sidewinder show a steady gain of about 3 ms (45)
and 4-5 ms (30) against 60.

Pictures (frame 1000, 160x120 PNGs) in `docs/shots/viewdistance/<map>-<distance>.png`.

- bloodgulch: the spawn looks at a hill with the sun behind haze. At 60 a distant ridge on the right
  is visible with a bit of sky glow; at 45 the ridge is fainter and the haze is thicker; at 30 the
  distant ridge is nearly gone into fog and the hill top is washed out. The nearby hill and the
  rifle are the same at all three. No bases or objects were in view, so I cannot judge those.
- sidewinder: at 60 a thin distant tower/spire (centre-left) and a tree on the right are visible
  against the sky; at 45 the spire is gone and the tree is slightly fogged; at 30 the picture is
  hazier still, tree barely visible. The nearby rocks and the foreground are unchanged.
  A fog wall is visible at 45 and 30 (the cut-off turns into grey), not a hard pop.
- a30: the spawn faces the inside of a corridor (ceiling lights and a wall), so all three
  pictures are the same and the view distance cannot be judged there.

Judging from 160x120 pictures; small details may have been missed.
