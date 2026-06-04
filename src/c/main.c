// Pebble Sand Sim
// A falling-sand cellular automaton.
//   - Grid of cells; each cell holds an index into a material table.
//   - A material = a (behavior, visual) pair. Behavior drives physics (powder /
//     liquid / solid); visual drives color. The two axes are independent, so the
//     same look can have different physics and vice versa.
//   - Touch (Emery): tap or drag to paint the selected material; Eraser removes.
//   - Tilt gravity: the accelerometer steers which way "down" is.
//   - Rendered by writing the captured framebuffer directly (fast path).
//   - Select opens an N-level menu (Material / Tool / Size / Gravity / Bounds /
//     Clear, with submenus like Gravity > Static > direction). Select descends
//     or commits a leaf; Back ascends/cancels and finally exits.

#include <pebble.h>

// --- Grid geometry -----------------------------------------------------------
// Emery is 200x228. CELL_SIZE divides both evenly (50x57 cells).
#define CELL_SIZE 4
#define GRID_W (200 / CELL_SIZE)   // 50
#define GRID_H (228 / CELL_SIZE)   // 57

// --- Materials & visuals -----------------------------------------------------
// Cell value 0 = empty; 1..s_material_count index the material table.
#define MAT_EMPTY 0

// Behavior axis (physics).
typedef enum { BEH_POWDER, BEH_LIQUID, BEH_SOLID } Behavior;

// Visual axis (color). Indices into the visuals table. Ember and Gold are
// special-cased in cell_color to animate (glow / glisten).
#define NUM_VISUALS 9
#define VIS_SAND   0
#define VIS_ICE    1
#define VIS_LIME   2
#define VIS_BERRY  3
#define VIS_CHERRY 4
#define VIS_AMBER  5
#define VIS_EMBER  6
#define VIS_GOLD   7
#define VIS_SLATE  8

// A material is a (behavior, visual) preset with a display name (auto-generated
// for created/edited ones, so no on-watch text entry).
typedef struct { uint8_t behavior; uint8_t visual; char name[20]; } Material;
#define MAX_MATERIALS 16

// Paint brush radius range, in cells (selectable via the menu).
#define BRUSH_MIN 1
#define BRUSH_MAX 5

// Tilt gravity speed: the sand feels the in-plane projection of gravity (what's
// left after the component pointing into the glass). Its magnitude sets how
// often a grain moves, in milli-g: at/above G_FULL grains fall every frame; at
// or below G_MIN (watch near-flat) they rest entirely.
#define G_FULL 1000
#define G_MIN  120

// Force-field tools (Attract/Repel): a local gravity perturbation centered on
// the finger that augments tilt gravity during the sim. FIELD_STR (milli-g) at
// the center > G_FULL so material is pulled hard; falls off to the edge.
#define FIELD_MAX_R 48
#define FIELD_STR   3500   // pull at the finger; tapers to 0 at the rim

// Screen center in cells, for the Center gravity mode.
#define GRAV_CX (GRID_W / 2)
#define GRAV_CY (GRID_H / 2)

// --- Timing ------------------------------------------------------------------
#define FRAME_MS 33                // ~30 fps

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

// The world. Row-major: grid[gy * GRID_W + gx]. Each value is a material index
// (0 = empty). s_moved marks cells already moved this frame so no grain moves
// twice (lets liquids flow sideways safely).
static uint8_t s_grid[GRID_W * GRID_H];
static uint8_t s_moved[GRID_W * GRID_H];

static uint32_t s_frame;

// Gravity for this frame, derived from the accelerometer. Defaults to full-speed
// straight down. A grain moves one step "forward" (the dominant cardinal axis)
// plus an optional lateral step toward the tilt:
//   - (s_fdx,s_fdy)  forward: the dominant cardinal direction.
//   - (s_pdx,s_pdy)  perpendicular unit (the lateral axis).
//   - s_tside        which lateral way the tilt leans (-1/0/+1).
//   - s_lean_num/den  P(take the lateral step) = num/den (how diagonal the tilt).
//   - s_move_p256    P(grain moves at all this frame) * 256 (gravity magnitude).
static int s_fdx = 0, s_fdy = 1;
static int s_pdx = 1, s_pdy = 0;
static int s_tside = 0;
static int s_lean_num = 0, s_lean_den = 1;
static int s_move_p256 = 256;

// Tilt-gravity vector this frame (screen-space, x right / y down). Force-field
// tools add a per-cell vector to this. Field params set each frame in sim_step.
static int  s_grav_x = 0, s_grav_y = 1000;
static bool s_grav_center = false;            // Center mode: gravity per-cell
static bool s_field_on = false;
static int  s_field_fx = 0, s_field_fy = 0;   // finger cell
static int  s_field_sign = -1;                // -1 attract (toward), +1 repel
static int  s_field_R = 0, s_field_rr = 0;
static bool s_field_push = false;             // Push: uniform field in drag dir
static int  s_field_dirx = 0, s_field_diry = 0;

// Latest accelerometer sample, refreshed by the data-service handler (which
// keeps the sensor continuously sampling -- far fresher than on-demand peek).
// Default: top up, so gravity starts pointing down.
static int s_accel_x = 0;
static int s_accel_y = -1000;

#if defined(PBL_TOUCH)
// Touch state: grid cell under the finger, active while a finger is down.
// Only compiled where the SDK has the TouchService (Pebble Time 2 / Emery).
static bool s_touch_active = false;
static int  s_touch_gx = 0;
static int  s_touch_gy = 0;
static int  s_touch_px = 0, s_touch_py = 0;   // current pixel position
static int  s_prev_px = 0,  s_prev_py = 0;    // pixel position last field pass
#endif

// Each visual is a 4-shade ramp (raw argb bytes), so a grain can be tinted for a
// speckled texture at zero per-cell cost. Built at init by build_visuals().
#define NUM_SHADES 4
static uint8_t s_visuals[NUM_VISUALS][NUM_SHADES];

// Material table. Index 0 is the empty placeholder; 1..s_material_count are real
// materials. cell_color reads .visual; sim_step switches on .behavior.
static Material s_materials[MAX_MATERIALS] = {
  { BEH_SOLID,  0,         ""      },  // 0: empty (placeholder, never painted)
  { BEH_POWDER, VIS_SAND,  "Sand"  },  // 1
  { BEH_LIQUID, VIS_ICE,   "Water" },  // 2
  { BEH_SOLID,  VIS_SLATE, "Stone" },  // 3
  { BEH_LIQUID, VIS_EMBER, "Lava"  },  // 4
};
static int s_material_count = 4;       // valid material indices are 1..4
#define NUM_DEFAULT_MATS 4             // 1..4 are built-in (no edit/delete)

// Names for the two axes (used to auto-name materials and in the edit menus).
static const char *BEH_NAMES[3] = { "Powder", "Liquid", "Solid" };
static const char *VIS_NAMES[NUM_VISUALS] = {
  "Sand", "Ice", "Lime", "Berry", "Cherry", "Amber", "Ember", "Gold", "Slate"
};

static int s_edit_mat = 1;             // material being edited in the menu
static bool s_mat_is_new = false;      // editing an unsaved draft (Custom)

// Name a material "<Behavior> <Visual>".
static void material_autoname(int i) {
  snprintf(s_materials[i].name, sizeof(s_materials[i].name), "%s %s",
           BEH_NAMES[s_materials[i].behavior], VIS_NAMES[s_materials[i].visual]);
}

// Set up a draft material (default Powder+Sand) in the slot just past the table,
// without counting it yet; returns its index. Committed by NODE_MATEDIT's "Add".
static int material_draft(void) {
  int i = s_material_count + 1;
  if (i >= MAX_MATERIALS) i = s_material_count;  // full: reuse last (no room)
  s_materials[i].behavior = BEH_POWDER;
  s_materials[i].visual = VIS_SAND;
  material_autoname(i);
  return i;
}

// Tools: how the finger interacts with the canvas. Brush paints the selected
// material; Eraser removes; Attract/Repel bend gravity locally (sink / source).
typedef enum {
  TOOL_BRUSH, TOOL_ERASER, TOOL_ATTRACT, TOOL_REPEL, TOOL_PUSH, NUM_TOOLS
} Tool;
static const char *TOOL_NAMES[NUM_TOOLS] = {
  "Brush", "Eraser", "Attract", "Repel", "Push"
};

// Gravity modes: Sensor = accelerometer tilt; Off = none (freeze/build); Static
// = always down (later: a submenu for N/S/E/W); Center = toward screen center.
typedef enum {
  GMODE_SENSOR, GMODE_OFF, GMODE_STATIC, GMODE_CENTER, NUM_GMODES
} GravMode;
static const char *GMODE_NAMES[NUM_GMODES] = { "Sensor", "Off", "Static", "Center" };

// --- User settings (committed from the menu) ---------------------------------
static int  s_brush_mat = 1;          // selected material, 1..s_material_count
static int  s_tool = TOOL_BRUSH;      // active tool
static int  s_brush_r = 2;            // paint brush radius, BRUSH_MIN..BRUSH_MAX
static int  s_gravity_mode = GMODE_SENSOR;
static bool s_bounds_on = true;       // false lets material flow off-screen

// --- Menu --------------------------------------------------------------------
// N-level menu over a black bar, shown one item at a time. Each screen is a
// "node" (a list of items); Up/Down pick an item, Select descends into a child
// node or commits a leaf value, Back ascends one level (and finally exits). A
// nav stack of (node, selected index) tracks the path so Back can restore it.
typedef enum {
  NODE_ROOT, NODE_MATERIAL, NODE_TOOL, NODE_SIZE,
  NODE_GRAVITY, NODE_GRAVDIR, NODE_BOUNDS, NODE_CLEAR,
  NODE_MATITEM,    // per-material actions: Use / Edit / Delete
  NODE_MATEDIT,    // edit axes: Behavior / Visual [/ Add, when new]
  NODE_BEHAVIOR,   // pick a behavior (leaf)
  NODE_VISUAL,     // pick a visual (leaf)
  NODE_DELCONFIRM  // confirm material delete
} MenuNode;

// Root items (also the category names).
typedef enum {
  CAT_MATERIAL, CAT_TOOL, CAT_SIZE, CAT_GRAVITY, CAT_BOUNDS, CAT_CLEAR, NUM_CATS
} MenuCat;
static const char *CAT_NAMES[NUM_CATS] = {
  "Material", "Tool", "Size", "Gravity", "Bounds", "Clear"
};

// Static-gravity directions (the NODE_GRAVDIR submenu). Index -> screen dir.
static const char *DIR_NAMES[4] = { "Down", "Up", "Left", "Right" };
static int s_static_dir = 0;          // committed Static direction (default Down)

#define MENU_MAX_DEPTH 6
static int s_nav_node[MENU_MAX_DEPTH];
static int s_nav_sel[MENU_MAX_DEPTH];
static int s_nav_depth = 0;           // 0 = closed; current level = depth - 1
static int s_root_sel = 0;            // remembered Root selection across opens

// Affordance arrows (filled triangles): up/down = options cycle (Up/Down),
// right = Select/descend, left = Back/ascend. Built at window load.
static const GPathInfo ARROW_UP_INFO    = { 3, (GPoint[]) { {-6, 3},  {6, 3},  {0, -4} } };
static const GPathInfo ARROW_DOWN_INFO   = { 3, (GPoint[]) { {-6, -3}, {6, -3}, {0, 4}  } };
static const GPathInfo ARROW_LEFT_INFO   = { 3, (GPoint[]) { {3, -6},  {3, 6},  {-4, 0} } };
static const GPathInfo ARROW_RIGHT_INFO  = { 3, (GPoint[]) { {-3, -6}, {-3, 6}, {4, 0}  } };
static GPath *s_arrow_up, *s_arrow_down, *s_arrow_left, *s_arrow_right;

// Level-transition slide animation: the label text slides horizontally while the
// bar and arrows stay put. Driven by the frame timer. dir +1 descends (old label
// exits left, new enters from right); -1 ascends (mirror).
#define MENU_ANIM_FRAMES 7
static bool s_anim_active = false;
static int  s_anim_t = 0;
static int  s_anim_dir = 0;
static char s_anim_from[24];
static char s_anim_to[24];

// --- Tiny fast PRNG (xorshift32) --------------------------------------------
static uint32_t s_rng = 0x1a2b3c4d;
static inline uint32_t xrand(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

// --- Helpers -----------------------------------------------------------------
static inline uint8_t cell_at(int gx, int gy) {
  return s_grid[gy * GRID_W + gx];
}
static inline void set_cell(int gx, int gy, uint8_t m) {
  s_grid[gy * GRID_W + gx] = m;
}

static inline int clamp_i(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline bool in_bounds(int gx, int gy) {
  return gx >= 0 && gx < GRID_W && gy >= 0 && gy < GRID_H;
}

// Tiny integer sqrt (inputs are small: distances within the field radius).
static inline int isqrt_i(int v) {
  int r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

// Color a cell from its material's visual. Hash the coordinate into the visual's
// 4-shade ramp so a settled grain keeps a stable speckle (the xxHash-style
// finalizer avalanches the bits so even the low bits look like noise, not bands).
// Ember and Gold animate their shade index instead of holding it static.
static inline uint8_t cell_color(uint8_t mv, int gx, int gy) {
  int visual = s_materials[mv].visual;
  uint32_t h = (uint32_t)gx * 374761393u + (uint32_t)gy * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;

  int idx;
  if (visual == VIS_EMBER) {
    // Triangle wave over time, offset per grain by its hash, so grains drift up
    // and down the 4-step ramp out of sync -> a shimmering ember glow.
    uint32_t a = (s_frame * 5u + (h & 0xFFu)) & 0xFFu;
    uint32_t tri = (a < 128u) ? (a * 2u) : ((255u - a) * 2u);  // 0..254
    idx = tri >> 6;                                            // 0..3
  } else if (visual == VIS_GOLD) {
    // Mostly a steady gold tone (0-2); each grain briefly flashes the bright
    // highlight (3) on its own phase, so a sparse few glint at any moment.
    uint32_t phase = (h >> 5) & 127u;
    uint32_t t = (s_frame + phase) & 127u;
    idx = (t < 3u) ? 3 : (int)(h % 3u);
  } else {
    idx = h & (NUM_SHADES - 1);
  }
  return s_visuals[visual][idx];
}

// --- Visuals -----------------------------------------------------------------
// Build all visual ramps once at init (GColorFromRGB resolves at runtime).
static void build_visuals(void) {
  #define SET4(v, a, b, c, d) do { \
      s_visuals[v][0] = (a).argb; s_visuals[v][1] = (b).argb; \
      s_visuals[v][2] = (c).argb; s_visuals[v][3] = (d).argb; } while (0)
  SET4(VIS_SAND,   GColorFromRGB(0xAA,0x88,0x44), GColorFromRGB(0xCC,0xAA,0x55),
                   GColorFromRGB(0xDD,0xBB,0x66), GColorFromRGB(0x99,0x77,0x33));
  SET4(VIS_ICE,    GColorFromRGB(0x55,0xAA,0xFF), GColorFromRGB(0x88,0xCC,0xFF),
                   GColorFromRGB(0xAA,0xEE,0xFF), GColorFromRGB(0x33,0x88,0xDD));
  SET4(VIS_LIME,   GColorFromRGB(0x66,0xCC,0x33), GColorFromRGB(0x99,0xEE,0x44),
                   GColorFromRGB(0xCC,0xFF,0x66), GColorFromRGB(0x44,0x99,0x22));
  SET4(VIS_BERRY,  GColorFromRGB(0xDD,0x44,0x99), GColorFromRGB(0xFF,0x77,0xBB),
                   GColorFromRGB(0xCC,0x55,0xFF), GColorFromRGB(0x99,0x33,0x88));
  SET4(VIS_CHERRY, GColorFromRGB(0xAA,0x00,0x00), GColorFromRGB(0xFF,0x00,0x00),
                   GColorFromRGB(0xFF,0x55,0x55), GColorFromRGB(0xAA,0x00,0x55));
  SET4(VIS_AMBER,  GColorFromRGB(0xFF,0xAA,0x00), GColorFromRGB(0xFF,0x55,0x00),
                   GColorFromRGB(0xFF,0xAA,0x55), GColorFromRGB(0xAA,0x55,0x00));
  SET4(VIS_EMBER,  GColorFromRGB(0x55,0x00,0x00), GColorFromRGB(0xAA,0x00,0x00),
                   GColorFromRGB(0xFF,0x55,0x00), GColorFromRGB(0xFF,0xAA,0x00));
  SET4(VIS_GOLD,   GColorFromRGB(0xAA,0x55,0x00), GColorFromRGB(0xFF,0xAA,0x00),
                   GColorFromRGB(0xFF,0xAA,0x55), GColorFromRGB(0xFF,0xFF,0xAA));
  SET4(VIS_SLATE,  GColorFromRGB(0x55,0x55,0x55), GColorFromRGB(0xAA,0xAA,0xAA),
                   GColorFromRGB(0x88,0x88,0x88), GColorFromRGB(0x66,0x66,0x66));
  #undef SET4
}

// --- Simulation --------------------------------------------------------------

// Stamp a filled disc centered on a cell. The Eraser tool overwrites anything
// with empty; the Brush only fills empty cells with the selected material (so
// you don't accidentally paint over existing structures -- erase first).
static void paint_brush(int cx, int cy) {
  int r = s_brush_r;
  bool erase = (s_tool == TOOL_ERASER);
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      // Rounded disc: the +1 fattens it from a diamond to a blob.
      if (dx * dx + dy * dy > r * r + 1) {
        continue;
      }
      int gx = cx + dx;
      int gy = cy + dy;
      if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H) {
        continue;
      }
      if (erase) {
        set_cell(gx, gy, MAT_EMPTY);
      } else if (cell_at(gx, gy) == MAT_EMPTY) {
        set_cell(gx, gy, (uint8_t)s_brush_mat);
      }
    }
  }
}

// One frame's gravity for a cell, reduced from a screen-space vector (x right,
// y down): a forward cardinal axis, a perpendicular lean toward the smaller
// component, and a move probability from the magnitude.
typedef struct {
  int fdx, fdy, pdx, pdy, tside, lnum, lden, mp;
} Grav;

// Reduce a gravity vector to a Grav (shared by the global tilt path and the
// per-cell force-field path). Mapping from the fluid-sim: right = a.x, down=-a.y.
static void reduce_grav(int gx, int gy, Grav *o) {
  int ax = gx < 0 ? -gx : gx;
  int ay = gy < 0 ? -gy : gy;
  int sx = gx > 0 ? 1 : (gx < 0 ? -1 : 0);
  int sy = gy > 0 ? 1 : (gy < 0 ? -1 : 0);

  int major, minor;
  if (ax >= ay) {           // horizontal dominant
    o->fdx = sx; o->fdy = 0; o->pdx = 0; o->pdy = 1; o->tside = sy;
    major = ax; minor = ay;
  } else {                  // vertical dominant
    o->fdx = 0; o->fdy = sy; o->pdx = 1; o->pdy = 0; o->tside = sx;
    major = ay; minor = ax;
  }
  o->lnum = minor;
  o->lden = major > 0 ? major : 1;

  int m = major + (minor * 7) / 16;  // ~|(gx,gy)|, sqrt-free
  o->mp = (m <= G_MIN) ? 0 : (m >= G_FULL ? 256 : (m - G_MIN) * 256 / (G_FULL - G_MIN));
}

// Derive this frame's global gravity from the current mode. Center is per-cell
// (handled in update_grain), so here it just flags that and leaves the global
// vector zero. Force fields still work on top of any mode.
static void update_gravity(void) {
  s_grav_center = false;
  int gx = 0, gy = 0;
  switch (s_gravity_mode) {
    case GMODE_SENSOR: gx = s_accel_x; gy = -s_accel_y; break;
    case GMODE_OFF:    break;                       // no gravity (freeze)
    case GMODE_STATIC:                              // fixed direction
      switch (s_static_dir) {
        case 0: gy =  G_FULL; break;  // Down
        case 1: gy = -G_FULL; break;  // Up
        case 2: gx = -G_FULL; break;  // Left
        case 3: gx =  G_FULL; break;  // Right
      }
      break;
    case GMODE_CENTER: s_grav_center = true; break; // toward center, per-cell
  }
  s_grav_x = gx;
  s_grav_y = gy;
  Grav g;
  reduce_grav(gx, gy, &g);
  s_fdx = g.fdx; s_fdy = g.fdy; s_pdx = g.pdx; s_pdy = g.pdy;
  s_tside = g.tside; s_lean_num = g.lnum; s_lean_den = g.lden; s_move_p256 = g.mp;
}

// Try to move the grain at (x,y) into (nx,ny). Moves into empty cells; a powder
// also sinks through a liquid (denser) by swapping with it. Marks moved cells so
// nothing moves twice this frame. Returns true if it moved.
static bool try_move(int x, int y, int nx, int ny, Behavior beh) {
  if (!in_bounds(nx, ny)) {
    if (!s_bounds_on) {
      set_cell(x, y, MAT_EMPTY);  // bounds off: the grain flows off-screen
      return true;
    }
    return false;  // closed container: the edge is a wall
  }
  uint8_t src = cell_at(x, y);
  uint8_t dst = cell_at(nx, ny);
  if (dst == MAT_EMPTY) {
    set_cell(nx, ny, src);
    set_cell(x, y, MAT_EMPTY);
    s_moved[ny * GRID_W + nx] = 1;
    return true;
  }
  if (beh == BEH_POWDER && s_materials[dst].behavior == BEH_LIQUID) {
    set_cell(nx, ny, src);   // powder sinks
    set_cell(x, y, dst);     // liquid rises into the vacated cell
    s_moved[ny * GRID_W + nx] = 1;
    s_moved[y * GRID_W + x] = 1;
    return true;
  }
  return false;
}

// Update one grain by its behavior. Powder/liquid fall along gravity with a
// lateral lean toward the tilt (same forward-slice rule); liquid additionally
// spreads sideways to level out. Solids never move.
static void update_grain(int gx, int gy) {
  int idx = gy * GRID_W + gx;
  if (s_moved[idx]) {
    return;  // already moved into this cell this frame
  }
  uint8_t mv = s_grid[idx];
  if (mv == MAT_EMPTY) {
    return;
  }
  Behavior beh = s_materials[mv].behavior;
  if (beh == BEH_SOLID) {
    return;
  }

  // Effective gravity for this cell. Fast path: uniform global gravity. Per-cell
  // path: Center mode (gravity toward screen center) and/or an active force field.
  int fdx = s_fdx, fdy = s_fdy, pdx = s_pdx, pdy = s_pdy;
  int tside = s_tside, lnum = s_lean_num, lden = s_lean_den, mp = s_move_p256;

  int frx = gx - s_field_fx, fry = gy - s_field_fy;
  int fd2 = frx * frx + fry * fry;
  bool field_here = s_field_on && fd2 > 0 && fd2 <= s_field_rr;

  if (s_grav_center || field_here) {
    int ex, ey;
    if (s_grav_center) {
      int rx = gx - GRAV_CX, ry = gy - GRAV_CY;
      int d2 = rx * rx + ry * ry;
      if (d2 == 0) { ex = 0; ey = 0; }
      else { int d = isqrt_i(d2); ex = -rx * G_FULL / d; ey = -ry * G_FULL / d; }
    } else {
      ex = s_grav_x; ey = s_grav_y;
    }
    if (field_here) {
      int dist = isqrt_i(fd2);
      // Falloff: strong at the finger, tapering to 0 at the rim.
      int mag = FIELD_STR * (s_field_R - dist) / s_field_R;
      if (s_field_push) {
        ex += s_field_dirx * mag;            // uniform drag direction
        ey += s_field_diry * mag;
      } else {
        ex += s_field_sign * frx * mag / dist;  // -toward / +away (radial)
        ey += s_field_sign * fry * mag / dist;
      }
    }
    Grav g;
    reduce_grav(ex, ey, &g);
    fdx = g.fdx; fdy = g.fdy; pdx = g.pdx; pdy = g.pdy;
    tside = g.tside; lnum = g.lnum; lden = g.lden; mp = g.mp;
  }

  // Gravity-magnitude gate: maybe rest this frame.
  if (mp < 256 && (int)(xrand() & 255) >= mp) {
    return;
  }

  // Lateral lean (perpendicular offset): toward the field/tilt with prob lnum/lden.
  int lean = 0;
  if (tside != 0 && (int)(xrand() % (uint32_t)lden) < lnum) {
    lean = tside;
  }

  // Forward-slice attempt order. Going straight: random side then the other so
  // heaps stay symmetric. Leaning: lean, then straight, then the far side.
  int order[3];
  int cnt = 0;
  order[cnt++] = lean;
  if (lean == 0) {
    int s = (xrand() & 1) ? 1 : -1;
    order[cnt++] = s;
    order[cnt++] = -s;
  } else {
    order[cnt++] = 0;
    order[cnt++] = -lean;
  }
  for (int i = 0; i < cnt; i++) {
    int p = order[i];
    if (try_move(gx, gy, gx + fdx + pdx * p, gy + fdy + pdy * p, beh)) {
      return;
    }
  }

  // Liquid: if it couldn't fall, spread sideways (pure perpendicular) to level.
  if (beh == BEH_LIQUID) {
    int s = (xrand() & 1) ? 1 : -1;
    for (int t = 0; t < 2; t++) {
      int dir = (t == 0) ? s : -s;
      if (try_move(gx, gy, gx + pdx * dir, gy + pdy * dir, beh)) {
        return;
      }
    }
  }
}

// One CA step. Every cell a grain can move to lies one step "forward" along the
// dominant gravity axis, so we visit cells from the forward edge backward to
// avoid moving a grain twice. Vertical-dominant gravity loops rows outer (inner
// columns alternate for symmetry); horizontal-dominant swaps the nesting.
static void sim_step(void) {
  // Set up the force field (if a field tool is held down) for this frame.
  s_field_on = false;
#if defined(PBL_TOUCH)
  if (s_touch_active &&
      (s_tool == TOOL_ATTRACT || s_tool == TOOL_REPEL || s_tool == TOOL_PUSH)) {
    s_field_on = true;
    s_field_fx = s_touch_gx;
    s_field_fy = s_touch_gy;
    s_field_R = s_brush_r * 8 + 16;   // ~4x the old reach
    if (s_field_R > FIELD_MAX_R) s_field_R = FIELD_MAX_R;
    s_field_rr = s_field_R * s_field_R;
    s_field_push = (s_tool == TOOL_PUSH);
    if (s_field_push) {
      int ddx = s_touch_px - s_prev_px;   // drag since last frame (pixels)
      int ddy = s_touch_py - s_prev_py;
      s_field_dirx = (ddx > 0) - (ddx < 0);
      s_field_diry = (ddy > 0) - (ddy < 0);
      if (s_field_dirx == 0 && s_field_diry == 0) {
        s_field_on = false;             // not dragging -> no push
      }
    } else {
      s_field_sign = (s_tool == TOOL_ATTRACT) ? -1 : 1;
    }
    s_prev_px = s_touch_px;
    s_prev_py = s_touch_py;
  }
#endif

  if (s_move_p256 == 0 && !s_field_on && !s_grav_center) {
    return;  // no gravity and no field: everything rests
  }
  memset(s_moved, 0, sizeof(s_moved));
  bool flip = (s_frame & 1);

  if (s_fdy != 0) {
    int ys = (s_fdy > 0) ? GRID_H - 1 : 0;
    int ystep = (s_fdy > 0) ? -1 : 1;
    for (int n = 0; n < GRID_H; n++) {
      int gy = ys + ystep * n;
      for (int m = 0; m < GRID_W; m++) {
        int gx = flip ? (GRID_W - 1 - m) : m;
        update_grain(gx, gy);
      }
    }
  } else {
    int xs = (s_fdx > 0) ? GRID_W - 1 : 0;
    int xstep = (s_fdx > 0) ? -1 : 1;
    for (int n = 0; n < GRID_W; n++) {
      int gx = xs + xstep * n;
      for (int m = 0; m < GRID_H; m++) {
        int gy = flip ? (GRID_H - 1 - m) : m;
        update_grain(gx, gy);
      }
    }
  }
}

// Delete a (custom) material: erase its grains, shift the table down, and fix up
// indices in the grid and the brush selection.
static void material_delete(int idx) {
  for (int i = 0; i < GRID_W * GRID_H; i++) {
    if (s_grid[i] == idx) s_grid[i] = MAT_EMPTY;
    else if (s_grid[i] > idx) s_grid[i]--;
  }
  for (int i = idx; i < s_material_count; i++) s_materials[i] = s_materials[i + 1];
  s_material_count--;
  if (s_brush_mat == idx) s_brush_mat = 1;
  else if (s_brush_mat > idx) s_brush_mat--;
}

// --- Menu helpers ------------------------------------------------------------
// Number of items in a node.
static int node_count(int node) {
  switch (node) {
    case NODE_ROOT:     return NUM_CATS;
    case NODE_MATERIAL: return s_material_count + 1;  // materials + "Add"
    case NODE_TOOL:     return NUM_TOOLS;
    case NODE_SIZE:     return BRUSH_MAX - BRUSH_MIN + 1;
    case NODE_GRAVITY:  return NUM_GMODES;
    case NODE_GRAVDIR:  return 4;
    case NODE_BOUNDS:   return 2;
    case NODE_CLEAR:    return 1;
    case NODE_MATITEM:  return (s_edit_mat <= NUM_DEFAULT_MATS) ? 1 : 3;  // Use [/ Edit / Delete]
    case NODE_MATEDIT:  return s_mat_is_new ? 3 : 2;  // Behavior / Visual [/ Add]
    case NODE_BEHAVIOR: return 3;
    case NODE_VISUAL:   return NUM_VISUALS;
    case NODE_DELCONFIRM: return 1;
  }
  return 1;
}

// Label for item `sel` of a node.
static void node_label(int node, int sel, char *buf, size_t n) {
  switch (node) {
    case NODE_ROOT:     snprintf(buf, n, "%s", CAT_NAMES[sel]); break;
    case NODE_MATERIAL:
      if (sel < s_material_count) snprintf(buf, n, "%s", s_materials[sel + 1].name);
      else snprintf(buf, n, "Custom");
      break;
    case NODE_TOOL:     snprintf(buf, n, "%s", TOOL_NAMES[sel]); break;
    case NODE_SIZE:     snprintf(buf, n, "%d", BRUSH_MIN + sel); break;
    case NODE_GRAVITY:  snprintf(buf, n, "%s", GMODE_NAMES[sel]); break;
    case NODE_GRAVDIR:  snprintf(buf, n, "%s", DIR_NAMES[sel]); break;
    case NODE_BOUNDS:   snprintf(buf, n, "%s", sel ? "On" : "Off"); break;
    case NODE_CLEAR:    snprintf(buf, n, "Confirm?"); break;
    case NODE_MATITEM:  snprintf(buf, n, "%s", sel == 0 ? "Use" : (sel == 1 ? "Edit" : "Delete")); break;
    case NODE_MATEDIT:  snprintf(buf, n, "%s", sel == 0 ? "Behavior" : (sel == 1 ? "Visual" : "Add")); break;
    case NODE_BEHAVIOR: snprintf(buf, n, "%s", BEH_NAMES[sel]); break;
    case NODE_VISUAL:   snprintf(buf, n, "%s", VIS_NAMES[sel]); break;
    case NODE_DELCONFIRM: snprintf(buf, n, "Delete?"); break;
    default:            buf[0] = '\0'; break;
  }
}

// Does selecting this item descend into a child node (vs commit a leaf)?
static bool node_item_descends(int node, int sel) {
  if (node == NODE_ROOT) return true;
  if (node == NODE_GRAVITY && sel == GMODE_STATIC) return true;
  if (node == NODE_MATERIAL) return true;        // pick material or Custom -> descend
  if (node == NODE_MATITEM) return sel != 0;     // Edit/Delete descend; Use commits
  if (node == NODE_MATEDIT) return sel < 2;      // Behavior/Visual descend; Add commits
  return false;
}

// Initial selection when entering a node: the committed value (so the editor
// opens on the current setting).
static int node_init_sel(int node) {
  switch (node) {
    case NODE_ROOT:     return s_root_sel;
    case NODE_MATERIAL: return 0;
    case NODE_TOOL:     return s_tool;
    case NODE_SIZE:     return s_brush_r - BRUSH_MIN;
    case NODE_GRAVITY:  return s_gravity_mode;
    case NODE_GRAVDIR:  return s_static_dir;
    case NODE_BOUNDS:   return s_bounds_on;
    case NODE_CLEAR:    return 0;
    case NODE_MATITEM:  return 0;
    case NODE_MATEDIT:  return 0;
    case NODE_BEHAVIOR: return s_materials[s_edit_mat].behavior;
    case NODE_VISUAL:   return s_materials[s_edit_mat].visual;
    case NODE_DELCONFIRM: return 0;
  }
  return 0;
}

// Draw a menu label centered in a full-width rect shifted by x_off (for slides).
static void menu_draw_label(GContext *ctx, const char *text, int x_off, int by, int bh, int w) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  // -4 compensates for the font's ascent padding so the glyphs sit optically
  // centered between the up/down arrows (the text box itself is centered).
  GRect tr = GRect(x_off, by + (bh - 26) / 2 - 4, w, 26);
  graphics_draw_text(ctx, text, font, tr, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// --- Rendering ---------------------------------------------------------------
// Clear to black, then write each filled cell's color block straight into the
// captured 8-bit framebuffer (1 byte per pixel on Emery).
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }

  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      uint8_t m = cell_at(gx, gy);
      if (m == MAT_EMPTY) {
        continue;
      }
      uint8_t c = cell_color(m, gx, gy);
      int x0 = gx * CELL_SIZE;
      for (int dy = 0; dy < CELL_SIZE; dy++) {
        GBitmapDataRowInfo info = gbitmap_get_data_row_info(fb, gy * CELL_SIZE + dy);
        memset(&info.data[x0], c, CELL_SIZE);
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);

  // Menu overlay: a full-width black bar centered vertically with white text
  // and affordance arrows.
  if (s_nav_depth > 0) {
    int node = s_nav_node[s_nav_depth - 1];
    int sel = s_nav_sel[s_nav_depth - 1];
    const int bh = 64;
    int by = (bounds.size.h - bh) / 2;
    int cx = bounds.size.w / 2;
    int cy = by + bh / 2;
    GRect bar = GRect(0, by, bounds.size.w, bh);

    int w = bounds.size.w;
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bar, 0, GCornerNone);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_line(ctx, GPoint(0, by), GPoint(w, by));
    graphics_draw_line(ctx, GPoint(0, by + bh - 1), GPoint(w, by + bh - 1));

    // Label(s): one centered label normally; during a transition, the old label
    // slides out and the new one slides in (ease-out).
    graphics_context_set_text_color(ctx, GColorWhite);
    if (!s_anim_active) {
      char buf[24];
      node_label(node, sel, buf, sizeof(buf));
      menu_draw_label(ctx, buf, 0, by, bh, w);
    } else {
      int q = MENU_ANIM_FRAMES - s_anim_t;            // frames remaining
      int rem_w = q * q * w / (MENU_ANIM_FRAMES * MENU_ANIM_FRAMES);  // (1-eased)*w
      int eased_w = w - rem_w;                         // eased*w
      menu_draw_label(ctx, s_anim_from, -s_anim_dir * eased_w, by, bh, w);
      menu_draw_label(ctx, s_anim_to,    s_anim_dir * rem_w,   by, bh, w);
    }

    // Arrows on top so they stay crisp over any sliding text. Left (Back) always
    // applies; right is an arrow if Select descends, else a checkmark (commit);
    // up/down only when there is more than one item to cycle.
    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_move_to(s_arrow_left, GPoint(13, cy));
    gpath_draw_filled(ctx, s_arrow_left);
    if (node_item_descends(node, sel)) {
      gpath_move_to(s_arrow_right, GPoint(w - 13, cy));
      gpath_draw_filled(ctx, s_arrow_right);
    } else {
      int bx = w - 18;
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_line(ctx, GPoint(bx, cy + 1), GPoint(bx + 4, cy + 5));
      graphics_draw_line(ctx, GPoint(bx + 4, cy + 5), GPoint(bx + 11, cy - 5));
      graphics_context_set_stroke_width(ctx, 1);
    }
    if (node_count(node) > 1) {
      gpath_move_to(s_arrow_up, GPoint(cx, by + 12));
      gpath_draw_filled(ctx, s_arrow_up);
      gpath_move_to(s_arrow_down, GPoint(cx, by + bh - 12));
      gpath_draw_filled(ctx, s_arrow_down);
    }
  }
}

// --- Frame loop --------------------------------------------------------------
static void timer_cb(void *data) {
#if defined(PBL_TOUCH)
  // Brush/Eraser paint each frame; Attract/Repel act via the sim's force field.
  if (s_touch_active && (s_tool == TOOL_BRUSH || s_tool == TOOL_ERASER)) {
    paint_brush(s_touch_gx, s_touch_gy);
  }
#endif
  update_gravity();
  sim_step();
  if (s_anim_active && ++s_anim_t >= MENU_ANIM_FRAMES) {
    s_anim_active = false;
  }
  s_frame++;
  layer_mark_dirty(s_canvas_layer);
  s_timer = app_timer_register(FRAME_MS, timer_cb, NULL);
}

// --- Input -------------------------------------------------------------------
#if defined(PBL_TOUCH)
// Track the finger and paint immediately on touchdown for instant response;
// the timer loop keeps painting each frame while the finger stays down/drags.
static void touch_handler(const TouchEvent *event, void *context) {
  (void)context;
  if (s_nav_depth > 0) {
    s_touch_active = false;  // menu is modal: ignore painting while it's open
    return;
  }
  switch (event->type) {
    case TouchEvent_Touchdown:
    case TouchEvent_PositionUpdate:
      s_touch_gx = clamp_i(event->x / CELL_SIZE, 0, GRID_W - 1);
      s_touch_gy = clamp_i(event->y / CELL_SIZE, 0, GRID_H - 1);
      if (event->type == TouchEvent_Touchdown) {
        s_prev_px = event->x;  // no drag delta on the first frame
        s_prev_py = event->y;
      }
      s_touch_px = event->x;
      s_touch_py = event->y;
      s_touch_active = true;
      // Paint immediately for instant feedback; force tools act in the sim.
      if (s_tool == TOOL_BRUSH || s_tool == TOOL_ERASER) {
        paint_brush(s_touch_gx, s_touch_gy);
      }
      break;
    case TouchEvent_Liftoff:
      s_touch_active = false;
      break;
  }
}
#endif

static void clear_grid(void) {
  memset(s_grid, MAT_EMPTY, sizeof(s_grid));
}

// Start a horizontal slide between two node labels. from_node < 0 means empty
// (opening). dir +1 descends, -1 ascends.
static void menu_slide(int from_node, int from_sel, int to_node, int to_sel, int dir) {
  if (from_node < 0) s_anim_from[0] = '\0';
  else node_label(from_node, from_sel, s_anim_from, sizeof(s_anim_from));
  if (to_node < 0) s_anim_to[0] = '\0';
  else node_label(to_node, to_sel, s_anim_to, sizeof(s_anim_to));
  s_anim_dir = dir;
  s_anim_t = 0;
  s_anim_active = true;
}

// Child node reached by descending into item `sel` of a node.
static int node_child(int node, int sel) {
  if (node == NODE_ROOT) {
    switch (sel) {
      case CAT_MATERIAL: return NODE_MATERIAL;
      case CAT_TOOL:     return NODE_TOOL;
      case CAT_SIZE:     return NODE_SIZE;
      case CAT_GRAVITY:  return NODE_GRAVITY;
      case CAT_BOUNDS:   return NODE_BOUNDS;
      case CAT_CLEAR:    return NODE_CLEAR;
    }
  }
  if (node == NODE_GRAVITY && sel == GMODE_STATIC) return NODE_GRAVDIR;
  if (node == NODE_MATERIAL) {
    if (sel < s_material_count) { s_edit_mat = sel + 1; return NODE_MATITEM; }
    s_edit_mat = material_draft();   // "Custom" -> edit an unsaved draft
    s_mat_is_new = true;
    return NODE_MATEDIT;
  }
  if (node == NODE_MATITEM) {
    if (sel == 1) { s_mat_is_new = false; return NODE_MATEDIT; }  // Edit existing
    return NODE_DELCONFIRM;                                       // Delete
  }
  if (node == NODE_MATEDIT) return (sel == 0) ? NODE_BEHAVIOR : NODE_VISUAL;
  return NODE_ROOT;
}

// Commit a leaf node's selected value (or run its action).
static void node_commit(int node, int sel) {
  switch (node) {
    case NODE_TOOL:     s_tool = sel; break;
    case NODE_SIZE:     s_brush_r = BRUSH_MIN + sel; break;
    case NODE_GRAVITY:  s_gravity_mode = sel; break;  // Static descends, not here
    case NODE_GRAVDIR:  s_gravity_mode = GMODE_STATIC; s_static_dir = sel; break;
    case NODE_BOUNDS:   s_bounds_on = sel; break;
    case NODE_CLEAR:    clear_grid(); break;
    case NODE_MATITEM:  if (sel == 0) s_brush_mat = s_edit_mat; break;  // Use
    case NODE_MATEDIT:  // "Add": save the draft, select it, close
      s_material_count = s_edit_mat;
      s_brush_mat = s_edit_mat;
      break;
    case NODE_BEHAVIOR: s_materials[s_edit_mat].behavior = sel;
                        material_autoname(s_edit_mat); break;
    case NODE_VISUAL:   s_materials[s_edit_mat].visual = sel;
                        material_autoname(s_edit_mat); break;
    case NODE_DELCONFIRM: material_delete(s_edit_mat); break;
    default: break;
  }
}

static void nav_cycle(int dir) {
  if (s_nav_depth == 0) return;
  s_anim_active = false;  // cycling is instant; don't fight a level slide
  int d = s_nav_depth - 1;
  int c = node_count(s_nav_node[d]);
  s_nav_sel[d] = (s_nav_sel[d] + dir + c) % c;
}

static void up_click(ClickRecognizerRef recognizer, void *context) { nav_cycle(-1); }
static void down_click(ClickRecognizerRef recognizer, void *context) { nav_cycle(+1); }

// Select: open the menu from the canvas, descend into a child node, or commit a
// leaf value (closing the menu).
static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_nav_depth == 0) {
#if defined(PBL_TOUCH)
    s_touch_active = false;  // stop any in-progress painting as we open
#endif
    s_nav_node[0] = NODE_ROOT;
    s_nav_sel[0] = s_root_sel;
    menu_slide(-1, 0, NODE_ROOT, s_root_sel, +1);
    s_nav_depth = 1;
    return;
  }

  int d = s_nav_depth - 1;
  int node = s_nav_node[d], sel = s_nav_sel[d];
  if (node_item_descends(node, sel)) {
    if (s_nav_depth < MENU_MAX_DEPTH) {
      int child = node_child(node, sel);
      int csel = node_init_sel(child);
      menu_slide(node, sel, child, csel, +1);
      s_nav_node[s_nav_depth] = child;
      s_nav_sel[s_nav_depth] = csel;
      s_nav_depth++;
    }
  } else {
    node_commit(node, sel);
    // Editing an axis (Behavior/Visual) applies live and stays in the editor;
    // everything else (Use / Add / Delete confirm) closes to the canvas.
    if (node == NODE_BEHAVIOR || node == NODE_VISUAL) {
      menu_slide(node, sel, s_nav_node[d - 1], s_nav_sel[d - 1], -1);
      s_nav_depth--;
    } else {
      s_root_sel = s_nav_sel[0];  // remember the category for next open
      s_nav_depth = 0;            // close (instant)
      s_anim_active = false;
    }
  }
}

// Back ascends one level; at the root it closes to the canvas; on the canvas it
// exits the app.
static void back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_nav_depth == 0) {
    window_stack_pop(true);  // exit the app, as Back normally would
    return;
  }
  if (s_nav_depth > 1) {
    int d = s_nav_depth - 1;
    menu_slide(s_nav_node[d], s_nav_sel[d], s_nav_node[d - 1], s_nav_sel[d - 1], -1);
    s_nav_depth--;
  } else {
    s_root_sel = s_nav_sel[0];
    s_nav_depth = 0;  // close to canvas (instant)
    s_anim_active = false;
  }
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// --- Window / app plumbing ---------------------------------------------------
static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_arrow_up    = gpath_create(&ARROW_UP_INFO);
  s_arrow_down  = gpath_create(&ARROW_DOWN_INFO);
  s_arrow_left  = gpath_create(&ARROW_LEFT_INFO);
  s_arrow_right = gpath_create(&ARROW_RIGHT_INFO);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  gpath_destroy(s_arrow_up);
  gpath_destroy(s_arrow_down);
  gpath_destroy(s_arrow_left);
  gpath_destroy(s_arrow_right);
}

// Accel data handler: keep the freshest sample from each batch.
static void accel_handler(AccelData *data, uint32_t num_samples) {
  if (num_samples == 0) {
    return;
  }
  AccelData last = data[num_samples - 1];
  s_accel_x = last.x;
  s_accel_y = last.y;
}

static void init(void) {
  build_visuals();

  // Continuously sample the accelerometer for low-latency tilt response. The
  // handler keeps the latest reading; update_gravity reads it each frame.
  accel_data_service_subscribe(2, accel_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_50HZ);

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

#if defined(PBL_TOUCH)
  // Touchscreen (Pebble Time 2): tap or drag to paint sand at the finger.
  if (touch_service_is_enabled()) {
    touch_service_subscribe(touch_handler, NULL);
  }
#endif

  s_timer = app_timer_register(FRAME_MS, timer_cb, NULL);
}

static void deinit(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
  accel_data_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
