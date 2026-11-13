// Minimal Windows screensaver host shared by both savers.
//
// A .scr file is an ordinary .exe that Windows launches with:
//   /s            run full screen
//   /p <hwnd>     draw a preview inside the given window (Settings dialog)
//   /c            show options (we have none)
// plus one extra mode used by the build's smoke test:
//   /t <seconds> <out.bmp>   run in a window, then save a frame and exit
//
// The including file defines SAVER_NAME and SAVER_FPS, then implements:
//   saver_start, saver_frame, saver_stop, saver_capture

#ifndef RETRO_SAVER_H
#define RETRO_SAVER_H

#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void saver_start(HWND hwnd, int w, int h);
static void saver_frame(HWND hwnd, double dt);
static void saver_stop(HWND hwnd);
static int  saver_capture(HWND hwnd, const char *path);

static int   g_fullscreen = 0;
static int   g_running = 1;
static int   g_started = 0;
static int   g_haveMouse = 0;
static POINT g_mouse0;

static double frand(void) { return rand() / (RAND_MAX + 1.0); }
static int    irand(int n) { return n > 0 ? (int)(((rand() << 15) | rand()) % (unsigned)n) : 0; }

// rows: bottom-up, BGR, each row padded to a multiple of 4 bytes
static int write_bmp24(const char *path, int w, int h, const unsigned char *rows) {
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)stride * (DWORD)h;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(rows, 1, (size_t)stride * (size_t)h, f);
    fclose(f);
    return 1;
}

static LRESULT CALLBACK SaverWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSCOMMAND:
        if (g_fullscreen && (wp & 0xFFF0) == SC_SCREENSAVE) return 0;   // already running
        break;
    case WM_SETCURSOR:
        if (g_fullscreen) { SetCursor(NULL); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (g_fullscreen) {
            POINT p;
            GetCursorPos(&p);
            if (!g_haveMouse) { g_mouse0 = p; g_haveMouse = 1; }
            else if (abs(p.x - g_mouse0.x) > 6 || abs(p.y - g_mouse0.y) > 6)
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (g_fullscreen) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_ACTIVATEAPP:
        if (g_fullscreen && !wp) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, NULL);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_started) { saver_stop(hwnd); g_started = 0; }
        g_running = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    char mode = 'c';
    HWND parent = NULL, hwnd = NULL;
    double testSecs = 0, elapsed = 0;
    char testPath[MAX_PATH] = "";
    const char *p = cmd ? cmd : "";
    WNDCLASSA wc;
    LARGE_INTEGER freq, last, now;
    int w = 0, h = 0, exitCode = 0;
    (void)prev; (void)show;

    while (*p == ' ' || *p == '"') p++;
    if (*p == '/' || *p == '-') p++;
    if (*p) { mode = (char)tolower((unsigned char)*p); p++; }
    while (*p == ' ' || *p == ':') p++;

    if (mode == 'p') {
        parent = (HWND)(UINT_PTR)_strtoui64(p, NULL, 10);
        if (!parent || !IsWindow(parent)) return 0;
    } else if (mode == 't') {
        char *end = NULL;
        testSecs = strtod(p, &end);
        p = end ? end : p;
        while (*p == ' ') p++;
        strncpy(testPath, p, MAX_PATH - 1);
    } else if (mode == 'c') {
        MessageBoxA(NULL, "This screen saver has no options.", SAVER_NAME,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    } else if (mode != 's') {
        return 0;
    }

    SetProcessDPIAware();
    srand((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId());

    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = SaverWndProc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "RetroSaverWindow";
    if (!RegisterClassA(&wc)) return 1;

    if (mode == 's') {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_fullscreen = 1;
        hwnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, SAVER_NAME, WS_POPUP | WS_VISIBLE,
                               x, y, w, h, NULL, NULL, inst, NULL);
        if (hwnd) { SetForegroundWindow(hwnd); SetCursor(NULL); }
    } else if (mode == 'p') {
        RECT rc;
        GetClientRect(parent, &rc);
        w = rc.right;
        h = rc.bottom;
        hwnd = CreateWindowExA(0, wc.lpszClassName, SAVER_NAME, WS_CHILD | WS_VISIBLE,
                               0, 0, w, h, parent, NULL, inst, NULL);
    } else {
        RECT rc = {0, 0, 1280, 800};
        w = 1280;
        h = 800;
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowExA(0, wc.lpszClassName, SAVER_NAME, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               40, 40, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    }
    if (!hwnd || w < 1 || h < 1) return 1;

    saver_start(hwnd, w, h);
    g_started = 1;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (g_running) {
        MSG msg;
        double dt;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        if (mode == 't') {
            dt = 1.0 / 30.0;                       // simulate as fast as possible
        } else {
            QueryPerformanceCounter(&now);
            dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
            if (dt < 1.0 / SAVER_FPS) { Sleep(1); continue; }
            last = now;
            if (dt > 0.1) dt = 0.1;
        }
        saver_frame(hwnd, dt);
        elapsed += dt;

        if (mode == 't' && elapsed >= testSecs) {
            exitCode = saver_capture(hwnd, testPath) ? 0 : 3;
            DestroyWindow(hwnd);
        }
    }
    return exitCode;
}

#endif
