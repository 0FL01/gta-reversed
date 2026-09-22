#include "NativePcSaveSemantics.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "pc-save-semantics-fail: %s\n", message); std::exit(1); }
}
}

int main() {
    NativePcSemanticState source{{
        {-10, 10, -20, 20, -2, 8, true, true},
        {100, 150, 200, 260, 0, 40, false, false},
    }};
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
    auto canonical = decoded.Blocks[pathIndex];
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
    std::printf("native-pc-save-semantics-ok checks=%d block=Paths records=2 envelope=%zu semantic=roundtrip references=none\n",
        g_Checks, bytes.size());
}
