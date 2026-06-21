#include "spotlight.h"
#include "llm.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

static HWND    g_hwnd     = NULL;
static HWND    g_editHwnd = NULL;
static HBRUSH  g_bgBrush  = NULL;
static HFONT   g_font     = NULL;
static WNDPROC g_editProc = NULL;

static const int      SPOT_W      = 360;
static const int      SPOT_H      = 52;
static const int      SPOT_PAD    = 14;
static const COLORREF SPOT_BG     = RGB(255, 252, 230);
static const COLORREF SPOT_BORDER = RGB(160, 148, 110);
static const COLORREF SPOT_TEXT   = RGB(50, 42, 32);

static void handleCommand(const std::string& raw) {
    for (char c : raw) if (!isspace((unsigned char)c)) { queryLlm(raw); return; }
}

static LRESULT CALLBACK editSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            char buf[512] = {};
            GetWindowText(hwnd, buf, sizeof(buf));
            ShowWindow(g_hwnd, SW_HIDE);
            handleCommand(buf);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            ShowWindow(g_hwnd, SW_HIDE);
            return 0;
        }
        if (wp == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DWORD selStart, selEnd;
            SendMessage(hwnd, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
            if (selStart == selEnd && selStart > 0) {
                char buf[512] = {};
                GetWindowText(hwnd, buf, sizeof(buf));
                int pos = (int)selStart;
                while (pos > 0 && buf[pos - 1] == ' ') pos--;
                while (pos > 0 && buf[pos - 1] != ' ') pos--;
                SendMessage(hwnd, EM_SETSEL, pos, selStart);
                SendMessage(hwnd, EM_REPLACESEL, TRUE, (LPARAM)"");
            }
            return 0;
        }
    }
    // Block WM_CHAR for Enter and Ctrl+Backspace — the single-line EDIT control
    // beeps when it receives characters it can't insert (0x0D, 0x7F).
    if (msg == WM_CHAR && (wp == '\r' || wp == 0x7F)) return 0;

    return CallWindowProc(g_editProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK spotlightWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_bgBrush);

        HPEN    pen      = CreatePen(PS_SOLID, 2, SPOT_BORDER);
        HGDIOBJ oldPen   = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, 1, 1, rc.right - 1, rc.bottom - 1, 14, 14);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);

        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, SPOT_BG);
        SetTextColor(hdc, SPOT_TEXT);
        return (LRESULT)g_bgBrush;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void initSpotlight(HINSTANCE hInstance, HWND mainHwnd) {
    g_bgBrush = CreateSolidBrush(SPOT_BG);
    g_font    = CreateFont(
        20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI"
    );

    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = spotlightWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = "BirdSpotlight";
    RegisterClassEx(&wc);

    int sx = (GetSystemMetrics(SM_CXSCREEN) - SPOT_W) / 2;
    int sy = GetSystemMetrics(SM_CYSCREEN) * 2 / 5;

    g_hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "BirdSpotlight", "",
        WS_POPUP,
        sx, sy, SPOT_W, SPOT_H,
        mainHwnd, NULL, hInstance, NULL
    );

    // Clip to rounded rectangle so corners are transparent
    HRGN rgn = CreateRoundRectRgn(0, 0, SPOT_W, SPOT_H, 14, 14);
    SetWindowRgn(g_hwnd, rgn, FALSE);

    // Single-line text input, no border — background set via WM_CTLCOLOREDIT
    int ey = (SPOT_H - 26) / 2;
    g_editHwnd = CreateWindowEx(
        0,
        "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        SPOT_PAD, ey, SPOT_W - SPOT_PAD * 2, 26,
        g_hwnd, NULL, hInstance, NULL
    );
    SendMessage(g_editHwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessage(g_editHwnd, EM_SETCUEBANNER, TRUE, (LPARAM)L"talk to birb...");

    // Subclass to intercept Enter and Escape
    g_editProc = (WNDPROC)SetWindowLongPtr(g_editHwnd, GWLP_WNDPROC, (LONG_PTR)editSubclassProc);
}

void toggleSpotlight() {
    if (!g_hwnd) return;
    if (IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }
    // Re-center each time it opens (multi-monitor friendlier)
    int sx = (GetSystemMetrics(SM_CXSCREEN) - SPOT_W) / 2;
    int sy = GetSystemMetrics(SM_CYSCREEN) * 2 / 5;
    SetWindowPos(g_hwnd, HWND_TOPMOST, sx, sy, 0, 0, SWP_NOSIZE);
    SetWindowText(g_editHwnd, "");
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_editHwnd);
}

bool isSpotlightOpen() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}
