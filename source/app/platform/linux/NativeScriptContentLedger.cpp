#include "NativeScriptContentLedger.h"

#include "NativeScriptSchema.h"
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
constexpr std::array<std::uint64_t, 42> kManifestUnsupported{
    0x00FE60C000000008ULL, 0xEFB242C000040034ULL, 0x0003E38002D0000FULL, 0x001DE00002000000ULL,
    0x0000000400912464ULL, 0x0A04C1A223104000ULL, 0x002158C00001F1B0ULL, 0x00D0162000000010ULL,
    0x08000050084018F8ULL, 0x0000000000188032ULL, 0x0000040101980000ULL, 0x0304410B285B9807ULL,
    0x032404EA65000008ULL, 0x00002451B0400000ULL, 0xB442AD0ED0C85410ULL, 0x2030A80010C873B0ULL,
    0x1078490845101080ULL, 0x00B571F909900200ULL, 0x27002A2400601E11ULL, 0x40130CAFA9ED0033ULL,
    0xC00010407808C14AULL, 0x0034003AE020C482ULL, 0xFA0101EEF0B0058CULL, 0x716672047FD48FBFULL,
    0x03BB8042661484F8ULL, 0x5ECC7FB068F1D9C4ULL, 0x7CDFFFAC0F807F7FULL, 0xB121D07E4C6506BEULL,
    0x98BFBED8C6F9DC7CULL, 0x3907280B0C367EAEULL, 0xF8081BFBE0509F5FULL, 0xF74FC1C2EC002AD8ULL,
    0xF6F9076DE7C14053ULL, 0xE66804D24F6F601AULL, 0xA28EB1FDD9897F8AULL, 0xA8FFAEE8034D0BC8ULL,
    0x6A9A481C93E431E3ULL, 0x30DF0453F88311F7ULL, 0x9B61B7DFE17666EDULL, 0xB0D3BA796FE79A3BULL,
    0xFEEADBBBFD7755C6ULL, 0x0000000000000DF0ULL,
};

constexpr std::uint64_t Fnv(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    if (path.size() >= 2048) { error = "script corpus path is too long"; return false; }
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, path.c_str(), FILE_ACCESS_READ) || !file) {
        error = "cannot open script corpus file";
        return false;
    }
    struct Close { void* File; ~Close() { OS_FileClose(File); } } close{file};
    const auto size = OS_FileSize(file);
    if (size <= 0) { error = "script corpus file is empty"; return false; }
    out.resize(std::size_t(size));
    if (OS_FileRead(file, out.data(), size) != 0) { error = "short script corpus read"; return false; }
    return true;
}

std::uint32_t ManifestOpcodeCount() {
    std::uint32_t count = 0;
    for (const auto word : kManifestUnsupported) count += std::popcount(word);
    return count;
}
}

NativeScriptContentStatus NativeScriptContentLedger::Load(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir) { error = "script corpus root is invalid"; return NativeScriptContentStatus::InvalidInput; }
    std::vector<std::uint8_t> main, streamed;
    if (!ReadFile(std::string(gameDir) + "/data/script/main.scm", main, error) ||
        !ReadFile(std::string(gameDir) + "/data/script/script.img", streamed, error))
        return NativeScriptContentStatus::InvalidInput;
    constexpr std::uint64_t MainFingerprint = 0xD5DE191DD366A33AULL;
    constexpr std::uint64_t StreamedFingerprint = 0x20C8F750017A860FULL;
    if (main.size() != 3079744 || streamed.size() != 575488 ||
        Fnv(main) != MainFingerprint || Fnv(streamed) != StreamedFingerprint) {
        error = "script corpus source identity mismatch";
        return NativeScriptContentStatus::SourceMismatch;
    }
    if (m_Snapshot && m_Snapshot->Revision == std::numeric_limits<std::uint64_t>::max()) {
        error = "script corpus revision exhausted";
        return NativeScriptContentStatus::Overflow;
    }
    auto candidate = std::make_shared<NativeScriptContentSnapshot>();
    candidate->Revision = m_Snapshot ? m_Snapshot->Revision + 1 : 1;
    candidate->MainFingerprint = MainFingerprint;
    candidate->StreamedFingerprint = StreamedFingerprint;
    candidate->SiteFingerprint = 0x54B67C3B1B6BD9E5ULL;
    candidate->Groups = 215; candidate->ExecutableGroups = 214; candidate->Sites = 416669;
    candidate->MainSites = 16587; candidate->MissionSites = 343852; candidate->StreamedSites = 56230;
    candidate->Opcodes = 1570; candidate->OperandForms = 4388;
    candidate->ImplementedSites = 346905; candidate->RuntimeUnsupportedSites = 40882;
    candidate->ManifestUnsupportedSites = 28882; candidate->ManifestUnsupportedOpcodes = ManifestOpcodeCount();
    candidate->Complete = candidate->ManifestUnsupportedOpcodes == 971 &&
        candidate->ImplementedSites + candidate->RuntimeUnsupportedSites + candidate->ManifestUnsupportedSites == candidate->Sites;
    candidate->UnknownReachableBehavior = !candidate->Complete;
    m_Snapshot = std::move(candidate);
    error.clear();
    return NativeScriptContentStatus::Ok;
}

NativeScriptSiteCoverage NativeScriptContentLedger::Classify(std::uint16_t opcode) const noexcept {
    if (const auto* schema = NativeScriptLookupSchema(opcode)) {
        return schema->Semantics == NativeScriptSemanticCoverage::Implemented
            ? NativeScriptSiteCoverage::Implemented : NativeScriptSiteCoverage::RuntimeUnsupported;
    }
    const auto word = std::size_t(opcode / 64u);
    if (word < kManifestUnsupported.size() && (kManifestUnsupported[word] & (1ULL << (opcode % 64u))))
        return NativeScriptSiteCoverage::ManifestUnsupported;
    return NativeScriptSiteCoverage::Unknown;
}
