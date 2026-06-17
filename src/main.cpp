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

const int TARGET_FPS        = 60;
const int ANIM_FPS          = 12;
const int FLY_ANIM_FPS      = 18;
const UINT WM_TRAYICON      = WM_USER + 1;
const UINT ID_TRAY_QUIT     = 1001;
const int W                 = 215;
const int H                 = 215;
const int PET_MIN_MOVE_PX   = 2;
const int PET_REVERSAL_COUNT = 3;
const uint32_t PET_WINDOW_MS         = 1000;
const float FLY_SHORT_THRESHOLD      = 500.0f;
const float FLY_MIN_DISTANCE         = 300.0f;
const int   HOP_STEP_PX              = 8;
const float TAKEOFF_LIFTOFF_FRAC     = 0.25f;
const float LANDING_TOUCHDOWN_FRAC   = 0.65f;
const int   LEDGE_MIN_AIR_ROWS       = 60;
const int   LEDGE_EDGE_THRESHOLD     = 15;
const int   LEDGE_UNIFORM_THRESHOLD  = 15;
const int   LEDGE_MIN_SURFACE_WIDTH  = W;

struct Frame { int x, y, w, h; };

// Pixels are pre-multiplied BGRA (Win32 format), top-down, sheetW*sheetH uint32s.
struct Animation {
    uint32_t* pixels;
    int sheetW, sheetH;
    std::vector<Frame> frames;
    float footY = 0.85f;
    std::vector<bool> airborne;
};

struct FlyState {
    float startX = 0, startY = 0;
    float destX  = 0, destY  = 0;
    float ctrlX  = 0, ctrlY  = 0;
    float takeoffEnd   = 0.5f;
    float landingStart = 0.5f;
    int flyTotalFrames  = 0;
    int flyFramesPlayed = 0;
};

enum Facing { FACING_RIGHT, FACING_LEFT };

struct CompanionTracker { int lastX=0, lastDir=0, reversals=0; uint32_t windowStart=0; };

struct BirdState {
    // Animation
    std::map<std::string, std::vector<Animation>> animations;
    std::map<std::string, float> weights;
    std::string currentType;
    int currentVariantIdx = 0;
    std::deque<std::string> animQueue;
    int frameIndex = 0;
    Facing facing = FACING_RIGHT;
    float animInterval = 1000.0f / ANIM_FPS;

    // Flight
    FlyState flyState;
    bool flySequenceActive = false;

    // Position / screen
    int winX = 0, winY = 0;
    int screenW = 0, screenH = 0;

    // Ledge
    bool hasLedge = false;
    int ledgeY = 0;
    uint32_t ledgeRefColors[3] = {};

    // GDI / rendering
    HWND hwnd = NULL;
    HDC memDC = NULL;
    HBITMAP hBitmap = NULL;
    void* pvBits = nullptr;
    uint32_t staging[W * H];

    // Input
    CompanionTracker companionTracker;
    bool mouseOverBird = false;

    // System
    NOTIFYICONDATA trayIcon = {};
    std::mt19937 rng{std::random_device{}()};
};

BirdState bird;

// ---------------------------------------------------------------------------

std::string pickAnimation() {
    float total = 0.0f;
    for (auto& [name, w] : bird.weights)
        if (bird.animations.count(name) && !bird.animations[name].empty()) total += w;

    std::uniform_real_distribution<float> dist(0.0f, total);
    float r = dist(bird.rng);

    for (auto& [name, w] : bird.weights) {
        if (!bird.animations.count(name) || bird.animations[name].empty()) continue;
        r -= w;
        if (r <= 0.0f) return name;
    }

    for (auto& [name, _] : bird.weights)
        if (bird.animations.count(name) && !bird.animations[name].empty()) return name;

    return "";
}

int pickVariant(const std::string& type) {
    if (!bird.animations.count(type) || bird.animations[type].empty()) return 0;
    int n = (int)bird.animations[type].size();
    if (n <= 1) return 0;
    std::uniform_int_distribution<int> d(0, n - 1);
    return d(bird.rng);
}

float currentFootY() {
    if (bird.animations.count(bird.currentType) &&
        bird.currentVariantIdx < (int)bird.animations[bird.currentType].size())
        return bird.animations[bird.currentType][bird.currentVariantIdx].footY;
    return 0.85f;
}

int animFrameCount(const std::string& type) {
    if (bird.animations.count(type) && !bird.animations[type].empty())
        return (int)bird.animations[type][0].frames.size();
    return 51;
}

float flightGlobalT(const std::string& phase, int fIdx, int fTotal) {
    float frac = fTotal > 1 ? (float)fIdx / (fTotal - 1) : 1.0f;

    if (phase == "takeoff") {
        if (frac < TAKEOFF_LIFTOFF_FRAC) return 0.0f;
        float airFrac = (frac - TAKEOFF_LIFTOFF_FRAC) / (1.0f - TAKEOFF_LIFTOFF_FRAC);
        return airFrac * bird.flyState.takeoffEnd;
    }
    if (phase == "fly") {
        float ff = bird.flyState.flyTotalFrames > 0
            ? (float)bird.flyState.flyFramesPlayed / bird.flyState.flyTotalFrames : 1.0f;
        return bird.flyState.takeoffEnd + ff * (bird.flyState.landingStart - bird.flyState.takeoffEnd);
    }
    if (phase == "landing") {
        if (frac >= LANDING_TOUCHDOWN_FRAC) return 1.0f;
        float airFrac = frac / LANDING_TOUCHDOWN_FRAC;
        return bird.flyState.landingStart + airFrac * (1.0f - bird.flyState.landingStart);
    }

    return 0.0f;
}

void bezierAt(float t, int& outX, int& outY) {
    // Cubic ease-in-out: accelerates faster than quintic, still lands smoothly
    float te = t < 0.5f ? 4*t*t*t : 1.0f - powf(-2*t + 2, 3) / 2.0f;
    outX = (int)((1-te)*(1-te)*bird.flyState.startX + 2*(1-te)*te*bird.flyState.ctrlX + te*te*bird.flyState.destX);
    outY = (int)((1-te)*(1-te)*bird.flyState.startY + 2*(1-te)*te*bird.flyState.ctrlY + te*te*bird.flyState.destY);
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
    bmi.bmiHeader.biHeight      = -scanH;
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
    int ledgeY  = -1;
    int airRows = 0;

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
    int endX   = std::min(bird.screenW - 1, centerX + minWidth);
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
    bird.flyState.startX = (float)bird.winX;
    bird.flyState.startY = (float)bird.winY;

    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    float dx = 0, dy = 0, dist = 0;
    bool found = false;

    std::uniform_int_distribution<int> xDist(W / 2, bird.screenW - W / 2);
    for (int i = 0; i < 15 && !found; i++) {
        int scanX = xDist(bird.rng);
        if (scanX >= bird.winX && scanX < bird.winX + W) continue;

        int ledgeY = findLedgeAtX(scanX);
        if (ledgeY < 0) continue;

        if (!checkLedgeWidth(scanX, ledgeY, LEDGE_MIN_SURFACE_WIDTH)) continue;

        float destX = (float)(scanX - W / 2);
        // Use idle's foot_y as the ground reference so the landing bezier ends
        // at the same position idle expects — eliminates the snap on transition.
        float groundFootY = bird.animations.count("idle") && !bird.animations["idle"].empty()
            ? bird.animations["idle"][0].footY : 0.82f;
        float destY = (float)(ledgeY - (int)(groundFootY * H));
        bird.ledgeY = ledgeY;
        destX = std::max(0.0f, std::min(destX, (float)(bird.screenW - W)));
        destY = std::max(0.0f, std::min(destY, (float)(bird.screenH - H)));

        dx   = destX - bird.flyState.startX;
        dy   = destY - bird.flyState.startY;
        dist = sqrtf(dx * dx + dy * dy);

        if (dist >= FLY_MIN_DISTANCE) {
            bird.flyState.destX = destX;
            bird.flyState.destY = destY;
            found = true;
        }
    }

    if (!found) {
        float groundFootY = bird.animations.count("idle") && !bird.animations["idle"].empty()
            ? bird.animations["idle"][0].footY : 0.82f;
        int attempts = 0;
        do {
            bird.flyState.destX = std::uniform_real_distribution<float>(0, (float)(bird.screenW - W))(bird.rng);
            bird.ledgeY = workArea.bottom;
            bird.flyState.destY = (float)(bird.ledgeY - (int)(groundFootY * H));
            dx   = bird.flyState.destX - bird.flyState.startX;
            dy   = bird.flyState.destY - bird.flyState.startY;
            dist = sqrtf(dx * dx + dy * dy);
            attempts++;
        } while (dist < FLY_MIN_DISTANCE && attempts < 10);
    }

    float arcH = std::min(200.0f, dist * 0.3f);
    bird.flyState.ctrlX = (bird.flyState.startX + bird.flyState.destX) / 2.0f;
    bird.flyState.ctrlY = std::min(bird.flyState.startY, bird.flyState.destY) - arcH;
    bird.flyState.ctrlY = std::max(bird.flyState.ctrlY, 0.0f);

    int tfF = animFrameCount("takeoff");
    int ldF = animFrameCount("landing");
    int flF = animFrameCount("fly");
    bool hasFly = bird.animations.count("fly") && !bird.animations["fly"].empty();

    if (dist <= FLY_SHORT_THRESHOLD || !hasFly) {
        bird.flyState.flyTotalFrames  = 0;
        bird.flyState.takeoffEnd      = 0.5f;
        bird.flyState.landingStart    = 0.5f;
    } else {
        int n = std::max(1, (int)((dist - FLY_SHORT_THRESHOLD) / 700.0f));
        bird.flyState.flyTotalFrames = n * flF;
        int total = tfF + bird.flyState.flyTotalFrames + ldF;
        bird.flyState.takeoffEnd   = (float)tfF / total;
        bird.flyState.landingStart = 1.0f - (float)ldF / total;
    }

    bird.flyState.flyFramesPlayed = 0;
    bird.facing = (bird.flyState.destX > bird.flyState.startX) ? FACING_RIGHT : FACING_LEFT;
}

void startFlySequence() {
    planFly();
    bird.flySequenceActive = true;
    bird.animQueue.push_front("landing");
    if (bird.flyState.flyTotalFrames > 0) bird.animQueue.push_front("fly");
    bird.currentType = "takeoff";
    bird.currentVariantIdx = pickVariant(bird.currentType);
    bird.frameIndex = 0;
}

// ---------------------------------------------------------------------------

void checkLedgeValidity() {
    if (!bird.hasLedge || bird.flySequenceActive) return;

    auto lum = [](uint32_t c) { return (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3; };

    HDC screenDC = GetDC(NULL);
    int changed = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t cur = (uint32_t)GetPixel(screenDC, bird.winX + (i + 1) * W / 4, bird.ledgeY);
        if (abs((int)lum(cur) - (int)lum(bird.ledgeRefColors[i])) > 20) changed++;
    }
    ReleaseDC(NULL, screenDC);

    if (changed >= 2) {
        bird.hasLedge = false;
        bird.animQueue.clear();
        bird.flySequenceActive = false;
        bird.flyState = FlyState{};
        startFlySequence();
    }
}

// ---------------------------------------------------------------------------

void loadWeights() {
    std::ifstream f("weights.json");
    json data = json::parse(f);
    bird.weights.clear();
    for (auto& [name, val] : data.items())
        bird.weights[name] = val.get<float>();
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
            anim.footY = paths.value("foot_y", 0.85f);
            if (paths.contains("airborne_profile"))
                for (auto& v : paths["airborne_profile"])
                    anim.airborne.push_back(v.get<bool>());

            bird.animations[type].push_back(anim);
        }
    }
}

// ---------------------------------------------------------------------------

// Scale the current frame into bird.staging, optionally flip, then push to window.
void presentFrame(const Animation& anim, int fIdx, Facing renderFacing, int winX, int winY) {
    const Frame& f = anim.frames[fIdx];

    // Nearest-neighbor scale from spritesheet frame region → W×H staging buffer
    for (int y = 0; y < H; y++) {
        int sy = f.y + y * f.h / H;
        uint32_t* row = bird.staging + y * W;
        for (int x = 0; x < W; x++) {
            int sx = f.x + x * f.w / W;
            row[x] = anim.pixels[sy * anim.sheetW + sx];
        }
    }

    // Horizontal flip for FACING_LEFT
    if (renderFacing == FACING_LEFT) {
        for (int y = 0; y < H; y++) {
            uint32_t* row = bird.staging + y * W;
            for (int x = 0; x < W / 2; x++)
                std::swap(row[x], row[W - 1 - x]);
        }
    }

    memcpy(bird.pvBits, bird.staging, W * H * 4);

    POINT ptSrc = {0, 0};
    SIZE szWnd  = {W, H};
    POINT ptDst = {winX, winY};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(bird.hwnd, NULL, &ptDst, &szWnd, bird.memDC, &ptSrc, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        Shell_NotifyIcon(NIM_DELETE, &bird.trayIcon);
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
        Shell_NotifyIcon(NIM_DELETE, &bird.trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_NCHITTEST)
        return bird.mouseOverBird ? HTCLIENT : HTTRANSPARENT;

    if (msg == WM_MOUSEMOVE) {
        if (bird.flySequenceActive) return 0;
        int x = (int)(short)LOWORD(lp);
        int dx = x - bird.companionTracker.lastX;

        if (abs(dx) > PET_MIN_MOVE_PX) {
            int dir = (dx > 0) ? 1 : -1;
            uint32_t now = GetTickCount();

            if (dir != bird.companionTracker.lastDir && bird.companionTracker.lastDir != 0) {
                if (now - bird.companionTracker.windowStart > PET_WINDOW_MS) {
                    bird.companionTracker.reversals = 0;
                    bird.companionTracker.windowStart = now;
                }
                bird.companionTracker.reversals++;
                if (bird.companionTracker.reversals >= PET_REVERSAL_COUNT) {
                    bird.animQueue.clear();
                    bird.animQueue.push_back("being_pet");
                    bird.companionTracker = CompanionTracker{};
                }
            }

            if (bird.companionTracker.lastDir == 0) bird.companionTracker.windowStart = now;
            bird.companionTracker.lastDir = dir;
            bird.companionTracker.lastX   = x;
        }

        return 0;
    }

    if (msg == WM_LBUTTONUP) {
        if (!bird.flySequenceActive) {
            bird.animQueue.clear();
            startFlySequence();
            bird.companionTracker = CompanionTracker{};
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    bird.screenW = GetSystemMetrics(SM_CXSCREEN);
    bird.screenH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "AiDesktopCompanion";
    RegisterClassEx(&wc);

    bird.winX = W;
    bird.winY = bird.screenH - H - 40;

    bird.hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "AiDesktopCompanion", "ai-desktop-companion",
        WS_POPUP,
        bird.winX, bird.winY, W, H,
        NULL, NULL, hInstance, NULL
    );
    ShowWindow(bird.hwnd, SW_SHOW);

    // GDI DIBSection — top-down 32-bit bitmap.
    // pvBits layout matches bird.staging: [B][G][R][A] per pixel = premultiplied Win32 BGRA.
    {
        HDC screenDC = GetDC(NULL);
        bird.memDC = CreateCompatibleDC(screenDC);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = W;
        bmi.bmiHeader.biHeight      = -H;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bird.hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bird.pvBits, NULL, 0);
        SelectObject(bird.memDC, bird.hBitmap);
        ReleaseDC(NULL, screenDC);
    }

    // System tray
    bird.trayIcon.cbSize          = sizeof(bird.trayIcon);
    bird.trayIcon.hWnd            = bird.hwnd;
    bird.trayIcon.uID             = 1;
    bird.trayIcon.uFlags          = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    bird.trayIcon.uCallbackMessage = WM_TRAYICON;
    bird.trayIcon.hIcon           = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(bird.trayIcon.szTip, "AI Desktop Companion");
    Shell_NotifyIcon(NIM_ADD, &bird.trayIcon);

    loadWeights();
    loadAnimations();

    bird.currentType       = pickAnimation();
    bird.currentVariantIdx = pickVariant(bird.currentType);
    bird.frameIndex        = 0;

    uint32_t lastTime     = GetTickCount();
    uint32_t lastAnimTick = lastTime;
    MSG  winMsg  = {};
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
            POINT mp; GetCursorPos(&mp); ScreenToClient(bird.hwnd, &mp);
            bird.mouseOverBird = false;
            if (mp.x >= 0 && mp.x < W && mp.y >= 0 && mp.y < H &&
                bird.animations.count(bird.currentType) &&
                bird.currentVariantIdx < (int)bird.animations[bird.currentType].size() &&
                bird.frameIndex < (int)bird.animations[bird.currentType][bird.currentVariantIdx].frames.size()) {
                const Animation& anim = bird.animations[bird.currentType][bird.currentVariantIdx];
                const Frame&     f    = anim.frames[bird.frameIndex];
                int sx = f.x + mp.x * f.w / W;
                int sy = f.y + mp.y * f.h / H;
                uint8_t a = (anim.pixels[sy * anim.sheetW + sx] >> 24) & 0xFF;
                bird.mouseOverBird = (a > 32);
            }
        }

        // --- Animation tick ---
        uint32_t tickInterval = bird.flySequenceActive
            ? (uint32_t)(1000.0f / FLY_ANIM_FPS)
            : (uint32_t)bird.animInterval;
        while (now - lastAnimTick >= tickInterval) {
            lastAnimTick += tickInterval;
            checkLedgeValidity();
            bird.frameIndex++;

            if (bird.currentType == "hop" && !bird.flySequenceActive &&
                bird.animations.count("hop") &&
                bird.currentVariantIdx < (int)bird.animations["hop"].size()) {
                const auto& air = bird.animations["hop"][bird.currentVariantIdx].airborne;
                if (bird.frameIndex < (int)air.size() && air[bird.frameIndex]) {
                    int step = (bird.facing == FACING_RIGHT) ? HOP_STEP_PX : -HOP_STEP_PX;
                    bird.winX = std::max(0, std::min(bird.winX + step, bird.screenW - W));
                }
            }

            bool transition = false;

            if (bird.currentType == "fly" && bird.flySequenceActive) {
                bird.flyState.flyFramesPlayed++;
                if (bird.flyState.flyFramesPlayed < bird.flyState.flyTotalFrames) {
                    int sz = (int)bird.animations[bird.currentType][bird.currentVariantIdx].frames.size();
                    if (bird.frameIndex >= sz) bird.frameIndex = 0;
                    break;
                }
                transition = true;
            }

            if (transition || bird.frameIndex >= (int)bird.animations[bird.currentType][bird.currentVariantIdx].frames.size()) {
                loadWeights();

                if (bird.currentType == "turnaround")
                    bird.facing = (bird.facing == FACING_RIGHT) ? FACING_LEFT : FACING_RIGHT;
                if (bird.currentType == "landing") {
                    bird.flySequenceActive = false;
                    bird.hasLedge = true;
                    HDC screenDC = GetDC(NULL);
                    for (int i = 0; i < 3; i++)
                        bird.ledgeRefColors[i] = (uint32_t)GetPixel(screenDC, bird.winX + (i + 1) * W / 4, bird.ledgeY);
                    ReleaseDC(NULL, screenDC);
                }

                std::string nextType;
                if (!bird.animQueue.empty()) {
                    nextType = bird.animQueue.front(); bird.animQueue.pop_front();
                } else {
                    nextType = pickAnimation();
                }

                if (nextType == "fly" && !bird.flySequenceActive) {
                    planFly();
                    bird.flySequenceActive = true;
                    bird.animQueue.push_front("landing");
                    if (bird.flyState.flyTotalFrames > 0) bird.animQueue.push_front("fly");
                    nextType = "takeoff";
                } else if (nextType != "fly" &&
                           (!bird.animations.count(nextType) || bird.animations[nextType].empty())) {
                    nextType = pickAnimation();
                }

                bird.currentType       = nextType;
                bird.currentVariantIdx = pickVariant(bird.currentType);

                if (bird.currentType == "hop" && bird.animations.count("hop") &&
                    bird.currentVariantIdx < (int)bird.animations["hop"].size()) {
                    int airborneCount = 0;
                    for (bool a : bird.animations["hop"][bird.currentVariantIdx].airborne)
                        if (a) airborneCount++;
                    int hopDist    = airborneCount * HOP_STEP_PX;
                    int destCenter = bird.winX + W / 2 + (bird.facing == FACING_RIGHT ? hopDist : -hopDist);
                    if (!checkLedgeWidth(destCenter, bird.ledgeY, W)) {
                        bird.currentType       = "idle";
                        bird.currentVariantIdx = pickVariant("idle");
                    }
                }

                bird.frameIndex = 0;
                break;
            }
        }
        // --- Render frame ---
        if (now - lastTime >= (uint32_t)(1000 / TARGET_FPS)) {
            lastTime = now;

            if (!bird.animations.count(bird.currentType) || bird.animations[bird.currentType].empty()) {
                bird.currentType = pickAnimation(); bird.currentVariantIdx = 0; bird.frameIndex = 0;
            }
            if (bird.currentVariantIdx >= (int)bird.animations[bird.currentType].size())
                bird.currentVariantIdx = 0;
            if (bird.frameIndex >= (int)bird.animations[bird.currentType][bird.currentVariantIdx].frames.size())
                bird.frameIndex = 0;

            const Animation& anim = bird.animations[bird.currentType][bird.currentVariantIdx];

            bool inFlight = bird.flySequenceActive &&
                (bird.currentType == "takeoff" || bird.currentType == "fly" || bird.currentType == "landing");
            if (inFlight) {
                int fTotal = (int)anim.frames.size();
                float gt = flightGlobalT(bird.currentType, bird.frameIndex, fTotal);
                bezierAt(gt, bird.winX, bird.winY);
            }

            presentFrame(anim, bird.frameIndex, bird.facing, bird.winX, bird.winY);
        }

        Sleep(1);
    }

    for (auto& [type, vars] : bird.animations)
        for (auto& a : vars) free(a.pixels);

    DeleteObject(bird.hBitmap);
    DeleteDC(bird.memDC);
    return 0;
}
