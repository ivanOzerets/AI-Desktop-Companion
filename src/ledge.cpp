#include "ledge.h"
#include <algorithm>

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
// pixels match the surface color. Returns false if span < minWidth — rejects text, accepts borders.
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
