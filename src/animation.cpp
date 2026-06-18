#define STB_IMAGE_IMPLEMENTATION
#include "animation.h"
#include <fstream>
#include <algorithm>
#include "../lib/json/json.hpp"
#include "../lib/stb_image.h"

using json = nlohmann::json;

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

// Scale the current frame into bird.staging, optionally flip, then push to window.
void presentFrame(const Animation& anim, int fIdx, Facing renderFacing, int winX, int winY) {
    const Frame& f = anim.frames[fIdx];

    for (int y = 0; y < H; y++) {
        int sy = f.y + y * f.h / H;
        uint32_t* row = bird.staging + y * W;
        for (int x = 0; x < W; x++) {
            int sx = f.x + x * f.w / W;
            row[x] = anim.pixels[sy * anim.sheetW + sx];
        }
    }

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
