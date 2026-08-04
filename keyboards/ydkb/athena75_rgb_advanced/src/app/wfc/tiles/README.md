# WFC tile art

The four tilesets are **drawn as pixel art in `../make_tiles.py`**, at their
final 16×16 size. Nothing is sliced or downsampled into the firmware any more,
so the illustrations this started from are not kept — the repo ignores `*.png`
except each app's committed `icon.png`, and none of them were build inputs.

The launcher icon is the one leftover. `../icon.png` is the build input, same as
every other app; it was resized once from a local illustration with

```
python3 ../make_tiles.py --icon-src tiles/icon_src.png --icon-out ../icon.png
```

which is still there if it ever needs redoing from a fresh source.

Why it is drawn rather than generated from art:

* WFC needs a trace or coastline to leave a tile at **exactly** the pixel row
  its neighbour expects. Resampling a loose illustration cannot guarantee that,
  which is what produced the visible seams and dead-ended wires before.
* 16×16 is small enough that a hand-placed palette and one-pixel bevels read far
  better than a downscaled painting.

## Editing the art

Everything lives in the `render_*` functions of `make_tiles.py`. Each theme has
a small named palette and a mask that comes from its adjacency bits. To keep the
tiles joinable, two rules must hold — `make_tiles.py` fails the build otherwise:

1. Shape may only depend on the adjacency bits, in the way the solver's matching
   rule expects. Circuit and pipes label **edges** (`N=8 E=4 S=2 W=1`), so each
   edge row/column must be filled exactly when its bit is set. Dungeon and island
   label **corners** (`NW=8 NE=4 SE=2 SW=1`), which is the stronger constraint:
   there the whole `BAND`-wide strip along an edge has to match the neighbour's,
   not just the touching line.
2. Surface texture must repeat on a period that divides 16, so a pattern started
   in one tile continues correctly in the next.

There is a third rule that no script can check: the two materials of a theme have
to be told apart at a glance. Bevelling is what says "this stands up", so only
one of them may have it. The dungeon floor was drawn as bevelled slabs once and
promptly read as a second wall seen face-on, which is why it is flat now — depth
belongs to the wall alone.

Corner labels exist because shading looks *past* the joint. A beach or a bevel is
decided by what lies within two pixels, so matching only the touching column
still lets a beach stop dead at a tile border. The fix is to keep every corner's
influence from reaching the opposite border: then the strip along an edge depends
on that edge's two corners alone, and a legal neighbour has the very same two.

The two corner-labelled themes use that same trick with different geometry, which
is what keeps them from being each other in a different palette:

* `corner_mask()` — island. A round falloff per set corner, summed and
  thresholded, then `roughen()`ed into a ragged coast. Nothing runs at 45°.
* `room_mask()` — dungeon. A square block per set corner, no roughening, so
  rooms come out with straight walls and right-angled junctions.

Free-form detail is fine anywhere `BAND` or more pixels in from the border; that
is the room `roughen()` works in, and it is also where anything *built* goes — a
campfire, a stand of trees, a hut. Those tiles are ordinary members of the set
that happen to carry furniture, which is why a tile id is no longer just its
label: `DUNGEON_TILES` and `ISLAND_TILES` map each id to a shape plus what
stands on it, and `wfc_app.c` keeps the same table.

Doors are the one piece of furniture that cannot live in that free interior. A
wall is `2 * (LAST - ROOM)` pixels of rock straddling a lattice line, half of it
in each of the two tiles either side, so no single tile holds a whole door. Each
half is drawn from the same columns and the matching rule is extended with a
door flag per horizontal edge, so a door tile can only ever be placed against
its other half. Every corner pattern that leaves the shared edge solid gets a
door variant, including the all-rock one, so propagation can never back the
solver into a corner where the partner it needs does not exist.

Doors go in horizontal walls only. Partly it is a budget — the solver's option
mask is a `uint32`, which caps a set at 32 tiles — but a door in a wall running
away from the viewer would be drawn edge-on, and at this size that is a smudge.

## Layout comes from the tile odds, not the art

Seamless tiles are only half of it. Collapsing to a uniformly random legal tile
flips roughly every second lattice corner, and the board reads as speckle: the
dungeon becomes pillars rather than rooms, the island becomes confetti. So
`dungeon_weight[]` and `island_weight[]` in `wfc_app.c` weight the choice — the
two solid tiles heavily, the diagonal pinches barely at all. That couples
neighbouring corners, and same-material domains grow into rooms and landmasses.

The weights are a balance, and both ends are visible: too flat and the board is
speckle again, too steep and the whole board falls into a single material.

Once several tiles share one shape, the odds tuned for that shape are *split*
between them rather than added to. Planting trees on a third of the inland tiles
must not make inland any more likely than it was, or furnishing a theme quietly
redraws the floor plan it sits in.

Two different things decide how much wall a dungeon shows, and they are worth
keeping apart. How *thick* a wall is comes from `ROOM` in the art: bigger blocks
leave less rock between two rooms, and at the maximum the seam rule allows a
partition is 6 px rather than a full tile. How much wall there *is* comes from
the odds.

Read against the lattice, the dungeon tiles are pieces of wall rather than
shapes: a tile with two adjacent rock corners is a run of wall, one rock corner
is a turn or a junction, three is a dead end, and none at all is rock two tiles
deep. Weighting runs and junctions up while holding dead ends and tile 0 down is
what turns loose stubs of wall into a network that actually encloses rooms.

Weights only mean anything if the draw is fair. `rng_below()` takes the range
from the top of the word, because `rng_u32() % n` on this LCG samples its low
bits, where bit 0 merely alternates — that flattened the table above toward a
coin toss and gave the collapse repeating habits.

## Checking a change

```
python3 make_tiles.py                                   # per-theme seam check
bash ../tools/test_wfc.sh 300 400                       # solver still converges
bash ../tools/preview_wfc.sh 12                         # solved boards, drawn offline
bash ../../../tools/sim_app_preview.sh wfc 9000 24000   # real screenshots
python3 build/grade_shot.py DUNG <shot.png>             # score what the device drew
python3 build/check_render_seams.py <shot.png>          # measured on the render
```

`preview_wfc.sh` is the one to reach for while tuning layout: it runs the real C
solver and draws the finished boards with the real tiles in about two seconds,
against a minute and a half for a device simulation. `grade_shot.py` reads the
tile ids back out of a capture and scores it the same way, so the offline numbers
can be checked against what the device actually drew — that comparison is what
turned up the LCG bug above.
