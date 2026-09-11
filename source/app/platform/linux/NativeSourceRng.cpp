#include "NativeSourceRng.h"

#include <limits>

// Local source authority: General.cpp:160-166 asserts RAND_MAX == 0x7fff
// and calls CRT rand(). The established recurrence is also recorded in
// artifacts/graphics/round8-cargens-contract.md, Shared Source RNG.
// Microsoft CRT source corroboration (research only, no build dependency):
// https://github.com/ojdkbuild/tools_toolchain_sdk10_1607/blob/master/Source/10.0.14393.0/ucrt/stdlib/rand.cpp
// srand assigns the supplied seed; rand advances the per-thread 32-bit state.

NativeSourceRng::NativeSourceRng() : m_OwnerThread(std::this_thread::get_id()) {}

NativeSourceRngStatus NativeSourceRng::Readiness() const {
    if (std::this_thread::get_id() != m_OwnerThread) {
        return NativeSourceRngStatus::WrongThread;
    }
    return m_Seeded ? NativeSourceRngStatus::Ready : NativeSourceRngStatus::Unseeded;
}

NativeSourceRngStatus NativeSourceRng::SeedOnce(std::uint32_t seed) {
    const auto status = Readiness();
    if (status != NativeSourceRngStatus::Unseeded) {
        return status == NativeSourceRngStatus::Ready ? NativeSourceRngStatus::AlreadySeeded : status;
    }
    m_Provenance = {seed, seed, 0};
    m_Seeded = true;
    return NativeSourceRngStatus::Ready;
}

NativeSourceRngInspection NativeSourceRng::Inspect() const {
    const auto status = Readiness();
    if (status != NativeSourceRngStatus::Ready) {
        return {status, std::nullopt};
    }
    return {status, m_Provenance};
}

NativeSourceRngDraw NativeSourceRng::NextRand15() {
    const auto status = Readiness();
    if (status != NativeSourceRngStatus::Ready) {
        return {status, std::nullopt};
    }
    m_Provenance.State = m_Provenance.State * 214013u + 2531011u;
    ++m_Provenance.DrawCount;
    return {status, static_cast<std::uint16_t>((m_Provenance.State >> 16u) & 0x7fffu)};
}

NativeSourceRngStatus NativeSourceRngRef::Readiness() const {
    return m_Owner ? m_Owner->Readiness() : NativeSourceRngStatus::UnknownAuthority;
}

NativeSourceRngDraw NativeSourceRngRef::NextRand15() const {
    return m_Owner ? m_Owner->NextRand15() : NativeSourceRngDraw{};
}

NativeSourceRngDraw NativeSourceRngRef::NextCarGeneratorPercent() const {
    auto draw = NextRand15();
    if (draw.Status != NativeSourceRngStatus::Ready) {
        return draw;
    }
    // General.h:104-122 maps exclusive ints through inclusive float endpoints
    // min, max-1; common.h:196-198 lerps to*t + from*(1-t), then int truncates.
    // Specialize only the (0,100) call actually needed by CarGenerator.cpp:318,
    // 321. This follows this fork's helper, not a claim about retail x87 range
    // instructions. Do not replace by modulo, /32768, or a uniform distribution.
    static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
    const float fraction = static_cast<float>(*draw.Value) * (1.0f / 32767.0f);
    draw.Value = static_cast<std::uint16_t>(99.0f * fraction);
    return draw;
}
