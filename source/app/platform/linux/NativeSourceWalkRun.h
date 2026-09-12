#pragma once
#include "NativeSourceAnimClump.h"

struct NativeSourceWalkRunInput {
    std::int32_t Group = 0;
    float MoveRatio = 0, TimeCanRun = 0;
    bool SprintRequested = false, TurningInPlace = false, Adrenaline = false;
    bool ResetWalkAnimations = false;
};
struct NativeSourceWalkRunResult {
    NativeSourceMoveState Move = NativeSourceMoveState::Still;
    bool Starting = false;
    bool ResetWalkAnimationsConsumed = false;
    // The >=2 branch calls the running-stat update. The enclosing gameplay
    // owner must apply it; this association primitive does not invent stats.
    bool RunningActivity = false;
    bool operator==(const NativeSourceWalkRunResult&) const = default;
};
enum class NativeSourceWalkRunStatus { Ok, InvalidInput, Unsupported, MissingClip };

// Source SetRealMoveAnim's ordinary non-sprint, non-turning, non-exhausted,
// non-adrenaline path. The caller supplies source player predicates; this is
// NOT the complete PlayerOnFoot task or an invented controller/physics loop.
// Walk/run stop here means source idle blending, not a manufactured run_stop
// clip (that clip belongs to the sprint branch). Unsupported/missing/invalid
// preflight leaves clump and out unchanged. As in source task processing,
// allocation exceptions do not imply rollback of earlier association effects.
NativeSourceWalkRunStatus NativeSourceProcessWalkRun(NativeSourceAnimClump& clump,
    const NativeSourceWalkRunInput& input, NativeSourceWalkRunResult& out);
