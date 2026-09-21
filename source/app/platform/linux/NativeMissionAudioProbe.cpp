#include "NativeMissionAudio.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    NativeMissionAudio audio;
    std::string error;
    if (!audio.LoadBeforeWorker(argv[1], error) ||
        audio.Request(1, 43200).Status != NativeScriptServiceStatus::Ready ||
        !audio.Loaded(1) || !audio.Slot(1) || !audio.Slot(1)->MetadataHash || !audio.Slot(1)->DurationMs ||
        audio.Play(1, 100).Status != NativeScriptServiceStatus::Ready || !audio.Slot(1)->PlaybackRequested ||
        (audio.Advance(100 + audio.Slot(1)->DurationMs - 1), audio.Finished(1)) ||
        (audio.Advance(100 + audio.Slot(1)->DurationMs), !audio.Finished(1)) ||
        audio.Request(0, 43200).Status != NativeScriptServiceStatus::Error ||
        audio.Request(5, 43200).Status != NativeScriptServiceStatus::Error ||
        audio.Clear(1).Status != NativeScriptServiceStatus::Ready || audio.Loaded(1)) {
        std::fprintf(stderr, "native-mission-audio-fail %s\n", error.c_str());
        return 1;
    }
    std::printf("native-mission-audio-ok audio=43200 metadata=owned duration=source playback-clock=1 output=0\n");
}
