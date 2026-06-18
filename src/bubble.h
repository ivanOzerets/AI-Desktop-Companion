#pragma once
#include "types.h"

void initBubble(HINSTANCE hInstance);
void showBubble(const std::string& text, int durationMs = -1);
void hideBubble();
void tickBubble();
bool isBubbleActive();
