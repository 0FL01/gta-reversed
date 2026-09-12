#pragma once

#include "app/platform/linux/NativeScriptSchema.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class NativeScriptThreadForm : std::uint8_t { Main, Mission, Streamed };

struct NativeScriptInstructionForm {
    std::uint64_t Session = 0, Sequence = 0, ThreadGeneration = 0;
    std::uint32_t ThreadIndex = 0, IP = 0, NextIP = 0, BaseIP = 0, LocalCount = 0;
    std::int32_t MissionIndex = -1;
    std::uint16_t Opcode = 0, RawOpcode = 0;
    std::array<NativeScriptOperandType, 16> OperandTypes{};
    std::array<std::uint8_t, 16> OperandTags{};
    // Tag7/8 carry declared length and flags (index bank + element type).
    // Scalar operands keep both zero.
    std::array<std::uint8_t, 16> ArrayCounts{}, ArrayFlags{};
    std::uint8_t OperandCount = 0;
    NativeScriptSemanticCoverage Semantics = NativeScriptSemanticCoverage::Unsupported;
    NativeScriptThreadForm ThreadForm = NativeScriptThreadForm::Main;
    bool Negated = false, UsesMissionCleanup = false, ExclusiveMission = false, External = false;
    bool operator==(const NativeScriptInstructionForm&) const = default;
};

struct NativeScriptCorpusSite {
    NativeScriptInstructionForm Form;
    std::uint64_t Visits = 0;
    bool operator==(const NativeScriptCorpusSite&) const = default;
};

struct NativeScriptCorpusThread {
    std::uint32_t ThreadIndex = 0, BaseIP = 0, LocalCount = 0;
    std::uint64_t Generation = 0;
    std::int32_t MissionIndex = -1;
    NativeScriptThreadForm Form = NativeScriptThreadForm::Main;
    bool UsesMissionCleanup = false, ExclusiveMission = false, External = false;
    bool operator==(const NativeScriptCorpusThread&) const = default;
};

struct NativeScriptCorpusSummary {
    std::uint64_t Encounters = 0;
    std::size_t Sites = 0, Threads = 0, Opcodes = 0, OperandForms = 0;
    std::size_t MainSites = 0, MissionSites = 0, StreamedSites = 0;
    std::size_t ImplementedSites = 0, UnsupportedSites = 0;
    bool operator==(const NativeScriptCorpusSummary&) const = default;
};

// Owned, value-only inventory of encountered instruction sites. Observe rejects
// a site whose schema/thread identity changes and retains the previous manifest.
// It does not execute, skip, or reinterpret a command.
class NativeScriptCorpusManifest {
public:
    bool Observe(const NativeScriptInstructionForm& form, std::string& error);
    const std::vector<NativeScriptCorpusSite>& Sites() const noexcept { return m_Sites; }
    const std::vector<NativeScriptCorpusThread>& Threads() const noexcept { return m_Threads; }
    NativeScriptCorpusSummary Summary() const;
    std::uint64_t Fingerprint() const noexcept;

private:
    bool ObserveInPlace(const NativeScriptInstructionForm& form, std::string& error);
    std::uint64_t m_Session = 0;
    std::vector<NativeScriptCorpusSite> m_Sites;
    std::vector<NativeScriptCorpusThread> m_Threads;
};
