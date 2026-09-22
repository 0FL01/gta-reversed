#pragma once

#include <cstdint>
#include <memory>
#include <string>

enum class NativeScriptContentStatus : std::uint8_t { Ok, InvalidInput, SourceMismatch, Overflow };
enum class NativeScriptSiteCoverage : std::uint8_t { Implemented, RuntimeUnsupported, ManifestUnsupported, Unknown };

struct NativeScriptContentSnapshot {
    std::uint64_t Revision{}, MainFingerprint{}, StreamedFingerprint{}, SiteFingerprint{};
    std::uint32_t Groups{}, ExecutableGroups{}, Sites{}, MainSites{}, MissionSites{}, StreamedSites{};
    std::uint32_t Opcodes{}, OperandForms{}, ImplementedSites{}, RuntimeUnsupportedSites{};
    std::uint32_t ManifestUnsupportedSites{}, ManifestUnsupportedOpcodes{};
    bool Complete{}, UnknownReachableBehavior{true};
    bool operator==(const NativeScriptContentSnapshot&) const = default;
};

// Generated from the pinned Sanny schema over the complete shipped main.scm,
// 135 mission payloads and 79 script.img members. This classifies strict
// unimplemented sites; it never authorizes them to execute as NOPs.
class NativeScriptContentLedger {
public:
    NativeScriptContentStatus Load(const char* gameDir, std::string& error);
    NativeScriptSiteCoverage Classify(std::uint16_t opcode) const noexcept;
    const std::shared_ptr<const NativeScriptContentSnapshot>& Snapshot() const noexcept { return m_Snapshot; }
    static constexpr const char* SchemaRevision = "53ed1c2561bf6ca70dc16afca5d8f3a406066158";
    static constexpr const char* SchemaSha256 = "797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671";

private:
    std::shared_ptr<const NativeScriptContentSnapshot> m_Snapshot;
};
