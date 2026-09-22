#include "NativeSourceEventFlow.h"

#include <algorithm>
#include <limits>

namespace {
template<typename Slot, typename Ref, std::size_t N>
Slot* Resolve(std::array<Slot, N>& slots, Ref reference) {
    if (!reference.Value) return nullptr;
    const auto index = std::size_t(reference.Value >> 8u);
    const auto generation = std::uint8_t(reference.Value & 0xFFu);
    return index < slots.size() && slots[index].Alive && slots[index].Generation == generation
        ? &slots[index] : nullptr;
}
}

NativeSourceEventFlow::ProducerSlot* NativeSourceEventFlow::Producer(NativeSourceProducerRef reference) noexcept {
    return Resolve(m_Producers, reference);
}
const NativeSourceEventFlow::ProducerSlot* NativeSourceEventFlow::Producer(NativeSourceProducerRef reference) const noexcept {
    return Resolve(const_cast<std::array<ProducerSlot, ProducerCapacity>&>(m_Producers), reference);
}
NativeSourceEventFlow::GroupSlot* NativeSourceEventFlow::Group(NativeSourceGroupFlowRef reference) noexcept {
    return Resolve(m_Groups, reference);
}

NativeSourceFlowStatus NativeSourceEventFlow::CreateGroup(NativeSourceGroupFlowRef& out, std::string& error) {
    const auto slot = std::ranges::find_if(m_Groups, [](const GroupSlot& value) { return !value.Alive; });
    if (slot == m_Groups.end()) { error = "source group flow capacity exceeded"; return NativeSourceFlowStatus::CapacityExceeded; }
    const auto index = std::size_t(slot - m_Groups.begin());
    slot->Generation = std::uint8_t(slot->Generation + 1u);
    if (!slot->Generation) slot->Generation = 1;
    slot->Alive = true;
    slot->Reference = {std::uint32_t(index << 8u) | slot->Generation};
    slot->MemberCount = 0;
    out = slot->Reference;
    error.clear();
    return NativeSourceFlowStatus::Ok;
}

NativeSourceFlowStatus NativeSourceEventFlow::CreateProducer(NativeSourceGroupFlowRef group,
    std::int32_t initialTask, NativeSourceProducerRef& out, std::string& error) {
    auto* groupSlot = Group(group);
    if (!groupSlot) { error = "source producer group reference is stale"; return NativeSourceFlowStatus::StaleReference; }
    if (groupSlot->MemberCount == GroupMembers) { error = "source group member capacity exceeded"; return NativeSourceFlowStatus::CapacityExceeded; }
    const auto slot = std::ranges::find_if(m_Producers, [](const ProducerSlot& value) { return !value.Alive; });
    if (slot == m_Producers.end()) { error = "source producer capacity exceeded"; return NativeSourceFlowStatus::CapacityExceeded; }
    const auto index = std::size_t(slot - m_Producers.begin());
    slot->Generation = std::uint8_t(slot->Generation + 1u);
    if (!slot->Generation) slot->Generation = 1;
    slot->Alive = true;
    slot->Reference = {std::uint32_t(index << 8u) | slot->Generation};
    slot->Group = group;
    slot->Task = initialTask;
    slot->EventCount = 0;
    slot->LastScannerSequence = 0;
    groupSlot->Members[groupSlot->MemberCount++] = slot->Reference;
    out = slot->Reference;
    error.clear();
    return NativeSourceFlowStatus::Ok;
}

NativeSourceFlowStatus NativeSourceEventFlow::Submit(NativeSourceProducerRef reference,
    const NativeSourceFlowEvent& event, std::string& error) {
    auto* producer = Producer(reference);
    if (!producer) { error = "source event producer reference is stale"; return NativeSourceFlowStatus::StaleReference; }
    if (!event.ScannerSequence || event.Priority < 0 || event.ResponseTask < 0) {
        error = "source scanner event is invalid";
        return NativeSourceFlowStatus::InvalidInput;
    }
    if (event.ScannerSequence <= producer->LastScannerSequence) {
        error = "source scanner sequence is stale or conflicting";
        return NativeSourceFlowStatus::SequenceConflict;
    }
    if (producer->EventCount == producer->Events.size()) {
        error = "source event group capacity exceeded";
        return NativeSourceFlowStatus::CapacityExceeded;
    }
    producer->Events[producer->EventCount++] = event;
    producer->LastScannerSequence = event.ScannerSequence;
    error.clear();
    return NativeSourceFlowStatus::Ok;
}

NativeSourceFlowStatus NativeSourceEventFlow::SubmitGroup(NativeSourceGroupFlowRef reference,
    const NativeSourceFlowEvent& event, std::string& error) {
    auto* group = Group(reference);
    if (!group) { error = "source event group reference is stale"; return NativeSourceFlowStatus::StaleReference; }
    for (std::size_t i = 0; i < group->MemberCount; ++i) {
        auto copy = event;
        copy.ScannerSequence += i;
        const auto status = Submit(group->Members[i], copy, error);
        if (status != NativeSourceFlowStatus::Ok) return status;
    }
    error.clear();
    return NativeSourceFlowStatus::Ok;
}

NativeSourceFlowStatus NativeSourceEventFlow::Process(std::string& error) {
    for (auto& producer : m_Producers) {
        if (!producer.Alive || !producer.EventCount) continue;
        std::size_t selected = 0;
        std::int32_t highest = -1;
        for (std::size_t i = 0; i < producer.EventCount; ++i) {
            const auto& event = producer.Events[i];
            const bool higher = event.ScriptCommand ? event.Priority > highest : event.Priority >= highest;
            if (higher) { highest = event.Priority; selected = i; }
        }
        const auto event = producer.Events[selected];
        for (std::size_t i = selected + 1; i < producer.EventCount; ++i) producer.Events[i - 1] = producer.Events[i];
        --producer.EventCount;
        if (m_TransitionSequence == std::numeric_limits<std::uint64_t>::max()) {
            error = "source task transition sequence exhausted";
            return NativeSourceFlowStatus::Overflow;
        }
        m_Transitions.push_back({++m_TransitionSequence, producer.Reference, producer.Group,
            producer.Task, event.ResponseTask, event.Type, event.Priority, event.ScannerSequence});
        producer.Task = event.ResponseTask;
    }
    error.clear();
    return NativeSourceFlowStatus::Ok;
}

std::int32_t NativeSourceEventFlow::CurrentTask(NativeSourceProducerRef reference) const noexcept {
    const auto* producer = Producer(reference);
    return producer ? producer->Task : -1;
}
