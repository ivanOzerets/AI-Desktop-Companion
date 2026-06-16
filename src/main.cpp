#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <random>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include "../lib/json/json.hpp"
#include "../lib/stb_image.h"

using json = nlohmann::json;

const int TARGET_FPS   = 60;
const int ANIM_FPS     = 12;
const int FLY_ANIM_FPS = 18;
const UINT WM_TRAYICON = WM_USER + 1;
const UINT ID_TRAY_QUIT = 1001;
const int W = 215;
const int H = 215;
const int PET_MIN_MOVE_PX = 2;
const int PET_REVERSAL_COUNT = 3;
const uint32_t PET_WINDOW_MS = 1000;
const float FLY_SHORT_THRESHOLD      = 500.0f;
const float FLY_MIN_DISTANCE         = 300.0f;
const float FOOT_Y                   = 0.85f;  // approx feet position in sprite (0=top, 1=bottom)
const float TAKEOFF_LIFTOFF_FRAC     = 0.25f;  // fraction of takeoff frames before window starts moving
const float LANDING_TOUCHDOWN_FRAC   = 0.65f;  // fraction of landing frames before window stops moving
const int   LEDGE_MIN_AIR_ROWS       = 60;     // uniform rows needed above a ledge (higher = skip text)
const int   LEDGE_EDGE_THRESHOLD     = 15;     // brightness diff to call something an edge
const int   LEDGE_UNIFORM_THRESHOLD  = 15;     // max diff between rows to count as open air
const int   LEDGE_MIN_SURFACE_WIDTH  = W;      // horizontal pixels the surface color must hold

struct Frame { int x, y, w, h; };

// Pixels are pre-multiplied BGRA (Win32 format), top-down, sheetW*sheetH uint32s.
struct Animation {
    uint32_t* pixels;
    int sheetW, sheetH;
    std::vector<Frame> frames;
};

struct FlyState {
    float startX = 0, startY = 0;
    float destX = 0, destY = 0;
    float ctrlX = 0, ctrlY = 0;
    float takeoffEnd = 0.5f;
    float landingStart = 0.5f;
    int flyTotalFrames = 0;
    int flyFramesPlayed = 0;
};

std::map<std::string, std::vector<Animation>> animations;
std::map<std::string, float> weights;

std::string currentType;
int currentVariantIdx = 0;
std::deque<std::string> animQueue;
int frameIndex = 0;

enum Facing { FACING_RIGHT, FACING_LEFT };
Facing facing = FACING_RIGHT;

FlyState flyState;
bool flySequenceActive = false;
int  screenW = 0, screenH = 0;
HWND g_hwnd  = NULL;
float animInterval = 1000.0f / ANIM_FPS;

// GDI objects for UpdateLayeredWindow
HDC g_memDC   = NULL;
HBITMAP  g_hBitmap = NULL;
void* g_pvBits  = nullptr;

// Staging buffer: one frame scaled+flipped to W×H before copy to pvBits
uint32_t g_staging[W * H];

// Tracked window position (UpdateLayeredWindow owns the position)
int g_winX = 0, g_winY = 0;

// Ledge validity monitoring
uint32_t g_ledgeSnapshot[5] = {};
bool     g_hasLedge         = false;

std::mt19937 rng(std::random_device{}());
NOTIFYICONDATA trayIcon = {};

struct CompanionTracker { int lastX=0, lastDir=0, reversals=0; uint32_t windowStart=0; };
CompanionTracker companionTracker;
bool mouseOverBird = false;

// ---------------------------------------------------------------------------

std::string pickAnimation() {
    float total = 0.0f;

    for (auto& [name, w] : weights)
        if (animations.count(name) && !animations[name].empty()) total += w;
        
    std::uniform_real_distribution<float> dist(0.0f, total);
    float r = dist(rng);

    for (auto& [name, w] : weights) {
        if (!animations.count(name) || animations[name].empty()) continue;
        r -= w;
        if (r <= 0.0f) return name;
    }

    for (auto& [name, _] : weights)
        if (animations.count(name) && !animations[name].empty()) return name;

    return "";
}

int pickVariant(const std::string& type) {
    if (!animations.count(type) || animations[type].empty()) return 0;
    int n = (int)animations[type].size();
    if (n <= 1) return 0;
    std::uniform_int_distribution<int> d(0, n - 1);
    return d(rng);
}

int animFrameCount(const std::string& type) {
    if (animations.count(type) && !animations[type].empty())
        return (int)animations[type][0].frames.size();

    return 51;
}

float flightGlobalT(const std::string& phase, int fIdx, int fTotal) {
    float frac = fTotal > 1 ? (float)fIdx / (fTotal - 1) : 1.0f;

    if (phase == "takeoff") {
        if (frac < TAKEOFF_LIFTOFF_FRAC) return 0.0f;
        float airFrac = (frac - TAKEOFF_LIFTOFF_FRAC) / (1.0f - TAKEOFF_LIFTOFF_FRAC);
        return airFrac * flyState.takeoffEnd;
    }

    if (phase == "fly") {
        float ff = flyState.flyTotalFrames > 0
            ? (float)flyState.flyFramesPlayed / flyState.flyTotalFrames : 1.0f;
        return flyState.takeoffEnd + ff * (flyState.landingStart - flyState.takeoffEnd);
    }

    if (phase == "landing") {
        if (frac >= LANDING_TOUCHDOWN_FRAC) return 1.0f;
        float airFrac = frac / LANDING_TOUCHDOWN_FRAC;
        return flyState.landingStart + airFrac * (1.0f - flyState.landingStart);
    }

    return 0.0f;
}

void bezierAt(float t, int& outX, int& outY) {
    // Cubic ease-in-out: accelerates faster than quintic, still lands smoothly
    float te = t < 0.5f ? 4*t*t*t : 1.0f - powf(-2*t + 2, 3) / 2.0f;
    outX = (int)((1-te)*(1-te)*flyState.startX + 2*(1-te)*te*flyState.ctrlX + te*te*flyState.destX);
    outY = (int)((1-te)*(1-te)*flyState.startY + 2*(1-te)*te*flyState.ctrlY + te*te*flyState.destY);
}

// ---------------------------------------------------------------------------

// Captures a 1-pixel-wide column from the screen at x and scans top-to-bottom for
// the first ledge: a high-contrast edge preceded by LEDGE_MIN_AIR_ROWS uniform rows.
// Returns the screen Y of the ledge surface, or -1 if nothing found.
int findLedgeAtX(int x) {
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    int scanH = workArea.bottom;

    HDC screenDC = GetDC(NULL);
    HDC memDC    = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = 1;
    bmi.bmiHeader.biHeight      = -scanH;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   colBits   = nullptr;
    HBITMAP colBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &colBits, NULL, 0);
    if (!colBitmap) { DeleteDC(memDC); ReleaseDC(NULL, screenDC); return -1; }
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, colBitmap);

    BitBlt(memDC, 0, 0, 1, scanH, screenDC, x, 0, SRCCOPY);

    // BI_RGB 32bpp: each uint32 = 0x00RRGGBB (B in low byte, A byte unused)
    uint32_t* px = (uint32_t*)colBits;
    int ledgeY   = -1;
    int airRows  = 0;

    for (int y = 50; y < scanH - 1; y++) {
        auto lum = [](uint32_t c) {
            return ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) / 3;
        };
        int diff = abs((int)lum(px[y]) - (int)lum(px[y - 1]));

        if (diff < LEDGE_UNIFORM_THRESHOLD) {
            airRows++;
        } else {
            if (airRows >= LEDGE_MIN_AIR_ROWS && diff >= LEDGE_EDGE_THRESHOLD) {
                ledgeY = y;
                break;
            }
            airRows = 0;
        }
    }

    SelectObject(memDC, oldBitmap);
    DeleteObject(colBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    return ledgeY;
}

// At the found ledge Y, scan left and right along that row checking how many consecutive
// pixels match the surface color (the pixel just at the edge). Returns false if the
// consistent-color span is narrower than minWidth — rejects text, accepts window borders.
bool checkLedgeWidth(int centerX, int ledgeY, int minWidth) {
    int startX = std::max(0, centerX - minWidth);
    int endX   = std::min(screenW - 1, centerX + minWidth);
    int rowW   = endX - startX;

    HDC screenDC = GetDC(NULL);
    HDC memDC    = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = rowW;
    bmi.bmiHeader.biHeight      = -1;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   rowBits   = nullptr;
    HBITMAP rowBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &rowBits, NULL, 0);
    if (!rowBitmap) { DeleteDC(memDC); ReleaseDC(NULL, screenDC); return false; }
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, rowBitmap);
    BitBlt(memDC, 0, 0, rowW, 1, screenDC, startX, ledgeY, SRCCOPY);

    uint32_t* row = (uint32_t*)rowBits;
    auto lum = [](uint32_t c) {
        return ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) / 3;
    };

    int centerIdx = centerX - startX;
    int refLum    = lum(row[centerIdx]);

    int left = 0;
    for (int i = centerIdx - 1; i >= 0; i--) {
        if (abs((int)lum(row[i]) - refLum) <= 20) left++; else break;
    }
    int right = 0;
    for (int i = centerIdx + 1; i < rowW; i++) {
        if (abs((int)lum(row[i]) - refLum) <= 20) right++; else break;
    }

    SelectObject(memDC, oldBitmap);
    DeleteObject(rowBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    return (left + right + 1) >= minWidth;
}

void planFly() {
    flyState.startX = (float)g_winX;
    flyState.startY = (float)g_winY;

    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    float dx = 0, dy = 0, dist = 0;
    bool found = false;

    // Try to land on an actual ledge detected by screen contrast scan
    std::uniform_int_distribution<int> xDist(W / 2, screenW - W / 2);
    for (int i = 0; i < 15 && !found; i++) {
        int scanX = xDist(rng);
        // Don't scan through our own window
        if (scanX >= g_winX && scanX < g_winX + W) continue;

        int ledgeY = findLedgeAtX(scanX);
        if (ledgeY < 0) continue;

        // Reject narrow surfaces (text, icons): surface color at ledgeY must hold
        // for at least LEDGE_MIN_SURFACE_WIDTH pixels horizontally.
        if (!checkLedgeWidth(scanX, ledgeY, LEDGE_MIN_SURFACE_WIDTH)) continue;

        float destX = (float)(scanX - W / 2);
        float destY = (float)(ledgeY - (int)(FOOT_Y * H));
        destX = std::max(0.0f, std::min(destX, (float)(screenW - W)));
        destY = std::max(0.0f, std::min(destY, (float)(screenH - H)));

        dx   = destX - flyState.startX;
        dy   = destY - flyState.startY;
        dist = sqrtf(dx * dx + dy * dy);

        if (dist >= FLY_MIN_DISTANCE) {
            flyState.destX = destX;
            flyState.destY = destY;
            found = true;
        }
    }

    // Fallback: land somewhere on the floor (top of taskbar)
    if (!found) {
        int attempts = 0;
        do {
            flyState.destX = std::uniform_real_distribution<float>(0, (float)(screenW - W))(rng);
            flyState.destY = (float)(workArea.bottom - H);
            dx   = flyState.destX - flyState.startX;
            dy   = flyState.destY - flyState.startY;
            dist = sqrtf(dx * dx + dy * dy);
            attempts++;
        } while (dist < FLY_MIN_DISTANCE && attempts < 10);
    }

    float arcH = std::min(200.0f, dist * 0.3f);
    flyState.ctrlX = (flyState.startX + flyState.destX) / 2.0f;
    flyState.ctrlY = std::min(flyState.startY, flyState.destY) - arcH;
    flyState.ctrlY = std::max(flyState.ctrlY, 0.0f);

    int tfF = animFrameCount("takeoff");
    int ldF = animFrameCount("landing");
    int flF = animFrameCount("fly");
    bool hasFly = animations.count("fly") && !animations["fly"].empty();

    if (dist <= FLY_SHORT_THRESHOLD || !hasFly) {
        flyState.flyTotalFrames  = 0;
        flyState.takeoffEnd      = 0.5f;
        flyState.landingStart    = 0.5f;
    } else {
        int n = std::max(1, (int)((dist - FLY_SHORT_THRESHOLD) / 700.0f));
        flyState.flyTotalFrames = n * flF;
        int total = tfF + flyState.flyTotalFrames + ldF;
        flyState.takeoffEnd   = (float)tfF / total;
        flyState.landingStart = 1.0f - (float)ldF / total;
    }

    flyState.flyFramesPlayed = 0;
    facing = (flyState.destX > flyState.startX) ? FACING_RIGHT : FACING_LEFT;
}

void startFlySequence() {
    planFly();  // sets facing to destination direction
    flySequenceActive = true;

    animQueue.push_front("landing");
    if (flyState.flyTotalFrames > 0) animQueue.push_front("fly");
    currentType = "takeoff";

    currentVariantIdx = pickVariant(currentType);
    frameIndex = 0;
}

// ---------------------------------------------------------------------------

void captureLedgeSnapshot() {
    // Sample 5 points just below the bird's feet. The sprite is transparent below
    // the feet so GetPixel returns the ledge surface behind our window.
    int footY = g_winY + (int)(FOOT_Y * H) + 3;
    HDC screenDC = GetDC(NULL);
    for (int i = 0; i < 5; i++)
        g_ledgeSnapshot[i] = (uint32_t)GetPixel(screenDC, g_winX + (i + 1) * W / 6, footY);
    ReleaseDC(NULL, screenDC);
    g_hasLedge = true;
}

void checkLedgeValidity(HWND hwnd) {
    if (!g_hasLedge || flySequenceActive) return;

    int footY = g_winY + (int)(FOOT_Y * H) + 3;
    HDC screenDC = GetDC(NULL);
    int changed = 0;
    for (int i = 0; i < 5; i++) {
        COLORREF now = GetPixel(screenDC, g_winX + (i + 1) * W / 6, footY);
        auto lum = [](uint32_t c) { return (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3; };
        if (abs((int)lum(now) - (int)lum(g_ledgeSnapshot[i])) > 30) changed++;
    }
    ReleaseDC(NULL, screenDC);

    if (changed >= 2) {
        g_hasLedge = false;
        animQueue.clear();
        flySequenceActive = false;
        flyState = FlyState{};
        startFlySequence();
    }
}

// ---------------------------------------------------------------------------

void loadWeights() {
    std::ifstream f("weights.json");
    json data = json::parse(f);
    weights.clear();
    for (auto& [name, val] : data.items())
        weights[name] = val.get<float>();
}

void loadAnimations() {
    std::ifstream f("animations.json");
    json registry = json::parse(f);

    for (auto& [type, varList] : registry.items()) {
        for (auto& paths : varList) {
            std::string sheetPath = paths["spritesheet"].get<std::string>();
            std::string atlasPath = paths["atlas"].get<std::string>();

            int w, h, ch;
            // stbi_load gives RGBA bytes: [R][G][B][A] per pixel, top-down.
            uint8_t* raw = stbi_load(sheetPath.c_str(), &w, &h, &ch, 4);
            if (!raw) continue;

            // Convert RGBA → premultiplied BGRA (Win32 DIB format) in-place.
            //
            // stbi RGBA as uint32 on LE: bits[7:0]=R, [15:8]=G, [23:16]=B, [31:24]=A
            // Win32 BGRA as uint32 on LE: bits[7:0]=B, [15:8]=G, [23:16]=R, [31:24]=A
            // UpdateLayeredWindow+AC_SRC_ALPHA requires premultiplied RGB values.
            uint32_t* px = (uint32_t*)raw;
            for (int i = 0; i < w * h; i++) {
                uint8_t r =  px[i]        & 0xFF;
                uint8_t g = (px[i] >>  8) & 0xFF;
                uint8_t b = (px[i] >> 16) & 0xFF;
                uint8_t a = (px[i] >> 24) & 0xFF;
                r = (uint8_t)((int)r * a / 255);
                g = (uint8_t)((int)g * a / 255);
                b = (uint8_t)((int)b * a / 255);
                // Store as Win32 BGRA: A in byte3, R in byte2, G in byte1, B in byte0
                px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }

            std::ifstream af(atlasPath);
            json atlas = json::parse(af);

            Animation anim;
            anim.pixels = px;
            anim.sheetW = w;
            anim.sheetH = h;
            for (auto& fd : atlas["frames"]) {
                Frame fr;
                fr.x = fd["frame"]["x"]; fr.y = fd["frame"]["y"];
                fr.w = fd["frame"]["w"]; fr.h = fd["frame"]["h"];
                anim.frames.push_back(fr);
            }
            animations[type].push_back(anim);
        }
    }
}

// ---------------------------------------------------------------------------

// Scale the current frame into g_staging, optionally flip, then push to window.
void presentFrame(HWND hwnd, const Animation& anim, int fIdx,
                  Facing renderFacing, int winX, int winY) {
    const Frame& f = anim.frames[fIdx];

    // Nearest-neighbor scale from spritesheet frame region → W×H staging buffer
    for (int y = 0; y < H; y++) {
        int sy = f.y + y * f.h / H;
        uint32_t* row = g_staging + y * W;
        for (int x = 0; x < W; x++) {
            int sx = f.x + x * f.w / W;
            row[x] = anim.pixels[sy * anim.sheetW + sx];
        }
    }

    // Horizontal flip for FACING_LEFT
    if (renderFacing == FACING_LEFT) {
        for (int y = 0; y < H; y++) {
            uint32_t* row = g_staging + y * W;
            for (int x = 0; x < W / 2; x++)
                std::swap(row[x], row[W - 1 - x]);
        }
    }

    memcpy(g_pvBits, g_staging, W * H * 4);

    POINT ptSrc = {0, 0};
    SIZE szWnd = {W, H};
    POINT ptDst = {winX, winY};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, NULL, &ptDst, &szWnd, g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        Shell_NotifyIcon(NIM_DELETE, &trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_TRAYICON && lp == WM_RBUTTONUP) {
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        HMENU menu = CreatePopupMenu();
        AppendMenu(menu, MF_STRING, ID_TRAY_QUIT, "Quit");
        TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_TRAY_QUIT) {
        Shell_NotifyIcon(NIM_DELETE, &trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_NCHITTEST)
        return mouseOverBird ? HTCLIENT : HTTRANSPARENT;

    if (msg == WM_MOUSEMOVE) {
        if (flySequenceActive) return 0;
        int x = (int)(short)LOWORD(lp);
        int dx = x - companionTracker.lastX;

        if (abs(dx) > PET_MIN_MOVE_PX) {
            int dir = (dx > 0) ? 1 : -1;
            uint32_t now = GetTickCount();

            if (dir != companionTracker.lastDir && companionTracker.lastDir != 0) {
                if (now - companionTracker.windowStart > PET_WINDOW_MS) {
                    companionTracker.reversals = 0;
                    companionTracker.windowStart = now;
                }
                companionTracker.reversals++;
                if (companionTracker.reversals >= PET_REVERSAL_COUNT) {
                    animQueue.clear();
                    animQueue.push_back("being_pet");
                    companionTracker = CompanionTracker{};
                }
            }

            if (companionTracker.lastDir == 0) companionTracker.windowStart = now;
            companionTracker.lastDir = dir;
            companionTracker.lastX   = x;
        }

        return 0;
    }

    if (msg == WM_LBUTTONUP) {
        if (!flySequenceActive) {
            animQueue.clear();
            startFlySequence();
            companionTracker = CompanionTracker{};
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    screenW = GetSystemMetrics(SM_CXSCREEN);
    screenH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "AiDesktopCompanion";
    RegisterClassEx(&wc);

    g_winX = W;
    g_winY = screenH - H - 40;

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "AiDesktopCompanion", "ai-desktop-companion",
        WS_POPUP,
        g_winX, g_winY, W, H,
        NULL, NULL, hInstance, NULL
    );
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);

    // GDI DIBSection — top-down 32-bit bitmap.
    // pvBits layout matches g_staging: [B][G][R][A] per pixel = premultiplied Win32 BGRA.
    {
        HDC screenDC = GetDC(NULL);
        g_memDC = CreateCompatibleDC(screenDC);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = W;
        bmi.bmiHeader.biHeight = -H;   // negative = top-down, matches stbi_load
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        g_hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &g_pvBits, NULL, 0);
        SelectObject(g_memDC, g_hBitmap);
        ReleaseDC(NULL, screenDC);
    }

    // System tray
    trayIcon.cbSize = sizeof(trayIcon);
    trayIcon.hWnd = hwnd;
    trayIcon.uID = 1;
    trayIcon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    trayIcon.uCallbackMessage = WM_TRAYICON;
    trayIcon.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(trayIcon.szTip, "AI Desktop Companion");
    Shell_NotifyIcon(NIM_ADD, &trayIcon);

    loadWeights();
    loadAnimations();

    currentType  = pickAnimation();
    currentVariantIdx = pickVariant(currentType);
    frameIndex = 0;

    uint32_t lastTime = GetTickCount();
    uint32_t lastAnimTick = lastTime;
    MSG  winMsg = {};
    bool running = true;

    while (running) {
        while (PeekMessage(&winMsg, NULL, 0, 0, PM_REMOVE)) {
            if (winMsg.message == WM_QUIT) running = false;
            TranslateMessage(&winMsg);
            DispatchMessage(&winMsg);
        }

        uint32_t now = GetTickCount();

        // --- Pixel hit test ---
        {
            POINT mp; GetCursorPos(&mp); ScreenToClient(hwnd, &mp);
            mouseOverBird = false;
            if (mp.x >= 0 && mp.x < W && mp.y >= 0 && mp.y < H &&
                animations.count(currentType) &&
                currentVariantIdx < (int)animations[currentType].size() &&
                frameIndex < (int)animations[currentType][currentVariantIdx].frames.size()) {
                const Animation& anim = animations[currentType][currentVariantIdx];
                const Frame&     f    = anim.frames[frameIndex];
                int sx = f.x + mp.x * f.w / W;
                int sy = f.y + mp.y * f.h / H;
                uint8_t a = (anim.pixels[sy * anim.sheetW + sx] >> 24) & 0xFF;
                mouseOverBird = (a > 32);
            }
        }

        // --- Animation tick ---
        uint32_t tickInterval = flySequenceActive
            ? (uint32_t)(1000.0f / FLY_ANIM_FPS)
            : (uint32_t)animInterval;
        while (now - lastAnimTick >= tickInterval) {
            lastAnimTick += tickInterval;
            frameIndex++;

            bool transition = false;

            if (currentType == "fly" && flySequenceActive) {
                flyState.flyFramesPlayed++;
                if (flyState.flyFramesPlayed < flyState.flyTotalFrames) {
                    int sz = (int)animations[currentType][currentVariantIdx].frames.size();
                    if (frameIndex >= sz) frameIndex = 0;
                    break;
                }
                transition = true;
            }

            if (transition || frameIndex >= (int)animations[currentType][currentVariantIdx].frames.size()) {
                loadWeights();

                if (currentType == "turnaround")
                    facing = (facing == FACING_RIGHT) ? FACING_LEFT : FACING_RIGHT;
                if (currentType == "landing") {
                    flySequenceActive = false;
                    captureLedgeSnapshot();
                }

                std::string nextType;
                if (!animQueue.empty()) {
                    nextType = animQueue.front(); animQueue.pop_front();
                } else {
                    nextType = pickAnimation();
                }

                if (nextType == "fly" && !flySequenceActive) {
                    planFly();  // sets facing to destination direction
                    flySequenceActive = true;
                    animQueue.push_front("landing");
                    if (flyState.flyTotalFrames > 0) animQueue.push_front("fly");
                    nextType = "takeoff";
                } else if (nextType != "fly" &&
                           (!animations.count(nextType) || animations[nextType].empty())) {
                    nextType = pickAnimation();
                }

                currentType       = nextType;
                currentVariantIdx = pickVariant(currentType);
                frameIndex        = 0;
                break;
            }
        }
        checkLedgeValidity(hwnd);

        // --- Render frame ---
        if (now - lastTime >= (uint32_t)(1000 / TARGET_FPS)) {
            lastTime = now;

            if (!animations.count(currentType) || animations[currentType].empty()) {
                currentType = pickAnimation(); currentVariantIdx = 0; frameIndex = 0;
            }
            if (currentVariantIdx >= (int)animations[currentType].size())
                currentVariantIdx = 0;
            if (frameIndex >= (int)animations[currentType][currentVariantIdx].frames.size())
                frameIndex = 0;

            const Animation& anim = animations[currentType][currentVariantIdx];

            bool inFlight = flySequenceActive &&
                (currentType == "takeoff" || currentType == "fly" || currentType == "landing");
            if (inFlight) {
                int fTotal = (int)anim.frames.size();
                float gt = flightGlobalT(currentType, frameIndex, fTotal);
                bezierAt(gt, g_winX, g_winY);
            }

            // During flight, facing was committed at takeoff start — just use it directly
            Facing renderFacing = facing;

            presentFrame(hwnd, anim, frameIndex, renderFacing, g_winX, g_winY);
        }

        Sleep(1);
    }

    for (auto& [type, vars] : animations)
        for (auto& a : vars) free(a.pixels);

    DeleteObject(g_hBitmap);
    DeleteDC(g_memDC);
    return 0;
}