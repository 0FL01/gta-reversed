#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class NativeTextLanguage : std::uint8_t {
    American,
    French,
    German,
    Italian,
    Spanish,
};

struct NativeTextKeyEntry {
    std::uint32_t Hash = 0;
    std::uint32_t Offset = 0;
};

struct NativeTextTableFamily {
    std::array<char, 8> Name{};
    std::vector<NativeTextKeyEntry> Entries;
    std::vector<char> Data;
};

struct NativeTextLanguageFamily {
    NativeTextLanguage Language = NativeTextLanguage::American;
    std::string File;
    std::uint16_t Version = 0;
    std::vector<NativeTextTableFamily> Tables;
};

struct NativeTextSubstitutions {
    std::array<std::int32_t, 6> Numbers{};
    std::array<bool, 6> HasNumber{};
    std::string String;
    std::map<std::string, std::string, std::less<>> Controls;
};

class NativeTextFamilies {
public:
    bool Load(const char* gameDir, std::string& error);
    bool Find(NativeTextLanguage language, const char* table, const char* key,
        std::string& out, std::string& error) const;
    bool FindHash(NativeTextLanguage language, const char* table, std::uint32_t hash,
        std::string& out, std::string& error) const;
    bool Resolve(NativeTextLanguage language, const char* table, const char* key,
        const NativeTextSubstitutions& substitutions, std::string& out,
        std::string& error) const;

    const std::array<NativeTextLanguageFamily, 5>& Languages() const noexcept { return m_Languages; }
    bool Loaded() const noexcept { return m_Loaded; }
    static constexpr bool FontPresentationFeedback = false;

private:
    std::array<NativeTextLanguageFamily, 5> m_Languages{};
    bool m_Loaded = false;
};
