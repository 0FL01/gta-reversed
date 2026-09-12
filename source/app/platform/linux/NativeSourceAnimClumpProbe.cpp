#include "NativeSourceAnimClump.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
constexpr auto Ok = NativeSourceClumpStatus::Ok;
std::size_t s_Checks = 0;
void Check(bool ok, const char* message) {
    ++s_Checks;
    if (!ok) { std::fprintf(stderr, "source-clump FAIL: %s\n", message); std::exit(1); }
}
NativeSourceAnimClip Clip(int id, float duration, bool moving = false, bool partial = false) {
    NativeSourceAnimClip clip;
    clip.Key = {0, id}; clip.Duration = duration; clip.Sequences = 1;
    clip.Synchronised = clip.Looped = moving;
    clip.Partial = partial; clip.FinishAutoRemove = partial;
    return clip;
}
void Synchronisation() {
    NativeSourceAnimClump clump;
    Check(clump.LoadClips({Clip(0, 2, true), Clip(1, 1, true), Clip(2, 3, true)}) == Ok, "load explicit synthetic clip metadata");
    NativeSourceAnimHandle walk, run, sprint;
    Check(clump.Add({0, 0}, walk) == Ok && clump.Get(walk)->State.BlendAmount == 1, "source Add constructor starts fully blended");
    std::vector<NativeSourceClumpEvent> events;
    Check(clump.Update(0.25f, events) == Ok && clump.Get(walk)->State.CurrentTime == 0.25f, "single sync association");
    Check(clump.Add({0, 1}, run) == Ok && clump.Get(run)->State.CurrentTime == 0.125f, "Add synchronises normalized phase");
    Check(clump.SetBlend(walk, 0.5f, 0) == Ok && clump.SetBlend(run, 0.5f, 0) == Ok, "walk/run weights");
    Check(clump.Update(0.3f, events) == Ok && std::abs(clump.Get(run)->State.TimeStep - 0.2f) < 0.000001f &&
        std::abs(clump.Get(walk)->State.TimeStep - 0.4f) < 0.000001f, "source weighted common cycle multiplier");
    Check(clump.Restart(run) == Ok, "make different source sync candidates");
    Check(clump.Add({0, 2}, sprint) == Ok && clump.Get(sprint)->State.CurrentTime == 0, "Add uses FIRST moving association");
    Check(clump.Remove(sprint) == Ok, "remove temporary association");
    Check(clump.Blend({0, 2}, 4, sprint) == Ok && clump.Get(sprint)->State.CurrentTime > 0.9f, "Blend uses LAST moving association, not Add's first");
    Check(clump.Get(walk)->State.BlendAutoRemove && clump.Get(run)->State.BlendAutoRemove, "same-category source fade");
    const auto before = clump.Snapshot(); const auto oldEvents = events;
    Check(clump.SetSpeed(sprint, 0) == Ok, "source speed field can be zero");
    const auto zeroSpeed = clump.Snapshot();
    Check(clump.Update(0.1f, events) == NativeSourceClumpStatus::InvalidInput && clump.Snapshot() == zeroSpeed && events == oldEvents, "nonfinite sync denominator rejects entire update");
    Check(before.size() == 3, "retained value snapshot independent of mutations");
}
void Lifetimes() {
    NativeSourceAnimClump clump;
    Check(clump.LoadClips({Clip(116, 0.1f, false, true), Clip(119, 0.1f, false, true)}) == Ok, "partial clip metadata");
    NativeSourceAnimHandle a, b;
    Check(clump.Add({0, 116}, a) == Ok && clump.Add({0, 119}, b) == Ok, "head-first source list");
    Check(clump.BindFinish(a, 11) == Ok && clump.BindFinish(b, 22) == Ok, "owned callback tokens");
    std::vector<NativeSourceClumpEvent> events;
    Check(clump.Update(0.1f, events) == Ok && events.size() == 2 && events[0].Event.FinishToken == 22 && events[1].Event.FinishToken == 11, "time callbacks follow source list order");
    Check(clump.Get(a) && clump.Get(b), "task finish does not delete clump associations");
    const auto retained = clump.Snapshot();
    Check(clump.Update(0.25f, events) == Ok && events.size() == 2 && events[0].Event.Removed && !events[0].Event.FinishToken && !clump.Get(a), "clump owns post-task fade retirement without double callback");
    Check(retained.size() == 2 && retained[0].State.Alive, "retired metadata snapshots remain readable");
    Check(clump.SetPlaying(a, true) == NativeSourceClumpStatus::StaleHandle, "retired token cannot mutate replacement");
    Check(clump.Blend({0, 116}, 8, a) == Ok, "new partial association starts fade-in");
    Check(clump.Get(a)->State.BlendAmount == 0 && clump.Get(a)->State.BlendDelta == 8, "source partial fade initialization");
    Check(clump.BindDelete(a, 33) == Ok, "source delete callback binding");
    Check(clump.Update(0.1f, events) == Ok && events.empty() && clump.Get(a)->State.DeleteToken == 33, "time finish does NOT fire delete callback");
    Check(clump.Update(0.25f, events) == Ok && events.size() == 1 && events[0].Event.DeleteToken == 33 && !events[0].Event.FinishToken, "blend deletion fires delete callback");
    Check(clump.Add({0, 116}, a) == Ok && clump.BindFinish(a, 44) == Ok, "manual removal fixture");
    Check(clump.Remove(a) == Ok && clump.Update(0, events) == Ok && events.empty(), "explicit source destructor has no callback");
}
void IdentityAndRejection() {
    NativeSourceAnimClump a, b;
    auto definition = Clip(3, 1);
    Check(a.LoadClips({definition}) == Ok && b.LoadClips({definition}) == Ok, "separate owners");
    NativeSourceAnimHandle old, other;
    Check(a.Add({0, 3}, old) == Ok && b.Add({0, 3}, other) == Ok && old.Owner != other.Owner, "owner-qualified handles");
    Check(b.Remove(old) == NativeSourceClumpStatus::StaleHandle && b.Get(other), "cross-owner rejection");
    const auto snapshot = a.Snapshot();
    Check(a.LoadClips({definition, definition}) == NativeSourceClumpStatus::InvalidInput && a.Snapshot() == snapshot, "duplicate definition reload atomic");
    NativeSourceAnimHandle out{900, 901};
    Check(a.Blend({0, 3}, std::numeric_limits<float>::quiet_NaN(), out) == NativeSourceClumpStatus::InvalidInput &&
        out == NativeSourceAnimHandle{900, 901} && a.Snapshot() == snapshot, "invalid blend retains output and owner");
    Check(a.Add({0, 999}, out) == NativeSourceClumpStatus::MissingClip && a.Snapshot() == snapshot, "missing asset metadata never fabricated");
    Check(a.LoadClips({definition}) == Ok && !a.Get(old) && a.Add({0, 3}, out) == Ok && out.Serial > old.Serial, "reload cannot recycle instance handles");
    NativeSourceAnimHandle duplicate;
    Check(a.Add({0, 3}, duplicate) == Ok && a.Find(3)->Handle == duplicate, "source Find uses newest list match");
    NativeSourceAnimHandle running;
    Check(a.Blend({0, 3}, 4, running) == Ok && running == out, "source Blend picks last matching duplicate");
}
}
int main() {
    Synchronisation(); Lifetimes(); IdentityAndRejection();
    std::printf("source-clump-ok checks=%zu association-control-only no-pose-or-gameplay-claim\n", s_Checks);
}
