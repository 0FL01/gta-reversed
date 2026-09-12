// Versioned portable envelope for one quiescent NativeScriptSession owner graph.
// This is deliberately NOT the original PC save/settings format. Script asset
// bytes are never embedded: a restarted process must load the exact source SCM
// and any resident streamed payloads before restoring this value state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class NativeScriptSession;

inline constexpr std::uint16_t NativeScriptPortableSaveMajor = 1;
inline constexpr std::uint16_t NativeScriptPortableSaveMinor = 0;

enum class NativeScriptPortableSaveStatus : std::uint8_t {
    Ok,
    NotLoaded,
    Busy,
    InvalidState,
    InvalidEnvelope,
    UnsupportedVersion,
    Truncated,
    ChecksumMismatch,
    SourceMismatch,
    Overflow,
};

struct NativeScriptPortableSaveInfo {
    std::uint16_t Major = 0, Minor = 0;
    std::uint32_t Bytes = 0, PayloadBytes = 0;
    std::uint64_t SourceFingerprint = 0, SchemaFingerprint = 0, PayloadChecksum = 0;
    std::uint32_t Globals = 0, Threads = 0, ActiveThreads = 0, StreamedScripts = 0;
    bool operator==(const NativeScriptPortableSaveInfo&) const = default;
};

class NativeScriptPortableSave {
public:
    // On failure output/info remain unchanged. Only a completed scheduler
    // boundary can be encoded; pending services, open passes and faults reject.
    static NativeScriptPortableSaveStatus Encode(const NativeScriptSession& session,
        std::vector<std::uint8_t>& output, NativeScriptPortableSaveInfo& info, std::string& error);
    // Header/checksum inspection has no owner or asset dependency.
    static NativeScriptPortableSaveStatus Inspect(std::span<const std::uint8_t> envelope,
        NativeScriptPortableSaveInfo& info, std::string& error);
    // Destination must already own the exact source SCM and every saved loaded
    // streamed payload. SessionId is intentionally retained from destination;
    // stale pre-restart service IDs therefore cannot become current.
    static NativeScriptPortableSaveStatus Restore(NativeScriptSession& destination,
        std::span<const std::uint8_t> envelope, NativeScriptPortableSaveInfo& info, std::string& error);

private:
    static bool IsInstructionBoundary(const NativeScriptSession& session,
        std::size_t threadIndex, std::uint32_t target);
    static bool ValidateGraph(const NativeScriptSession& session, std::string& error);
};
