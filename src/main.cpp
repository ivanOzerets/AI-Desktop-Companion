#include "types.h"
#include "animation.h"
#include "ledge.h"
#include "flight.h"
#include "sleep.h"
#include "bubble.h"
#include "spotlight.h"
#include "llm.h"
#include <cstring>

const UINT WM_TRAYICON         = WM_USER + 1;
const UINT ID_TRAY_QUIT        = 1001;
const UINT ID_HOTKEY_SPOTLIGHT = 2;

BirdState bird;

// ---------------------------------------------------------------------------
// updateHitTest
// Runs every render frame. Maps the cursor position into sprite space and checks
// the alpha channel so WM_NCHITTEST can decide click-through vs. interactive.
// ---------------------------------------------------------------------------
static void updateHitTest() {
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

// ---------------------------------------------------------------------------
// advanceAnimation
// Called when the current animation's frames are exhausted (or the fly loop
// finishes). Picks the next animation from the queue or weighted random, applies
// facing changes on turnaround completions, handles landing bookkeeping, and
// guards fly / hop / turnaround from firing while a speech bubble is active.
// ---------------------------------------------------------------------------
static void advanceAnimation() {
    loadWeights();

    if (bird.currentType == "turnaround" || bird.currentType == "takeoff_turnaround")
        bird.facing = (bird.facing == FACING_RIGHT) ? FACING_LEFT : FACING_RIGHT;

    if (bird.currentType == "landing") {
        bird.flySequenceActive = false;
        bird.hasLedge = true;
        bird.winX = (int)bird.flyState.destX;
        bird.winY = (int)bird.flyState.destY;
        HDC screenDC = GetDC(NULL);
        for (int i = 0; i < LEDGE_VALIDITY_SAMPLES; i++)
            bird.ledgeRefColors[i] = (uint32_t)GetPixel(screenDC, bird.winX + (i + 1) * W / (LEDGE_VALIDITY_SAMPLES + 1), bird.ledgeY + LEDGE_VALIDITY_Y_OFFSET);
        ReleaseDC(NULL, screenDC);
        bird.animQueue.push_back("idle");  // always settle before next action
        if (bird.isSleeping) {
            bird.animQueue.clear();
            bird.animQueue.push_back("dozing_off");
            bird.animQueue.push_back("sleeping");
        }
    }

    std::string nextType;
    if (!bird.animQueue.empty()) {
        nextType = bird.animQueue.front(); bird.animQueue.pop_front();
    } else if (bird.currentType == "sleeping" && bird.isSleeping) {
        nextType = "sleeping";  // loop sleeping only when queue is empty and still asleep
    } else {
        nextType = pickAnimation();
    }

    if (nextType == "fly" && !bird.flySequenceActive) {
        startFlySequence();           // handles takeoff_turnaround facing logic
        nextType = bird.currentType;  // "takeoff" or "takeoff_turnaround"
    }

    // Hop and turnaround move the bird — skip them while a bubble is showing
    if (isBubbleActive() && (nextType == "hop" || nextType == "turnaround"))
        nextType = "idle";

    if (nextType != "fly" && (!bird.animations.count(nextType) || bird.animations[nextType].empty()))
        nextType = pickAnimation();

    bird.currentType       = nextType;
    bird.currentVariantIdx = pickVariant(bird.currentType);

    if (bird.currentType == "hop" && bird.animations.count("hop") &&
        bird.currentVariantIdx < (int)bird.animations["hop"].size()) {
        int airborneCount = 0;
        for (bool a : bird.animations["hop"][bird.currentVariantIdx].airborne)
            if (a) airborneCount++;
        int hopDist    = airborneCount * HOP_STEP_PX;
        int destCenter = bird.winX + W / 2 + (bird.facing == FACING_RIGHT ? hopDist : -hopDist);
        int leadEdge   = destCenter + (bird.facing == FACING_RIGHT ? (W/2 - 10) : -(W/2 - 10));

        // Use findLedgeAtX to confirm a real ledge surface exists at both points,
        // at the same Y the bird is currently standing on (within tolerance).
        // checkLedgeWidth was checking color consistency only, which passes for open air too.
        int centerY = findLedgeAtX(destCenter);
        int leadY   = findLedgeAtX(leadEdge);
        bool centerOk = (centerY >= 0 && abs(centerY - bird.ledgeY) <= 20);
        bool leadOk   = (leadY   >= 0 && abs(leadY   - bird.ledgeY) <= 20);

        if (!centerOk || !leadOk) {
            bird.currentType       = "idle";
            bird.currentVariantIdx = pickVariant("idle");
        }
    }

    bird.frameIndex = 0;
}

// ---------------------------------------------------------------------------
// renderFrame
// Runs at TARGET_FPS. Moves the window along the bezier if in-flight, then
// scales and blits the current sprite frame to the layered window.
// ---------------------------------------------------------------------------
static void renderFrame() {
    if (!bird.animations.count(bird.currentType) || bird.animations[bird.currentType].empty()) {
        bird.currentType = pickAnimation(); bird.currentVariantIdx = 0; bird.frameIndex = 0;
    }
    if (bird.currentVariantIdx >= (int)bird.animations[bird.currentType].size())
        bird.currentVariantIdx = 0;
    if (bird.frameIndex >= (int)bird.animations[bird.currentType][bird.currentVariantIdx].frames.size())
        bird.frameIndex = 0;

    const Animation& anim = bird.animations[bird.currentType][bird.currentVariantIdx];

    bool inFlight = bird.flySequenceActive &&
        (bird.currentType == "takeoff" || bird.currentType == "takeoff_turnaround" ||
         bird.currentType == "fly"     || bird.currentType == "landing");
    if (inFlight) {
        int fTotal = (int)anim.frames.size();
        float gt = flightGlobalT(bird.currentType, bird.frameIndex, fTotal);
        bezierAt(gt, bird.winX, bird.winY);
    }

    presentFrame(anim, bird.frameIndex, bird.facing, bird.winX, bird.winY);
    tickBubble();
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LLM_RESPONSE) {
        LlmResponse* resp = reinterpret_cast<LlmResponse*>(lp);
        if (!resp->speech.empty()) showBubble(resp->speech);
        if (resp->action == "fly") {
            bird.animQueue.push_back("fly");
        } else if (resp->action == "sleep") {
            enterSleep();
        } else if (resp->action == "dance") {
            bird.animQueue.push_back("dancing");
        } else if (resp->action == "sing") {
            bird.animQueue.push_back("singing");
        } else if (resp->action == "yawn") {
            bird.animQueue.push_back("yawning");
        } else if (resp->action == "flap") {
            bird.animQueue.push_back("flapping_in_place");
        } else if (resp->action == "peck") {
            bird.animQueue.push_back("pecking");
        }
        delete resp;
        return 0;
    }
    if (msg == WM_SLOW_LOOP_DONE) {
        loadIdentityTimes();
        loadWeights();
        std::string* speech = reinterpret_cast<std::string*>(lp);
        if (speech && !speech->empty()) showBubble(*speech);
        delete speech;
        return 0;
    }
    if (msg == WM_DESTROY) {
        UnregisterHotKey(hwnd, ID_HOTKEY_SPOTLIGHT);
        Shell_NotifyIcon(NIM_DELETE, &bird.trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_HOTKEY && wp == ID_HOTKEY_SPOTLIGHT) {
        wakeUp();
        toggleSpotlight();
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

    if (msg == WM_SETCURSOR) {
        if (bird.mouseOverBird) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
    }

    if (msg == WM_MOUSEMOVE) {
        if (bird.flySequenceActive || isNighttime()) return 0;
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
            hideBubble();
            if (bird.isSleeping) {
                wakeUp();
                bird.animQueue.push_back("fly");  // fly after awoken finishes
            } else {
                startFlySequence();
            }
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

    bird.winX = -W;
    bird.winY = bird.screenH / 2;

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
    bird.trayIcon.cbSize           = sizeof(bird.trayIcon);
    bird.trayIcon.hWnd             = bird.hwnd;
    bird.trayIcon.uID              = 1;
    bird.trayIcon.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    bird.trayIcon.uCallbackMessage = WM_TRAYICON;
    bird.trayIcon.hIcon            = (HICON)LoadImage(NULL, "bird-icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    strcpy(bird.trayIcon.szTip, "AI Desktop Companion");
    Shell_NotifyIcon(NIM_ADD, &bird.trayIcon);

    initBubble(hInstance);
    initSpotlight(hInstance, bird.hwnd);
    initLlm();
    RegisterHotKey(bird.hwnd, ID_HOTKEY_SPOTLIGHT, MOD_CONTROL | MOD_SHIFT, VK_SPACE);

    loadWeights();
    loadAnimations();
    loadIdentityTimes();

    startFlySequence();  // fly in from off-screen on first launch

    uint32_t lastTime      = GetTickCount();
    uint32_t lastAnimTick  = lastTime;
    uint32_t lastSlowLoop  = lastTime;
    static const uint32_t SLOW_LOOP_INTERVAL = 5 * 60 * 1000;  // 5 minutes
    MSG      winMsg        = {};
    bool     running       = true;

    while (running) {
        while (PeekMessage(&winMsg, NULL, 0, 0, PM_REMOVE)) {
            if (winMsg.message == WM_QUIT) running = false;
            TranslateMessage(&winMsg);
            DispatchMessage(&winMsg);
        }

        uint32_t now = GetTickCount();

        updateHitTest();

        // Slow loop — periodic self-reflection every 5 minutes
        if (now - lastSlowLoop >= SLOW_LOOP_INTERVAL) {
            lastSlowLoop = now;
            runSlowLoop();
        }

        // Animation tick — runs at ANIM_FPS (ground) or FLY_ANIM_FPS (in-flight)
        uint32_t tickInterval = bird.flySequenceActive
            ? (uint32_t)(1000.0f / FLY_ANIM_FPS)
            : (uint32_t)bird.animInterval;
        while (now - lastAnimTick >= tickInterval) {
            lastAnimTick += tickInterval;
            checkLedgeValidity();
            updateSleepState();
            bird.frameIndex++;

            // Move window sideways on airborne hop frames
            if (bird.currentType == "hop" && !bird.flySequenceActive &&
                bird.animations.count("hop") &&
                bird.currentVariantIdx < (int)bird.animations["hop"].size()) {
                const auto& air = bird.animations["hop"][bird.currentVariantIdx].airborne;
                if (bird.frameIndex < (int)air.size() && air[bird.frameIndex]) {
                    int step = (bird.facing == FACING_RIGHT) ? HOP_STEP_PX : -HOP_STEP_PX;
                    bird.winX = std::max(0, std::min(bird.winX + step, bird.screenW - W));
                }
            }

            // Track fly-loop progress; only transition once the loop count is satisfied
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
                advanceAnimation();
                break;
            }
        }

        if (now - lastTime >= (uint32_t)(1000 / TARGET_FPS)) {
            lastTime = now;
            renderFrame();
        }

        Sleep(1);
    }

    for (auto& [type, vars] : bird.animations)
        for (auto& a : vars) free(a.pixels);

    DeleteObject(bird.hBitmap);
    DeleteDC(bird.memDC);
    return 0;
}
