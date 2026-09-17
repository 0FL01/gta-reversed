#include "app/platform/linux/NativeBeatTrack.h"

#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: probe GAME");
        NativeBeatTrack owner;
        std::string error;
        if (!owner.LoadBeforeWorker(argv[1], error)) throw std::runtime_error(error);
        if (owner.Preload(10).Status != NativeScriptServiceStatus::Ready || owner.State().ScriptTrack != 10 ||
            owner.State().SourceTrack != 184 || owner.State().Status != 2 || !owner.State().InfoHash)
            throw std::runtime_error("source intro beat-track identity");
        const auto before = owner.State();
        if (owner.Preload(14).Status != NativeScriptServiceStatus::Error || owner.State().InfoHash != before.InfoHash)
            throw std::runtime_error("invalid beat-track request changed owner");
        std::printf("native-beat-track-ok checks=7 script=10 source=184 status=2 metadata=%llu playback=0\n",
            static_cast<unsigned long long>(owner.State().InfoHash));
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "FAIL %s\n", exception.what());
        return 1;
    }
}
