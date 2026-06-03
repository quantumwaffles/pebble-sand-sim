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

// Tilt gravity: ignore in-plane tilt weaker than this (milli-g) so a near-flat
// watch (gravity mostly into the screen) keeps its last direction instead of
// jittering between axes.
#define GRAV_DEADZONE 200

// --- Timing ------------------------------------------------------------------
#define FRAME_MS 33                // ~30 fps

static Window *s_window;
static Layer *s_canvas_layer;
static AppTimer *s_timer;

// The world. Row-major: grid[gy * GRID_W + gx].
static uint8_t s_grid[GRID_W * GRID_H];

static uint32_t s_frame;

// Current gravity as a cardinal unit vector in screen space (x right, y down).
// Defaults to straight down; updated each frame from the accelerometer.
static int s_gdx = 0;
static int s_gdy = 1;

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

// The 8 neighbor directions, ordered clockwise around the compass so that two
// entries one apart are 45 deg apart. Gravity points at one of these; a grain's
// two slide fallbacks are its +/-45 deg ring-neighbors.
static const int8_t RING[8][2] = {
  { 0,  1},  // 0: down
  { 1,  1},  // 1: down-right
  { 1,  0},  // 2: right
  { 1, -1},  // 3: up-right
  { 0, -1},  // 4: up
  {-1, -1},  // 5: up-left
  {-1,  0},  // 6: left
  {-1,  1},  // 7: down-left
};

static int ring_index(int dx, int dy) {
  for (int i = 0; i < 8; i++) {
    if (RING[i][0] == dx && RING[i][1] == dy) {
      return i;
    }
  }
  return 0;
}

// Pick this frame's gravity direction from the accelerometer with probabilistic
// blending, so the time-averaged flow tracks the exact tilt angle (analog feel)
// while each frame still uses one discrete ring direction (keeps the sim_step
// scan order correct). Mapping (from the fluid-sim, tested): screen-right = a.x,
// down = -a.y. Any in-plane tilt sits between the dominant cardinal direction
// and the quadrant diagonal; we pick the diagonal with probability minor/major
// (how diagonal the tilt is) and the cardinal otherwise. Within the deadzone
// (watch near-flat) we keep the previous direction.
static void update_gravity(void) {
  AccelData a;
  if (accel_service_peek(&a) < 0) {
    return;  // sensor busy this frame; keep last direction
  }
  int gx = a.x;    // gravity component toward screen-right
  int gy = -a.y;   // gravity component toward screen-down
  int ax = gx < 0 ? -gx : gx;
  int ay = gy < 0 ? -gy : gy;
  int major = ax > ay ? ax : ay;
  int minor = ax > ay ? ay : ax;
  if (major < GRAV_DEADZONE) {
    return;  // too flat to tell which way is down; keep last direction
  }

  int sx = gx > 0 ? 1 : (gx < 0 ? -1 : 0);
  int sy = gy > 0 ? 1 : (gy < 0 ? -1 : 0);

  if ((int)(xrand() % (uint32_t)major) < minor) {
    // Diagonal: chosen more often the more diagonal the tilt is.
    s_gdx = sx;
    s_gdy = sy;
  } else if (ax >= ay) {
    s_gdx = sx;  // dominant cardinal is horizontal
    s_gdy = 0;
  } else {
    s_gdx = 0;   // dominant cardinal is vertical
    s_gdy = sy;
  }
}

// Move one grain: try straight "forward" (along gravity), then the two slide
// fallbacks (forward's +/-45 deg ring-neighbors) in a randomized order.
static void update_grain(int gx, int gy,
                         int fdx, int fdy,
                         int adx, int ady,
                         int bdx, int bdy) {
  if (cell_at(gx, gy) != MAT_SAND) {
    return;
  }

  // 1. Straight along gravity.
  if (in_bounds(gx + fdx, gy + fdy) && cell_at(gx + fdx, gy + fdy) == MAT_EMPTY) {
    set_cell(gx, gy, MAT_EMPTY);
    set_cell(gx + fdx, gy + fdy, MAT_SAND);
    return;
  }

  // 2/3. The two slide fallbacks, in a random order.
  int ox[2] = { adx, bdx };
  int oy[2] = { ady, bdy };
  int first = xrand() & 1;
  for (int t = 0; t < 2; t++) {
    int k = first ? t : (1 - t);
    int nx = gx + ox[k];
    int ny = gy + oy[k];
    if (in_bounds(nx, ny) && cell_at(nx, ny) == MAT_EMPTY) {
      set_cell(gx, gy, MAT_EMPTY);
      set_cell(nx, ny, MAT_SAND);
      return;
    }
  }
}

// One CA step under the current (8-way) gravity direction. Every destination a
// grain can move to lies one step "forward" along gravity, so we must visit
// cells from the gravity-forward edge backward to avoid moving a grain twice.
// For vertical/diagonal gravity the outer loop runs over rows (forward edge
// first) and the inner over x; pure-horizontal gravity swaps the nesting so its
// forward axis (x) is the outer loop instead.
static void sim_step(void) {
  int k = ring_index(s_gdx, s_gdy);
  int fdx = RING[k][0], fdy = RING[k][1];
  int adx = RING[(k + 1) & 7][0], ady = RING[(k + 1) & 7][1];  // +45 deg
  int bdx = RING[(k + 7) & 7][0], bdy = RING[(k + 7) & 7][1];  // -45 deg
  bool flip = (s_frame & 1);

  if (fdy != 0) {
    // Vertical or diagonal gravity: outer over rows, inner over columns.
    int ys = (fdy > 0) ? GRID_H - 1 : 0;
    int ystep = (fdy > 0) ? -1 : 1;
    // Columns: forward-first if gravity has an x component, else alternate.
    int xs, xstep;
    if (fdx > 0)      { xs = GRID_W - 1; xstep = -1; }
    else if (fdx < 0) { xs = 0;          xstep = 1;  }
    else if (flip)    { xs = GRID_W - 1; xstep = -1; }
    else              { xs = 0;          xstep = 1;  }
    for (int n = 0; n < GRID_H; n++) {
      int gy = ys + ystep * n;
      for (int m = 0; m < GRID_W; m++) {
        int gx = xs + xstep * m;
        update_grain(gx, gy, fdx, fdy, adx, ady, bdx, bdy);
      }
    }
  } else {
    // Pure horizontal gravity: outer over columns (forward edge first), inner y.
    int xs = (fdx > 0) ? GRID_W - 1 : 0;
    int xstep = (fdx > 0) ? -1 : 1;
    int ys = flip ? GRID_H - 1 : 0;
    int ystep = flip ? -1 : 1;
    for (int n = 0; n < GRID_W; n++) {
      int gx = xs + xstep * n;
      for (int m = 0; m < GRID_H; m++) {
        int gy = ys + ystep * m;
        update_grain(gx, gy, fdx, fdy, adx, ady, bdx, bdy);
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
