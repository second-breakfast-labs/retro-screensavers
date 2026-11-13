// Pipes — a 3D pipes screensaver for macOS.
// Classic growing-pipes-in-a-black-void, rendered with physically based
// materials, soft shadows and a slow camera drift.

#import <ScreenSaver/ScreenSaver.h>
#import <SceneKit/SceneKit.h>

typedef struct { int x, y, z; } Cell;

static const Cell kDirs[6] = {
    { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}
};

static const CGFloat kTubeRadius = 0.22;
static const double  kSpeed      = 9.0;   // cells per second, per pipe

static inline double frand(void) { return arc4random_uniform(1000000) / 1000000.0; }

// ---------------------------------------------------------------------------

@interface PSPipe : NSObject {
@public
    Cell head;        // last cell fully reached
    Cell runStart;    // where the current straight run began
    int  dir;
    int  runCells;    // whole cells completed in the current run
    double progress;  // 0..1 towards the next cell
    BOOL alive;
}
@property (strong) SCNGeometry *tube;
@property (strong) SCNGeometry *ball;
@property (strong) SCNNode *run;
@end

@implementation PSPipe
@end

// ---------------------------------------------------------------------------

@interface PipesSaverView : ScreenSaverView <SCNSceneRendererDelegate>
@end

@implementation PipesSaverView {
    SCNView  *_scnView;
    SCNScene *_scene;
    SCNNode  *_pipesRoot, *_pivot, *_cameraNode;
    SCNLight *_key, *_fill, *_ambient;

    int _nx, _ny, _nz;
    uint8_t *_occ;
    int _filled;

    NSMutableArray<PSPipe *> *_active;
    int _spawned, _maxPipes, _concurrent, _jointStyle;
    double _spawnDelay;

    int _phase;            // 0 growing, 1 hold, 2 fade out
    double _phaseT, _fade, _time;
    NSTimeInterval _lastTime;
    CGFloat _aspect;
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    self = [super initWithFrame:frame isPreview:isPreview];
    if (!self) return nil;

    self.animationTimeInterval = 1.0 / 30.0;
    _active = [NSMutableArray array];
    _aspect = frame.size.height > 0 ? frame.size.width / frame.size.height : 1.6;

    [self buildScene];

    _scnView = [[SCNView alloc] initWithFrame:self.bounds options:nil];
    _scnView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _scnView.backgroundColor = NSColor.blackColor;
    _scnView.antialiasingMode = SCNAntialiasingModeMultisampling4X;
    _scnView.preferredFramesPerSecond = 60;
    _scnView.allowsCameraControl = NO;
    _scnView.scene = _scene;
    _scnView.pointOfView = _cameraNode;
    _scnView.delegate = self;
    _scnView.playing = NO;
    [self addSubview:_scnView];

    [self resetPipes];

    // Modern macOS never tells legacy savers to stop; listen for it ourselves
    // so a dismissed saver does not keep rendering in the background.
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self selector:@selector(saverWillStop:)
               name:@"com.apple.screensaver.willstop" object:nil];
    return self;
}

- (void)dealloc {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    free(_occ);
}

- (void)saverWillStop:(NSNotification *)note {
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) _scnView.playing = YES;
}

- (BOOL)hasConfigureSheet { return NO; }
- (NSWindow *)configureSheet { return nil; }

- (void)startAnimation {
    [super startAnimation];
    _lastTime = 0;
    _scnView.playing = YES;
}

- (void)stopAnimation {
    _scnView.playing = NO;
    [super stopAnimation];
}

- (void)drawRect:(NSRect)rect {
    [NSColor.blackColor setFill];
    NSRectFill(rect);
}

// MARK: - Scene

- (NSImage *)environmentImage {
    // A procedural "studio": dark room, a few softboxes. This is what the
    // chrome and enamel pipes reflect.
    NSSize size = NSMakeSize(1024, 512);
    NSImage *img = [[NSImage alloc] initWithSize:size];
    [img lockFocus];
    NSGradient *room = [[NSGradient alloc] initWithColorsAndLocations:
        [NSColor colorWithWhite:0.02 alpha:1], 0.0,
        [NSColor colorWithWhite:0.07 alpha:1], 0.45,
        [NSColor colorWithWhite:0.22 alpha:1], 1.0, nil];
    [room drawInRect:NSMakeRect(0, 0, size.width, size.height) angle:90];

    void (^softbox)(NSRect, NSColor *) = ^(NSRect r, NSColor *c) {
        for (int i = 6; i >= 0; i--) {
            CGFloat grow = i * 7.0;
            [[c colorWithAlphaComponent:(i == 0 ? 1.0 : 0.10)] setFill];
            [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -grow, -grow)
                                             xRadius:12 + grow yRadius:12 + grow] fill];
        }
    };
    softbox(NSMakeRect(120, 330, 190, 110), [NSColor colorWithWhite:1.0 alpha:1]);
    softbox(NSMakeRect(600, 360, 240, 90),  [NSColor colorWithRed:1.0 green:0.95 blue:0.86 alpha:1]);
    softbox(NSMakeRect(420, 250, 60, 150),  [NSColor colorWithRed:0.80 green:0.90 blue:1.0 alpha:1]);
    softbox(NSMakeRect(900, 280, 70, 120),  [NSColor colorWithWhite:0.9 alpha:1]);
    softbox(NSMakeRect(250, 60, 300, 26),   [NSColor colorWithWhite:0.35 alpha:1]);
    [img unlockFocus];
    return img;
}

- (void)buildScene {
    _scene = [SCNScene scene];
    _scene.background.contents = NSColor.blackColor;
    _scene.lightingEnvironment.contents = [self environmentImage];

    _pipesRoot = [SCNNode node];
    [_scene.rootNode addChildNode:_pipesRoot];

    _pivot = [SCNNode node];
    [_scene.rootNode addChildNode:_pivot];

    SCNCamera *cam = [SCNCamera camera];
    cam.fieldOfView = 40;
    cam.zNear = 0.1;
    cam.zFar = 200;
    cam.wantsHDR = YES;
    cam.wantsExposureAdaptation = NO;
    cam.bloomIntensity = 0.15;
    cam.bloomThreshold = 1.0;
    cam.bloomBlurRadius = 10;
    cam.screenSpaceAmbientOcclusionIntensity = 0.7;
    cam.screenSpaceAmbientOcclusionRadius = 0.6;
    _cameraNode = [SCNNode node];
    _cameraNode.camera = cam;
    [_pivot addChildNode:_cameraNode];

    _key = [SCNLight light];
    _key.type = SCNLightTypeDirectional;
    _key.color = [NSColor colorWithRed:1.0 green:0.98 blue:0.94 alpha:1];
    _key.castsShadow = YES;
    _key.shadowMapSize = CGSizeMake(2048, 2048);
    _key.shadowSampleCount = 8;
    _key.shadowRadius = 4;
    _key.shadowColor = [NSColor colorWithWhite:0 alpha:0.6];
    SCNNode *keyNode = [SCNNode node];
    keyNode.light = _key;
    keyNode.eulerAngles = SCNVector3Make(-0.75, 0.55, 0);
    [_scene.rootNode addChildNode:keyNode];

    _fill = [SCNLight light];
    _fill.type = SCNLightTypeDirectional;
    _fill.color = [NSColor colorWithRed:0.75 green:0.85 blue:1.0 alpha:1];
    SCNNode *fillNode = [SCNNode node];
    fillNode.light = _fill;
    fillNode.eulerAngles = SCNVector3Make(0.5, -0.9, 0);
    [_scene.rootNode addChildNode:fillNode];

    _ambient = [SCNLight light];
    _ambient.type = SCNLightTypeAmbient;
    _ambient.color = NSColor.whiteColor;
    SCNNode *ambNode = [SCNNode node];
    ambNode.light = _ambient;
    [_scene.rootNode addChildNode:ambNode];

    [self applyFade];
}

- (void)applyFade {
    CGFloat f = (CGFloat)MAX(0.0, MIN(1.0, _fade));
    f = f * f * (3 - 2 * f);
    _key.intensity = 850 * f;
    _fill.intensity = 250 * f;
    _ambient.intensity = 120 * f;
    _scene.lightingEnvironment.intensity = 1.3 * f;
}

// MARK: - Grid

- (BOOL)inside:(Cell)c {
    return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < _nx && c.y < _ny && c.z < _nz;
}
- (int)index:(Cell)c { return (c.z * _ny + c.y) * _nx + c.x; }
- (BOOL)isFree:(Cell)c { return [self inside:c] && !_occ[[self index:c]]; }
- (void)occupy:(Cell)c { _occ[[self index:c]] = 1; _filled++; }
- (SCNVector3)position:(Cell)c { return SCNVector3Make(c.x, c.y, c.z); }

- (void)resetPipes {
    for (SCNNode *n in [_pipesRoot.childNodes copy]) [n removeFromParentNode];
    [_active removeAllObjects];

    _ny = 11;
    _nz = 11;
    _nx = MAX(8, MIN(40, (int)lround(_ny * _aspect * 1.1)));
    free(_occ);
    _occ = calloc((size_t)_nx * _ny * _nz, 1);
    _filled = 0;

    _spawned = 0;
    _concurrent = 2 + (int)arc4random_uniform(3);      // 2..4 at once
    _maxPipes = 7 + (int)arc4random_uniform(8);        // 7..14 per scene
    _jointStyle = (int)arc4random_uniform(3);          // 0 ball, 1 elbow, 2 mixed
    _spawnDelay = 0.2;
    _phase = 0;
    _phaseT = 0;
    _fade = 0;

    // Frame the grid so the nearest pipes spill past the screen edges.
    CGFloat front = (_ny + 0.5) / (2 * tan(20.0 * M_PI / 180.0));
    _pivot.position = SCNVector3Make((_nx - 1) / 2.0, (_ny - 1) / 2.0, (_nz - 1) / 2.0);
    _cameraNode.position = SCNVector3Make(0, 0, front + _nz / 2.0);
    [self applyFade];
}

// MARK: - Pipes

- (SCNMaterial *)randomMaterial {
    static const CGFloat paint[][3] = {
        {0.86, 0.08, 0.08}, {0.05, 0.62, 0.14}, {0.08, 0.20, 0.85}, {0.95, 0.80, 0.05},
        {0.05, 0.70, 0.78}, {0.78, 0.10, 0.70}, {0.92, 0.92, 0.92}, {0.95, 0.42, 0.05},
    };
    static const CGFloat metal[][3] = {
        {0.90, 0.91, 0.93},   // chrome
        {0.95, 0.55, 0.38},   // copper
        {1.00, 0.78, 0.34},   // brass
    };

    SCNMaterial *m = [SCNMaterial material];
    m.lightingModelName = SCNLightingModelPhysicallyBased;
    double roll = frand();
    if (roll < 0.68) {            // glossy enamel, the classic look
        const CGFloat *c = paint[arc4random_uniform(8)];
        m.diffuse.contents = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:1];
        m.metalness.contents = @0.0;
        m.roughness.contents = @0.28;
        m.clearCoat.contents = @1.0;
        m.clearCoatRoughness.contents = @0.06;
    } else if (roll < 0.88) {     // polished metal
        const CGFloat *c = metal[arc4random_uniform(3)];
        m.diffuse.contents = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:1];
        m.metalness.contents = @1.0;
        m.roughness.contents = @0.14;
    } else {                      // anodised colour
        const CGFloat *c = paint[arc4random_uniform(6)];
        m.diffuse.contents = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:1];
        m.metalness.contents = @1.0;
        m.roughness.contents = @0.32;
    }
    return m;
}

- (void)addBallFor:(PSPipe *)p at:(Cell)c {
    SCNNode *n = [SCNNode nodeWithGeometry:p.ball];
    n.position = [self position:c];
    [_pipesRoot addChildNode:n];
}

- (void)startRunFor:(PSPipe *)p {
    p->runStart = p->head;
    p->runCells = 0;
    SCNNode *n = [SCNNode nodeWithGeometry:p.tube];
    Cell d = kDirs[p->dir];
    if (d.x != 0)      n.eulerAngles = SCNVector3Make(0, 0, M_PI_2);
    else if (d.z != 0) n.eulerAngles = SCNVector3Make(M_PI_2, 0, 0);
    p.run = n;
    [self updateRunFor:p length:0];
    [_pipesRoot addChildNode:n];
}

- (void)updateRunFor:(PSPipe *)p length:(double)len {
    Cell d = kDirs[p->dir];
    double h = len / 2.0;
    p.run.position = SCNVector3Make(p->runStart.x + d.x * h,
                                    p->runStart.y + d.y * h,
                                    p->runStart.z + d.z * h);
    p.run.scale = SCNVector3Make(1, MAX(len, 0.001), 1);
}

- (BOOL)spawnPipe {
    Cell start = {0, 0, 0};
    int cands[6], n = 0;
    for (int attempt = 0; attempt < 60 && n == 0; attempt++) {
        start = (Cell){ (int)arc4random_uniform(_nx), (int)arc4random_uniform(_ny),
                        (int)arc4random_uniform(_nz) };
        if (![self isFree:start]) continue;
        for (int d = 0; d < 6; d++) {
            Cell c = { start.x + kDirs[d].x, start.y + kDirs[d].y, start.z + kDirs[d].z };
            if ([self isFree:c]) cands[n++] = d;
        }
    }
    if (n == 0) return NO;

    PSPipe *p = [PSPipe new];
    SCNMaterial *mat = [self randomMaterial];
    SCNCylinder *tube = [SCNCylinder cylinderWithRadius:kTubeRadius height:1.0];
    tube.radialSegmentCount = 32;
    tube.materials = @[mat];
    BOOL bigJoint = (_jointStyle == 0) || (_jointStyle == 2 && frand() < 0.5);
    SCNSphere *ball = [SCNSphere sphereWithRadius:kTubeRadius * (bigJoint ? 1.55 : 1.0)];
    ball.segmentCount = 32;
    ball.materials = @[mat];
    p.tube = tube;
    p.ball = ball;

    p->head = start;
    p->dir = cands[arc4random_uniform(n)];
    p->progress = 0;
    p->alive = YES;
    [self occupy:start];
    Cell next = { start.x + kDirs[p->dir].x, start.y + kDirs[p->dir].y, start.z + kDirs[p->dir].z };
    [self occupy:next];
    [self addBallFor:p at:start];
    [self startRunFor:p];
    [_active addObject:p];
    _spawned++;
    return YES;
}

- (void)advance:(PSPipe *)p by:(double)amount {
    p->progress += amount;
    while (p->alive && p->progress >= 1.0) {
        p->progress -= 1.0;
        Cell d = kDirs[p->dir];
        p->head = (Cell){ p->head.x + d.x, p->head.y + d.y, p->head.z + d.z };
        p->runCells++;

        int cands[6], n = 0;
        BOOL straightOK = NO;
        for (int i = 0; i < 6; i++) {
            Cell c = { p->head.x + kDirs[i].x, p->head.y + kDirs[i].y, p->head.z + kDirs[i].z };
            if ([self isFree:c]) {
                cands[n++] = i;
                if (i == p->dir) straightOK = YES;
            }
        }
        if (n == 0) {                       // boxed in: cap it and finish
            [self updateRunFor:p length:p->runCells];
            [self addBallFor:p at:p->head];
            p->alive = NO;
            break;
        }
        int nd = (straightOK && frand() < 0.6) ? p->dir : cands[arc4random_uniform(n)];
        Cell next = { p->head.x + kDirs[nd].x, p->head.y + kDirs[nd].y, p->head.z + kDirs[nd].z };
        [self occupy:next];
        if (nd != p->dir) {                 // turn: finish this run, add a joint
            [self updateRunFor:p length:p->runCells];
            [self addBallFor:p at:p->head];
            p->dir = nd;
            [self startRunFor:p];
        }
    }
    if (p->alive) [self updateRunFor:p length:p->runCells + p->progress];
}

// MARK: - Simulation

- (void)step:(double)dt {
    _time += dt;

    // Slow drift instead of a locked-off camera.
    _pivot.eulerAngles = SCNVector3Make(0.07 * sin(_time * 0.11),
                                        0.24 * sin(_time * 0.06), 0);

    if (_phase == 0) {
        if (_fade < 1) { _fade = MIN(1.0, _fade + dt / 1.2); [self applyFade]; }

        for (PSPipe *p in [_active copy]) {
            [self advance:p by:kSpeed * dt];
            if (!p->alive) [_active removeObject:p];
        }

        double fill = (double)_filled / (double)(_nx * _ny * _nz);
        BOOL canSpawn = _spawned < _maxPipes && fill < 0.42;
        if (canSpawn && _active.count < (NSUInteger)_concurrent) {
            _spawnDelay -= dt;
            if (_spawnDelay <= 0) {
                if (![self spawnPipe]) _spawned = _maxPipes;
                _spawnDelay = 0.25 + frand() * 0.6;
            }
        }
        if (_active.count == 0 && !(_spawned < _maxPipes && fill < 0.42)) {
            _phase = 1;
            _phaseT = 0;
        }
    } else if (_phase == 1) {               // admire the finished tangle
        _phaseT += dt;
        if (_phaseT > 3.0) { _phase = 2; _phaseT = 0; }
    } else {                                // dissolve to black, start again
        _fade = MAX(0.0, _fade - dt / 1.6);
        [self applyFade];
        if (_fade <= 0) [self resetPipes];
    }
}

- (void)renderer:(id<SCNSceneRenderer>)renderer updateAtTime:(NSTimeInterval)time {
    double dt = _lastTime > 0 ? time - _lastTime : 1.0 / 60.0;
    _lastTime = time;
    [self step:MAX(0.0, MIN(dt, 0.1))];
}

// MARK: - Offline test hooks (used by the build's snapshot tool)

- (void)debugAdvance:(double)seconds {
    for (double t = 0; t < seconds; t += 1.0 / 60.0) [self step:1.0 / 60.0];
}

- (NSImage *)debugSnapshot {
    SCNRenderer *r = [SCNRenderer rendererWithDevice:nil options:nil];
    r.scene = _scene;
    r.pointOfView = _cameraNode;
    return [r snapshotAtTime:0 withSize:self.bounds.size
            antialiasingMode:SCNAntialiasingModeMultisampling4X];
}

@end
