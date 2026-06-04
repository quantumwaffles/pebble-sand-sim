# Pebble Sand Sim

A falling-sand sandbox for the **Pebble Time 2 (Emery)** — color display and
capacitive touch. Paint powders, liquids, and walls; tilt the watch to pour
them; bend gravity with your finger; build your own materials. Everything is a
fixed-point cellular automaton tuned to run at ~30 fps inside the watch's RAM
and CPU budget.

The entire app is a single file: [`src/c/main.c`](src/c/main.c).

## Contents

- [Interaction](#interaction)
- [The simulation](#the-simulation)
  - [World representation](#world-representation)
  - [The falling-sand update](#the-falling-sand-update)
  - [Analog tilt gravity](#analog-tilt-gravity)
  - [Gravity modes](#gravity-modes)
  - [Force-field tools](#force-field-tools)
  - [Materials: two orthogonal axes](#materials-two-orthogonal-axes)
- [Rendering](#rendering)
- [The menu](#the-menu)
- [Persistence](#persistence)
- [Building & installing](#building--installing)
- [Known limitations](#known-limitations)

## Interaction

**Touch** paints (or applies a force tool) at your finger. **Tilt** drives
gravity. The four buttons drive an on-screen menu:

| Button | On the canvas | In the menu |
| --- | --- | --- |
| **Select** | open the menu | descend into a submenu, or commit the highlighted value |
| **Up / Down** | — | cycle the current item |
| **Back** | exit the app | go up one level (and finally exit) |

The menu bar shows one item at a time with affordance arrows: **up/down** =
cyclable, **left** = Back, and on the right either a **►** (Select descends) or a
**✓** (Select commits). Values are **committed on Select**, not applied live, so
you can dial a value in and cancel with Back.

Top-level categories: **Material · Tool · Size · Gravity · Bounds · Clear**.

- **Material** — pick a material to paint (auto-switches to the Brush tool), or
  create/edit your own (see [Materials](#materials-two-orthogonal-axes)).
- **Tool** — how the finger acts: **Brush** (paint), **Eraser** (remove),
  **Attract** / **Repel** / **Push** (force fields).
- **Size** — brush/field radius (also scales the force tools' reach).
- **Gravity** — **Sensor / Off / Static / Center** (see below).
- **Bounds** — **On** = closed dish; **Off** = material flows off-screen and is
  removed.
- **Clear** — wipe the canvas (with a confirm).

## The simulation

### World representation

The world is a `GRID_W × GRID_H` grid of 4-pixel cells (50 × 57 on Emery's
200 × 228 screen). Each cell is **one byte holding a material *index*** into a
small material table — not the material's properties directly. This indirection
is what makes "edit a material and have every grain of it change instantly" free
(see [Materials](#materials-two-orthogonal-axes)). Index `0` is empty.

A second byte-per-cell array, `s_moved`, is cleared each frame and marks cells a
grain has already moved into, so **no grain moves twice per frame** — which also
lets liquids flow sideways safely.

### The falling-sand update

Each frame, `sim_step()` scans the grid and calls `update_grain()` per cell. The
classic rule, generalized to an arbitrary gravity direction:

1. Try to move **forward** (one step along gravity).
2. Else try the two **forward diagonals**.
3. **Liquids** additionally try to spread **sideways** (to find their level).
4. **Solids** never move.

Two details keep it correct and natural:

- **Scan order** runs from the gravity-forward edge backward, so a grain that
  falls into a forward cell isn't re-processed the same frame. (The `s_moved`
  flag is the belt-and-suspenders that also makes sideways liquid flow safe.)
- **Density**: a powder falling onto a liquid **swaps** with it (sinks), so sand
  poured into water sinks instead of floating.

### Analog tilt gravity

Gravity is a 2-D vector (from the accelerometer in Sensor mode). `reduce_grav()`
turns any vector into a move decision:

- The **dominant axis** becomes the cardinal "forward" direction.
- The smaller component sets a **lean probability** = `minor / major`. Each grain
  independently leans toward the tilt with that probability — so the *aggregate*
  flow tracks the true tilt angle continuously even though each grain only ever
  steps to one of 8 neighbors. This **per-grain probabilistic lean** is what
  gives the analog feel without the field "snapping" between 8 directions.
- The **in-plane magnitude** (`sqrt`-free, via alpha-max-plus-beta-min) sets a
  per-frame **move probability**: a shallow tilt creeps, a steep tilt pours, and
  a near-flat watch (gravity pointing into the glass) freezes — grains hang in
  place like sand suspended in a petri dish.

The accelerometer is read via a **continuous data subscription** (50 Hz), not
on-demand `peek` — the latter leaves the sensor dormant and returns stale
readings, which caused a ~½-second tilt lag.

### Gravity modes

`update_gravity()` produces the base gravity per mode:

- **Sensor** — accelerometer tilt (the analog model above).
- **Off** — none; everything freezes (a build mode — draw stable structures, then
  switch gravity back on and watch them go).
- **Static** — a fixed direction (Down / Up / Left / Right), ignoring tilt.
- **Center** — gravity points toward the **screen center**, computed *per cell*,
  so material collects into a blob in the middle.

For Sensor/Off/Static the whole grid shares one gravity vector (fast path).
Center and the force tools take a **per-cell** path, which the `s_moved` flag
keeps collision-safe even though the direction now varies cell to cell.

### Force-field tools

Attract, Repel, and Push don't move particles directly — they **bend gravity
locally**, and the normal falling rules do the rest (so piling, displacement,
and density all keep working inside the field). Effective gravity for a cell in
range = tilt gravity **plus** a field vector:

- **Attract** — points toward the finger (a sink: material pours in and piles).
- **Repel** — points away (a source: blasts a crater).
- **Push** — a uniform vector in the finger's **drag direction** (a gust that
  sweeps material along the swipe).

Field strength is strong at the finger (well above normal gravity, so it wins)
and **tapers to zero at the rim**; the radius scales with brush Size. They work
even with Gravity Off, so you can sculpt a frozen scene.

### Materials: two orthogonal axes

A material is a **(behavior, visual)** pair — physics and look are independent:

- **Behavior** — Powder / Liquid / Solid.
- **Visual** — a color ramp: Sand, Ice, Lime, Berry, Cherry, Amber, Ember, Gold,
  Slate.

Because they're orthogonal, the same look can have different physics and vice
versa: *Ember + Liquid = lava*; *Ice + Solid = a frozen pane*. The four built-in
materials (Sand, Water, Stone, Lava) are read-only; you can **create** your own
via **Material → Custom** (set Behavior/Visual, then **Add**), and **Edit** or
**Delete** customs.

Editing is instant across the whole canvas: cells store a material *index*, so
changing slot N's definition redefines every grain pointing at it — recolor all
your "water" to cherry, or melt your "stone" by switching it to Liquid, in one
press. Custom materials are auto-named `"<Behavior> <Visual>"` (no on-watch
keyboard needed).

## Rendering

Drawing thousands of cells with `graphics_fill_rect` would be far too slow, so
the canvas writes **straight into the captured framebuffer** (8-bit color, one
byte per pixel on Emery): each filled cell `memset`s its `CELL_SIZE`-wide color
block per row.

Each grain's shade comes from an **xxHash-style hash of its (x, y)**, indexing
its visual's 4-shade ramp — a stable, speckled texture at zero per-cell storage.
The hash *avalanches* so even the low bits look like noise (a weaker hash made
visible diagonal banding). Two visuals are **animated** instead of static:

- **Ember** — each grain rides a triangle wave up and down a dark→hot ramp on its
  own phase, so the bed shimmers like coals.
- **Gold** — mostly a steady metallic tone with sparse, brief bright **glints**,
  like light catching flecks of metal.

Because the renderer runs every frame regardless of motion, these keep animating
even on a settled pile.

## The menu

The menu is an **N-level navigation stack** of `(node, selected index)` pairs.
Each "node" is a list of items; small helpers describe each node
(`node_count` / `node_label` / `node_child` / `node_commit` /
`node_item_descends` / `node_init_sel`). Select either **descends** (push a child
node, opened on the current committed value) or **commits a leaf** (and closes);
Back pops a level, then closes, then exits the app. Level transitions **slide**
horizontally (ease-out); value cycling is instant. This stack is what makes
submenus like *Gravity → Static → direction* and *Material → Custom →
Behavior/Visual* possible.

## Persistence

The canvas, custom materials, and settings are saved on exit and restored on
launch. Pebble persist fields cap at 256 bytes, so:

- The **grid** is **nibble-packed** (material indices are ≤ 15, so 4 bits/cell)
  to ~1.4 KB, then split across several 256-byte chunk fields.
- **Custom materials** store just `(behavior, visual)` (names are regenerated;
  built-ins keep their static names).
- **Settings** (brush / tool / size / gravity mode / static direction / bounds)
  fit in one small field. A format key invalidates old saves, and all reads are
  range-clamped with stale material indices dropped.

Saving happens on a clean Back-to-exit (in `deinit`), not on a force-quit.

## Building & installing

Uses the current Core Devices PebbleOS SDK (4.9+, needed for the TouchService).
The repo ships its own build image. Build it once:

```sh
docker build -t pebble-sdk-touch .
```

Then build and install (developer connection enabled in the Pebble phone app):

```sh
docker run --rm -i -w /app -v "$PWD:/app" pebble-sdk-touch pebble build
docker run --rm -i -w /app -v "$PWD:/app" pebble-sdk-touch pebble install --phone <phone-ip>
```

On Windows, `build.cmd` and `install.cmd <phone-ip>` wrap those (the latter
caches the IP in `.pebble-ip`). A native `pebble` SDK install works too. Targets
**emery** only.

## Known limitations

- **Fast liquid flow shows the 8-way grid.** The analog feel is a *statistical
  aggregate* of per-grain leans — great for dense piling sand, but thin/fast
  water reveals individual grains stepping in 8 discrete directions. A true fix
  needs sub-cell velocity/momentum (fractional positions), a larger CA change.
- **Center gravity packs a square.** Grid packing follows a Chebyshev metric, so
  the Center-mode blob is square-ish rather than round. A Euclidean-distance
  gather (experiment on the `circular-center` branch) makes it round but didn't
  feel right; parked.
- **No save on crash/force-quit** — only on a clean exit.
