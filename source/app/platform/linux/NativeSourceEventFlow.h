#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct NativeSourceProducerRef {
    std::uint32_t Value = 0;
    bool operator==(const NativeSourceProducerRef&) const = default;
};
struct NativeSourceGroupFlowRef {
    std::uint32_t Value = 0;
    bool operator==(const NativeSourceGroupFlowRef&) const = default;
};

enum class NativeSourceFlowStatus : std::uint8_t {
    Ok,
    InvalidInput,
    StaleReference,
    CapacityExceeded,
    SequenceConflict,
    Overflow,
};

struct NativeSourceFlowEvent {
    std::uint64_t ScannerSequence = 0;
    std::int32_t Type = 0;
    std::int32_t Priority = 0;
    std::int32_t ResponseTask = 0;
    bool ScriptCommand = false;
    bool operator==(const NativeSourceFlowEvent&) const = default;
};

struct NativeSourceTaskTransition {
    std::uint64_t Sequence = 0;
    NativeSourceProducerRef Producer;
    NativeSourceGroupFlowRef Group;
    std::int32_t PreviousTask = 0;
    std::int32_t CurrentTask = 0;
    std::int32_t EventType = 0;
    std::int32_t EventPriority = 0;
    std::uint64_t ScannerSequence = 0;
    bool operator==(const NativeSourceTaskTransition&) const = default;
};

class NativeSourceEventFlow {
public:
    static constexpr std::size_t ProducerCapacity = 140;
    static constexpr std::size_t GroupCapacity = 8;
    static constexpr std::size_t GroupMembers = 8;
    static constexpr std::size_t EventsPerProducer = 16;

    NativeSourceFlowStatus CreateGroup(NativeSourceGroupFlowRef& out, std::string& error);
    NativeSourceFlowStatus CreateProducer(NativeSourceGroupFlowRef group,
        std::int32_t initialTask, NativeSourceProducerRef& out, std::string& error);
    NativeSourceFlowStatus Submit(NativeSourceProducerRef, const NativeSourceFlowEvent&, std::string& error);
    NativeSourceFlowStatus SubmitGroup(NativeSourceGroupFlowRef, const NativeSourceFlowEvent&, std::string& error);
    NativeSourceFlowStatus Process(std::string& error);

    std::span<const NativeSourceTaskTransition> Transitions() const { return m_Transitions; }
    std::int32_t CurrentTask(NativeSourceProducerRef) const noexcept;

private:
    struct ProducerSlot {
        std::uint8_t Generation = 0;
        bool Alive = false;
        NativeSourceProducerRef Reference;
        NativeSourceGroupFlowRef Group;
        std::int32_t Task = 0;
        std::array<NativeSourceFlowEvent, EventsPerProducer> Events{};
        std::size_t EventCount = 0;
        std::uint64_t LastScannerSequence = 0;
    };
    struct GroupSlot {
        std::uint8_t Generation = 0;
        bool Alive = false;
        NativeSourceGroupFlowRef Reference;
        std::array<NativeSourceProducerRef, GroupMembers> Members{};
        std::size_t MemberCount = 0;
    };

    ProducerSlot* Producer(NativeSourceProducerRef) noexcept;
    const ProducerSlot* Producer(NativeSourceProducerRef) const noexcept;
    GroupSlot* Group(NativeSourceGroupFlowRef) noexcept;

    std::array<ProducerSlot, ProducerCapacity> m_Producers{};
    std::array<GroupSlot, GroupCapacity> m_Groups{};
    std::vector<NativeSourceTaskTransition> m_Transitions;
    std::uint64_t m_TransitionSequence = 0;
};
