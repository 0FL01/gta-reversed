#include "NativeProgressionRuntime.h"
#include "NativeStoryLifecycle.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
std::array<char, 8> Name(const char* text) {
    std::array<char, 8> out{};
    for (std::size_t i = 0; i < out.size() && text[i]; ++i) out[i] = text[i];
    return out;
}
bool Read(const char* path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size <= 0 || size > 1'000'000) return false;
    bytes.resize(std::size_t(size));
    file.seekg(0);
    return bool(file.read(reinterpret_cast<char*>(bytes.data()), size));
}
bool Write(const char* path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    return bool(file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())));
}
bool RunMission(const char* gameDir, std::string& error) {
    NativeStoryLifecycle story;
    return story.Initialize(gameDir, 2, Name("PROLOG1"), 43200, error) == NativeStoryStatus::Ok &&
        story.Start(100, error) == NativeStoryStatus::Ok &&
        story.StartCutscene(110, error) == NativeStoryStatus::Ok &&
        story.ResolveCutscene(112, true, error) == NativeStoryStatus::Ok &&
        story.RegisterResources(3, 1, 1, 2, error) == NativeStoryStatus::Ok &&
        story.Fail(120, error) == NativeStoryStatus::Ok &&
        story.Cleanup(121, error) == NativeStoryStatus::Ok &&
        story.Retry(130, error) == NativeStoryStatus::Ok &&
        story.StartCutscene(140, error) == NativeStoryStatus::Ok &&
        story.Advance(23'000, error) == NativeStoryStatus::Ok &&
        story.ResolveCutscene(23'000, false, error) == NativeStoryStatus::Ok &&
        story.RegisterResources(4, 2, 1, 3, error) == NativeStoryStatus::Ok &&
        story.StartAudio(23'100, error) == NativeStoryStatus::Ok &&
        story.Advance(50'000, error) == NativeStoryStatus::Ok &&
        story.Complete(50'001, error) == NativeStoryStatus::Ok &&
        story.Cleanup(50'002, error) == NativeStoryStatus::Ok &&
        story.LastCommitted()->Phase == NativeStoryPhase::Cleaned &&
        story.LastCommitted()->Attempt == 2 && story.LastCommitted()->Failures == 1 &&
        story.LastCommitted()->Retries == 1 && story.LastCommitted()->Skips == 1 &&
        story.LastCommitted()->Completions == 1 && story.LastCommitted()->Cleanups == 2;
}
int Reader(const char* mode, const char* gameDir, const char* input, const char* output) {
    std::vector<std::uint8_t> bytes;
    NativeProgressionRuntime progression;
    std::string error;
    if (!Read(input, bytes) || progression.Restore(bytes, error) != NativeProgressionStatus::Ok) return 10;
    if (!std::strcmp(mode, "post")) {
        if (progression.IntegerStat(146) != 1) return 11;
        std::printf("first-mission-save-post-ok stat146=1 restart=2\n");
        return 0;
    }
    if (progression.IntegerStat(146) != 0 || !RunMission(gameDir, error) ||
        progression.SetIntegerStat(146, 1, error) != NativeProgressionStatus::Ok ||
        progression.Encode(bytes, error) != NativeProgressionStatus::Ok || !Write(output, bytes)) return 12;
    std::printf("first-mission-save-reader-ok fail=1 retry=1 skip=1 complete=1 cleanup=2 restart=1\n");
    return 0;
}
bool Exec(const char* self, const char* mode, const char* gameDir, const char* input, const char* output) {
    const auto pid = fork();
    if (pid < 0) return false;
    if (!pid) {
        execl(self, self, "--reader", mode, gameDir, input, output, nullptr);
        _exit(127);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}

int main(int argc, char** argv) {
    if (argc == 6 && !std::strcmp(argv[1], "--reader"))
        return Reader(argv[2], argv[3], argv[4], argv[5]);
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s GAME_DIR PRE_SAVE POST_SAVE\n", argv[0]);
        return 2;
    }
    NativeProgressionRuntime progression;
    std::string error;
    std::vector<std::uint8_t> pre;
    if (progression.SetIntegerStat(146, 0, error) != NativeProgressionStatus::Ok ||
        progression.Encode(pre, error) != NativeProgressionStatus::Ok || !Write(argv[2], pre) ||
        !Exec(argv[0], "pre", argv[1], argv[2], argv[3]) ||
        !Exec(argv[0], "post", argv[1], argv[3], argv[3])) {
        std::fprintf(stderr, "first-mission-save-fail %s\n", error.c_str());
        return 1;
    }
    std::printf("native-first-mission-save-ok mission=2 start=1 fail=1 retry=1 skip=1 complete=1 "
        "cleanup=2 pre-post-restart=2 progression=matched\n");
}
