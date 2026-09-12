// Owned deterministic copy of the source pad sampling phase used by the
// native track. No OS clock, no device, no thread, no RNG, no framework.
// Source profile (parent-reviewed, not a blind copy):
//   Pad.h:38-43 (BUTTON_IS_DOWN / BUTTON_IS_PRESSED / BUTTON_JUST_UP over
//     NewState/OldState full samples);
//   Pad.h:215 (Mode0 GetJump over ButtonSquare);
//   Pad.cpp:402-409 (default deadzone abs<=0.3f else trunc(v*128), no rescale,
//     no inversion/swap);
//   Pad.cpp:880 (Mode0 GetExitVehicle over ButtonTriangle);
//   Pad.cpp:1022 (Mode0 GetSprint over ButtonCross);
//   ControllerState.h (full sticks/buttons; this seam owns only signed left
//     axes MoveX/MoveY and three digital Mode0 buttons).
// Owned integrity: caller-stamped nonzero strictly increasing Seq,
// nondecreasing Tick (equal valid). Identical Seq replay is idempotent with
// no relatch; conflicts/backwards/invalid leave owner and out unchanged.
// Digital default profile: Jump=Square value 1, Sprint=Cross value 2,
// Enter=Triangle value 4; valid Buttons mask 0..7. No movement/gameplay
// implementation, no inversion/swap, no keyboard/remap, no physical hardware,
// no source full device state.
#pragma once

#include <array>
#include <cstdint>

enum class NativeSourcePadStatus {
    Ok,
    DuplicateIdempotent,
    InvalidSequence,
    DuplicateConflict,
    BackwardTick,
    InvalidButtons,
    NonFiniteAxis,
    AxisOutOfRange
};

struct NativeSourcePadSample {
    std::uint64_t Seq{};
    std::uint64_t Tick{};
    std::int16_t MoveX{};
    std::int16_t MoveY{};
    std::uint8_t Buttons{};
    bool operator==(const NativeSourcePadSample&) const = default;
};

struct NativeSourcePadFrame {
    NativeSourcePadSample Sample{};
    std::uint8_t Down{};
    std::uint8_t Pressed{};
    std::uint8_t Released{};
    bool operator==(const NativeSourcePadFrame&) const = default;
};

class NativeSourcePad {
public:
    NativeSourcePad() = default;

    // Complete caller-stamped sample. New Seq Down=Buttons,
    // Pressed=New&~Old, Released=Old&~New. Repeated LastFrame reads are
    // non-consuming. Duplicate exact Seq returns the same old frame with no
    // relatch. All other rejections leave owner and out unchanged.
    NativeSourcePadStatus SubmitSample(const NativeSourcePadSample& sample, NativeSourcePadFrame& out) noexcept;
    const NativeSourcePadFrame& LastFrame() const noexcept;
    bool HasFrame() const noexcept;

    // Default source axis profile: finite [-1,1], abs<=0.3f -> 0 else
    // trunc(v*128) with no rescaling. Failures leave out unchanged.
    static NativeSourcePadStatus QuantizeAxis(float value, std::int16_t& out) noexcept;
    // Fixed ASCII "seq,tick,movex,movey,down,pressed,released", NUL
    // terminated, no newline, no allocation. Bounded types always fit 128.
    static std::array<char, 128> FormatFrame(const NativeSourcePadFrame& frame) noexcept;
    static const char* StatusName(NativeSourcePadStatus status) noexcept;

private:
    NativeSourcePadSample m_OldSample{};
    NativeSourcePadSample m_NewSample{};
    NativeSourcePadFrame m_LastFrame{};
    bool m_HasFrame = false;
};
