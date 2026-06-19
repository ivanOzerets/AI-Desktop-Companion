#include "bubble.h"
#include <algorithm>

static HWND        g_hwnd         = NULL;
static HFONT       g_font         = NULL;
static std::string g_text;
static int         g_visibleChars = 0;
static uint32_t    g_endTime      = 0;
static uint32_t    g_nextCharTime = 0;
static uint32_t    g_fadeStart    = 0;
static bool        g_fadingOut    = false;
static bool        g_fadeInDone   = false;  // set true once fade-in reaches 255, avoids 60fps SLWA calls

static const COLORREF CHROMA        = RGB(255, 0, 255);
static const COLORREF BUBBLE_BG     = RGB(255, 252, 230);
static const COLORREF BUBBLE_BORDER = RGB(160, 148, 110);
static const COLORREF BUBBLE_TEXT   = RGB(50, 42, 32);
static const int BUBBLE_FONT_SIZE   = 20;    // ← font size for bubble text
static const int MAX_W              = 280;   // ← max bubble width before text wraps
static const int PAD                = 10;    // ← internal padding inside the roundrect

static const int CHAR_MS         = 10;   // ← ms per typewriter character reveal
static const int FADE_MS         = 180;  // ← fade-in and fade-out duration (ms)
static const int MAX_TEXT_LEN    = 300;  // ← messages longer than this are truncated
static const int POST_REVEAL_MS  = 5000; // ← extra display time after last character is shown

// ── Thought-bubble circle chain ───────────────────────────────────────────────
// Tune these to adjust the circles. TAIL_H is derived automatically.
static const int C_OFFSET = 4;              // ← extra gap between bubble edge and first circle
static const int C_R[]    = { 10, 7, 4 };  // radii:   largest (near bubble) → smallest
static const int C_DY[]   = {  0, 14, 23 }; // Y steps: from bubble edge, cumulative
static const int C_DX[]   = { 12,  7,  4 }; // X inset: from bubble corner, inward
static const int TAIL_H   = C_OFFSET + C_DY[2] + C_R[2] + 3;  // window space for circles

static int  g_bubbleW    = 0;
static int  g_contentH   = 0;
static int  g_windowH    = 0;
static bool g_isAbove    = true;
static bool g_cornerLeft = true;

// Returns true while the message is actively displaying (not during fade-out).
// Game logic (fly/hop/turnaround guards) uses this so the bird is free to move
// as soon as the reading window ends, even while the fade-out is still running.
bool isBubbleActive() { return g_endTime != 0; }

// No IsWindowVisible guard — must work before the window is shown (flash fix).
static void reposition() {
    int birdCenterX = bird.winX + W / 2;

    int bx = birdCenterX + 50;
    g_cornerLeft = true;

    int by = bird.winY - g_contentH + 30;
    g_isAbove = (by >= 0);

    if (!g_isAbove)
        by = bird.winY + H;

    if (bx + g_bubbleW > bird.screenW - 10) {
        bx = birdCenterX - g_bubbleW;
        g_cornerLeft = false;
    }

    bx = std::max(10, bx);

    SetWindowPos(g_hwnd, HWND_TOPMOST, bx, by, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static LRESULT CALLBACK BubbleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT cr; GetClientRect(hwnd, &cr);
        int bw = cr.right;

        int rrTop = g_isAbove ? 0          : TAIL_H;
        int rrBot = g_isAbove ? g_contentH : TAIL_H + g_contentH;

        // 1. Chroma fill — transparent background
        {
            HBRUSH cb = CreateSolidBrush(CHROMA);
            FillRect(hdc, &cr, cb);
            DeleteObject(cb);
        }

        // 2. Thought-bubble circles — three shrinking circles pointing toward the bird.
        //    Drawn before the roundrect so the roundrect covers any overlap at the base.
        {
            HBRUSH  cream    = CreateSolidBrush(BUBBLE_BG);
            HPEN    border   = CreatePen(PS_SOLID, 1, BUBBLE_BORDER);
            HGDIOBJ oldBrush = SelectObject(hdc, cream);
            HGDIOBJ oldPen   = SelectObject(hdc, border);

            // All 4 orientations reduce to sign-flips on the same offsets.
            // g_isAbove:    sy=+1 → circles go down; sy=-1 → circles go up
            // g_cornerLeft: sx=+1 → inset from left;  sx=-1 → inset from right
            int sy      = g_isAbove    ? +1 : -1;
            int sx      = g_cornerLeft ? +1 : -1;
            int anchorY = g_isAbove    ? rrBot : TAIL_H;
            int anchorX = g_cornerLeft ? 0     : bw;

            // Draw smallest first so the largest (drawn last) wins the overlap at the bubble edge
            for (int i = 2; i >= 0; i--) {
                int cx = anchorX + sx * C_DX[i];
                int cy = anchorY + sy * (C_OFFSET + C_DY[i]);
                Ellipse(hdc, cx - C_R[i], cy - C_R[i], cx + C_R[i], cy + C_R[i]);
            }

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(cream);
            DeleteObject(border);
        }

        // 3. RoundRect — cream fill + border, drawn after circles to cover the base overlap
        {
            HBRUSH  fill      = CreateSolidBrush(BUBBLE_BG);
            HPEN    border    = CreatePen(PS_SOLID, 2, BUBBLE_BORDER);
            HGDIOBJ oldFill   = SelectObject(hdc, fill);
            HGDIOBJ oldBorder = SelectObject(hdc, border);
            RoundRect(hdc, 1, rrTop + 1, bw - 1, rrBot - 1, 14, 14);
            SelectObject(hdc, oldFill);
            SelectObject(hdc, oldBorder);
            DeleteObject(fill);
            DeleteObject(border);
        }

        // 4. Text — typewriter reveal using the shared cached font
        {
            HFONT oldFont = (HFONT)SelectObject(hdc, g_font);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, BUBBLE_TEXT);
            std::string visible = g_text.substr(0, g_visibleChars);
            RECT tr = { PAD, rrTop + PAD + 3, bw - PAD, rrBot - PAD };
            DrawText(hdc, visible.c_str(), -1, &tr, DT_WORDBREAK | DT_LEFT);
            SelectObject(hdc, oldFont);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void initBubble(HINSTANCE hInstance) {
    // Font created once here and reused for both text measurement and painting.
    g_font = CreateFont(
        BUBBLE_FONT_SIZE, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI"
    );

    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = BubbleWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = "BirdBubble";
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        "BirdBubble", "",
        WS_POPUP,
        0, 0, 100, 50,
        NULL, NULL, hInstance, NULL
    );
    // LWA_COLORKEY keeps chroma transparent; LWA_ALPHA controls overall opacity for fade.
    SetLayeredWindowAttributes(g_hwnd, CHROMA, 0, LWA_COLORKEY | LWA_ALPHA);
}

void showBubble(const std::string& text, int durationMs) {
    if (!g_hwnd) return;

    // Truncate at a word boundary if the message exceeds MAX_TEXT_LEN
    std::string t = text;
    if ((int)t.size() > MAX_TEXT_LEN) {
        size_t cut = t.rfind(' ', MAX_TEXT_LEN - 3);
        if (cut == std::string::npos) cut = MAX_TEXT_LEN - 3;
        t = t.substr(0, cut) + "...";
    }

    g_text         = t;
    g_visibleChars = 0;
    uint32_t now   = GetTickCount();
    g_nextCharTime = now + CHAR_MS;

    int typeMs = (int)t.size() * CHAR_MS;
    if (durationMs < 0)
        durationMs = POST_REVEAL_MS;
    g_endTime = now + (uint32_t)typeMs + (uint32_t)durationMs;

    // Measure text size using the cached font
    HDC   hdc     = GetDC(g_hwnd);
    HFONT oldFont = (HFONT)SelectObject(hdc, g_font);
    RECT  r       = { 0, 0, MAX_W - PAD * 2, 1000 };
    DrawText(hdc, t.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT);
    SelectObject(hdc, oldFont);
    ReleaseDC(g_hwnd, hdc);

    g_bubbleW  = r.right  + PAD * 2 + 4;
    g_contentH = r.bottom + PAD * 2 + 4;
    g_windowH  = g_contentH + TAIL_H;

    g_fadingOut  = false;
    g_fadeInDone = false;
    g_fadeStart  = now;  // fade-in starts now at alpha=0
    SetLayeredWindowAttributes(g_hwnd, CHROMA, 0, LWA_COLORKEY | LWA_ALPHA);

    // reposition() runs without the IsWindowVisible guard, so it moves the window to
    // the correct screen position while it is still hidden — no top-left flash.
    reposition();
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_bubbleW, g_windowH,
                 SWP_NOMOVE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, NULL, TRUE);
    UpdateWindow(g_hwnd);
}

void hideBubble() {
    if (g_hwnd) {
        SetLayeredWindowAttributes(g_hwnd, CHROMA, 0, LWA_COLORKEY | LWA_ALPHA);
        ShowWindow(g_hwnd, SW_HIDE);
    }
    g_endTime    = 0;
    g_fadingOut  = false;
    g_fadeInDone = false;
}

void tickBubble() {
    if (!g_endTime && !g_fadingOut) return;
    uint32_t now = GetTickCount();

    if (g_fadingOut) {
        uint32_t e = now - g_fadeStart;
        if (e >= (uint32_t)FADE_MS) {
            ShowWindow(g_hwnd, SW_HIDE);
            g_fadingOut = false;
            return;
        }
        BYTE a = (BYTE)(255 - (int)(e * 255 / FADE_MS));
        SetLayeredWindowAttributes(g_hwnd, CHROMA, a, LWA_COLORKEY | LWA_ALPHA);
        return;
    }

    // Fade in — ramp alpha from 0 to 255 over FADE_MS, then stop calling SLWA
    if (!g_fadeInDone) {
        uint32_t fi = now - g_fadeStart;
        if (fi < (uint32_t)FADE_MS) {
            BYTE a = (BYTE)(fi * 255 / FADE_MS);
            SetLayeredWindowAttributes(g_hwnd, CHROMA, a, LWA_COLORKEY | LWA_ALPHA);
        } else {
            SetLayeredWindowAttributes(g_hwnd, CHROMA, 255, LWA_COLORKEY | LWA_ALPHA);
            g_fadeInDone = true;
        }
    }

    // Reading window expired → start fade out
    if (now >= g_endTime) {
        g_endTime   = 0;
        g_fadingOut = true;
        g_fadeStart = now;
        return;
    }

    // Typewriter
    if (g_visibleChars < (int)g_text.size() && now >= g_nextCharTime) {
        g_visibleChars++;
        g_nextCharTime = now + CHAR_MS;
        InvalidateRect(g_hwnd, NULL, FALSE);
        UpdateWindow(g_hwnd);
    }

    reposition();
}
