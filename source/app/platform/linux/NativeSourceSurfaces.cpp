#include "app/platform/linux/NativeSourceSurfaces.h"
#include "game_sa/SurfaceNameLookup.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <type_traits>
#include <vector>

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

std::filesystem::path Child(const std::filesystem::path& parent, const char* name) {
    const auto exact = parent / name;
    if (std::filesystem::exists(exact)) return exact;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (Lower(entry.path().filename().string()) == name) return entry.path();
    }
    throw std::runtime_error(std::string("missing surface asset: ") + name);
}

std::string Read(const std::filesystem::path& path) {
    struct File {
        void* Handle{};
        ~File() { if (Handle) OS_FileClose(Handle); }
    } file;
    const auto absolute = path.string();
    Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file.Handle, absolute.c_str(), FILE_ACCESS_READ) == 0 && file.Handle,
            "surface OS_FileOpen failed");
    const auto size = OS_FileSize(file.Handle);
    Require(size >= 0, "surface file outside OS_File range");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size) Require(OS_FileRead(file.Handle, bytes.data(), size) == 0, "surface OS_FileRead failed");
    return bytes;
}

// CFileLoader::LoadLine: controls and commas become spaces; trim leading space.
// Reject overlong or embedded-NUL records rather than reproduce buffer hazards.
template<typename Visitor>
void Lines(std::string_view bytes, char comment, Visitor visit) {
    while (!bytes.empty()) {
        const auto end = bytes.find('\n');
        std::string line(bytes.substr(0, end));
        Require(line.size() <= 511 && line.find('\0') == std::string::npos, "unsafe surface record");
        for (auto& c : line) if (static_cast<unsigned char>(c) < 32 || c == ',') c = ' ';
        const auto first = line.find_first_not_of(' ');
        if (first != std::string::npos && line[first] != comment) {
            std::vector<std::string_view> fields;
            std::size_t p = first;
            while (p < line.size()) {
                const auto stop = line.find(' ', p);
                fields.emplace_back(line.data() + p, (stop == std::string::npos ? line.size() : stop) - p);
                if (stop == std::string::npos) break;
                p = line.find_first_not_of(' ', stop);
                if (p == std::string::npos) break;
            }
            visit(fields);
        }
        if (end == std::string_view::npos) break;
        bytes.remove_prefix(end + 1);
    }
}

template<typename T>
T Number(std::string_view word) {
    if (!word.empty() && word.front() == '+') {
        word.remove_prefix(1);
        Require(!word.empty() && word.front() != '-' && word.front() != '+', "invalid surface sign");
    }
    T value{};
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    Require(parsed.ec == std::errc{} && parsed.ptr == word.data() + word.size(), "invalid surface number");
    if constexpr (std::is_floating_point_v<T>) Require(std::isfinite(value), "nonfinite surface number");
    return value;
}
}

bool NativeSourceSurfaces::Load(const std::string& gameDir, std::string& error) {
    try {
        Require(!gameDir.empty(), "empty surface game directory");
        const auto data = Child(std::filesystem::absolute(gameDir), "data");
        const auto adhesive = Read(Child(data, "surface.dat"));
        const auto materials = Read(Child(data, "surfinfo.dat"));
        return LoadBytes(adhesive, materials, error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool NativeSourceSurfaces::LoadBytes(std::string_view adhesive, std::string_view materials, std::string& error) {
    try {
        static_assert(TOTAL_NUM_SURFACE_TYPES == 179);
        auto candidate = m_Data;
        std::size_t row = 0;
        // Retail 0x573F40: labels are ignored, lower triangle is mirrored.
        Lines(adhesive, ';', [&](const auto& fields) {
            Require(row < 6 && fields.size() >= row + 2 && fields[0].size() < 256, "invalid adhesive row");
            for (std::size_t column = 0; column <= row; ++column) {
                const auto word = fields[column + 1];
                // The original tests the FIRST character, not equality to "-".
                const float value = word.front() == '-' ? 0.0f : Number<float>(word);
                candidate.AdhesiveLimits[row][column] = candidate.AdhesiveLimits[column][row] = value;
            }
            ++row;
        });
        Require(row == 6, "incomplete adhesive matrix");

        candidate.MaterialRows = 0;
        constexpr std::array<std::string_view, 6> groups{"RUBBER", "HARD", "ROAD", "LOOSE", "SAND", "WET"};
        Lines(materials, '#', [&](const auto& fields) {
            // SurfaceInfo::Read consumes exactly these 36 conversions; source
            // ignores additional fields. Only adhesion is adopted by this owner.
            Require(fields.size() >= 36 && fields[0].size() < 64, "invalid surface material record");
            for (const auto index : {1, 4, 5, 35}) Require(fields[index].size() < 32, "oversized surface token");
            (void)Number<float>(fields[2]);
            (void)Number<float>(fields[3]);
            for (std::size_t i = 6; i < 35; ++i) (void)Number<std::int32_t>(fields[i]);
            const auto id = GetSourceSurfaceIdFromName(std::string(fields[0]).c_str());
            const auto found = std::find(groups.begin(), groups.end(), fields[1]);
            if (found != groups.end()) candidate.AdhesionGroups[id] = static_cast<std::uint8_t>(found - groups.begin());
            ++candidate.MaterialRows;
        });
        candidate.Loaded = true;
        m_Data = candidate;
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

NativeSourceSurfaceStatus NativeSourceSurfaces::AdhesiveLimit(std::uint16_t a, std::uint16_t b, float& out) const noexcept {
    if (!m_Data.Loaded) return NativeSourceSurfaceStatus::NotLoaded;
    if (a >= m_Data.AdhesionGroups.size() || b >= m_Data.AdhesionGroups.size()) return NativeSourceSurfaceStatus::InvalidMaterial;
    // SurfaceInfos_c::GetAdhesiveLimit / retail 0x5772F0: B first, A second.
    out = m_Data.AdhesiveLimits[m_Data.AdhesionGroups[b]][m_Data.AdhesionGroups[a]];
    return NativeSourceSurfaceStatus::Ok;
}
