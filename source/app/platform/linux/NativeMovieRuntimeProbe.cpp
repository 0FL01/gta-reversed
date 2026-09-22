#include "NativeMovieRuntime.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    NativeMovieClip logo, titles, retained;
    std::string error;
    if (!NativeMovieRuntime_Decode(argv[1], "Logo.mpg", logo, error) ||
        !NativeMovieRuntime_Decode(argv[1], "GTAtitles.mpg", titles, error)) {
        std::fprintf(stderr, "native-movie-runtime-fail %s\n", error.c_str()); return 1;
    }
    retained = titles;
    if (NativeMovieRuntime_Decode(argv[1], "missing.mpg", titles, error) || titles != retained ||
        logo.Width <= 0 || logo.Height <= 0 || logo.AudioRate <= 0 ||
        titles.Width <= 0 || titles.Height <= 0 || titles.AudioRate <= 0 ||
        logo.DurationMs == titles.DurationMs || logo.AudioFrameHash == titles.AudioFrameHash) return 1;
    std::printf("native-movie-runtime-ok clips=Logo,GTAtitles video=%s,%s audio=%s,%s "
        "frames=%dx%d,%dx%d rates=%d,%d duration=%u,%u splash=next codec-license=FFmpeg-LGPL dynamic=1\n",
        logo.VideoCodec.c_str(), titles.VideoCodec.c_str(), logo.AudioCodec.c_str(), titles.AudioCodec.c_str(),
        logo.Width, logo.Height, titles.Width, titles.Height, logo.AudioRate, titles.AudioRate,
        logo.DurationMs, titles.DurationMs);
}
