#include "NativePcSaveCodec.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool condition, const char* message) {
    ++g_Checks;
    if (!condition) { std::fprintf(stderr, "pc-save-codec-fail: %s\n", message); std::exit(1); }
}

void Put32(std::vector<std::uint8_t>& data, std::size_t offset, std::int32_t value) {
    const auto bits = std::uint32_t(value);
    data[offset] = std::uint8_t(bits);
    data[offset + 1] = std::uint8_t(bits >> 8u);
    data[offset + 2] = std::uint8_t(bits >> 16u);
    data[offset + 3] = std::uint8_t(bits >> 24u);
}
}

int main() {
    NativePcSaveImage source;
    for (std::size_t i = 0; i < NativePcSaveBlockCount; ++i) {
        source.Blocks[i].resize(8 + i);
        for (std::size_t n = 0; n < source.Blocks[i].size(); ++n)
            source.Blocks[i][n] = std::uint8_t((i * 17 + n * 3) & 0xFFu);
    }
    Put32(source.Blocks[std::size_t(NativePcSaveBlock::Scripts)], 0, 1001);
    Put32(source.Blocks[std::size_t(NativePcSaveBlock::Pools)], 4, 2002);
    Put32(source.Blocks[std::size_t(NativePcSaveBlock::Garages)], 0, -1);
    const auto layout = NativePcSaveCodec::LayoutOf(source);
    Check(NativePcSaveCodec::BlockNames().size() == 28, "all original PC blocks are named");

    std::vector<std::uint8_t> encoded;
    std::string error;
    Check(NativePcSaveCodec::Encode(source, encoded, error) == NativePcSaveStatus::Ok &&
        encoded.size() == NativePcSaveFileBytes, "fixed-size original PC envelope");
    NativePcSaveImage decoded;
    Check(NativePcSaveCodec::Decode(encoded, layout, decoded, error) == NativePcSaveStatus::Ok &&
        decoded == source, "all blocks round trip byte-identically");
    for (std::size_t i = 0; i < NativePcSaveBlockCount; ++i)
        Check(decoded.Blocks[i] == source.Blocks[i], "individual block codec round trip");

    const auto beforeRepair = decoded;
    const std::array fields{
        NativePcSaveReferenceField{NativePcSaveBlock::Scripts, 0, false},
        NativePcSaveReferenceField{NativePcSaveBlock::Pools, 4, false},
        NativePcSaveReferenceField{NativePcSaveBlock::Garages, 0, true},
    };
    const std::array mappings{std::pair{1001, 11}, std::pair{2002, 22}};
    Check(NativePcSaveCodec::RepairReferences(decoded, fields, mappings, error) == NativePcSaveStatus::Ok,
        "generation-independent references repaired");
    const auto repaired = decoded;
    NativePcSaveImage missing = beforeRepair;
    const std::array incomplete{std::pair{1001, 11}};
    Check(NativePcSaveCodec::RepairReferences(missing, fields, incomplete, error) ==
        NativePcSaveStatus::ReferenceUnavailable && missing == beforeRepair,
        "missing reference rejects atomically");

    auto corrupt = encoded;
    corrupt[123] ^= 0x80;
    NativePcSaveImage retained = repaired;
    Check(NativePcSaveCodec::Decode(corrupt, layout, retained, error) ==
        NativePcSaveStatus::ChecksumMismatch && retained == repaired, "checksum rejection retains output");
    corrupt = encoded;
    corrupt[0] = 'X';
    const auto checksum = NativePcSaveCodec::Checksum(
        std::span<const std::uint8_t>{corrupt}.first(NativePcSaveDataBytes));
    Put32(corrupt, NativePcSaveDataBytes, std::int32_t(checksum));
    Check(NativePcSaveCodec::Decode(corrupt, layout, retained, error) ==
        NativePcSaveStatus::TagMismatch && retained == repaired, "BLOCK tag rejection retains output");
    Check(NativePcSaveCodec::Decode(std::span<const std::uint8_t>{encoded}.first(encoded.size() - 1),
        layout, retained, error) == NativePcSaveStatus::SizeMismatch && retained == repaired,
        "truncation rejection retains output");
    auto badLayout = layout;
    badLayout.PayloadBytes[0] = NativePcSaveDataBytes;
    Check(NativePcSaveCodec::Decode(encoded, badLayout, retained, error) ==
        NativePcSaveStatus::Overflow && retained == repaired, "layout overflow rejection retains output");

    std::printf("native-pc-save-codec-ok checks=%d blocks=28 bytes=%zu checksum=additive references=repaired padding=zero\n",
        g_Checks, encoded.size());
}
