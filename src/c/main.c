// Pebble Sand Sim
// Milestone 1-3: a falling-sand cellular automaton.
//   - Grid of cells, each holding a material id.
//   - Touch (Emery): tap or drag to paint a brush of sand at your finger.
//   - Tilt gravity: the accelerometer steers which way "down" is.
//   - Update applies the classic forward / forward-diagonal slide rule.
//   - Rendered by writing the captured framebuffer directly (fast path).
//   - Select opens a two-level menu (Color / Size / Gravity / Clear); values
//     apply live, Back ascends the tree and finally exits.

#include <pebble.h>

// --- Grid geometry -----------------------------------------------------------
// Emery is 200x228. CELL_SIZE divides both evenly (50x57 cells).
#define CELL_SIZE 4
#define GRID_W (200 / CELL_SIZE)   // 50
#define GRID_H (228 / CELL_SIZE)   // 57

// --- Materials ---------------------------------------------------------------
#define MAT_EMPTY 0
#define MAT_SAND  1

// Paint brush radius range, in cells (selectable via the menu).
#define BRUSH_MIN 1
#define BRUSH_MAX 5

// Grain color palettes selectable via the menu. Ember is special-cased in
// cell_color to animate (glow) instead of using a static per-grain shade.
#define NUM_PALETTES 7
#define PALETTE_EMBER 6

// Tilt gravity speed: the sand feels the in-plane projection of gravity (what's
// left after the component pointing into the glass). Its magnitude sets how
// often a grain moves, in milli-g: at/above G_FULL grains fall every frame; at
// or below G_MIN (watch near-flat) they rest entirely.
#define G_FULL 1000
#define G_MIN  120

// --- Timing ------------------------------------------------------------------
#define FRAME_MS 33                // ~30 fps

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

// The world. Row-major: grid[gy * GRID_W + gx].
static uint8_t s_grid[GRID_W * GRID_H];

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

#if defined(PBL_TOUCH)
// Touch state: grid cell under the finger, active while a finger is down.
// Only compiled where the SDK has the TouchService (Pebble Time 2 / Emery).
static bool s_touch_active = false;
static int  s_touch_gx = 0;
static int  s_touch_gy = 0;
#endif

// A few sand shades (filled in at init from GColor, stored as raw argb bytes)
// so each grain can be tinted for a speckled, sandy texture at zero memory cost.
#define NUM_SAND 4
static uint8_t s_sand[NUM_SAND];

// --- User settings (changed live from the menu) ------------------------------
static int  s_palette = 0;            // index into the color palettes
static int  s_brush_r = 2;            // paint brush radius, BRUSH_MIN..BRUSH_MAX
static bool s_gravity_on = true;      // false freezes the sand (build mode)

// --- Menu --------------------------------------------------------------------
// Two-level menu over a black bar. Select descends (canvas -> categories ->
// values); Back ascends and finally exits. Values apply live as you cycle them.
typedef enum { MENU_CLOSED, MENU_L1, MENU_L2 } MenuState;
static MenuState s_menu = MENU_CLOSED;

typedef enum { CAT_COLOR, CAT_SIZE, CAT_GRAVITY, CAT_CLEAR, NUM_CATS } MenuCat;
static const char *CAT_NAMES[NUM_CATS] = { "Color", "Size", "Gravity", "Clear" };
static int s_cat = 0;                 // selected category at level 1

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

// Per-grain tint: hash the cell coordinate into the palette so a settled grain
// keeps the same shade frame to frame. The integer finalizer (xxHash-style)
// avalanches the bits so even the low bits we index with look like random noise
// rather than regular diagonal bands. Ember is the exception: each grain's index
// is animated along the dark->hot ramp on its own phase, so the bed glows.
static inline uint8_t cell_color(uint8_t m, int gx, int gy) {
  if (m == MAT_SAND) {
    uint32_t h = (uint32_t)gx * 374761393u + (uint32_t)gy * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    int idx;
    if (s_palette == PALETTE_EMBER) {
      // Triangle wave over time, offset per grain by its hash, so grains drift
      // up and down the 4-step ramp out of sync -> a shimmering ember glow.
      uint32_t a = (s_frame * 5u + (h & 0xFFu)) & 0xFFu;
      uint32_t tri = (a < 128u) ? (a * 2u) : ((255u - a) * 2u);  // 0..254
      idx = tri >> 6;                                            // 0..3
    } else {
      idx = h & (NUM_SAND - 1);
    }
    return s_sand[idx];
  }
  return GColorWhite.argb;
}

// --- Color palettes ----------------------------------------------------------
static const char *PALETTE_NAMES[NUM_PALETTES] = {
  "Sand", "Ice", "Lime", "Berry", "Cherry", "Amber", "Ember"
};

// Fill the active sand shades from the chosen palette (4 shades each).
static void set_palette(int idx) {
  switch (idx) {
    default:
    case 0:  // Sand
      s_sand[0] = GColorFromRGB(0xAA, 0x88, 0x44).argb;
      s_sand[1] = GColorFromRGB(0xCC, 0xAA, 0x55).argb;
      s_sand[2] = GColorFromRGB(0xDD, 0xBB, 0x66).argb;
      s_sand[3] = GColorFromRGB(0x99, 0x77, 0x33).argb;
      break;
    case 1:  // Ice
      s_sand[0] = GColorFromRGB(0x55, 0xAA, 0xFF).argb;
      s_sand[1] = GColorFromRGB(0x88, 0xCC, 0xFF).argb;
      s_sand[2] = GColorFromRGB(0xAA, 0xEE, 0xFF).argb;
      s_sand[3] = GColorFromRGB(0x33, 0x88, 0xDD).argb;
      break;
    case 2:  // Lime
      s_sand[0] = GColorFromRGB(0x66, 0xCC, 0x33).argb;
      s_sand[1] = GColorFromRGB(0x99, 0xEE, 0x44).argb;
      s_sand[2] = GColorFromRGB(0xCC, 0xFF, 0x66).argb;
      s_sand[3] = GColorFromRGB(0x44, 0x99, 0x22).argb;
      break;
    case 3:  // Berry
      s_sand[0] = GColorFromRGB(0xDD, 0x44, 0x99).argb;
      s_sand[1] = GColorFromRGB(0xFF, 0x77, 0xBB).argb;
      s_sand[2] = GColorFromRGB(0xCC, 0x55, 0xFF).argb;
      s_sand[3] = GColorFromRGB(0x99, 0x33, 0x88).argb;
      break;
    case 4:  // Cherry
      s_sand[0] = GColorFromRGB(0xAA, 0x00, 0x00).argb;
      s_sand[1] = GColorFromRGB(0xFF, 0x00, 0x00).argb;
      s_sand[2] = GColorFromRGB(0xFF, 0x55, 0x55).argb;
      s_sand[3] = GColorFromRGB(0xAA, 0x00, 0x55).argb;
      break;
    case 5:  // Amber
      s_sand[0] = GColorFromRGB(0xFF, 0xAA, 0x00).argb;
      s_sand[1] = GColorFromRGB(0xFF, 0x55, 0x00).argb;
      s_sand[2] = GColorFromRGB(0xFF, 0xAA, 0x55).argb;
      s_sand[3] = GColorFromRGB(0xAA, 0x55, 0x00).argb;
      break;
    case 6:  // Ember (dark -> hot ramp; cell_color animates the index)
      s_sand[0] = GColorFromRGB(0x55, 0x00, 0x00).argb;
      s_sand[1] = GColorFromRGB(0xAA, 0x00, 0x00).argb;
      s_sand[2] = GColorFromRGB(0xFF, 0x55, 0x00).argb;
      s_sand[3] = GColorFromRGB(0xFF, 0xAA, 0x00).argb;
      break;
  }
}

// --- Simulation --------------------------------------------------------------

// Stamp a filled disc of sand centered on a cell (the paint brush).
static void paint_brush(int cx, int cy) {
  int r = s_brush_r;
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
      if (cell_at(gx, gy) == MAT_EMPTY) {
        set_cell(gx, gy, MAT_SAND);
      }
    }
  }
}

// Read the accelerometer and derive this frame's gravity. Mapping (from the
// fluid-sim, tested): screen-right = a.x, down = -a.y. The dominant axis is the
// "forward" fall direction; the smaller component leans the fall toward the tilt
// (lean probability = minor/major, so the time-averaged flow tracks the exact
// angle). The in-plane magnitude sets the per-frame move probability, so a
// shallow tilt creeps, a steep tilt pours, and a flat watch rests.
static void update_gravity(void) {
  if (!s_gravity_on) {
    s_move_p256 = 0;  // gravity off: freeze the sand (build mode)
    return;
  }
  AccelData a;
  if (accel_service_peek(&a) < 0) {
    return;  // sensor busy this frame; keep last gravity
  }
  int gx = a.x;    // gravity component toward screen-right
  int gy = -a.y;   // gravity component toward screen-down
  int ax = gx < 0 ? -gx : gx;
  int ay = gy < 0 ? -gy : gy;
  int sx = gx > 0 ? 1 : (gx < 0 ? -1 : 0);
  int sy = gy > 0 ? 1 : (gy < 0 ? -1 : 0);

  int major, minor;
  if (ax >= ay) {
    // Horizontal dominant: fall sideways, lean up/down toward the tilt.
    s_fdx = sx; s_fdy = 0;
    s_pdx = 0;  s_pdy = 1;
    s_tside = sy;
    major = ax; minor = ay;
  } else {
    // Vertical dominant: fall down/up, lean left/right toward the tilt.
    s_fdx = 0;  s_fdy = sy;
    s_pdx = 1;  s_pdy = 0;
    s_tside = sx;
    major = ay; minor = ax;
  }
  s_lean_num = minor;
  s_lean_den = major > 0 ? major : 1;

  // In-plane magnitude ~= |(gx,gy)| via alpha-max-plus-beta-min (sqrt-free),
  // mapped to a 0..256 per-frame move probability.
  int m = major + (minor * 7) / 16;
  if (m <= G_MIN) {
    s_move_p256 = 0;
  } else if (m >= G_FULL) {
    s_move_p256 = 256;
  } else {
    s_move_p256 = (m - G_MIN) * 256 / (G_FULL - G_MIN);
  }
}

// Move one grain. First gate on gravity magnitude (it may rest this frame), then
// pick a lateral lean toward the tilt and try the forward cell at that lean,
// falling back through the other forward-slice cells so piles still spread. All
// candidates lie one step forward along the dominant axis, which keeps the
// sim_step scan order correct (no grain moves twice per frame).
static void update_grain(int gx, int gy) {
  if (cell_at(gx, gy) != MAT_SAND) {
    return;
  }

  // Gravity-magnitude gate: maybe rest this frame.
  if (s_move_p256 < 256 && (int)(xrand() & 255) >= s_move_p256) {
    return;
  }

  // Lateral lean (perpendicular offset): toward the tilt with prob minor/major,
  // otherwise straight forward.
  int lean = 0;
  if (s_tside != 0 && (int)(xrand() % (uint32_t)s_lean_den) < s_lean_num) {
    lean = s_tside;
  }

  // Attempt order over the three forward-slice cells. When going straight, try a
  // random side then the other so heaps stay symmetric; when leaning, try the
  // lean, then straight, then the far side.
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
    int nx = gx + s_fdx + s_pdx * p;
    int ny = gy + s_fdy + s_pdy * p;
    if (in_bounds(nx, ny) && cell_at(nx, ny) == MAT_EMPTY) {
      set_cell(gx, gy, MAT_EMPTY);
      set_cell(nx, ny, MAT_SAND);
      return;
    }
  }
}

// One CA step. Every cell a grain can move to lies one step "forward" along the
// dominant gravity axis, so we visit cells from the forward edge backward to
// avoid moving a grain twice. Vertical-dominant gravity loops rows outer (inner
// columns alternate for symmetry); horizontal-dominant swaps the nesting.
static void sim_step(void) {
  if (s_move_p256 == 0) {
    return;  // flat: everything rests
  }
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

// --- Menu helpers ------------------------------------------------------------
// Format the label shown for a given menu level + category (values read live).
static void menu_label_for(char *buf, size_t n, MenuState menu, int cat) {
  if (menu == MENU_L1) {
    snprintf(buf, n, "%s", CAT_NAMES[cat]);
    return;
  }
  switch (cat) {
    case CAT_COLOR:   snprintf(buf, n, "%s", PALETTE_NAMES[s_palette]); break;
    case CAT_SIZE:    snprintf(buf, n, "%d", s_brush_r); break;
    case CAT_GRAVITY: snprintf(buf, n, "%s", s_gravity_on ? "On" : "Off"); break;
    case CAT_CLEAR:   snprintf(buf, n, "Confirm?"); break;
    default:          buf[0] = '\0'; break;
  }
}

// Draw a menu label centered in a full-width rect shifted by x_off (for slides).
static void menu_draw_label(GContext *ctx, const char *text, int x_off, int by, int bh, int w) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GRect tr = GRect(x_off, by + (bh - 26) / 2, w, 26);
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
  if (s_menu != MENU_CLOSED) {
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

    // Label(s): one centered label normally; during a level transition, the old
    // label slides out and the new one slides in (ease-out).
    graphics_context_set_text_color(ctx, GColorWhite);
    if (!s_anim_active) {
      char buf[24];
      menu_label_for(buf, sizeof(buf), s_menu, s_cat);
      menu_draw_label(ctx, buf, 0, by, bh, w);
    } else {
      int q = MENU_ANIM_FRAMES - s_anim_t;            // frames remaining
      int rem_w = q * q * w / (MENU_ANIM_FRAMES * MENU_ANIM_FRAMES);  // (1-eased)*w
      int eased_w = w - rem_w;                         // eased*w
      menu_draw_label(ctx, s_anim_from, -s_anim_dir * eased_w, by, bh, w);
      menu_draw_label(ctx, s_anim_to,    s_anim_dir * rem_w,   by, bh, w);
    }

    // Arrows on top so they stay crisp over any sliding text. Left (Back) and
    // right (Select) always apply; up/down only when there are options to cycle.
    graphics_context_set_fill_color(ctx, GColorWhite);
    gpath_move_to(s_arrow_left, GPoint(13, cy));
    gpath_draw_filled(ctx, s_arrow_left);
    if (s_menu == MENU_L1) {
      // Right arrow: Select descends into the values.
      gpath_move_to(s_arrow_right, GPoint(w - 13, cy));
      gpath_draw_filled(ctx, s_arrow_right);
    } else {
      // Checkmark: at L2 Select confirms/closes (no deeper level).
      int bx = w - 18;
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_line(ctx, GPoint(bx, cy + 1), GPoint(bx + 4, cy + 5));
      graphics_draw_line(ctx, GPoint(bx + 4, cy + 5), GPoint(bx + 11, cy - 5));
      graphics_context_set_stroke_width(ctx, 1);
    }
    bool cyclable = (s_menu == MENU_L1) || (s_cat != CAT_CLEAR);
    if (cyclable) {
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
  // Keep depositing while the finger is held down or dragging.
  if (s_touch_active) {
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
  if (s_menu != MENU_CLOSED) {
    s_touch_active = false;  // menu is modal: ignore painting while it's open
    return;
  }
  switch (event->type) {
    case TouchEvent_Touchdown:
    case TouchEvent_PositionUpdate:
      s_touch_gx = clamp_i(event->x / CELL_SIZE, 0, GRID_W - 1);
      s_touch_gy = clamp_i(event->y / CELL_SIZE, 0, GRID_H - 1);
      s_touch_active = true;
      paint_brush(s_touch_gx, s_touch_gy);
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

// Start a horizontal slide between two menu labels. A MENU_CLOSED endpoint maps
// to an empty label so opening slides the first label in (and nothing slides on
// a plain open). dir +1 descends, -1 ascends.
static void menu_slide(MenuState from_m, int from_c, MenuState to_m, int to_c, int dir) {
  menu_label_for(s_anim_from, sizeof(s_anim_from), from_m, from_c);
  menu_label_for(s_anim_to, sizeof(s_anim_to), to_m, to_c);
  if (from_m == MENU_CLOSED) s_anim_from[0] = '\0';
  if (to_m == MENU_CLOSED) s_anim_to[0] = '\0';
  s_anim_dir = dir;
  s_anim_t = 0;
  s_anim_active = true;
}

// Change the focused category's value (dir is +1/-1) and apply it live.
static void menu_adjust(int dir) {
  switch (s_cat) {
    case CAT_COLOR:
      s_palette = (s_palette + dir + NUM_PALETTES) % NUM_PALETTES;
      set_palette(s_palette);
      break;
    case CAT_SIZE:
      s_brush_r += dir;
      if (s_brush_r < BRUSH_MIN) s_brush_r = BRUSH_MAX;
      if (s_brush_r > BRUSH_MAX) s_brush_r = BRUSH_MIN;
      break;
    case CAT_GRAVITY:
      s_gravity_on = !s_gravity_on;  // two states: either direction toggles
      break;
    case CAT_CLEAR:
      break;  // no value; Select performs the clear
  }
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  s_anim_active = false;  // cycling is instant; don't fight a level slide
  if (s_menu == MENU_L1) {
    s_cat = (s_cat - 1 + NUM_CATS) % NUM_CATS;
  } else if (s_menu == MENU_L2) {
    menu_adjust(+1);
  }
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  s_anim_active = false;
  if (s_menu == MENU_L1) {
    s_cat = (s_cat + 1) % NUM_CATS;
  } else if (s_menu == MENU_L2) {
    menu_adjust(-1);
  }
}

// Select descends: canvas -> categories -> values. At a value, Select closes the
// menu (values already applied live); for Clear it performs the wipe first.
static void select_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_menu) {
    case MENU_CLOSED:
#if defined(PBL_TOUCH)
      s_touch_active = false;  // stop any in-progress painting as we open
#endif
      menu_slide(MENU_CLOSED, s_cat, MENU_L1, s_cat, +1);
      s_menu = MENU_L1;
      break;
    case MENU_L1:
      menu_slide(MENU_L1, s_cat, MENU_L2, s_cat, +1);
      s_menu = MENU_L2;
      break;
    case MENU_L2:
      if (s_cat == CAT_CLEAR) {
        clear_grid();
      }
      s_menu = MENU_CLOSED;  // closing is instant
      s_anim_active = false;
      break;
  }
}

// Back ascends the menu tree; at the top it falls through to exit the app.
static void back_click(ClickRecognizerRef recognizer, void *context) {
  switch (s_menu) {
    case MENU_L2:
      menu_slide(MENU_L2, s_cat, MENU_L1, s_cat, -1);
      s_menu = MENU_L1;
      break;
    case MENU_L1:
      s_menu = MENU_CLOSED;  // closing is instant
      s_anim_active = false;
      break;
    case MENU_CLOSED:
      window_stack_pop(true);  // exit the app, as Back normally would
      break;
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

static void init(void) {
  set_palette(s_palette);

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
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
