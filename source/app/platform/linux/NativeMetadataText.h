// Shared bounded CFileLoader text grammar for the startup metadata readers.
#pragma once

#include "NativeWorldEntityInfo.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace NativeMetadataText {
inline constexpr size_t MaxFileBytes = 8 * 1024 * 1024;

inline void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
inline std::string Lower(std::string_view input) {
    std::string result(input);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
inline uint32_t Key(std::string_view input) {
    uint32_t key = 0xffffffff;
    for (unsigned char c : input) {
        Require(c >= 32 && c < 127, "non-ASCII model name");
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        key ^= c;
        for (int bit = 0; bit < 8; ++bit) key = (key >> 1) ^ ((key & 1) ? 0xedb88320u : 0u);
    }
    return key; // CKeyGen: no final xor.
}

// LoadLine sanitizes controls/commas and skips leading spaces. Do not invent
// split-line behavior for a physical line larger than the source buffer.
template<typename F> void Lines(const NativeWorldEntitySourceText& source, F visit) {
    Require(!source.Source.empty() && source.Source.size() <= 1024 && source.Text.size() <= MaxFileBytes,
        "metadata source bounds");
    Require(source.Text.find('\0') == std::string::npos, "NUL in metadata text");
    uint32_t number = 0;
    for (size_t pos = 0; pos < source.Text.size();) {
        auto end = source.Text.find('\n', pos);
        if (end == std::string::npos) end = source.Text.size();
        auto line = source.Text.substr(pos, end - pos);
        pos = end == source.Text.size() ? end : end + 1;
        ++number;
        Require(line.size() < 511, "metadata physical line exceeds source LoadLine bound");
        for (auto& c : line) if (static_cast<unsigned char>(c) < 32 || c == ',') c = ' ';
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        line.erase(0, first);
        if (!visit(line, NativeWorldSourceRow{source.Source, number})) break;
    }
}

// Sequential scanf-compatible conversions. Undefined/out-of-range source
// inputs and unbounded strings are rejected, not assigned guessed values.
struct Scan {
    const char* At;
    bool Ok = true;
    explicit Scan(const std::string& line) : At(line.c_str()) {}
    void Space() { while (*At == ' ') ++At; }
    bool Word(std::string& out, size_t max = 255) {
        if (!Ok) return false;
        Space(); const auto* start = At;
        while (*At && *At != ' ') ++At;
        out.assign(start, At);
        return Ok = !out.empty() && out.size() <= max;
    }
    bool Int(int& out) {
        if (!Ok) return false;
        Space(); char* end{}; errno = 0;
        const auto value = std::strtol(At, &end, 10);
        if (end == At || errno == ERANGE || value < INT32_MIN || value > INT32_MAX) return Ok = false;
        At = end; out = static_cast<int>(value); return true;
    }
    bool Float(float& out) {
        if (!Ok) return false;
        Space(); char* end{}; errno = 0;
        const auto value = std::strtof(At, &end);
        if (end == At || errno == ERANGE || !std::isfinite(value)) return Ok = false;
        At = end; out = value; return true;
    }
    bool Hex(uint32_t& out) {
        if (!Ok) return false;
        Space(); char* end{}; errno = 0;
        if (*At == '-') return Ok = false;
        const auto value = std::strtoull(At, &end, 16);
        if (end == At || errno == ERANGE || value > UINT32_MAX) return Ok = false;
        At = end; out = static_cast<uint32_t>(value); return true;
    }
};
}
