#include "NativeSourceAnimClump.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr auto Ok = NativeSourceClumpStatus::Ok;
std::atomic<std::uint64_t> s_NextOwner{1};
std::uint64_t AllocateOwner() {
    auto value = s_NextOwner.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (s_NextOwner.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    throw std::overflow_error("source animation owner identity exhausted");
}
bool Sync(NativeSourceAnimAssociation& to, const NativeSourceAnimAssociation& from) {
    to.CurrentTime = (from.CurrentTime / from.TotalTime) * to.TotalTime;
    if (!std::isfinite(to.CurrentTime)) return false;
    // Source SetCurrentTime: repeating associations wrap, nonrepeating clamp.
    while (to.CurrentTime >= to.TotalTime) {
        if (!to.Looped) { to.CurrentTime = to.TotalTime; break; }
        const float previous = to.CurrentTime;
        to.CurrentTime -= to.TotalTime;
        if (to.CurrentTime == previous) return false; // no float progress, not an infinite loop
    }
    to.Playing = true;
    return true;
}
}

NativeSourceAnimClump::NativeSourceAnimClump() : m_Owner(AllocateOwner()) {}
std::uint64_t NativeSourceAnimClump::NewCallbackToken() {
    if (m_NextCallback == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("source animation callback identity exhausted");
    return m_NextCallback++;
}

NativeSourceClumpStatus NativeSourceAnimClump::LoadClips(const std::vector<NativeSourceAnimClip>& clips) {
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& clip = clips[i];
        if (!std::isfinite(clip.Duration) || clip.Duration <= 0 || !clip.Sequences || clip.Key.Group < 0 || clip.Key.Animation < 0)
            return NativeSourceClumpStatus::InvalidInput;
        for (std::size_t j = 0; j < i; ++j) if (clips[j].Key == clip.Key) return NativeSourceClumpStatus::InvalidInput;
    }
    auto candidate = clips;
    m_Clips.swap(candidate);
    m_Instances.clear(); // destruction has no finish callback; serials never reset
    m_LastEvents.clear();
    return Ok;
}

const NativeSourceAnimClip* NativeSourceAnimClump::Clip(NativeSourceAnimKey key) const {
    for (const auto& clip : m_Clips) if (clip.Key == key) return &clip;
    return nullptr;
}
NativeSourceAnimInstance* NativeSourceAnimClump::Mutable(NativeSourceAnimHandle handle) {
    if (handle.Owner != m_Owner) return nullptr;
    for (auto& instance : m_Instances) if (instance.Handle == handle) return &instance;
    return nullptr;
}
const NativeSourceAnimInstance* NativeSourceAnimClump::Get(NativeSourceAnimHandle handle) const {
    if (handle.Owner != m_Owner) return nullptr;
    for (const auto& instance : m_Instances) if (instance.Handle == handle) return &instance;
    return nullptr;
}
const NativeSourceAnimInstance* NativeSourceAnimClump::Find(std::int32_t animation) const {
    for (const auto& instance : m_Instances) if (instance.Clip.Key.Animation == animation) return &instance;
    return nullptr;
}

NativeSourceClumpStatus NativeSourceAnimClump::Add(NativeSourceAnimKey key, NativeSourceAnimHandle& out) {
    return Insert(key, nullptr, out);
}
NativeSourceClumpStatus NativeSourceAnimClump::Blend(NativeSourceAnimKey key, float delta, NativeSourceAnimHandle& out) {
    if (!std::isfinite(delta)) return NativeSourceClumpStatus::InvalidInput;
    return Insert(key, &delta, out);
}
NativeSourceClumpStatus NativeSourceAnimClump::Insert(NativeSourceAnimKey key, const float* delta, NativeSourceAnimHandle& out) {
    const auto* definition = Clip(key);
    if (!definition) return NativeSourceClumpStatus::MissingClip;
    auto candidate = m_Instances;
    std::size_t running = candidate.size(), moving = candidate.size();
    bool fadeOther = false;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        auto& a = candidate[i];
        if (definition->Synchronised && a.State.Synchronised && (delta || moving == candidate.size())) moving = i;
        if (!delta) continue;
        if (a.Clip.Key == key) running = i;
        else if (definition->Partial == a.Clip.Partial && definition->Facial == a.Clip.Facial) {
            if (a.State.BlendAmount <= 0) a.State.BlendDelta = -1;
            else {
                const float change = a.State.BlendAmount * -*delta;
                if (!std::isfinite(change)) return NativeSourceClumpStatus::InvalidInput;
                if (change <= a.State.BlendDelta || !definition->Partial) a.State.BlendDelta = std::min(-0.05f, change);
            }
            a.State.BlendAutoRemove = true;
            fadeOther = true;
        }
    }
    if (running != candidate.size()) {
        auto& a = candidate[running];
        a.State.BlendDelta = (1 - a.State.BlendAmount) * *delta;
        if (!std::isfinite(a.State.BlendDelta)) return NativeSourceClumpStatus::InvalidInput;
        if (a.State.CurrentTime == a.State.TotalTime) { a.State.CurrentTime = 0; a.State.Playing = true; }
        const auto handle = a.Handle;
        m_Instances.swap(candidate);
        out = handle;
        return Ok;
    }
    if (m_NextSerial == std::numeric_limits<std::uint64_t>::max()) return NativeSourceClumpStatus::InvalidInput;
    NativeSourceAnimInstance a;
    a.Handle = {m_Owner, m_NextSerial};
    a.Clip = *definition;
    a.State.TotalTime = definition->Duration;
    a.State.Synchronised = definition->Synchronised;
    a.State.Looped = definition->Looped;
    a.State.FinishAutoRemove = definition->FinishAutoRemove;
    a.State.BlendAutoRemove = definition->BlendAutoRemove;
    a.State.BlendAmount = 1; // source association constructor, NOT a default fade-in
    if (moving != candidate.size() && !Sync(a.State, candidate[moving].State)) return NativeSourceClumpStatus::InvalidInput;
    if (delta && (fadeOther || definition->Partial)) { a.State.BlendAmount = 0; a.State.BlendDelta = *delta; }
    candidate.insert(candidate.begin(), a);
    m_Instances.swap(candidate);
    ++m_NextSerial;
    out = a.Handle;
    return Ok;
}

NativeSourceClumpStatus NativeSourceAnimClump::Remove(NativeSourceAnimHandle handle) {
    if (!Get(handle)) return NativeSourceClumpStatus::StaleHandle;
    std::erase_if(m_Instances, [&](const auto& a) { return a.Handle == handle; });
    return Ok;
}

NativeSourceClumpStatus NativeSourceAnimClump::Update(float seconds, std::vector<NativeSourceClumpEvent>& events) {
    if (!std::isfinite(seconds) || seconds < 0 || m_UpdateSequence == std::numeric_limits<std::uint64_t>::max()) return NativeSourceClumpStatus::InvalidInput;
    auto candidate = m_Instances;
    std::vector<NativeSourceClumpEvent> observed;
    observed.reserve(candidate.size());
    float totalTime = 0, totalBlend = 0;
    for (auto it = candidate.begin(); it != candidate.end();) {
        NativeSourceAnimEvent event;
        if (NativeSourceAnimUpdateBlend(it->State, seconds, event) != NativePedControlStatus::Ok) return NativeSourceClumpStatus::InvalidInput;
        if (event.Removed) {
            observed.push_back({it->Handle, event, it->State});
            it = candidate.erase(it);
            continue;
        }
        if (it->State.Synchronised) {
            totalTime += it->State.TotalTime / it->State.Speed * it->State.BlendAmount;
            totalBlend += it->State.BlendAmount;
        }
        ++it;
    }
    if (!std::isfinite(totalTime) || !std::isfinite(totalBlend)) return NativeSourceClumpStatus::InvalidInput;
    const float multiplier = totalTime == 0 ? 1 : 1 / totalTime * totalBlend;
    for (auto& a : candidate) {
        if (NativeSourceAnimUpdateStep(a.State, seconds, multiplier) != NativePedControlStatus::Ok) return NativeSourceClumpStatus::InvalidInput;
    }
    for (auto& a : candidate) {
        NativeSourceAnimEvent event;
        if (NativeSourceAnimUpdateTime(a.State, event) != NativePedControlStatus::Ok) return NativeSourceClumpStatus::InvalidInput;
        if (event.FinishToken || event.DeleteToken) observed.push_back({a.Handle, event, a.State});
    }
    auto retainedEvents = observed; // allocate before committing either authority
    m_Instances.swap(candidate);
    events.swap(observed);
    m_LastEvents.swap(retainedEvents);
    ++m_UpdateSequence;
    return Ok;
}

NativeSourceClumpStatus NativeSourceAnimClump::SetBlend(NativeSourceAnimHandle handle, float amount, float delta) {
    if (!std::isfinite(amount) || !std::isfinite(delta)) return NativeSourceClumpStatus::InvalidInput;
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.BlendAmount = amount; a->State.BlendDelta = delta;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::SetSpeed(NativeSourceAnimHandle handle, float speed) {
    if (!std::isfinite(speed)) return NativeSourceClumpStatus::InvalidInput;
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.Speed = speed;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::SetPlaying(NativeSourceAnimHandle handle, bool playing) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.Playing = playing;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::SetAutoRemove(NativeSourceAnimHandle handle, bool blend, bool finish) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.BlendAutoRemove = blend; a->State.FinishAutoRemove = finish;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::Restart(NativeSourceAnimHandle handle) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.CurrentTime = 0; a->State.Playing = true;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::ResetTime(NativeSourceAnimHandle handle) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.CurrentTime = 0; // SetCurrentTime(0), NOT Start(0)
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::BindFinish(NativeSourceAnimHandle handle, std::uint64_t token) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.FinishToken = token; a->State.DeleteToken = 0;
    return Ok;
}
NativeSourceClumpStatus NativeSourceAnimClump::BindDelete(NativeSourceAnimHandle handle, std::uint64_t token) {
    auto* a = Mutable(handle);
    if (!a) return NativeSourceClumpStatus::StaleHandle;
    a->State.DeleteToken = token; a->State.FinishToken = 0;
    return Ok;
}
