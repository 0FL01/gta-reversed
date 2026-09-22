#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class NativeAudioFamily : std::uint8_t { Sfx, Speech, Environment };
enum class NativeAudioEncoding : std::uint8_t { Pcm16Mono, OggVorbis };

struct NativeAudioFamilyAsset {
    NativeAudioFamily Family = NativeAudioFamily::Sfx;
    NativeAudioEncoding Encoding = NativeAudioEncoding::Pcm16Mono;
    std::int32_t SourceEvent = -1;
    std::int32_t Bank = -1;
    std::int32_t Sound = -1;
    std::uint32_t Rate = 0;
    std::uint32_t DurationMs = 0;
    std::int32_t LoopStart = -1;
    std::uint64_t Hash = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> Payload;
};

class NativeAudioFamilies {
public:
    bool Load(const char* gameDir, std::string& error);
    const std::array<NativeAudioFamilyAsset, 3>& Assets() const noexcept { return m_Assets; }
    bool Loaded() const noexcept { return m_Loaded; }
    static constexpr bool OutputFeedback = false;

private:
    std::array<NativeAudioFamilyAsset, 3> m_Assets{};
    bool m_Loaded = false;
};
