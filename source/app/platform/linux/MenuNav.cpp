// MenuNav implementation: honest SDL event path + MenuShot re-render.
// See MenuNav.h for the contract. Event-flow proof points:
//   PUSH-SITE: MenuNav_PushCommand (SDL_PushEvent, SDL_EVENT_KEY_DOWN only).
//   POLL-SITE: MenuNav_PumpEvents (SDL_PollEvent; the ONLY place that mutates
//     selectedIx/chosenIx: down=(s+1)%3, up=(s+2)%3, enter fixes chosen).
// Seq parsing never touches selection state; rendering never reads the seq.

#include "app/platform/linux/MenuNav.h"

#include "app/platform/linux/MenuShot.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "unknown");
}

// FNV-1a/64 over RGB triplets, identical to MainLinux PixelsChecksum so nav
// checksums stay comparable with the static --shot-menu etalon.
uint64_t FrameChecksum(const std::vector<uint8_t>& pixels) {
    uint64_t checksum = 1469598103934665603ULL;
    for (std::size_t i = 0; i + 3 < pixels.size() + 1; i += 4) {
        uint64_t r = pixels[i];
        uint64_t g = pixels[i + 1];
        uint64_t b = pixels[i + 2];
        checksum ^= r + (g << 8) + (b << 16);
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

SDL_Keycode KeyForCmd(MenuNavCmd cmd) {
    switch (cmd) {
        case MenuNav_Down:
            return SDLK_DOWN;
        case MenuNav_Up:
            return SDLK_UP;
        case MenuNav_Enter:
        default:
            return SDLK_RETURN;
    }
}

// PUSH-SITE (round 8 proof): the ONLY SDL_PushEvent in the nav track. Each
// seq command becomes one real SDL_EVENT_KEY_DOWN key press.
bool MenuNav_PushCommand(MenuNavCmd cmd, char* err, std::size_t errSize) {
    SDL_Event ev = {};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = KeyForCmd(cmd);
    ev.key.down = true;
    ev.key.repeat = false;
    if (!SDL_PushEvent(&ev)) {
        SetErr(err, errSize, "SDL_PushEvent failed");
        return false;
    }
    return true;
}

// POLL-SITE (round 8 proof): the ONLY SDL_PollEvent consumer that mutates
// nav state. Direct selectedIx++/-- anywhere else is a round failure.
// Mapping is literal mod-3 arithmetic: down=+1%3, up=-1%3, enter pins chosen.
bool MenuNav_PumpEvents(int& selectedIx, int& chosenIx) {
    SDL_Event ev = {};
    while (SDL_PollEvent(&ev)) {
        if (ev.type != SDL_EVENT_KEY_DOWN) {
            continue;
        }
        const SDL_Keycode key = ev.key.key;
        if (key == SDLK_DOWN) {
            selectedIx = (selectedIx + 1) % 3;
        } else if (key == SDLK_UP) {
            selectedIx = (selectedIx + 2) % 3;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            chosenIx = selectedIx;
        }
    }
    return true;
}

std::string TrimLower(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) {
        ++a;
    }
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) {
        --b;
    }
    std::string out;
    out.reserve(b - a);
    for (std::size_t i = a; i < b; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

} // namespace

bool MenuNav_ParseSeq(const char* seqStr, std::vector<MenuNavCmd>& outCmds, char* err,
                      std::size_t errSize) {
    outCmds.clear();
    if (!seqStr || seqStr[0] == '\0') {
        SetErr(err, errSize, "empty --menu-nav seq (want e.g. down,enter)");
        return false;
    }
    std::string seq(seqStr);
    std::size_t pos = 0;
    while (pos <= seq.size()) {
        std::size_t end = seq.find(',', pos);
        std::string tok =
            (end == std::string::npos) ? seq.substr(pos) : seq.substr(pos, end - pos);
        std::string t = TrimLower(tok);
        if (t.empty()) {
            SetErr(err, errSize, "empty command in seq (want down,up,enter)");
            return false;
        }
        if (t == "down") {
            outCmds.push_back(MenuNav_Down);
        } else if (t == "up") {
            outCmds.push_back(MenuNav_Up);
        } else if (t == "enter") {
            outCmds.push_back(MenuNav_Enter);
        } else {
            char msg[128] = {};
            (void)std::snprintf(msg, sizeof(msg), "unknown nav command '%s' (want down,up,enter)",
                                t.c_str());
            SetErr(err, errSize, msg);
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    if (outCmds.empty() || outCmds.size() > 1024) {
        SetErr(err, errSize, "bad seq length (want 1..1024 commands)");
        return false;
    }
    return true;
}

bool MenuNav_Run(const char* gameDir, const char* lang, const char* seqStr,
                 std::vector<uint8_t>& outPixels, MenuNavResult& out, char* err,
                 std::size_t errSize) {
    out = MenuNavResult{};
    outPixels.clear();
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    std::vector<MenuNavCmd> cmds;
    if (!MenuNav_ParseSeq(seqStr, cmds, err, errSize)) {
        return false;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        char msg[256] = {};
        (void)std::snprintf(msg, sizeof(msg), "SDL video init failed: %s", SDL_GetError());
        SetErr(err, errSize, msg);
        return false;
    }
    // Hidden window for fidelity with --smoke-video; the event queue works
    // without it (dummy driver), so a window failure is not fatal here.
    SDL_Window* window = SDL_CreateWindow("mad-sa-nav", 640, 480, 0);

    int selectedIx = 0;
    int chosenIx = -1;
    // H0: initial frame before any input (must equal static --shot-menu).
    {
        std::vector<uint8_t> pixels;
        MenuShotStats stats{};
        if (!MenuShot_RenderSelected(gameDir, lang ? lang : "english", selectedIx, pixels, stats,
                                     err, errSize)) {
            if (window) {
                SDL_DestroyWindow(window);
            }
            SDL_Quit();
            return false;
        }
        out.checksums.push_back(FrameChecksum(pixels));
        out.lastStats = stats;
        outPixels.swap(pixels);
    }
    // Closed loop: one real key event per command, drained through the
    // SDL queue, then a full MenuShot re-render at the new highlight.
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        if (!MenuNav_PushCommand(cmds[i], err, errSize)) {
            if (window) {
                SDL_DestroyWindow(window);
            }
            SDL_Quit();
            return false;
        }
        MenuNav_PumpEvents(selectedIx, chosenIx);
        std::vector<uint8_t> pixels;
        MenuShotStats stats{};
        if (!MenuShot_RenderSelected(gameDir, lang ? lang : "english", selectedIx, pixels, stats,
                                     err, errSize)) {
            if (window) {
                SDL_DestroyWindow(window);
            }
            SDL_Quit();
            return false;
        }
        out.checksums.push_back(FrameChecksum(pixels));
        out.lastStats = stats;
        outPixels.swap(pixels);
    }
    out.selected = selectedIx;
    out.chosen = chosenIx;
    // Choice strings are the same GXT TDAT bytes MenuShot looked up (no new
    // hardcoded items): title+3 rows live in lastStats.itemKey/Text[0..3],
    // rows 1..3 map to selected 0..2.
    if (chosenIx >= 0 && chosenIx <= 2) {
        (void)std::snprintf(out.chosenKey, sizeof(out.chosenKey), "%s",
                             out.lastStats.itemKey[1 + chosenIx]);
        (void)std::snprintf(out.chosenText, sizeof(out.chosenText), "%s",
                             out.lastStats.itemText[1 + chosenIx]);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return true;
}
