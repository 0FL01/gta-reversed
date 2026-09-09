// Source/asset-based skinning oracle, enabled only in the isolated pose probe.
#pragma once
#include "app/platform/linux/IfpAnim.h"

struct RealtimeGameplayPoseAudit {
    int Bones = 0;
    int ParentMismatches = 0;
    int InvalidInfluences = 0;
    int Vertices = 0;
    float MaxQuaternionError = 0.0f;
    float MaxJointError = 0.0f;
    float MaxLegacyJointError = 0.0f; // negative control: original wrong product
    float MaxVertexError = 0.0f;
    float MaxNormalError = 0.0f;
};

// Does not alter the pose. Compares emitted CPU skinning with sequential
// stored -> inverse bind -> animated bone world transforms (librw GL path).
bool RealtimeGameplay_AuditPose(const char* gameDir, const char* anim, double phase,
    WorldShotScene& scene, IfpAnimStats& stats, RealtimeGameplayPoseAudit& audit,
    char* error, std::size_t errorSize);
