#include "app/platform/linux/NativeCarRecordings.h"

#include <cstdio>
#include <stdexcept>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: probe GAME");
        NativeCarRecordings owner;
        std::string error;
        Check(owner.LoadBeforeWorker(argv[1], error), error.c_str());
        Check(owner.Count() == 426 && !owner.IsLoaded(1), "archive census");
        Check(owner.Request(1).Status == NativeScriptServiceStatus::Ready && owner.IsLoaded(1) &&
            owner.Request(0).Status == NativeScriptServiceStatus::Error, "request state");
        const NativeScriptVehicleRef vehicle{0x101};
        Check(owner.Start(vehicle, 1).Status == NativeScriptServiceStatus::Ready &&
            owner.IsPlaybackActive(vehicle), "start source playback");
        std::vector<NativeCarRecordingUpdate> updates;
        Check(owner.Advance(1000, updates, error) && !updates.empty() && !updates.front().Finished,
            "interpolate source frame");
        Check(owner.Advance(0x7FFFFFFF, updates, error) && !owner.IsPlaybackActive(vehicle) &&
            !updates.empty() && updates.back().Finished, "finish non-looped source playback");
        Check(NativeCarRecordings::RuntimePlayback, "runtime playback coverage");
        std::printf("native-car-recordings-ok checks=%d entries=%zu loaded=%zu playback=1\n",
            g_Checks, owner.Count(), owner.LoadedCount());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
