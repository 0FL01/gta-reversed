#include "NativePcSaveSemantics.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int g_Checks = 0;

void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "pc-save-semantics-fail: %s\n", message); std::exit(1); }
}

NativePcSemanticState SourceState() {
    return {{
        {-10, 10, -20, 20, -2, 8, true, true},
        {100, 150, 200, 260, 0, 40, false, false},
    }};
}

NativePcSaveLayout PathLayout() {
    NativePcSaveLayout layout;
    layout.PayloadBytes[static_cast<std::size_t>(NativePcSaveBlock::Paths)] =
        4 + 2 * NativePcSaveSemantics::PathSwitchBytes;
    return layout;
}

std::vector<std::uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int Reader(const char* path) {
    std::string error;
    const auto bytes = ReadFile(path);
    NativePcSaveImage image;
    Check(NativePcSaveCodec::Decode(bytes, PathLayout(), image, error) == NativePcSaveStatus::Ok,
        "fresh-process original envelope import");
    NativePcSemanticState state;
    Check(NativePcSaveSemantics::ImportPaths(image, state, error) == NativePcSemanticStatus::Ok &&
        state == SourceState(), "fresh-process semantic import");

    // Representative play/change: the first source path box is switched back on.
    state.PathSwitches[0].Off = false;
    NativePcSaveImage changed;
    Check(NativePcSaveSemantics::ExportPaths(state, changed, error) == NativePcSemanticStatus::Ok,
        "post-import semantic change export");
    std::vector<std::uint8_t> exported;
    Check(NativePcSaveCodec::Encode(changed, exported, error) == NativePcSaveStatus::Ok,
        "post-change original envelope export");
    NativePcSaveImage redecoded;
    Check(NativePcSaveCodec::Decode(exported, NativePcSaveCodec::LayoutOf(changed), redecoded, error) ==
        NativePcSaveStatus::Ok, "post-change envelope reimport");
    NativePcSemanticState restored;
    Check(NativePcSaveSemantics::ImportPaths(redecoded, restored, error) == NativePcSemanticStatus::Ok &&
        restored == state && !restored.PathSwitches[0].Off, "post-change semantic reimport");
    std::printf("native-pc-save-semantics-reader-ok checks=%d change=path-on import-export=semantic\n", g_Checks);
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--reader") return Reader(argv[2]);
    const char* path = argc == 2 ? argv[1] : "build/pc-save-semantics.bin";
    const auto source = SourceState();
    NativePcSaveImage image;
    std::string error;
    Check(NativePcSaveSemantics::ExportPaths(source, image, error) == NativePcSemanticStatus::Ok,
        "export source Paths block");
    const auto pathIndex = static_cast<std::size_t>(NativePcSaveBlock::Paths);
    Check(image.Blocks[pathIndex].size() == 4 + 2 * NativePcSaveSemantics::PathSwitchBytes,
        "exact CPathFind block bytes");
    std::vector<std::uint8_t> bytes;
    Check(NativePcSaveCodec::Encode(image, bytes, error) == NativePcSaveStatus::Ok &&
        bytes.size() == NativePcSaveFileBytes, "original PC envelope encode");
    NativePcSaveImage decoded;
    Check(NativePcSaveCodec::Decode(bytes, NativePcSaveCodec::LayoutOf(image), decoded, error) ==
        NativePcSaveStatus::Ok, "original PC envelope decode");
    NativePcSemanticState restored;
    Check(NativePcSaveSemantics::ImportPaths(decoded, restored, error) == NativePcSemanticStatus::Ok &&
        restored == source, "semantic path-state round trip");
    const auto canonical = decoded.Blocks[pathIndex];
    Check(canonical[30] == 0 && canonical[31] == 0 && canonical[58] == 0 && canonical[59] == 0,
        "canonical struct padding zero");
    auto corrupt = decoded;
    corrupt.Blocks[pathIndex][28] = 2;
    const auto held = restored;
    Check(NativePcSaveSemantics::ImportPaths(corrupt, restored, error) ==
        NativePcSemanticStatus::InvalidBlock && restored == held, "invalid Boolean retains semantic owner");
    corrupt = decoded;
    corrupt.Blocks[pathIndex].pop_back();
    Check(NativePcSaveSemantics::ImportPaths(corrupt, restored, error) ==
        NativePcSemanticStatus::InvalidBlock && restored == held, "truncation retains semantic owner");
    auto invalid = source;
    invalid.PathSwitches[0].MinX = 20;
    Check(NativePcSaveSemantics::ExportPaths(invalid, decoded, error) ==
        NativePcSemanticStatus::InvalidInput && decoded.Blocks[pathIndex] == canonical,
        "invalid export retains image");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        Check(bool(file) && bool(file.write(reinterpret_cast<const char*>(bytes.data()),
            std::streamsize(bytes.size()))), "write explicit restart envelope artifact");
    }
    const pid_t child = fork();
    Check(child >= 0, "fork fresh semantic reader");
    if (child == 0) {
        execl(argv[0], argv[0], "--reader", path, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fresh-process import change export import");
    std::remove(path);
    std::printf("native-pc-save-semantics-ok checks=%d block=Paths records=2 envelope=%zu "
        "fresh-process=1 play-change=path-on semantic=roundtrip references=none\n",
        g_Checks, bytes.size());
}
