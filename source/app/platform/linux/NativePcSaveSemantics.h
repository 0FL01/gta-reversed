#pragma once

#include "NativePcSaveCodec.h"

#include <cstdint>
#include <string>
#include <vector>

struct NativePcPathSwitch {
    float MinX = 0.0f, MaxX = 0.0f;
    float MinY = 0.0f, MaxY = 0.0f;
    float MinZ = 0.0f, MaxZ = 0.0f;
    bool Off = false;
    bool Cars = false;
    bool operator==(const NativePcPathSwitch&) const = default;
};

struct NativePcSemanticState {
    std::vector<NativePcPathSwitch> PathSwitches;
    bool operator==(const NativePcSemanticState&) const = default;
};

enum class NativePcSemanticStatus : std::uint8_t { Ok, InvalidInput, InvalidBlock, Overflow };

// First semantic adapter over the original PC envelope. The Paths block is
// exactly CPathFind::Save/Load: uint32 count followed by count raw 0x1C
// CNodesSwitchedOnOrOff records. Other original blocks remain opaque and are
// preserved by NativePcSaveCodec rather than interpreted here.
class NativePcSaveSemantics {
public:
    static constexpr std::size_t PathSwitchCapacity = 64;
    static constexpr std::size_t PathSwitchBytes = 0x1C;

    static NativePcSemanticStatus ExportPaths(const NativePcSemanticState&,
        NativePcSaveImage&, std::string& error);
    static NativePcSemanticStatus ImportPaths(const NativePcSaveImage&,
        NativePcSemanticState&, std::string& error);
};
