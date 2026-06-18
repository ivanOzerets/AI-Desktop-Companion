#include "llm.h"
#include "bubble.h"
#include "sleep.h"
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <windows.h>
#include <winhttp.h>
#include "../lib/json/json.hpp"

using json = nlohmann::json;

const UINT WM_LLM_RESPONSE = WM_USER + 2;

static std::string       g_model     = "llama3.2:3b";
static int               g_maxTokens = 200;
static std::atomic<bool> g_pending   = false;

// ---------------------------------------------------------------------------
// initLlm
// Reads optional config.json for model / max_tokens overrides.
// ---------------------------------------------------------------------------
void initLlm() {
    std::ifstream f("config.json");
    if (!f.is_open()) return;
    try {
        json cfg = json::parse(f);
        g_model     = cfg.value("model",      g_model);
        g_maxTokens = cfg.value("max_tokens", g_maxTokens);
    } catch (...) {}
}

bool isLlmPending() { return g_pending.load(); }

// ---------------------------------------------------------------------------
// buildSystemPrompt
// Combines identity.md with live context and action-tag instructions so the
// model can both speak naturally and signal bird behaviors.
// Must be called from the main thread (reads bird state).
// ---------------------------------------------------------------------------
static std::string buildSystemPrompt() {
    std::ostringstream ss;

    // Hard rules first — small models are most influenced by the start and end of the prompt
    ss << "You are a small bird living on someone's computer screen. "
          "Respond only as this bird — first person, in character, never break character.\n\n";

    ss << "STRICT FORMAT RULES:\n";
    ss << "- Your entire response must be under " << g_maxTokens << " characters. Count carefully. This is a hard limit.\n";
    ss << "- Plain speech only. No lists, no markdown, no asterisks, no quotation marks.\n";
    ss << "- Short and natural — a real thought, not an essay. Shorter is better.\n\n";

    ss << "ACTIONS (optional — only use when clearly appropriate):\n";
    ss << "If the user asks you to move/leave/fly/go away, put [fly] on the very first line, then your speech.\n";
    ss << "If the user asks you to sleep/rest/nap, put [sleep] on the very first line, then your speech.\n";
    ss << "Most replies need no action tag at all.\n\n";

    // Personality and history from identity.md
    std::ifstream idf("identity.md");
    if (idf.is_open()) {
        ss << "YOUR PERSONALITY AND HISTORY:\n";
        ss << idf.rdbuf();
        ss << "\n\n";
    }

    // Live context
    SYSTEMTIME st; GetLocalTime(&st);
    ss << "CURRENT SITUATION:\n";
    ss << "Time: "            << st.wHour << ":" << (st.wMinute < 10 ? "0" : "") << st.wMinute << "\n";
    ss << "What you're doing: " << bird.currentType << "\n";
    ss << "Asleep: "          << (bird.isSleeping ? "yes" : "no") << "\n";
    ss << "Idle for: "        << inactivitySeconds() << " seconds\n\n";

    // Restate the limit at the end — recency matters for small models
    ss << "Remember: respond as this bird, under " << g_maxTokens << " characters, plain speech only.";

    return ss.str();
}

// ---------------------------------------------------------------------------
// parseResponse
// Checks whether the model prefixed its reply with an action tag like [fly].
// Strips the tag and leading whitespace from speech, leaves action empty if
// no tag is found.
// ---------------------------------------------------------------------------
static LlmResponse parseResponse(const std::string& raw) {
    LlmResponse r;
    r.speech = raw;

    if (!raw.empty() && raw[0] == '[') {
        size_t close = raw.find(']');
        if (close != std::string::npos) {
            r.action = raw.substr(1, close - 1);
            // lowercase the action so "[Fly]" works too
            for (char& c : r.action) c = (char)tolower((unsigned char)c);
            // strip leading whitespace/newlines from the speech portion
            size_t start = raw.find_first_not_of(" \t\n\r", close + 1);
            r.speech = (start != std::string::npos) ? raw.substr(start) : "";
        }
    }

    return r;
}

// ---------------------------------------------------------------------------
// httpPost
// Synchronous HTTP POST to localhost:11434 (Ollama). Returns the raw response
// body, or empty string on connection failure (Ollama not running).
// Runs on the worker thread.
// ---------------------------------------------------------------------------
static std::string httpPost(const std::string& path, const std::string& body) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"Birb/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    // Port 11434, plain HTTP (no TLS flag)
    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 11434, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    std::wstring wpath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);  // no WINHTTP_FLAG_SECURE — plain HTTP
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/json\r\n",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);

    if (sent) WinHttpReceiveResponse(hRequest, NULL);

    DWORD bytesRead = 0;
    char buf[4096];
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        result += buf;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ---------------------------------------------------------------------------
// llmThread
// Calls Ollama, parses the response and any action tag, then posts
// WM_LLM_RESPONSE back to the main window. Heap-allocates LlmResponse;
// the message handler in main.cpp deletes it.
// ---------------------------------------------------------------------------
static void llmThread(std::string userMessage, std::string systemPrompt) {
    json reqBody = {
        {"model",  g_model},
        {"stream", false},
        {"messages", json::array({
            {{"role", "system"}, {"content", systemPrompt}},
            {{"role", "user"},   {"content", userMessage}}
        })}
    };

    std::string raw = httpPost("/api/chat", reqBody.dump());

    LlmResponse resp;
    if (!raw.empty()) {
        try {
            json j = json::parse(raw);
            std::string text = j["message"]["content"].get<std::string>();
            resp = parseResponse(text);
        } catch (...) {
            resp.speech = "(parse error)";
        }
    } else {
        resp.speech = "Ollama isn't running — start it and try again.";
    }

    g_pending = false;

    LlmResponse* heapResp = new LlmResponse(std::move(resp));
    PostMessage(bird.hwnd, WM_LLM_RESPONSE, 0, (LPARAM)heapResp);
}

// ---------------------------------------------------------------------------
// queryLlm
// Called from the main thread. Shows "..." placeholder immediately and
// fires the worker thread. Ignores calls while one is already in flight.
// ---------------------------------------------------------------------------
void queryLlm(const std::string& userMessage) {
    if (g_pending.load()) return;

    g_pending = true;
    std::string sysPrompt = buildSystemPrompt();
    showBubble("...");
    std::thread(llmThread, userMessage, std::move(sysPrompt)).detach();
}
