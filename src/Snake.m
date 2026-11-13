// LCD Snake — a self-playing snake screensaver in the style of an old
// monochrome phone display.

#import <ScreenSaver/ScreenSaver.h>

#define MAXCELLS 4096
#define CELL 4            // LCD pixels per snake cell (3 lit + 1 gap)

static const double kStepsPerSecond = 13.0;

// 3x5 digits, one row per 3 bits (top row first).
static const uint16_t kDigits[10] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF
};

static int bfs(int cols, int rows, const uint8_t *blocked, int from, int to, int *prev) {
    static int queue[MAXCELLS];
    static int dist[MAXCELLS];
    int n = cols * rows;
    for (int i = 0; i < n; i++) { dist[i] = -1; if (prev) prev[i] = -1; }
    int qh = 0, qt = 0;
    queue[qt++] = from;
    dist[from] = 0;
    while (qh < qt) {
        int c = queue[qh++];
        if (c == to) return dist[c];
        int x = c % cols, y = c / cols;
        int nb[4] = { x > 0 ? c - 1 : -1, x < cols - 1 ? c + 1 : -1,
                      y > 0 ? c - cols : -1, y < rows - 1 ? c + cols : -1 };
        for (int i = 0; i < 4; i++) {
            int m = nb[i];
            if (m < 0 || dist[m] >= 0 || (blocked[m] && m != to)) continue;
            dist[m] = dist[c] + 1;
            if (prev) prev[m] = c;
            queue[qt++] = m;
        }
    }
    return -1;
}

static int floodArea(int cols, int rows, const uint8_t *blocked, int from) {
    static int queue[MAXCELLS];
    static uint8_t seen[MAXCELLS];
    memset(seen, 0, (size_t)(cols * rows));
    int qh = 0, qt = 0;
    queue[qt++] = from;
    seen[from] = 1;
    while (qh < qt) {
        int c = queue[qh++];
        int x = c % cols, y = c / cols;
        int nb[4] = { x > 0 ? c - 1 : -1, x < cols - 1 ? c + 1 : -1,
                      y > 0 ? c - cols : -1, y < rows - 1 ? c + cols : -1 };
        for (int i = 0; i < 4; i++) {
            int m = nb[i];
            if (m < 0 || seen[m] || blocked[m]) continue;
            seen[m] = 1;
            queue[qt++] = m;
        }
    }
    return qt;
}

@interface SnakeSaverView : ScreenSaverView
@end

@implementation SnakeSaverView {
    // layout, in LCD pixels unless noted
    int _px;                 // points per LCD pixel
    int _lw, _lh;            // LCD size
    CGFloat _ox, _oy;        // point offset that centres the LCD
    int _fx, _fy;            // first playfield pixel
    int _cols, _rows;
    NSSize _laidOut;
    NSImage *_bg;

    uint8_t *_target;        // what should be lit
    float   *_level;         // what is lit (lags, for LCD ghosting)

    int _snake[MAXCELLS];    // cell indices, head first
    int _len, _food, _score, _hi, _stall;
    int _state;              // 0 playing, 1 dead, 2 board cleared
    double _stateT, _acc;
    CFTimeInterval _last;
    BOOL _stopped;
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    self = [super initWithFrame:frame isPreview:isPreview];
    if (!self) return nil;
    self.animationTimeInterval = 1.0 / 30.0;
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self selector:@selector(saverWillStop:)
               name:@"com.apple.screensaver.willstop" object:nil];
    return self;
}

- (void)dealloc {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    free(_target);
    free(_level);
}

- (void)saverWillStop:(NSNotification *)note {
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)hasConfigureSheet { return NO; }
- (NSWindow *)configureSheet { return nil; }

- (void)startAnimation {
    [super startAnimation];
    _stopped = NO;
    _last = 0;
}

// MARK: - Layout

- (void)layoutIfNeeded {
    NSSize size = self.bounds.size;
    if (NSEqualSizes(size, _laidOut) && _target) return;
    _laidOut = size;

    _px = MAX(2, (int)floor(size.height / 64.0));
    _lw = MAX(40, (int)floor(size.width / _px));
    _lh = MAX(30, (int)floor(size.height / _px));
    _ox = floor((size.width - _lw * _px) / 2.0);
    _oy = floor((size.height - _lh * _px) / 2.0);

    const int margin = 3;
    _fy = margin + 10;                                   // score row + rule above
    _rows = MAX(4, MIN(48, (_lh - 1 - margin - _fy) / CELL));
    _cols = MAX(4, MIN(64, (_lw - 2 * margin - 3) / CELL));
    _fx = (_lw - (_cols * CELL + 3)) / 2 + 2;

    free(_target);
    free(_level);
    _target = calloc((size_t)_lw * _lh, 1);
    _level = calloc((size_t)_lw * _lh, sizeof(float));
    _bg = nil;
    [self newGame];
}

- (NSImage *)backgroundImage {
    if (_bg) return _bg;
    NSSize size = self.bounds.size;
    NSImage *img = [[NSImage alloc] initWithSize:size];
    [img lockFocusFlipped:YES];

    NSColor *lit  = [NSColor colorWithSRGBRed:0.76 green:0.83 blue:0.66 alpha:1];
    NSColor *edge = [NSColor colorWithSRGBRed:0.62 green:0.70 blue:0.54 alpha:1];
    [edge setFill];
    NSRectFill(NSMakeRect(0, 0, size.width, size.height));
    NSGradient *glow = [[NSGradient alloc] initWithStartingColor:lit endingColor:edge];
    [glow drawInRect:NSMakeRect(0, 0, size.width, size.height) relativeCenterPosition:NSZeroPoint];

    // Faint unlit pixels, as on a real passive LCD.
    CGFloat gap = MAX(1.0, round(_px * 0.09));
    [[NSColor colorWithSRGBRed:0.12 green:0.17 blue:0.10 alpha:0.035] setFill];
    for (int y = 0; y < _lh; y++)
        for (int x = 0; x < _lw; x++)
            NSRectFillUsingOperation(NSMakeRect(_ox + x * _px, _oy + y * _px, _px - gap, _px - gap),
                                     NSCompositingOperationSourceOver);
    [img unlockFocus];
    _bg = img;
    return img;
}

// MARK: - Game

- (void)newGame {
    _len = 3;
    int row = _rows / 2, col = _cols / 2;
    for (int i = 0; i < _len; i++) _snake[i] = row * _cols + (col - i);
    _score = 0;
    _stall = 0;
    _state = 0;
    _stateT = 0;
    _acc = 0;
    [self placeFood];
}

- (void)placeFood {
    int n = _cols * _rows;
    uint8_t used[MAXCELLS];
    memset(used, 0, (size_t)n);
    for (int i = 0; i < _len; i++) used[_snake[i]] = 1;
    int freeCount = n - _len;
    if (freeCount <= 0) { _food = -1; return; }
    int pick = (int)arc4random_uniform((uint32_t)freeCount);
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        if (pick-- == 0) { _food = i; return; }
    }
}

// After the snake's cells become `cells` (head first, `len` long), can the
// head still reach its own tail? If so the snake cannot have trapped itself.
- (BOOL)tailReachable:(const int *)cells length:(int)len {
    if (len >= _cols * _rows) return YES;
    uint8_t blocked[MAXCELLS];
    memset(blocked, 0, (size_t)(_cols * _rows));
    for (int i = 0; i < len - 1; i++) blocked[cells[i]] = 1;
    return bfs(_cols, _rows, blocked, cells[0], cells[len - 1], NULL) > 0;
}

- (int)neighbours:(int)c into:(int *)nb {
    int x = c % _cols, y = c / _cols, n = 0;
    if (x < _cols - 1) nb[n++] = c + 1;
    if (x > 0)         nb[n++] = c - 1;
    if (y < _rows - 1) nb[n++] = c + _cols;
    if (y > 0)         nb[n++] = c - _cols;
    return n;
}

- (int)chooseMove {
    static int prev[MAXCELLS], path[MAXCELLS], comb[MAXCELLS * 2];
    int n = _cols * _rows, head = _snake[0];

    uint8_t blocked[MAXCELLS];
    memset(blocked, 0, (size_t)n);
    for (int i = 0; i < _len - 1; i++) blocked[_snake[i]] = 1;   // the tail moves away

    // 1. Shortest route to the food, if taking it leaves an escape route.
    int greedy = -1;
    if (_food >= 0 && bfs(_cols, _rows, blocked, head, _food, prev) > 0) {
        int k = 0;
        for (int c = _food; c != head && c >= 0; c = prev[c]) path[k++] = c;   // food ... first step
        greedy = path[k - 1];
        int m = 0;
        for (int i = 0; i < k; i++) comb[m++] = path[i];
        for (int i = 0; i < _len; i++) comb[m++] = _snake[i];
        if ([self tailReachable:comb length:_len + 1]) return greedy;
    }

    // Stuck circling for too long: gamble on the direct route.
    if (greedy >= 0 && _stall > n * 2) { _stall = n; return greedy; }

    // 2. Stall safely: stay able to reach the tail, keep away from the food.
    int nb[4], cnt = [self neighbours:head into:nb];
    int best = -1, bestScore = -1;
    int fx = _food >= 0 ? _food % _cols : 0, fy = _food >= 0 ? _food / _cols : 0;
    for (int i = 0; i < cnt; i++) {
        int c = nb[i];
        if (blocked[c]) continue;
        BOOL eats = (c == _food);
        comb[0] = c;
        int newLen = eats ? _len + 1 : _len;
        for (int j = 1; j < newLen; j++) comb[j] = _snake[j - 1];
        if (![self tailReachable:comb length:newLen]) continue;
        int score = abs(c % _cols - fx) + abs(c / _cols - fy);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (best >= 0) return best;

    // 3. No safe move: head for the biggest open area and hope.
    int bestArea = -1;
    for (int i = 0; i < cnt; i++) {
        int c = nb[i];
        if (blocked[c]) continue;
        int area = floodArea(_cols, _rows, blocked, c);
        if (area > bestArea) { bestArea = area; best = c; }
    }
    return best;   // -1 means boxed in
}

- (void)tick {
    int move = [self chooseMove];
    if (move < 0) {
        _state = 1;
        _stateT = 0;
        return;
    }
    if (move == _food) {
        memmove(_snake + 1, _snake, sizeof(int) * (size_t)_len);
        _snake[0] = move;
        _len++;
        _score = MIN(9999, _score + 9);
        if (_score > _hi) _hi = _score;
        _stall = 0;
        [self placeFood];
        if (_food < 0) { _state = 2; _stateT = 0; }
    } else {
        memmove(_snake + 1, _snake, sizeof(int) * (size_t)(_len - 1));
        _snake[0] = move;
        _stall++;
    }
}

- (void)step:(double)dt {
    [self layoutIfNeeded];
    if (_state == 0) {
        _acc += dt * kStepsPerSecond;
        while (_acc >= 1.0 && _state == 0) { _acc -= 1.0; [self tick]; }
    } else {
        _stateT += dt;
        if (_stateT > 2.2) [self newGame];
    }
    [self renderTarget];

    // LCD response: pixels darken quickly and clear slowly, leaving a trail.
    float rise = (float)MIN(1.0, dt * 28.0), fall = (float)MIN(1.0, dt * 9.0);
    int count = _lw * _lh;
    for (int i = 0; i < count; i++) {
        float t = _target[i] ? 1.0f : 0.0f;
        _level[i] += (t - _level[i]) * (t > _level[i] ? rise : fall);
    }
}

// MARK: - Rendering

static inline void plot(uint8_t *buf, int w, int h, int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < w && y < h) buf[y * w + x] = v;
}

- (void)drawNumber:(int)value digits:(int)digits atX:(int)x y:(int)y {
    for (int d = digits - 1; d >= 0; d--) {
        uint16_t glyph = kDigits[value % 10];
        value /= 10;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (glyph & (1 << (14 - (r * 3 + c))))
                    plot(_target, _lw, _lh, x + d * 4 + c, y + r, 1);
    }
}

- (void)renderTarget {
    memset(_target, 0, (size_t)_lw * _lh);
    int bx0 = _fx - 2, bx1 = _fx + _cols * CELL;
    int by0 = _fy - 2, by1 = _fy + _rows * CELL;

    // score, best score, rule
    [self drawNumber:_score digits:4 atX:bx0 y:by0 - 8];
    [self drawNumber:_hi digits:4 atX:bx1 - 14 y:by0 - 8];

    // playfield border
    for (int x = bx0; x <= bx1; x++) { plot(_target, _lw, _lh, x, by0, 1); plot(_target, _lw, _lh, x, by1, 1); }
    for (int y = by0; y <= by1; y++) { plot(_target, _lw, _lh, bx0, y, 1); plot(_target, _lw, _lh, bx1, y, 1); }

    // food: a small diamond
    if (_food >= 0) {
        int x = _fx + (_food % _cols) * CELL, y = _fy + (_food / _cols) * CELL;
        plot(_target, _lw, _lh, x + 1, y, 1);
        plot(_target, _lw, _lh, x, y + 1, 1);
        plot(_target, _lw, _lh, x + 2, y + 1, 1);
        plot(_target, _lw, _lh, x + 1, y + 2, 1);
    }

    // snake (blinks after a crash or a cleared board)
    BOOL visible = _state == 0 || fmod(_stateT, 0.36) > 0.18;
    if (!visible) return;
    for (int i = 0; i < _len; i++) {
        int c = _snake[i];
        int x = _fx + (c % _cols) * CELL, y = _fy + (c / _cols) * CELL;
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
                plot(_target, _lw, _lh, x + dx, y + dy, 1);
        if (i + 1 < _len) {                       // bridge the gap to the next segment
            int d = _snake[i + 1] - c;
            for (int k = 0; k < 3; k++) {
                if (d == 1)           plot(_target, _lw, _lh, x + 3, y + k, 1);
                else if (d == -1)     plot(_target, _lw, _lh, x - 1, y + k, 1);
                else if (d == _cols)  plot(_target, _lw, _lh, x + k, y + 3, 1);
                else if (d == -_cols) plot(_target, _lw, _lh, x + k, y - 1, 1);
            }
        }
    }
    int h = _snake[0];                            // eye
    plot(_target, _lw, _lh, _fx + (h % _cols) * CELL + 1, _fy + (h / _cols) * CELL + 1, 0);
}

- (void)animateOneFrame {
    if (_stopped) return;
    CFTimeInterval now = CACurrentMediaTime();
    double dt = _last > 0 ? now - _last : 1.0 / 30.0;
    _last = now;
    [self step:MAX(0.0, MIN(dt, 0.1))];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect {
    [self layoutIfNeeded];
    [[self backgroundImage] drawInRect:self.bounds fromRect:NSZeroRect
                             operation:NSCompositingOperationCopy fraction:1.0
                        respectFlipped:YES hints:nil];

    CGFloat gap = MAX(1.0, round(_px * 0.09));
    CGFloat side = _px - gap;
    CGFloat lift = MAX(1.0, round(_px * 0.12));
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;

    for (int pass = 0; pass < 2; pass++) {        // 0: soft shadow on the backing, 1: the pixels
        for (int y = 0; y < _lh; y++) {
            for (int x = 0; x < _lw; x++) {
                float v = _level[y * _lw + x];
                if (v < 0.03f) continue;
                if (pass == 0) {
                    CGContextSetRGBFillColor(ctx, 0.12, 0.17, 0.10, 0.16 * v);
                    CGContextFillRect(ctx, CGRectMake(_ox + x * _px + lift, _oy + y * _px + lift, side, side));
                } else {
                    CGContextSetRGBFillColor(ctx, 0.11, 0.16, 0.09, 0.94 * v);
                    CGContextFillRect(ctx, CGRectMake(_ox + x * _px, _oy + y * _px, side, side));
                }
            }
        }
    }
}

// MARK: - Offline test hooks (used by the build's snapshot tool)

- (void)debugAdvance:(double)seconds {
    for (double t = 0; t < seconds; t += 1.0 / 30.0) [self step:1.0 / 30.0];
}

- (NSImage *)debugSnapshot {
    NSBitmapImageRep *rep = [self bitmapImageRepForCachingDisplayInRect:self.bounds];
    [self cacheDisplayInRect:self.bounds toBitmapImageRep:rep];
    NSImage *img = [[NSImage alloc] initWithSize:self.bounds.size];
    [img addRepresentation:rep];
    return img;
}

@end
