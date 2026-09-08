// MenuNav: interactive main-menu navigation (round 8, R6f).
// Closed loop input->state->frame: seq commands travel ONLY through the SDL
// event queue (SDL_PushEvent in MenuNav_PushCommand, SDL_PollEvent in
// MenuNav_PumpEvents). No direct selectedIx++/-- outside the poll-site.
// Frames reuse the existing MenuShot renderer with an explicit highlight
// index (MenuShot_RenderSelected); strings/glyphs stay GXT/TXD, no new text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "app/platform/linux/MenuShot.h"

enum MenuNavCmd {
    MenuNav_Down = 0,
    MenuNav_Up = 1,
    MenuNav_Enter = 2,
};

struct MenuNavResult {
    int selected = 0;   // 0..2 (Start Game / Options / Quit Game)
    int chosen = -1;    // -1 until an enter command fixes it
    std::vector<uint64_t> checksums; // H0 (initial sel=0) + one per command
    char chosenKey[16] = {};
    char chosenText[256] = {};
    MenuShotStats lastStats{};
};

// Parses "<seq>" (comma-separated down,up,enter, case-insensitive, spaces ok).
bool MenuNav_ParseSeq(const char* seqStr, std::vector<MenuNavCmd>& outCmds, char* err,
                      std::size_t errSize);

// Runs the full loop: SDL video init (dummy driver ok), H0 render (sel=0),
// then per command { Push -> Pump -> re-render }. outPixels holds the final
// frame (bottom-up RGBA, same layout as MenuShot). Leaves the font TXD
// resident; caller must MenuShot_Shutdown() after writing the TGA.
bool MenuNav_Run(const char* gameDir, const char* lang, const char* seqStr,
                 std::vector<uint8_t>& outPixels, MenuNavResult& out, char* err,
                 std::size_t errSize);
