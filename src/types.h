#pragma once
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <random>
#include <cstdint>

const int TARGET_FPS             = 60;
const int ANIM_FPS               = 12;
const int FLY_ANIM_FPS           = 18;
const int W                      = 215;
const int H                      = 215;
const int PET_MIN_MOVE_PX        = 2;
const int PET_REVERSAL_COUNT     = 3;
const uint32_t PET_WINDOW_MS     = 1000;
const float FLY_SHORT_THRESHOLD  = 500.0f;
const float FLY_MIN_DISTANCE     = 300.0f;
const int   HOP_STEP_PX          = 8;
const int   SLEEP_INACTIVITY_SECS = 180;
const float TAKEOFF_LIFTOFF_FRAC  = 0.25f;
const float LANDING_TOUCHDOWN_FRAC = 0.65f;
const int   LEDGE_MIN_AIR_ROWS    = 60;
const int   LEDGE_EDGE_THRESHOLD  = 15;
const int   LEDGE_UNIFORM_THRESHOLD = 15;
const int   LEDGE_MIN_SURFACE_WIDTH = W;

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
    float takeoffEnd    = 0.5f;
    float landingStart  = 0.5f;
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

    // Sleep / wake
    bool isSleeping = false;
    int bedtimeMinutes  = 22 * 60;
    int risetimeMinutes = 7 * 60;
    uint32_t sleepCooldownEnd = 0;
};

extern BirdState bird;
