#include "flight.h"
#include "ledge.h"
#include "animation.h"
#include <cmath>
#include <algorithm>

static const int   FLY_SCAN_ATTEMPTS    = 15;    // random X positions tried when looking for a landing spot
static const int   FLY_FALLBACK_ATTEMPTS = 10;   // attempts to find a distant-enough taskbar fallback position
static const float FLY_CTRL_HEIGHT_MAX  = 200.0f; // maximum arc height above the midpoint
static const float FLY_CTRL_HEIGHT_FRAC = 0.3f;  // arc height as a fraction of flight distance
static const float FLY_PIXELS_PER_LOOP  = 700.0f; // screen pixels of distance per one fly-loop cycle

float flightGlobalT(const std::string& phase, int fIdx, int fTotal) {
    float frac = fTotal > 1 ? (float)fIdx / (fTotal - 1) : 1.0f;

    if (phase == "takeoff" || phase == "takeoff_turnaround") {
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
    // Cubic ease-in-out
    float te = t < 0.5f ? 4*t*t*t : 1.0f - powf(-2*t + 2, 3) / 2.0f;
    outX = (int)((1-te)*(1-te)*bird.flyState.startX + 2*(1-te)*te*bird.flyState.ctrlX + te*te*bird.flyState.destX);
    outY = (int)((1-te)*(1-te)*bird.flyState.startY + 2*(1-te)*te*bird.flyState.ctrlY + te*te*bird.flyState.destY);
}

void planFly() {
    bird.flyState.startX = (float)bird.winX;
    bird.flyState.startY = (float)bird.winY;

    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    // Use idle's foot_y so the landing bezier ends where idle stance expects — no snap on transition.
    float groundFootY = bird.animations.count("idle") && !bird.animations["idle"].empty()
        ? bird.animations["idle"][0].footY : FOOT_Y;

    float dx = 0, dy = 0, dist = 0;
    bool found = false;

    std::uniform_int_distribution<int> xDist(W / 2, bird.screenW - W / 2);
    for (int i = 0; i < FLY_SCAN_ATTEMPTS && !found; i++) {
        int scanX = xDist(bird.rng);
        if (scanX >= bird.winX && scanX < bird.winX + W) continue;

        int ledgeY = findLedgeAtX(scanX);
        if (ledgeY < 0) continue;

        if (!checkLedgeWidth(scanX, ledgeY, LEDGE_MIN_SURFACE_WIDTH)) continue;

        float destX = (float)(scanX - W / 2);
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
        int attempts = 0;
        do {
            bird.flyState.destX = std::uniform_real_distribution<float>(0, (float)(bird.screenW - W))(bird.rng);
            bird.ledgeY = workArea.bottom;
            bird.flyState.destY = (float)(bird.ledgeY - (int)(groundFootY * H));
            dx   = bird.flyState.destX - bird.flyState.startX;
            dy   = bird.flyState.destY - bird.flyState.startY;
            dist = sqrtf(dx * dx + dy * dy);
            attempts++;
        } while (dist < FLY_MIN_DISTANCE && attempts < FLY_FALLBACK_ATTEMPTS);
    }

    float arcH = std::min(FLY_CTRL_HEIGHT_MAX, dist * FLY_CTRL_HEIGHT_FRAC);
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
        int n = std::max(1, (int)((dist - FLY_SHORT_THRESHOLD) / FLY_PIXELS_PER_LOOP));
        bird.flyState.flyTotalFrames = n * flF;
        int total = tfF + bird.flyState.flyTotalFrames + ldF;
        bird.flyState.takeoffEnd   = (float)tfF / total;
        bird.flyState.landingStart = 1.0f - (float)ldF / total;
    }

    bird.flyState.flyFramesPlayed = 0;
    bird.facing = (bird.flyState.destX > bird.flyState.startX) ? FACING_RIGHT : FACING_LEFT;
}

void startFlySequence() {
    Facing preFlyFacing = bird.facing;
    planFly();
    bird.flySequenceActive = true;
    bird.animQueue.push_front("landing");
    if (bird.flyState.flyTotalFrames > 0) bird.animQueue.push_front("fly");
    bool reversed = (preFlyFacing != bird.facing);
    bool hasTT = bird.animations.count("takeoff_turnaround") && !bird.animations["takeoff_turnaround"].empty();
    if (reversed && hasTT) {
        bird.currentType = "takeoff_turnaround";
        bird.facing = preFlyFacing;  // restore — facing toggles on animation end
    } else {
        bird.currentType = "takeoff";
    }
    bird.currentVariantIdx = pickVariant(bird.currentType);
    bird.frameIndex = 0;
}

void checkLedgeValidity() {
    if (!bird.hasLedge || bird.flySequenceActive) return;

    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (now - lastCheck < 2000) return;
    lastCheck = now;

    auto lum = [](uint32_t c) { return (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3; };

    HDC screenDC = GetDC(NULL);
    int changed = 0;
    for (int i = 0; i < LEDGE_VALIDITY_SAMPLES; i++) {
        uint32_t cur = (uint32_t)GetPixel(screenDC, bird.winX + (i + 1) * W / (LEDGE_VALIDITY_SAMPLES + 1), bird.ledgeY + LEDGE_VALIDITY_Y_OFFSET);
        if (abs((int)lum(cur) - (int)lum(bird.ledgeRefColors[i])) > LEDGE_VALIDITY_DRIFT) changed++;
    }
    ReleaseDC(NULL, screenDC);

    if (changed >= LEDGE_VALIDITY_MIN_CHANGES) {
        bird.hasLedge = false;
        bird.animQueue.clear();
        bird.flySequenceActive = false;
        bird.flyState = FlyState{};
        startFlySequence();
    }
}
