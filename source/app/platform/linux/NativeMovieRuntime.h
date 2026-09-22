#pragma once

#include <cstdint>
#include <string>

struct NativeMovieClip {
    std::string Name;
    std::string VideoCodec;
    std::string AudioCodec;
    std::int32_t Width = 0, Height = 0;
    std::int32_t AudioRate = 0, AudioChannels = 0, AudioSamples = 0;
    std::uint32_t DurationMs = 0;
    std::uint64_t VideoFrameHash = 0, AudioFrameHash = 0;
    bool operator==(const NativeMovieClip&) const = default;
};

bool NativeMovieRuntime_Decode(const char* gameDir, const char* file,
    NativeMovieClip& out, std::string& error);
