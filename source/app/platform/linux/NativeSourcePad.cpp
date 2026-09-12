// See NativeSourcePad.h for the owned source profile and honest limits.
#include "NativeSourcePad.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint8_t kValidButtonsMask = 7;
constexpr std::int16_t kAxisMin = -128;
constexpr std::int16_t kAxisMax = 128;
constexpr float kDeadzone = 0.3f;
constexpr float kAxisScale = 128.0f;
constexpr float kAxisLimit = 1.0f;

} // namespace

NativeSourcePadStatus NativeSourcePad::SubmitSample(const NativeSourcePadSample& sample, NativeSourcePadFrame& out) noexcept {
    // Stable priority: exact-duplicate / conflict gating comes before any
    // other sequence, value, or tick monotonic check.
    if (m_HasFrame && sample.Seq == m_NewSample.Seq) {
        if (sample == m_NewSample) {
            out = m_LastFrame;
            return NativeSourcePadStatus::DuplicateIdempotent;
        }
        return NativeSourcePadStatus::DuplicateConflict;
    }
    if (sample.Seq == 0) {
        return NativeSourcePadStatus::InvalidSequence;
    }
    if (m_HasFrame && sample.Seq < m_NewSample.Seq) {
        return NativeSourcePadStatus::InvalidSequence;
    }
    if ((sample.Buttons & static_cast<std::uint8_t>(~kValidButtonsMask)) != 0) {
        return NativeSourcePadStatus::InvalidButtons;
    }
    if (sample.MoveX < kAxisMin || sample.MoveX > kAxisMax) {
        return NativeSourcePadStatus::AxisOutOfRange;
    }
    if (sample.MoveY < kAxisMin || sample.MoveY > kAxisMax) {
        return NativeSourcePadStatus::AxisOutOfRange;
    }
    if (m_HasFrame && sample.Tick < m_NewSample.Tick) {
        return NativeSourcePadStatus::BackwardTick;
    }

    const std::uint8_t oldButtons = m_HasFrame ? m_NewSample.Buttons : static_cast<std::uint8_t>(0);
    const std::uint8_t newButtons = sample.Buttons;
    NativeSourcePadFrame frame;
    frame.Sample = sample;
    frame.Down = newButtons;
    frame.Pressed = static_cast<std::uint8_t>(newButtons & static_cast<std::uint8_t>(~oldButtons));
    frame.Released = static_cast<std::uint8_t>(oldButtons & static_cast<std::uint8_t>(~newButtons));

    m_OldSample = m_HasFrame ? m_NewSample : NativeSourcePadSample{};
    m_NewSample = sample;
    m_LastFrame = frame;
    m_HasFrame = true;
    out = frame;
    return NativeSourcePadStatus::Ok;
}

const NativeSourcePadFrame& NativeSourcePad::LastFrame() const noexcept {
    return m_LastFrame;
}

bool NativeSourcePad::HasFrame() const noexcept {
    return m_HasFrame;
}

NativeSourcePadStatus NativeSourcePad::QuantizeAxis(float value, std::int16_t& out) noexcept {
    if (!std::isfinite(value)) {
        return NativeSourcePadStatus::NonFiniteAxis;
    }
    if (value < -kAxisLimit || value > kAxisLimit) {
        return NativeSourcePadStatus::AxisOutOfRange;
    }
    if (std::fabs(value) <= kDeadzone) {
        out = 0;
        return NativeSourcePadStatus::Ok;
    }
    const float scaled = value * kAxisScale;
    const float truncated = std::trunc(scaled);
    out = static_cast<std::int16_t>(truncated);
    return NativeSourcePadStatus::Ok;
}

std::array<char, 128> NativeSourcePad::FormatFrame(const NativeSourcePadFrame& frame) noexcept {
    std::array<char, 128> buffer{};
    char* ptr = buffer.data();
    char* const end = buffer.data() + buffer.size();
    auto appendU64 = [&](std::uint64_t value) -> bool {
        const auto result = std::to_chars(ptr, end, value);
        if (result.ec != std::errc()) {
            return false;
        }
        ptr = result.ptr;
        return true;
    };
    auto appendI16 = [&](std::int16_t value) -> bool {
        const auto result = std::to_chars(ptr, end, static_cast<int>(value));
        if (result.ec != std::errc()) {
            return false;
        }
        ptr = result.ptr;
        return true;
    };
    auto appendU8 = [&](std::uint8_t value) -> bool {
        const auto result = std::to_chars(ptr, end, static_cast<unsigned>(value));
        if (result.ec != std::errc()) {
            return false;
        }
        ptr = result.ptr;
        return true;
    };
    auto appendComma = [&]() -> bool {
        if (ptr >= end) {
            return false;
        }
        *ptr++ = ',';
        return true;
    };
    // Bounded types always fit 128; on unexpected exhaustion keep the
    // zero-initialised prefix NUL terminated.
    if (!appendU64(frame.Sample.Seq)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendU64(frame.Sample.Tick)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendI16(frame.Sample.MoveX)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendI16(frame.Sample.MoveY)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendU8(frame.Down)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendU8(frame.Pressed)) {
        return buffer;
    }
    if (!appendComma()) {
        return buffer;
    }
    if (!appendU8(frame.Released)) {
        return buffer;
    }
    // buffer was zero-initialised, so the byte after the payload is already
    // NUL provided ptr < end. Guard explicitly for completeness.
    if (ptr < end) {
        *ptr = '\0';
    } else {
        buffer.back() = '\0';
    }
    return buffer;
}

const char* NativeSourcePad::StatusName(NativeSourcePadStatus status) noexcept {
    switch (status) {
    case NativeSourcePadStatus::Ok:
        return "ok";
    case NativeSourcePadStatus::DuplicateIdempotent:
        return "duplicate_idempotent";
    case NativeSourcePadStatus::InvalidSequence:
        return "invalid_sequence";
    case NativeSourcePadStatus::DuplicateConflict:
        return "duplicate_conflict";
    case NativeSourcePadStatus::BackwardTick:
        return "backward_tick";
    case NativeSourcePadStatus::InvalidButtons:
        return "invalid_buttons";
    case NativeSourcePadStatus::NonFiniteAxis:
        return "non_finite_axis";
    case NativeSourcePadStatus::AxisOutOfRange:
        return "axis_out_of_range";
    }
    return "unknown";
}
