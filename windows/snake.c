// LCD Snake for Windows — port of src/Snake.m.
// Same game logic and autopilot; drawn into a memory bitmap and copied to
// the screen, so it needs nothing beyond plain Windows.

#define SAVER_NAME "LCD Snake"
#define SAVER_FPS 30
#include "saver.h"

#define MAXCELLS 4096
#define CELL 4                     // LCD pixels per snake cell (3 lit + 1 gap)
#define STEPS_PER_SECOND 13.0

// 3x5 digits, one row per 3 bits (top row first).
static const unsigned short kDigits[10] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
};

// layout (LCD pixels unless noted)
static int g_w, g_h;               // window size in device pixels
static int g_px, g_lw, g_lh, g_ox, g_oy, g_fx, g_fy, g_cols, g_rows;

static unsigned char *g_target;    // what should be lit
static float         *g_level;     // what is lit (lags, for LCD ghosting)
static unsigned int  *g_bg;        // pre-rendered backdrop, 0x00RRGGBB
static unsigned int  *g_frame;     // DIB section pixels, top-down
static HDC     g_windowDC, g_memDC;
static HBITMAP g_dib, g_oldBitmap;

static int g_snake[MAXCELLS];      // cell indices, head first
static int g_len, g_food, g_score, g_hi, g_stall, g_state;
static double g_stateT, g_acc;

// ---------------------------------------------------------------- search

static int bfs(const unsigned char *blocked, int from, int to, int *prev) {
    static int queue[MAXCELLS], dist[MAXCELLS];
    int n = g_cols * g_rows, qh = 0, qt = 0, i;
    for (i = 0; i < n; i++) { dist[i] = -1; if (prev) prev[i] = -1; }
    queue[qt++] = from;
    dist[from] = 0;
    while (qh < qt) {
        int c = queue[qh++], x = c % g_cols, y = c / g_cols, nb[4];
        if (c == to) return dist[c];
        nb[0] = x > 0 ? c - 1 : -1;
        nb[1] = x < g_cols - 1 ? c + 1 : -1;
        nb[2] = y > 0 ? c - g_cols : -1;
        nb[3] = y < g_rows - 1 ? c + g_cols : -1;
        for (i = 0; i < 4; i++) {
            int m = nb[i];
            if (m < 0 || dist[m] >= 0 || (blocked[m] && m != to)) continue;
            dist[m] = dist[c] + 1;
            if (prev) prev[m] = c;
            queue[qt++] = m;
        }
    }
    return -1;
}

static int flood_area(const unsigned char *blocked, int from) {
    static int queue[MAXCELLS];
    static unsigned char seen[MAXCELLS];
    int qh = 0, qt = 0, i;
    memset(seen, 0, (size_t)(g_cols * g_rows));
    queue[qt++] = from;
    seen[from] = 1;
    while (qh < qt) {
        int c = queue[qh++], x = c % g_cols, y = c / g_cols, nb[4];
        nb[0] = x > 0 ? c - 1 : -1;
        nb[1] = x < g_cols - 1 ? c + 1 : -1;
        nb[2] = y > 0 ? c - g_cols : -1;
        nb[3] = y < g_rows - 1 ? c + g_cols : -1;
        for (i = 0; i < 4; i++) {
            int m = nb[i];
            if (m < 0 || seen[m] || blocked[m]) continue;
            seen[m] = 1;
            queue[qt++] = m;
        }
    }
    return qt;
}

// ---------------------------------------------------------------- game

static void place_food(void) {
    static unsigned char used[MAXCELLS];
    int n = g_cols * g_rows, freeCount = n - g_len, pick, i;
    memset(used, 0, (size_t)n);
    for (i = 0; i < g_len; i++) used[g_snake[i]] = 1;
    if (freeCount <= 0) { g_food = -1; return; }
    pick = irand(freeCount);
    for (i = 0; i < n; i++) {
        if (used[i]) continue;
        if (pick-- == 0) { g_food = i; return; }
    }
    g_food = -1;
}

static void new_game(void) {
    int row = g_rows / 2, col = g_cols / 2, i;
    g_len = 3;
    for (i = 0; i < g_len; i++) g_snake[i] = row * g_cols + (col - i);
    g_score = 0;
    g_stall = 0;
    g_state = 0;
    g_stateT = 0;
    g_acc = 0;
    place_food();
}

// With the snake occupying `cells` (head first), can the head still reach
// its own tail? If so it has not trapped itself.
static int tail_reachable(const int *cells, int len) {
    static unsigned char blocked[MAXCELLS];
    int i;
    if (len >= g_cols * g_rows) return 1;
    memset(blocked, 0, (size_t)(g_cols * g_rows));
    for (i = 0; i < len - 1; i++) blocked[cells[i]] = 1;
    return bfs(blocked, cells[0], cells[len - 1], NULL) > 0;
}

static int neighbours(int c, int *nb) {
    int x = c % g_cols, y = c / g_cols, n = 0;
    if (x < g_cols - 1) nb[n++] = c + 1;
    if (x > 0)          nb[n++] = c - 1;
    if (y < g_rows - 1) nb[n++] = c + g_cols;
    if (y > 0)          nb[n++] = c - g_cols;
    return n;
}

static int choose_move(void) {
    static int prev[MAXCELLS], path[MAXCELLS], comb[MAXCELLS * 2];
    static unsigned char blocked[MAXCELLS];
    int n = g_cols * g_rows, head = g_snake[0], greedy = -1, i, j;
    int nb[4], cnt, best = -1, bestScore = -1, bestArea = -1, fx, fy;

    memset(blocked, 0, (size_t)n);
    for (i = 0; i < g_len - 1; i++) blocked[g_snake[i]] = 1;     // the tail moves away

    // 1. Shortest route to the food, if taking it leaves an escape route.
    if (g_food >= 0 && bfs(blocked, head, g_food, prev) > 0) {
        int k = 0, m = 0, c;
        for (c = g_food; c != head && c >= 0; c = prev[c]) path[k++] = c;
        greedy = path[k - 1];
        for (i = 0; i < k; i++) comb[m++] = path[i];
        for (i = 0; i < g_len; i++) comb[m++] = g_snake[i];
        if (tail_reachable(comb, g_len + 1)) return greedy;
    }

    // Stuck circling for too long: gamble on the direct route.
    if (greedy >= 0 && g_stall > n * 2) { g_stall = n; return greedy; }

    // 2. Stall safely: stay able to reach the tail, keep away from the food.
    cnt = neighbours(head, nb);
    fx = g_food >= 0 ? g_food % g_cols : 0;
    fy = g_food >= 0 ? g_food / g_cols : 0;
    for (i = 0; i < cnt; i++) {
        int c = nb[i], newLen, score;
        if (blocked[c]) continue;
        newLen = (c == g_food) ? g_len + 1 : g_len;
        comb[0] = c;
        for (j = 1; j < newLen; j++) comb[j] = g_snake[j - 1];
        if (!tail_reachable(comb, newLen)) continue;
        score = abs(c % g_cols - fx) + abs(c / g_cols - fy);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (best >= 0) return best;

    // 3. No safe move: head for the biggest open area and hope.
    for (i = 0; i < cnt; i++) {
        int c = nb[i], area;
        if (blocked[c]) continue;
        area = flood_area(blocked, c);
        if (area > bestArea) { bestArea = area; best = c; }
    }
    return best;                                                 // -1 means boxed in
}

static void tick(void) {
    int move = choose_move();
    if (move < 0) { g_state = 1; g_stateT = 0; return; }
    if (move == g_food) {
        memmove(g_snake + 1, g_snake, sizeof(int) * (size_t)g_len);
        g_snake[0] = move;
        g_len++;
        g_score += 9;
        if (g_score > 9999) g_score = 9999;
        if (g_score > g_hi) g_hi = g_score;
        g_stall = 0;
        place_food();
        if (g_food < 0) { g_state = 2; g_stateT = 0; }
    } else {
        memmove(g_snake + 1, g_snake, sizeof(int) * (size_t)(g_len - 1));
        g_snake[0] = move;
        g_stall++;
    }
}

// ---------------------------------------------------------------- LCD

static void plot(int x, int y, unsigned char v) {
    if (x >= 0 && y >= 0 && x < g_lw && y < g_lh) g_target[y * g_lw + x] = v;
}

static void draw_number(int value, int digits, int x, int y) {
    int d, r, c;
    for (d = digits - 1; d >= 0; d--) {
        unsigned short glyph = kDigits[value % 10];
        value /= 10;
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (glyph & (1 << (14 - (r * 3 + c)))) plot(x + d * 4 + c, y + r, 1);
    }
}

static void render_target(void) {
    int bx0 = g_fx - 2, bx1 = g_fx + g_cols * CELL;
    int by0 = g_fy - 2, by1 = g_fy + g_rows * CELL;
    int x, y, i, k, dx, dy, h;

    memset(g_target, 0, (size_t)g_lw * (size_t)g_lh);
    draw_number(g_score, 4, bx0, by0 - 8);
    draw_number(g_hi, 4, bx1 - 14, by0 - 8);

    for (x = bx0; x <= bx1; x++) { plot(x, by0, 1); plot(x, by1, 1); }
    for (y = by0; y <= by1; y++) { plot(bx0, y, 1); plot(bx1, y, 1); }

    if (g_food >= 0) {                                   // food: a small diamond
        x = g_fx + (g_food % g_cols) * CELL;
        y = g_fy + (g_food / g_cols) * CELL;
        plot(x + 1, y, 1); plot(x, y + 1, 1); plot(x + 2, y + 1, 1); plot(x + 1, y + 2, 1);
    }

    // snake (blinks after a crash or a cleared board)
    if (g_state != 0 && fmod(g_stateT, 0.36) <= 0.18) return;
    for (i = 0; i < g_len; i++) {
        int c = g_snake[i];
        x = g_fx + (c % g_cols) * CELL;
        y = g_fy + (c / g_cols) * CELL;
        for (dy = 0; dy < 3; dy++)
            for (dx = 0; dx < 3; dx++) plot(x + dx, y + dy, 1);
        if (i + 1 < g_len) {                             // bridge the gap to the next segment
            int d = g_snake[i + 1] - c;
            for (k = 0; k < 3; k++) {
                if (d == 1)            plot(x + 3, y + k, 1);
                else if (d == -1)      plot(x - 1, y + k, 1);
                else if (d == g_cols)  plot(x + k, y + 3, 1);
                else if (d == -g_cols) plot(x + k, y - 1, 1);
            }
        }
    }
    h = g_snake[0];                                      // eye
    plot(g_fx + (h % g_cols) * CELL + 1, g_fy + (h / g_cols) * CELL + 1, 0);
}

static unsigned int mix(unsigned int under, float r, float g, float b, float a) {
    float ur = (float)((under >> 16) & 255), ug = (float)((under >> 8) & 255), ub = (float)(under & 255);
    unsigned int R = (unsigned int)(ur + (r * 255.0f - ur) * a + 0.5f);
    unsigned int G = (unsigned int)(ug + (g * 255.0f - ug) * a + 0.5f);
    unsigned int B = (unsigned int)(ub + (b * 255.0f - ub) * a + 0.5f);
    return (R << 16) | (G << 8) | B;
}

static void blend_rect(unsigned int *buf, int x, int y, int w, int h,
                       float r, float g, float b, float a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    int xx, yy;
    for (yy = y0; yy < y1; yy++) {
        unsigned int *row = buf + (size_t)yy * (size_t)g_w;
        for (xx = x0; xx < x1; xx++) row[xx] = mix(row[xx], r, g, b, a);
    }
}

static int pixel_gap(void)  { int v = (int)(g_px * 0.09 + 0.5); return v < 1 ? 1 : v; }
static int pixel_lift(void) { int v = (int)(g_px * 0.12 + 0.5); return v < 1 ? 1 : v; }

static void build_background(void) {
    double cx = g_w / 2.0, cy = g_h / 2.0, maxd = sqrt(cx * cx + cy * cy);
    int x, y, side = g_px - pixel_gap();
    for (y = 0; y < g_h; y++) {
        for (x = 0; x < g_w; x++) {
            double t = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxd;
            unsigned int R = (unsigned int)((0.76 + (0.62 - 0.76) * t) * 255.0 + 0.5);
            unsigned int G = (unsigned int)((0.83 + (0.70 - 0.83) * t) * 255.0 + 0.5);
            unsigned int B = (unsigned int)((0.66 + (0.54 - 0.66) * t) * 255.0 + 0.5);
            g_bg[(size_t)y * (size_t)g_w + x] = (R << 16) | (G << 8) | B;
        }
    }
    // Faint unlit pixels, as on a real passive LCD.
    for (y = 0; y < g_lh; y++)
        for (x = 0; x < g_lw; x++)
            blend_rect(g_bg, g_ox + x * g_px, g_oy + y * g_px, side, side, 0.12f, 0.17f, 0.10f, 0.035f);
}

static void draw_frame(void) {
    int side = g_px - pixel_gap(), lift = pixel_lift(), pass, x, y;
    memcpy(g_frame, g_bg, (size_t)g_w * (size_t)g_h * sizeof(unsigned int));
    for (pass = 0; pass < 2; pass++) {              // 0: soft shadow on the backing, 1: the pixels
        for (y = 0; y < g_lh; y++) {
            for (x = 0; x < g_lw; x++) {
                float v = g_level[y * g_lw + x];
                if (v < 0.03f) continue;
                if (pass == 0)
                    blend_rect(g_frame, g_ox + x * g_px + lift, g_oy + y * g_px + lift, side, side,
                               0.12f, 0.17f, 0.10f, 0.16f * v);
                else
                    blend_rect(g_frame, g_ox + x * g_px, g_oy + y * g_px, side, side,
                               0.11f, 0.16f, 0.09f, 0.94f * v);
            }
        }
    }
}

// ---------------------------------------------------------------- saver hooks

static void saver_start(HWND hwnd, int w, int h) {
    BITMAPINFO bi;
    void *bits = NULL;
    const int margin = 3;

    g_w = w;
    g_h = h;
    g_px = h / 64;
    if (g_px < 2) g_px = 2;
    g_lw = w / g_px; if (g_lw < 40) g_lw = 40;
    g_lh = h / g_px; if (g_lh < 30) g_lh = 30;
    g_ox = (w - g_lw * g_px) / 2;
    g_oy = (h - g_lh * g_px) / 2;

    g_fy = margin + 10;                                  // score row + rule above
    g_rows = (g_lh - 1 - margin - g_fy) / CELL;
    if (g_rows < 4) g_rows = 4;
    if (g_rows > 48) g_rows = 48;
    g_cols = (g_lw - 2 * margin - 3) / CELL;
    if (g_cols < 4) g_cols = 4;
    if (g_cols > 64) g_cols = 64;
    g_fx = (g_lw - (g_cols * CELL + 3)) / 2 + 2;

    g_target = (unsigned char *)calloc((size_t)g_lw * (size_t)g_lh, 1);
    g_level = (float *)calloc((size_t)g_lw * (size_t)g_lh, sizeof(float));
    g_bg = (unsigned int *)malloc((size_t)w * (size_t)h * sizeof(unsigned int));

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_windowDC = GetDC(hwnd);
    g_memDC = CreateCompatibleDC(g_windowDC);
    g_dib = CreateDIBSection(g_windowDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    g_frame = (unsigned int *)bits;
    if (!g_target || !g_level || !g_bg || !g_dib || !g_frame) ExitProcess(1);
    g_oldBitmap = (HBITMAP)SelectObject(g_memDC, g_dib);

    build_background();
    new_game();
}

static void saver_frame(HWND hwnd, double dt) {
    float rise = (float)(dt * 28.0), fall = (float)(dt * 9.0);
    int count = g_lw * g_lh, i;
    (void)hwnd;

    if (g_state == 0) {
        g_acc += dt * STEPS_PER_SECOND;
        while (g_acc >= 1.0 && g_state == 0) { g_acc -= 1.0; tick(); }
    } else {
        g_stateT += dt;
        if (g_stateT > 2.2) new_game();
    }
    render_target();

    // LCD response: pixels darken quickly and clear slowly, leaving a trail.
    if (rise > 1) rise = 1;
    if (fall > 1) fall = 1;
    for (i = 0; i < count; i++) {
        float t = g_target[i] ? 1.0f : 0.0f;
        g_level[i] += (t - g_level[i]) * (t > g_level[i] ? rise : fall);
    }

    draw_frame();
    GdiFlush();
    BitBlt(g_windowDC, 0, 0, g_w, g_h, g_memDC, 0, 0, SRCCOPY);
}

static void saver_stop(HWND hwnd) {
    if (g_memDC) { SelectObject(g_memDC, g_oldBitmap); DeleteDC(g_memDC); }
    if (g_dib) DeleteObject(g_dib);
    if (g_windowDC) ReleaseDC(hwnd, g_windowDC);
    free(g_target);
    free(g_level);
    free(g_bg);
    g_memDC = NULL; g_dib = NULL; g_windowDC = NULL;
    g_target = NULL; g_level = NULL; g_bg = NULL; g_frame = NULL;
}

static int saver_capture(HWND hwnd, const char *path) {
    int stride = (g_w * 3 + 3) & ~3, x, y, ok;
    unsigned char *buf = (unsigned char *)calloc((size_t)stride * (size_t)g_h, 1);
    (void)hwnd;
    if (!buf) return 0;
    for (y = 0; y < g_h; y++) {
        const unsigned int *src = g_frame + (size_t)(g_h - 1 - y) * (size_t)g_w;
        unsigned char *dst = buf + (size_t)y * (size_t)stride;
        for (x = 0; x < g_w; x++) {
            dst[x * 3 + 0] = (unsigned char)(src[x] & 255);
            dst[x * 3 + 1] = (unsigned char)((src[x] >> 8) & 255);
            dst[x * 3 + 2] = (unsigned char)((src[x] >> 16) & 255);
        }
    }
    ok = write_bmp24(path, g_w, g_h, buf);
    free(buf);
    return ok;
}
