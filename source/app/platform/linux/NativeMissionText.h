#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct NativeMissionTextTable {
    std::array<char, 8> Name{};
    std::uint32_t Offset = 0;
    std::vector<std::uint32_t> KeyHashes;
    bool operator==(const NativeMissionTextTable&) const = default;
};

struct NativeMissionTextStyle {
    float ScaleX = 1.0f, ScaleY = 1.0f, WrapX = 640.0f, CentreSize = 640.0f;
    std::array<std::uint8_t, 4> Colour{255,255,255,255}, BackgroundColour{};
    std::array<std::uint8_t, 4> DropShadowColour{0, 0, 0, 255};
    std::int8_t DropShadow = 2;
    bool Justify = false, Centre = false, Background = false, BackgroundOnlyText = false, Proportional = true;
    bool operator==(const NativeMissionTextStyle&) const = default;
};

struct NativeMissionTextDraw {
    std::array<char, 8> Key{};
    float X = 0.0f;
    float Y = 0.0f;
    NativeMissionTextStyle Style;
    bool operator==(const NativeMissionTextDraw&) const = default;
};

class NativeMissionText {
public:
    static constexpr bool RuntimePresentation = false;
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    void BeginFrame();
    NativeScriptServiceResult Select(const std::array<char, 8>& name);
    NativeScriptServiceResult SetCommandsEnabled(bool enabled);
    NativeScriptServiceResult SetDrawBeforeFade(bool enabled);
    NativeScriptServiceResult SetFont(std::int32_t font);
    NativeScriptServiceResult SetStyle(std::uint16_t opcode, const std::array<float, 2>& floats,
        const std::array<std::int32_t, 5>& integers);
    NativeScriptServiceResult Display(float x, float y, const std::array<char, 8>& key);
    const std::vector<NativeMissionTextTable>& Tables() const { return m_Tables; }
    const std::array<char, 8>& Active() const { return m_Active; }
    std::uint64_t Revision() const { return m_Revision; }
    bool CommandsEnabled() const { return m_CommandsEnabled; }
    bool DrawBeforeFade() const { return m_DrawBeforeFade; }
    std::int32_t Font() const { return m_Font; }
    const NativeMissionTextStyle& Style() const { return m_Style; }
    std::span<const NativeMissionTextDraw> Draws() const { return {m_Draws.data(), m_DrawCount}; }

private:
    std::vector<NativeMissionTextTable> m_Tables;
    std::array<char, 8> m_Active{};
    std::uint64_t m_Revision = 0;
    bool m_Loaded = false;
    bool m_CommandsEnabled = false;
    bool m_DrawBeforeFade = false;
    std::int32_t m_Font = 0;
    NativeMissionTextStyle m_Style;
    std::array<NativeMissionTextDraw, 96> m_Draws{};
    std::size_t m_DrawCount = 0;
};
