// Pipes for Windows — OpenGL port of src/Pipes.m.
// Same simulation as the macOS version; rendering uses classic fixed-function
// OpenGL with glossy highlights and sphere-mapped chrome so it runs on any PC.

#define SAVER_NAME "Pipes"
#define SAVER_FPS 60
#include "saver.h"
#include <GL/gl.h>
#include <GL/glu.h>

#define NY 11
#define NZ 11
#define MAXNX 40
#define MAXRUNS 8000
#define MAXBALLS 8000
#define MAXMATS 64
#define MAXPIPES 8

#define TUBE_RADIUS 0.22f
#define SPEED 9.0

#ifndef GL_BGR_EXT
#define GL_BGR_EXT 0x80E0
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { int x, y, z; } Cell;
typedef struct { float r, g, b; int metal; } Mat;
typedef struct { Cell start; int dir; float len; int mat; } Run;
typedef struct { Cell at; float radius; int mat; } Ball;
typedef struct {
    Cell head, runStart;
    int dir, runCells, alive, mat, run;
    float ballRadius;
    double progress;
} Pipe;

static const Cell kDirs[6] = {
    { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}
};

static HDC   g_dc;
static HGLRC g_rc;
static int   g_w, g_h;
static GLuint g_listTube, g_listBall, g_envTex;

static int g_nx;
static unsigned char g_occ[MAXNX * NY * NZ];
static int g_filled;

static Mat  g_mats[MAXMATS];  static int g_matCount;
static Run  g_runs[MAXRUNS];  static int g_runCount;
static Ball g_balls[MAXBALLS]; static int g_ballCount;
static Pipe g_pipes[MAXPIPES]; static int g_pipeCount;

static int g_spawned, g_maxPipes, g_concurrent, g_jointStyle, g_phase;
static double g_spawnDelay, g_phaseT, g_fade, g_time, g_camDist;

// ---------------------------------------------------------------- grid

static int inside(Cell c) {
    return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < g_nx && c.y < NY && c.z < NZ;
}
static int cell_index(Cell c) { return (c.z * NY + c.y) * g_nx + c.x; }
static int is_free(Cell c) { return inside(c) && !g_occ[cell_index(c)]; }
static void occupy(Cell c) { g_occ[cell_index(c)] = 1; g_filled++; }
static Cell step_cell(Cell c, int dir) {
    Cell r = { c.x + kDirs[dir].x, c.y + kDirs[dir].y, c.z + kDirs[dir].z };
    return r;
}

static void reset_pipes(void) {
    double aspect = g_h > 0 ? (double)g_w / g_h : 1.6;
    g_nx = (int)floor(NY * aspect * 1.1 + 0.5);
    if (g_nx < 8) g_nx = 8;
    if (g_nx > MAXNX) g_nx = MAXNX;
    memset(g_occ, 0, sizeof g_occ);
    g_filled = 0;
    g_matCount = g_runCount = g_ballCount = g_pipeCount = 0;

    g_spawned = 0;
    g_concurrent = 2 + irand(3);
    g_maxPipes = 7 + irand(8);
    g_jointStyle = irand(3);          // 0 ball joints, 1 elbows, 2 mixed
    g_spawnDelay = 0.2;
    g_phase = 0;
    g_phaseT = 0;
    g_fade = 0;
    g_camDist = (NY + 0.5) / (2 * tan(20.0 * M_PI / 180.0)) + NZ / 2.0;
}

// ---------------------------------------------------------------- pipes

static int random_material(void) {
    static const float paint[8][3] = {
        {0.86f, 0.08f, 0.08f}, {0.05f, 0.62f, 0.14f}, {0.08f, 0.20f, 0.85f}, {0.95f, 0.80f, 0.05f},
        {0.05f, 0.70f, 0.78f}, {0.78f, 0.10f, 0.70f}, {0.92f, 0.92f, 0.92f}, {0.95f, 0.42f, 0.05f},
    };
    static const float metal[3][3] = {
        {0.90f, 0.91f, 0.93f}, {0.95f, 0.55f, 0.38f}, {1.00f, 0.78f, 0.34f},
    };
    Mat m;
    double roll = frand();
    const float *c;
    if (g_matCount >= MAXMATS) return irand(MAXMATS);
    if (roll < 0.68)      { c = paint[irand(8)]; m.metal = 0; }
    else if (roll < 0.88) { c = metal[irand(3)]; m.metal = 1; }
    else                  { c = paint[irand(6)]; m.metal = 1; }
    m.r = c[0]; m.g = c[1]; m.b = c[2];
    g_mats[g_matCount] = m;
    return g_matCount++;
}

static void add_ball(const Pipe *p, Cell at) {
    if (g_ballCount >= MAXBALLS) return;
    g_balls[g_ballCount].at = at;
    g_balls[g_ballCount].radius = p->ballRadius;
    g_balls[g_ballCount].mat = p->mat;
    g_ballCount++;
}

static void start_run(Pipe *p) {
    p->runStart = p->head;
    p->runCells = 0;
    if (g_runCount >= MAXRUNS) { p->alive = 0; return; }
    p->run = g_runCount++;
    g_runs[p->run].start = p->head;
    g_runs[p->run].dir = p->dir;
    g_runs[p->run].len = 0;
    g_runs[p->run].mat = p->mat;
}

static int spawn_pipe(void) {
    Cell start = {0, 0, 0};
    int cands[6], n = 0, attempt, d, big;
    Pipe *p;
    if (g_pipeCount >= MAXPIPES) return 0;
    for (attempt = 0; attempt < 60 && n == 0; attempt++) {
        start.x = irand(g_nx); start.y = irand(NY); start.z = irand(NZ);
        if (!is_free(start)) continue;
        for (d = 0; d < 6; d++)
            if (is_free(step_cell(start, d))) cands[n++] = d;
    }
    if (n == 0) return 0;

    p = &g_pipes[g_pipeCount++];
    memset(p, 0, sizeof *p);
    p->mat = random_material();
    big = (g_jointStyle == 0) || (g_jointStyle == 2 && frand() < 0.5);
    p->ballRadius = TUBE_RADIUS * (big ? 1.55f : 1.0f);
    p->head = start;
    p->dir = cands[irand(n)];
    p->alive = 1;
    occupy(start);
    occupy(step_cell(start, p->dir));
    add_ball(p, start);
    start_run(p);
    g_spawned++;
    return 1;
}

static void advance_pipe(Pipe *p, double amount) {
    p->progress += amount;
    while (p->alive && p->progress >= 1.0) {
        int cands[6], n = 0, straightOK = 0, i, nd;
        p->progress -= 1.0;
        p->head = step_cell(p->head, p->dir);
        p->runCells++;
        for (i = 0; i < 6; i++) {
            if (is_free(step_cell(p->head, i))) {
                cands[n++] = i;
                if (i == p->dir) straightOK = 1;
            }
        }
        if (n == 0) {                               // boxed in: cap it and finish
            g_runs[p->run].len = (float)p->runCells;
            add_ball(p, p->head);
            p->alive = 0;
            break;
        }
        nd = (straightOK && frand() < 0.6) ? p->dir : cands[irand(n)];
        occupy(step_cell(p->head, nd));
        if (nd != p->dir) {                         // turn: finish this run, add a joint
            g_runs[p->run].len = (float)p->runCells;
            add_ball(p, p->head);
            p->dir = nd;
            start_run(p);
        }
    }
    if (p->alive) g_runs[p->run].len = (float)(p->runCells + p->progress);
}

static void step_sim(double dt) {
    int i;
    g_time += dt;
    if (g_phase == 0) {
        double fill;
        int canSpawn;
        if (g_fade < 1) { g_fade += dt / 1.2; if (g_fade > 1) g_fade = 1; }
        for (i = 0; i < g_pipeCount; ) {
            advance_pipe(&g_pipes[i], SPEED * dt);
            if (!g_pipes[i].alive) g_pipes[i] = g_pipes[--g_pipeCount];
            else i++;
        }
        fill = (double)g_filled / (double)(g_nx * NY * NZ);
        canSpawn = g_spawned < g_maxPipes && fill < 0.42;
        if (canSpawn && g_pipeCount < g_concurrent) {
            g_spawnDelay -= dt;
            if (g_spawnDelay <= 0) {
                if (!spawn_pipe()) g_spawned = g_maxPipes;
                g_spawnDelay = 0.25 + frand() * 0.6;
            }
        }
        if (g_pipeCount == 0 && !(g_spawned < g_maxPipes && fill < 0.42)) {
            g_phase = 1;
            g_phaseT = 0;
        }
    } else if (g_phase == 1) {                      // admire the finished tangle
        g_phaseT += dt;
        if (g_phaseT > 3.0) { g_phase = 2; g_phaseT = 0; }
    } else {                                        // dissolve to black, start again
        g_fade -= dt / 1.6;
        if (g_fade <= 0) reset_pipes();
    }
}

// ---------------------------------------------------------------- rendering

static float smooth01(float a, float b, float x) {
    float t = (x - a) / (b - a);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t * t * (3 - 2 * t);
}

// Soft-edged rectangle "softbox" in sphere-map space.
static float softbox(float u, float v, float cx, float cy, float hw, float hh) {
    float dx = (float)fabs(u - cx) - hw, dy = (float)fabs(v - cy) - hh;
    float d = dx > dy ? dx : dy;
    return 1.0f - smooth01(-0.02f, 0.08f, d);
}

static void make_environment_texture(void) {
    enum { N = 256 };
    static unsigned char tex[N * N * 3];
    int x, y;
    for (y = 0; y < N; y++) {
        for (x = 0; x < N; x++) {
            float u = (x + 0.5f) / N * 2 - 1, v = (y + 0.5f) / N * 2 - 1;
            float base = 0.10f + 0.16f * smooth01(-1.0f, 1.0f, v);
            float r = base, g = base, b = base, s;
            s = softbox(u, v, -0.38f, 0.48f, 0.20f, 0.11f); r += s;        g += s;        b += s;
            s = softbox(u, v,  0.42f, 0.30f, 0.08f, 0.24f); r += s;        g += 0.95f * s; b += 0.86f * s;
            s = softbox(u, v,  0.05f, -0.55f, 0.34f, 0.04f); r += 0.35f * s; g += 0.38f * s; b += 0.45f * s;
            s = softbox(u, v, -0.70f, -0.10f, 0.05f, 0.18f); r += 0.6f * s;  g += 0.7f * s;  b += 0.8f * s;
            if (r > 1) r = 1;
            if (g > 1) g = 1;
            if (b > 1) b = 1;
            tex[(y * N + x) * 3 + 0] = (unsigned char)(r * 255);
            tex[(y * N + x) * 3 + 1] = (unsigned char)(g * 255);
            tex[(y * N + x) * 3 + 2] = (unsigned char)(b * 255);
        }
    }
    glGenTextures(1, &g_envTex);
    glBindTexture(GL_TEXTURE_2D, g_envTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, N, N, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void make_display_lists(void) {
    GLUquadric *q = gluNewQuadric();
    gluQuadricNormals(q, GLU_SMOOTH);
    g_listTube = glGenLists(2);
    g_listBall = g_listTube + 1;
    glNewList(g_listTube, GL_COMPILE);
    gluCylinder(q, TUBE_RADIUS, TUBE_RADIUS, 1.0, 32, 1);     // along +Z, length 1
    glEndList();
    glNewList(g_listBall, GL_COMPILE);
    gluSphere(q, 1.0, 32, 20);
    glEndList();
    gluDeleteQuadric(q);
}

static void use_material(const Mat *m, float fade) {
    if (m->metal) {
        glDisable(GL_LIGHTING);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glColor3f(m->r * fade * 1.25f, m->g * fade * 1.25f, m->b * fade * 1.25f);
    } else {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        glEnable(GL_LIGHTING);
        glColor3f(m->r, m->g, m->b);
    }
}

static void draw_scene(void) {
    float f = (float)(g_fade < 0 ? 0 : (g_fade > 1 ? 1 : g_fade));
    GLfloat keyPos[4]  = { 0.45f, 0.75f, 0.55f, 0 };
    GLfloat fillPos[4] = {-0.70f, -0.30f, 0.45f, 0 };
    GLfloat keyDiff[4], keySpec[4], fillDiff[4], amb[4], black[4] = {0, 0, 0, 1};
    GLfloat matSpec[4] = {0.95f, 0.95f, 0.95f, 1};
    int i, lastMat = -1;

    f = f * f * (3 - 2 * f);
    keyDiff[0] = 1.00f * f; keyDiff[1] = 0.98f * f; keyDiff[2] = 0.94f * f; keyDiff[3] = 1;
    keySpec[0] = keySpec[1] = keySpec[2] = f; keySpec[3] = 1;
    fillDiff[0] = 0.26f * f; fillDiff[1] = 0.31f * f; fillDiff[2] = 0.40f * f; fillDiff[3] = 1;
    amb[0] = amb[1] = amb[2] = 0.20f * f; amb[3] = 1;

    glViewport(0, 0, g_w, g_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(40.0, (double)g_w / (double)g_h, 0.5, 200.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslated(0, 0, -g_camDist);
    glRotated(4.0 * sin(g_time * 0.11), 1, 0, 0);       // slow drift
    glRotated(13.8 * sin(g_time * 0.06), 0, 1, 0);
    glTranslated(-(g_nx - 1) / 2.0, -(NY - 1) / 2.0, -(NZ - 1) / 2.0);

    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_POSITION, keyPos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, keyDiff);
    glLightfv(GL_LIGHT0, GL_SPECULAR, keySpec);
    glLightfv(GL_LIGHT1, GL_POSITION, fillPos);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, fillDiff);
    glLightfv(GL_LIGHT1, GL_SPECULAR, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, matSpec);
    glMaterialf(GL_FRONT, GL_SHININESS, 90.0f);

    for (i = 0; i < g_runCount; i++) {
        const Run *r = &g_runs[i];
        if (r->len <= 0.0005f) continue;
        if (r->mat != lastMat) { use_material(&g_mats[r->mat], f); lastMat = r->mat; }
        glPushMatrix();
        glTranslatef((float)r->start.x, (float)r->start.y, (float)r->start.z);
        switch (r->dir) {
        case 0: glRotatef( 90, 0, 1, 0); break;
        case 1: glRotatef(-90, 0, 1, 0); break;
        case 2: glRotatef(-90, 1, 0, 0); break;
        case 3: glRotatef( 90, 1, 0, 0); break;
        case 5: glRotatef(180, 0, 1, 0); break;
        default: break;
        }
        glScalef(1, 1, r->len);
        glCallList(g_listTube);
        glPopMatrix();
    }
    for (i = 0; i < g_ballCount; i++) {
        const Ball *b = &g_balls[i];
        if (b->mat != lastMat) { use_material(&g_mats[b->mat], f); lastMat = b->mat; }
        glPushMatrix();
        glTranslatef((float)b->at.x, (float)b->at.y, (float)b->at.z);
        glScalef(b->radius, b->radius, b->radius);
        glCallList(g_listBall);
        glPopMatrix();
    }
}

// ---------------------------------------------------------------- GL setup

typedef BOOL (WINAPI *ChoosePixelFormatARBFn)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef BOOL (WINAPI *SwapIntervalFn)(int);

static void basic_pfd(PIXELFORMATDESCRIPTOR *pfd) {
    memset(pfd, 0, sizeof *pfd);
    pfd->nSize = sizeof *pfd;
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cDepthBits = 24;
}

// Anti-aliasing needs a pixel format that can only be asked for once a GL
// context exists, so probe with a throwaway window first.
static int find_multisample_format(void) {
    static const int attrs[] = {
        0x2001, 1,        // WGL_DRAW_TO_WINDOW_ARB
        0x2010, 1,        // WGL_SUPPORT_OPENGL_ARB
        0x2011, 1,        // WGL_DOUBLE_BUFFER_ARB
        0x2013, 0x202B,   // WGL_PIXEL_TYPE_ARB = RGBA
        0x2014, 32,       // WGL_COLOR_BITS_ARB
        0x2022, 24,       // WGL_DEPTH_BITS_ARB
        0x2041, 1,        // WGL_SAMPLE_BUFFERS_ARB
        0x2042, 4,        // WGL_SAMPLES_ARB
        0
    };
    PIXELFORMATDESCRIPTOR pfd;
    int format = 0, pf;
    UINT count = 0;
    HWND dummy = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL,
                                 GetModuleHandleA(NULL), NULL);
    HDC dc;
    HGLRC rc;
    if (!dummy) return 0;
    dc = GetDC(dummy);
    basic_pfd(&pfd);
    pf = ChoosePixelFormat(dc, &pfd);
    if (pf && SetPixelFormat(dc, pf, &pfd) && (rc = wglCreateContext(dc)) != NULL) {
        if (wglMakeCurrent(dc, rc)) {
            ChoosePixelFormatARBFn choose =
                (ChoosePixelFormatARBFn)(void *)wglGetProcAddress("wglChoosePixelFormatARB");
            if (choose && (!choose(dc, attrs, NULL, 1, &format, &count) || count == 0)) format = 0;
            wglMakeCurrent(NULL, NULL);
        }
        wglDeleteContext(rc);
    }
    ReleaseDC(dummy, dc);
    DestroyWindow(dummy);
    return format;
}

static void saver_start(HWND hwnd, int w, int h) {
    PIXELFORMATDESCRIPTOR pfd;
    int msaa = find_multisample_format(), pf;
    SwapIntervalFn swapInterval;

    g_w = w;
    g_h = h;
    g_dc = GetDC(hwnd);
    basic_pfd(&pfd);
    if (msaa && DescribePixelFormat(g_dc, msaa, sizeof pfd, &pfd) && SetPixelFormat(g_dc, msaa, &pfd)) {
        // anti-aliased format in use
    } else {
        basic_pfd(&pfd);
        pf = ChoosePixelFormat(g_dc, &pfd);
        SetPixelFormat(g_dc, pf, &pfd);
        msaa = 0;
    }
    g_rc = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, g_rc);

    swapInterval = (SwapIntervalFn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swapInterval) swapInterval(1);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1);
    glShadeModel(GL_SMOOTH);
    if (msaa) glEnable(GL_MULTISAMPLE);

    make_environment_texture();
    make_display_lists();
    reset_pipes();
}

static void saver_frame(HWND hwnd, double dt) {
    (void)hwnd;
    step_sim(dt);
    draw_scene();
    SwapBuffers(g_dc);
}

static void saver_stop(HWND hwnd) {
    wglMakeCurrent(NULL, NULL);
    if (g_rc) wglDeleteContext(g_rc);
    if (g_dc) ReleaseDC(hwnd, g_dc);
    g_rc = NULL;
    g_dc = NULL;
}

static int saver_capture(HWND hwnd, const char *path) {
    int stride = (g_w * 3 + 3) & ~3, ok;
    unsigned char *buf = (unsigned char *)malloc((size_t)stride * (size_t)g_h);
    (void)hwnd;
    if (!buf) return 0;
    draw_scene();
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, g_w, g_h, GL_BGR_EXT, GL_UNSIGNED_BYTE, buf);
    ok = write_bmp24(path, g_w, g_h, buf);
    free(buf);
    return ok;
}
