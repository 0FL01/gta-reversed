#include "NativeSourceRng.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <thread>
#include <type_traits>

namespace {
using Status = NativeSourceRngStatus;

void Expect(const NativeSourceRngDraw& draw, std::uint16_t value) {
    assert(draw.Status == Status::Ready && draw.Value == value);
}

void ExpectFailure(const NativeSourceRngDraw& draw, Status status) {
    assert(draw.Status == status && !draw.Value);
}

void CheckAuthority() {
    static_assert(!std::is_copy_constructible_v<NativeSourceRng>);
    static_assert(!std::is_move_constructible_v<NativeSourceRng>);
    static_assert(!std::is_copy_assignable_v<NativeSourceRng>);
    static_assert(!std::is_move_assignable_v<NativeSourceRng>);
    NativeSourceRngRef unknown;
    assert(unknown.Readiness() == Status::UnknownAuthority);
    ExpectFailure(unknown.NextRand15(), Status::UnknownAuthority);
    ExpectFailure(unknown.NextCarGeneratorPercent(), Status::UnknownAuthority);

    NativeSourceRng owner;
    const auto ref = owner.Reference();
    assert(owner.Readiness() == Status::Unseeded && ref.Readiness() == Status::Unseeded);
    ExpectFailure(ref.NextRand15(), Status::Unseeded);
    ExpectFailure(ref.NextCarGeneratorPercent(), Status::Unseeded);
    assert(owner.Inspect().Status == Status::Unseeded && !owner.Inspect().Value);

    const auto wrongThread = [&] {
        assert(owner.Readiness() == Status::WrongThread && ref.Readiness() == Status::WrongThread);
        assert(owner.SeedOnce(999) == Status::WrongThread);
        assert(owner.Inspect().Status == Status::WrongThread && !owner.Inspect().Value);
        ExpectFailure(ref.NextRand15(), Status::WrongThread);
        ExpectFailure(ref.NextCarGeneratorPercent(), Status::WrongThread);
        ExpectFailure(owner.Reference().NextRand15(), Status::WrongThread);
    };
    std::thread beforeSeed(wrongThread);
    beforeSeed.join();
    assert(owner.Readiness() == Status::Unseeded);
    assert(owner.SeedOnce(1) == Status::Ready);
    assert(owner.Inspect().Value == (NativeSourceRngProvenance{1, 1, 0}));
    assert(owner.SeedOnce(1) == Status::AlreadySeeded);
    assert(owner.SeedOnce(0xffffffffu) == Status::AlreadySeeded);
    assert(owner.Inspect().Value == (NativeSourceRngProvenance{1, 1, 0}));

    // Worker rejection must not race on mutable state while the owner draws.
    std::thread duringDraw(wrongThread);
    const auto constructorConsumer = owner.Reference();
    const auto generatorConsumer = ref; // copy shares the exact same authority
    Expect(constructorConsumer.NextRand15(), 41);
    Expect(generatorConsumer.NextCarGeneratorPercent(), 55); // raw 18467, alarm
    Expect(generatorConsumer.NextCarGeneratorPercent(), 19); // raw 6334, lock
    duringDraw.join();
    assert(owner.Inspect().Value == (NativeSourceRngProvenance{1, 0x18be873au, 3}));
    assert(owner.SeedOnce(1) == Status::AlreadySeeded);
    Expect(ref.NextRand15(), 26500);
    assert(owner.Inspect().Value == (NativeSourceRngProvenance{1, 0xe7847115u, 4}));
}

void CheckKnownVectors() {
    // Independent published Microsoft Learn crt_rand.c fixture, seed 1792:
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/rand
    constexpr std::array<std::uint16_t, 10> published{
        5890, 1279, 19497, 1207, 11420, 3377, 15317, 29489, 9716, 23323,
    };
    NativeSourceRng publishedOwner;
    assert(publishedOwner.SeedOnce(1792) == Status::Ready);
    const auto publishedRef = publishedOwner.Reference();
    for (const auto value : published) {
        Expect(publishedRef.NextRand15(), value);
    }
    assert(publishedOwner.Inspect().Value == (NativeSourceRngProvenance{1792, 0x5b1b2282u, 10}));

    // Explicit seed/state fixtures calculated with integer arithmetic modulo
    // 2^32, not by running the implementation under test.
    struct Fixture {
        std::uint32_t Seed;
        std::array<std::uint32_t, 3> States;
        std::array<std::uint16_t, 3> Draws;
    };
    constexpr std::array fixtures{
        Fixture{0, {0x00269ec3u, 0x1e278e7au, 0xd2f65b55u}, {38, 7719, 21238}},
        Fixture{0xffffffffu, {0x00235ac6u, 0x742b2671u, 0x8d2e2f70u}, {35, 29739, 3374}},
        Fixture{0x80000000u, {0x80269ec3u, 0x9e278e7au, 0x52f65b55u}, {38, 7719, 21238}},
    };
    for (const auto& fixture : fixtures) {
        NativeSourceRng owner;
        assert(owner.SeedOnce(fixture.Seed) == Status::Ready);
        const auto ref = owner.Reference();
        for (std::size_t index = 0; index < fixture.Draws.size(); ++index) {
            Expect(ref.NextRand15(), fixture.Draws[index]);
            assert(owner.Inspect().Value == (NativeSourceRngProvenance{
                fixture.Seed, fixture.States[index], index + 1,
            }));
        }
    }
}

void CheckEveryRangeInput() {
    // Invert the LCG once to arrange each possible raw 15-bit first draw.
    // 0xb9b33155 * 214013 == 1 (mod 2^32). This is fixture construction only;
    // there is no state injection or reseed/restore API in the runtime.
    constexpr std::uint32_t inverse = 0xb9b33155u;
    static_assert(static_cast<std::uint32_t>(std::uint64_t{inverse} * 214013u) == 1);
    for (std::uint32_t raw = 0; raw < 32768; ++raw) {
        const std::uint32_t target = raw << 16u;
        const std::uint32_t seed = (target - 2531011u) * inverse;
        NativeSourceRng owner;
        assert(owner.SeedOnce(seed) == Status::Ready);
        const auto ref = owner.Reference();
        // Independent integer oracle: exhaustive checking also establishes
        // that binary32 rounding never crosses an integer for this fixed range.
        // It distinguishes the local helper from modulo or raw*100/32768.
        const auto expected = static_cast<std::uint16_t>((raw * 99u) / 32767u);
        Expect(ref.NextCarGeneratorPercent(), expected);
        assert(owner.Inspect().Value == (NativeSourceRngProvenance{seed, target, 1}));
        // 0->0, 330->0, 331->1, 32766->98, 32767->99 are included.
    }
}
}

int main() {
    CheckAuthority();
    CheckKnownVectors();
    CheckEveryRangeInput();
    std::puts("source-rng-ok: published-vectors zero/high-bit/wrap shared-order exact-counts "
              "reseed-rejection unknown/unseeded/current-thread range-inputs=32768");
}
