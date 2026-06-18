#include "sleep.h"
#include <fstream>
#include <string>

void loadIdentityTimes() {
    std::ifstream f("identity.md");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto tryParse = [](const std::string& s, const std::string& key, int& out) {
            if (s.size() <= key.size() || s.substr(0, key.size()) != key) return;
            std::string val = s.substr(key.size());
            auto colon = val.find(':');
            if (colon == std::string::npos) return;
            int h = std::stoi(val.substr(0, colon));
            int m = std::stoi(val.substr(colon + 1));
            out = h * 60 + m;
        };
        tryParse(line, "bedtime: ", bird.bedtimeMinutes);
        tryParse(line, "risetime: ", bird.risetimeMinutes);
    }
}

int currentTimeMinutes() {
    SYSTEMTIME st; GetLocalTime(&st);
    return st.wHour * 60 + st.wMinute;
}

bool isNighttime() {
    int now = currentTimeMinutes();
    if (bird.bedtimeMinutes > bird.risetimeMinutes)  // spans midnight, e.g. 22:00–07:00
        return now >= bird.bedtimeMinutes || now < bird.risetimeMinutes;
    return now >= bird.bedtimeMinutes && now < bird.risetimeMinutes;
}

int inactivitySeconds() {
    LASTINPUTINFO lii = { sizeof(LASTINPUTINFO) };
    GetLastInputInfo(&lii);
    return (int)((GetTickCount() - lii.dwTime) / 1000);
}

void enterSleep() {
    bird.isSleeping = true;
    bird.animQueue.clear();
    bird.animQueue.push_back("dozing_off");
    bird.animQueue.push_back("sleeping");
}

void wakeUp() {
    if (!bird.isSleeping) return;
    bird.isSleeping = false;
    bird.sleepCooldownEnd = GetTickCount() + 60000;
    bird.animQueue.clear();
    bird.animQueue.push_front("awoken");
}

void updateSleepState() {
    if (bird.flySequenceActive) return;
    uint32_t now = GetTickCount();
    if (bird.isSleeping) {
        // Daytime: any recent input wakes the bird
        if (!isNighttime() && inactivitySeconds() < 5)
            wakeUp();
        // Night: stays asleep until clicked or chat command
    } else {
        if (now < bird.sleepCooldownEnd) return;
        if (isNighttime() || inactivitySeconds() >= SLEEP_INACTIVITY_SECS)
            enterSleep();
    }
}
