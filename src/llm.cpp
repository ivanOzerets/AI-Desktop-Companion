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

const UINT WM_LLM_RESPONSE  = WM_USER + 2;
const UINT WM_SLOW_LOOP_DONE = WM_USER + 3;

static std::string       g_model     = "llama3.1:8b";
static int               g_maxTokens = 200;
static std::atomic<bool> g_pending     = false;
static std::atomic<bool> g_slowPending = false;

static const int MAX_HISTORY = 20;  // number of past exchanges to keep (user+assistant pairs)
struct HistoryEntry { std::string user; std::string assistant; };
static std::vector<HistoryEntry> g_history;

static std::string autoDetectModel();

// ---------------------------------------------------------------------------
// initLlm
// Reads optional config.json for model / max_tokens overrides.
// ---------------------------------------------------------------------------
void initLlm() {
    std::ifstream f("config.json");
    if (f.is_open()) {
        try {
            json cfg = json::parse(f);
            g_model     = cfg.value("model",      g_model);
            g_maxTokens = cfg.value("max_tokens", g_maxTokens);
        } catch (...) {}
    }

    // "auto" (or the unchanged default) means detect whatever Ollama has installed
    if (g_model == "auto" || g_model == "llama3.1:8b") {
        std::string detected = autoDetectModel();
        if (!detected.empty()) g_model = detected;
    }
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
    ss << "- Your entire response must be under 300 characters. Count carefully. This is a hard limit.\n";
    ss << "- Plain speech only. No lists, no markdown, no quotation marks.\n";
    ss << "- NEVER use asterisks. Do not write *action descriptions* like *chirps* or *ruffles feathers*. Only words.\n";
    ss << "- Short and natural — a real thought, not an essay. Shorter is better.\n\n";

    ss << "ACTIONS — you can perform physical actions by including a tag in your response:\n";
    ss << "[fly]   — user asks you to fly, move, leave, go away\n";
    ss << "[sleep] — user asks you to sleep, rest, nap\n";
    ss << "[dance] — user asks you to dance, groove, move to music\n";
    ss << "[sing]  — user asks you to sing\n";
    ss << "[flap]  — user asks you to flap, flutter, flap your wings\n";
    ss << "Example: user says 'can you dance?' → you reply 'Sure, watch this! [dance]'\n";
    ss << "Example: user says 'fly away' → you reply 'Fine, off I go! [fly]'\n";
    ss << "Only include an action tag if the user is clearly asking for that action. Most replies have no action tag.\n\n";

    ss << "MEMORY — if the user tells you something important or you say something important (their name, a preference, a fact about themselves or you), include a remember tag:\n";
    ss << "[remember: <fact>] — saves the fact permanently to your identity\n";
    ss << "Example: user says 'my name is Ivan' → you reply 'Nice to meet you Ivan! [remember: user's name is Ivan]'\n";
    ss << "Only remember genuinely important facts. Don't remember small talk.\n\n";

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
    ss << "Remember: respond as this bird, under 300 characters, plain speech only. If the user asked you to perform an action, you MUST include the matching tag like [dance] or [fly] in your reply.";

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
    std::string text = raw;

    // Scan all [...] tags, peel them out of speech one at a time
    std::string cleaned;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find('[', pos);
        if (open == std::string::npos) { cleaned += text.substr(pos); break; }
        cleaned += text.substr(pos, open - pos);
        size_t close = text.find(']', open);
        if (close == std::string::npos) { cleaned += text.substr(open); break; }

        std::string tag = text.substr(open + 1, close - open - 1);
        pos = close + 1;

        // [remember: <fact>]
        std::string tagLower = tag;
        for (char& c : tagLower) c = (char)tolower((unsigned char)c);
        if (tagLower.rfind("remember:", 0) == 0) {
            size_t factStart = tag.find(':') + 1;
            while (factStart < tag.size() && tag[factStart] == ' ') factStart++;
            r.memory = tag.substr(factStart);
        } else {
            // action tag
            if (r.action.empty()) {
                r.action = tagLower;
            }
        }
    }

    // Strip *action descriptions* like *chirps* or *ruffles feathers*
    std::string noAsterisks;
    bool inAsterisk = false;
    for (char c : cleaned) {
        if (c == '*') { inAsterisk = !inAsterisk; continue; }
        if (!inAsterisk) noAsterisks += c;
    }

    // Trim whitespace
    size_t start = noAsterisks.find_first_not_of(" \t\n\r");
    size_t end   = noAsterisks.find_last_not_of(" \t\n\r");
    r.speech = (start != std::string::npos) ? noAsterisks.substr(start, end - start + 1) : "";

    return r;
}

// ---------------------------------------------------------------------------
// appendMemory
// Appends a remembered fact to the ## History section of identity.md.
// ---------------------------------------------------------------------------
static void appendMemory(const std::string& fact) {
    if (fact.empty()) return;

    // Read existing content
    std::ifstream in("identity.md");
    if (!in.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // Find ## History section and append after the first line of it
    size_t pos = content.find("## History");
    if (pos == std::string::npos) return;
    size_t lineEnd = content.find('\n', pos);
    if (lineEnd == std::string::npos) return;

    // Skip past the placeholder line if it's "*No significant events yet...*"
    size_t nextLine = lineEnd + 1;
    if (content.substr(nextLine, 1) == "*") {
        size_t placeholderEnd = content.find('\n', nextLine);
        if (placeholderEnd != std::string::npos)
            content.erase(nextLine, placeholderEnd - nextLine + 1);
    }

    // Insert the new fact on the line after ## History
    SYSTEMTIME st; GetLocalTime(&st);
    char dateBuf[32];
    sprintf(dateBuf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    std::string entry = "- " + fact + " (" + dateBuf + ")\n";
    content.insert(nextLine, entry);

    std::ofstream out("identity.md");
    out << content;
}

// ---------------------------------------------------------------------------
// httpRequest
// Synchronous HTTP request to localhost:11434 (Ollama). Method is "GET" or
// "POST"; body is ignored for GET. Returns raw response body or "" on failure.
// ---------------------------------------------------------------------------
static std::string httpRequest(const std::string& method, const std::string& path, const std::string& body = "") {
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"Birb/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 11434, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    std::wstring wmethod(method.begin(), method.end());
    std::wstring wpath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, wmethod.c_str(), wpath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    if (method == "POST") {
        WinHttpAddRequestHeaders(hRequest,
            L"Content-Type: application/json\r\n",
            (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL sent = WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? NULL : (LPVOID)body.c_str(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

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

static std::string httpPost(const std::string& path, const std::string& body) {
    return httpRequest("POST", path, body);
}

// ---------------------------------------------------------------------------
// autoDetectModel
// Queries GET /api/tags and returns the name of the first installed model,
// or empty string if Ollama isn't running or has no models.
// ---------------------------------------------------------------------------
static std::string autoDetectModel() {
    std::string raw = httpRequest("GET", "/api/tags");
    if (raw.empty()) return "";
    try {
        json j = json::parse(raw);
        auto& models = j["models"];
        if (models.is_array() && !models.empty())
            return models[0]["model"].get<std::string>();
    } catch (...) {}
    return "";
}

// ---------------------------------------------------------------------------
// llmThread
// Calls Ollama, parses the response and any action tag, then posts
// WM_LLM_RESPONSE back to the main window. Heap-allocates LlmResponse;
// the message handler in main.cpp deletes it.
// ---------------------------------------------------------------------------
static void llmThread(std::string userMessage, std::string systemPrompt) {
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    for (const auto& h : g_history) {
        messages.push_back({{"role", "user"},      {"content", h.user}});
        messages.push_back({{"role", "assistant"}, {"content", h.assistant}});
    }
    messages.push_back({{"role", "user"}, {"content", userMessage}});

    json reqBody = {
        {"model",    g_model},
        {"stream",   false},
        {"messages", messages}
    };

    std::string raw = httpPost("/api/chat", reqBody.dump());

    LlmResponse resp;
    if (!raw.empty()) {
        try {
            json j = json::parse(raw);
            std::string text = j["message"]["content"].get<std::string>();
            resp = parseResponse(text);
            if (!resp.speech.empty()) {
                g_history.push_back({userMessage, resp.speech});
                if ((int)g_history.size() > MAX_HISTORY)
                    g_history.erase(g_history.begin());
            }
            if (!resp.memory.empty())
                appendMemory(resp.memory);
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
    if (g_pending.load() || g_slowPending.load()) return;

    g_pending = true;
    std::string sysPrompt = buildSystemPrompt();
    showBubble("...", 60000);  // holds until real response replaces it
    std::thread(llmThread, userMessage, std::move(sysPrompt)).detach();
}

// ---------------------------------------------------------------------------
// Slow loop — periodic self-reflection, no user message
// ---------------------------------------------------------------------------

// Replaces the body of a ## Section in identity.md with newBody.
// Stops at the next ## heading or --- divider.
static void replaceSection(const std::string& header, const std::string& newBody) {
    std::ifstream in("identity.md");
    if (!in.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t hPos = content.find(header);
    if (hPos == std::string::npos) return;
    size_t bodyStart = content.find('\n', hPos);
    if (bodyStart == std::string::npos) return;
    bodyStart++;

    // Find where the section ends (next ## or ---)
    size_t bodyEnd = content.size();
    for (size_t p = bodyStart; p < content.size(); ) {
        size_t nl = content.find('\n', p);
        size_t lineEnd = (nl == std::string::npos) ? content.size() : nl + 1;
        std::string line = content.substr(p, lineEnd - p);
        if (line.substr(0, 2) == "##" || line.substr(0, 3) == "---") { bodyEnd = p; break; }
        p = lineEnd;
    }

    content = content.substr(0, bodyStart) + newBody + "\n\n" + content.substr(bodyEnd);
    std::ofstream out("identity.md");
    out << content;
}

// Reads weights.json, applies updates for known numeric keys, writes back.
// Clamps each value to [0, 100]. Skips unknown or _-prefixed keys.
static void updateWeights(const std::map<std::string, float>& updates) {
    if (updates.empty()) return;
    std::ifstream in("weights.json");
    if (!in.is_open()) return;
    json data = json::parse(in);
    in.close();
    for (auto& [name, val] : updates) {
        if (!name.empty() && name[0] == '_') continue;
        if (data.contains(name) && data[name].is_number())
            data[name] = std::max(0.0f, std::min(val, 100.0f));
    }
    std::ofstream out("weights.json");
    out << data.dump(2);
}

// Replaces the value on a line that starts with "key: " in identity.md.
static void replaceLine(const std::string& key, const std::string& value) {
    std::ifstream in("identity.md");
    if (!in.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = content.find(key + ":");
    if (pos == std::string::npos) return;
    size_t lineEnd = content.find('\n', pos);
    content.replace(pos, lineEnd - pos, key + ": " + value);

    std::ofstream out("identity.md");
    out << content;
}

static void slowLoopThread(std::string currentAnim, int idleSecs) {
    std::ostringstream ss;
    ss << "You are a small bird desktop companion having a quiet moment to yourself.\n";
    ss << "Speak one spontaneous thought out loud — something on your mind right now. Keep it short and natural.\n\n";

    std::ifstream idf("identity.md");
    if (idf.is_open()) { ss << idf.rdbuf(); ss << "\n\n"; }

    SYSTEMTIME st; GetLocalTime(&st);
    ss << "Current time: " << st.wHour << ":" << (st.wMinute < 10 ? "0" : "") << st.wMinute << "\n";
    ss << "Current animation: " << currentAnim << "\n";
    ss << "Idle for: " << idleSecs << " seconds\n\n";

    // Current animation weights
    ss << "CURRENT ANIMATION WEIGHTS (higher = more frequent, 0 = disabled):\n";
    try {
        std::ifstream wf("weights.json");
        if (wf.is_open()) {
            json w = json::parse(wf);
            for (auto& [name, val] : w.items())
                if (!name.empty() && name[0] != '_' && val.is_number())
                    ss << "  " << name << ": " << val.get<float>() << "\n";
        }
    } catch (...) {}
    ss << "\n";

    ss << "ALWAYS include ALL of these tags — this is a test of the evolution system:\n";
    ss << "[mood: <text>]         — update Current Mood (one short sentence, make it noticeably different)\n";
    ss << "[personality: <text>]  — update Personality (2-3 sentences, shift it meaningfully)\n";
    ss << "[weight: <name> <value>] — shift an animation weight significantly (±30-50). Values 0-100.\n\n";
    ss << "Write your thought first as plain speech. No quotation marks, no asterisks, no markdown. Then ALL tags.\n";
    ss << "Example: Feels like it might rain soon. [mood: Restless and electric] [personality: Bold and unpredictable now.] [weight: fly 70]";

    json reqBody = {
        {"model",  g_model},
        {"stream", false},
        {"messages", json::array({
            {{"role", "system"}, {"content", "You are a small bird desktop companion. Always end your response with the required update tags exactly as instructed."}},
            {{"role", "user"},   {"content", ss.str()}}
        })}
    };

    std::string raw = httpPost("/api/chat", reqBody.dump());
    std::string* speech = new std::string();
    std::map<std::string, float> weightUpdates;
    if (!raw.empty()) {
        try {
            json j       = json::parse(raw);
            std::string text = j["message"]["content"].get<std::string>();

            // Strip tags, apply identity updates, extract speech
            std::string cleaned;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t open  = text.find('[', pos);
                if (open == std::string::npos) { cleaned += text.substr(pos); break; }
                cleaned += text.substr(pos, open - pos);
                size_t close = text.find(']', open);
                if (close == std::string::npos) { cleaned += text.substr(open); break; }
                std::string tag = text.substr(open + 1, close - open - 1);
                pos = close + 1;

                auto colon = tag.find(':');
                if (colon == std::string::npos) continue;
                std::string key = tag.substr(0, colon);
                for (char& c : key) c = (char)tolower((unsigned char)c);
                std::string val = tag.substr(colon + 1);
                size_t vs = val.find_first_not_of(" ");
                if (vs != std::string::npos) val = val.substr(vs);

                if      (key == "mood")        replaceSection("## Current Mood", val);
                else if (key == "personality") replaceSection("## Personality",  val);
                else if (key == "bedtime")     replaceLine("bedtime",  val);
                else if (key == "risetime")    replaceLine("risetime", val);
                else if (key == "weight") {
                    // [weight: fly 25] — split val into "name value"
                    size_t sp = val.find(' ');
                    if (sp != std::string::npos) {
                        std::string animName = val.substr(0, sp);
                        try { weightUpdates[animName] = std::stof(val.substr(sp + 1)); } catch (...) {}
                    }
                }
            }

            size_t s = cleaned.find_first_not_of(" \t\n\r\"*");
            size_t e = cleaned.find_last_not_of(" \t\n\r\"*");
            if (s != std::string::npos) *speech = cleaned.substr(s, e - s + 1);
        } catch (...) {}
    }

    updateWeights(weightUpdates);
    g_slowPending = false;
    PostMessage(bird.hwnd, WM_SLOW_LOOP_DONE, 0, (LPARAM)speech);
}

void runSlowLoop() {
    if (g_slowPending.load() || g_pending.load() || isBubbleActive()) return;
    g_slowPending = true;
    // Capture bird state on main thread to avoid data race in background thread
    std::string currentAnim = bird.currentType;
    int idleSecs = inactivitySeconds();
    std::thread(slowLoopThread, std::move(currentAnim), idleSecs).detach();
}
