#pragma once
#include "types.h"

std::string pickAnimation();
int         pickVariant(const std::string& type);
float       currentFootY();
int         animFrameCount(const std::string& type);
void        loadWeights();
void        loadAnimations();
void        presentFrame(const Animation& anim, int fIdx, Facing renderFacing, int winX, int winY);
