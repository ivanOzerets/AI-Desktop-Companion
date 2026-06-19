#pragma once
#include "types.h"

// Posted to bird.hwnd when an API response arrives.
// LPARAM is a heap-allocated LlmResponse* — the handler must delete it.
extern const UINT WM_LLM_RESPONSE;
extern const UINT WM_SLOW_LOOP_DONE;  // posted when slow loop finishes; LPARAM unused

struct LlmResponse {
    std::string speech;  // text shown in speech bubble (may be empty)
    std::string action;  // optional bird command: "fly", "sleep", or empty
    std::string memory;  // fact to persist to identity.md, or empty
};

void initLlm();
void queryLlm(const std::string& userMessage);
bool isLlmPending();
void runSlowLoop();
