// Pebble Sand Sim
// Milestone 1-3: a falling-sand cellular automaton.
//   - Grid of cells, each holding a material id.
//   - Touch (Emery): tap or drag to paint a brush of sand at your finger.
//   - Tilt gravity: the accelerometer steers which way "down" is.
//   - Update applies the classic forward / forward-diagonal slide rule.
//   - Rendered by writing the captured framebuffer directly (fast path).
//   - Select clears the screen.

#include <pebble.h>

// --- Grid geometry -----------------------------------------------------------
// Emery is 200x228. CELL_SIZE divides both evenly (50x57 cells).
#define CELL_SIZE 4
#define GRID_W (200 / CELL_SIZE)   // 50
#define GRID_H (228 / CELL_SIZE)   // 57

// --- Materials ---------------------------------------------------------------
#define MAT_EMPTY 0
#define MAT_SAND  1

// Paint brush radius, in cells (a filled disc ~5 cells across).
#define BRUSH_R 2

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

// Stable per-grain tint: hash the cell coordinate into the sand palette so a
// settled grain keeps the same shade frame to frame. The integer finalizer
// (xxHash-style) avalanches the bits so even the low bits we index with look
// like random noise rather than regular diagonal bands.
static inline uint8_t cell_color(uint8_t m, int gx, int gy) {
  if (m == MAT_SAND) {
    uint32_t h = (uint32_t)gx * 374761393u + (uint32_t)gy * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return s_sand[h & (NUM_SAND - 1)];
  }
  return GColorWhite.argb;
}

// --- Simulation --------------------------------------------------------------

// Stamp a filled disc of sand centered on a cell (the paint brush).
static void paint_brush(int cx, int cy) {
  for (int dy = -BRUSH_R; dy <= BRUSH_R; dy++) {
    for (int dx = -BRUSH_R; dx <= BRUSH_R; dx++) {
      // Rounded disc: the +1 fattens it from a diamond to a ~5-wide blob.
      if (dx * dx + dy * dy > BRUSH_R * BRUSH_R + 1) {
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

static void select_click(ClickRecognizerRef recognizer, void *context) {
  memset(s_grid, MAT_EMPTY, sizeof(s_grid));
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

// --- Window / app plumbing ---------------------------------------------------
static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
}

static void init_palette(void) {
  s_sand[0] = GColorFromRGB(0xAA, 0x88, 0x44).argb;
  s_sand[1] = GColorFromRGB(0xCC, 0xAA, 0x55).argb;
  s_sand[2] = GColorFromRGB(0xDD, 0xBB, 0x66).argb;
  s_sand[3] = GColorFromRGB(0x99, 0x77, 0x33).argb;
}

static void init(void) {
  init_palette();

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
