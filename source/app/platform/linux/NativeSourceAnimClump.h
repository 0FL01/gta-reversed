#pragma once
#include "NativeSourcePedControl.h"
#include <vector>

struct NativeSourceAnimKey {
    std::int32_t Group = 0, Animation = 0;
    bool operator==(const NativeSourceAnimKey&) const = default;
};
struct NativeSourceAnimClip {
    NativeSourceAnimKey Key;
    float Duration = 0;
    std::uint32_t Sequences = 0;
    bool Looped = false, Synchronised = false, Partial = false, Facial = false;
    bool FinishAutoRemove = false, BlendAutoRemove = false;
    bool operator==(const NativeSourceAnimClip&) const = default;
};
struct NativeSourceAnimHandle {
    std::uint64_t Owner = 0, Serial = 0;
    bool operator==(const NativeSourceAnimHandle&) const = default;
};
struct NativeSourceAnimInstance {
    NativeSourceAnimHandle Handle;
    NativeSourceAnimClip Clip;
    NativeSourceAnimAssociation State;
    bool operator==(const NativeSourceAnimInstance&) const = default;
};
struct NativeSourceClumpEvent {
    NativeSourceAnimHandle Handle;
    NativeSourceAnimEvent Event;
    NativeSourceAnimAssociation State;
    bool operator==(const NativeSourceClumpEvent&) const = default;
};
enum class NativeSourceClumpStatus { Ok, InvalidInput, MissingClip, StaleHandle };

// Source association CONTROL ownership, not skeletal pose evaluation. Clip
// metadata comes from an external owned reader; no duplicate IFP loader or RW.
// Source Add/Blend and three-pass Update ordering; callback notifications are
// owned values, not arbitrary reentrant callbacks. Explicit removal/destruction
// does not invoke finish/delete callbacks (AnimBlendAssociation destructor).
// Single enclosing owner/thread; returned pointers are transient lookups only.
// Deliver LastEvents to attached tasks after every successful Update, before
// the next Update. Snapshots/events exported to presentation are value copies.
class NativeSourceAnimClump {
public:
    NativeSourceAnimClump();
    NativeSourceAnimClump(const NativeSourceAnimClump&) = delete;
    NativeSourceAnimClump& operator=(const NativeSourceAnimClump&) = delete;
    NativeSourceClumpStatus LoadClips(const std::vector<NativeSourceAnimClip>& clips);
    NativeSourceClumpStatus Add(NativeSourceAnimKey key, NativeSourceAnimHandle& out);
    NativeSourceClumpStatus Blend(NativeSourceAnimKey key, float delta, NativeSourceAnimHandle& out);
    NativeSourceClumpStatus Remove(NativeSourceAnimHandle handle);
    NativeSourceClumpStatus Update(float seconds, std::vector<NativeSourceClumpEvent>& events);
    NativeSourceClumpStatus SetBlend(NativeSourceAnimHandle handle, float amount, float delta);
    NativeSourceClumpStatus SetSpeed(NativeSourceAnimHandle handle, float speed);
    NativeSourceClumpStatus SetPlaying(NativeSourceAnimHandle handle, bool playing);
    NativeSourceClumpStatus SetAutoRemove(NativeSourceAnimHandle handle, bool blend, bool finish);
    NativeSourceClumpStatus Restart(NativeSourceAnimHandle handle);
    NativeSourceClumpStatus ResetTime(NativeSourceAnimHandle handle);
    NativeSourceClumpStatus BindFinish(NativeSourceAnimHandle handle, std::uint64_t token);
    NativeSourceClumpStatus BindDelete(NativeSourceAnimHandle handle, std::uint64_t token);
    const NativeSourceAnimClip* Clip(NativeSourceAnimKey key) const;
    const NativeSourceAnimInstance* Get(NativeSourceAnimHandle handle) const;
    // RpAnimBlendClumpGetAssociation searches by animation ID, not group.
    const NativeSourceAnimInstance* Find(std::int32_t animation) const;
    std::vector<NativeSourceAnimInstance> Snapshot() const { return m_Instances; }
    std::uint64_t UpdateSequence() const { return m_UpdateSequence; }
    const std::vector<NativeSourceClumpEvent>& LastEvents() const { return m_LastEvents; }
private:
    NativeSourceClumpStatus Insert(NativeSourceAnimKey key, const float* blendDelta, NativeSourceAnimHandle& out);
    NativeSourceAnimInstance* Mutable(NativeSourceAnimHandle handle);
    const std::uint64_t m_Owner;
    std::uint64_t m_NextSerial = 1;
    std::uint64_t m_UpdateSequence = 0;
    std::vector<NativeSourceClumpEvent> m_LastEvents;
    std::vector<NativeSourceAnimClip> m_Clips;
    std::vector<NativeSourceAnimInstance> m_Instances; // head first, like source Prepend
};
