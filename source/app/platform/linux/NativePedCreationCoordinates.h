#pragma once

#include "NativePathGraph.h"
#include "NativeSourceRng.h"

enum class NativePedCreationStatus : std::uint8_t {
    Position,
    NoPosition,
    InvalidInput,
    StaleGraph,
    UnknownRng,
    UnsupportedObservation,
    Overflow,
};

struct NativePedCreationInput {
    std::uint64_t Generation = 0;
    float X = 0, Y = 0;
    float VisibleMin = 42.5f, VisibleMax = 50.5f;
    float HiddenMin = 15.0f, HiddenMax = 25.0f;
    bool AllowSwitchedOff = false;
    bool AlternateCamera = false;
};

struct NativePedCreationPosition {
    NativeCollisionVector Position{};
    NativePathAddress First, Second;
    float Fraction = 0;
    bool operator==(const NativePedCreationPosition&) const = default;
};

// Observations come from the current SOURCE camera and building-world owners,
// not from presentation nodes. False return means unavailable authority, not
// invisible or missed ground. Implementations must be synchronous and read-only.
class NativePedCreationObservations {
public:
    virtual ~NativePedCreationObservations() = default;
    virtual bool SphereVisible(const NativeCollisionVector&, float radius,
        bool alternateCamera, bool& outVisible) noexcept = 0;
    virtual bool GroundZ(const NativeCollisionVector& rayStart,
        bool& outFound, float& outHeight) noexcept = 0;
};

// Source exterior GeneratePedCreationCoors: density draw, 300 node trials,
// original link order, five point trials, camera-specific annuli, hidden parity
// draw and the source ground-height rejection. This selects coordinates only;
// it neither constructs a ped nor certifies ambient census completeness.
// Every non-Position result retains out. Invalid preflight consumes no random
// state; observations/math after selection may consume the source draw prefix.
NativePedCreationStatus NativeGeneratePedCreationCoordinates(const NativePathGraph&,
    const NativePedCreationInput&, NativeSourceRngRef, NativePedCreationObservations&,
    NativePedCreationPosition& out);

// Separate source TakeWidthIntoAccountForCoors step used AFTER selection. It
// uses the caller's source random seed, never draws/forks a stream. Both nodes
// must belong to the same current graph generation; failure retains position.
NativePedCreationStatus NativeJitterPedCreationCoordinates(const NativePathGraph&,
    std::uint64_t generation, NativePathAddress first, NativePathAddress second,
    std::uint16_t seed, NativeCollisionVector& position);
