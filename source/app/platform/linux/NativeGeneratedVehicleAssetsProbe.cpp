// Actual source assets, no world placement or synthetic collision replacement.
#include "app/platform/linux/NativeGeneratedVehicleAssets.h"
#include "app/platform/linux/CarPose.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <type_traits>

using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"
#include <rw.h>

namespace {
std::string s_Game;
size_t s_Opens{};
bool s_RejectCommonTxd{}; // injected I/O failure, no game file edits/copies
bool s_RejectEmbeddedCol{}; // corrupt only the read buffer, never the source file
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
uint64 Hash(const NativeGeneratedVehiclePacket& packet) {
    uint64 hash = 14695981039346656037ull;
    const auto bytes = [&](const void* p, size_t size) {
        const auto* data = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
    };
    for (const auto& mesh : packet.Scene.meshes) {
        bytes(mesh.pos.data(), mesh.pos.size() * sizeof(float));
        bytes(mesh.triCol.data(), mesh.triCol.size() * sizeof(float));
        bytes(mesh.dayColors.data(), mesh.dayColors.size());
    }
    for (const auto& image : packet.Scene.images) bytes(image.rgba.data(), image.rgba.size());
    for (const auto& frame : packet.Frames) {
        bytes(frame.Name.data(), frame.Name.size());
        bytes(frame.ModelBind.data(), sizeof(frame.ModelBind));
    }
    bytes(packet.Collision->SourceChunk.data(), packet.Collision->SourceChunk.size());
    return hash;
}
}

// Isolated read-only OS_File boundary; the runner additionally straces fopen
// calls inside CarPose to include its bounded IMG body reads in the EXE audit.
int32 OS_FileOpen(OSFileDataArea, void** output, const char* path, OSFileAccessType access) {
    Require(access == FILE_ACCESS_READ, "asset probe attempted a write");
    std::string name(path);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Require(lower.find(".exe") == std::string::npos, "EXE access forbidden");
    if (!name.starts_with('/')) name = s_Game + "/" + name;
    ++s_Opens;
    if (s_RejectCommonTxd && name.ends_with("/models/generic/vehicle.txd")) {
        *output = nullptr;
        return 1;
    }
    *output = std::fopen(name.c_str(), "rb");
    return *output ? 0 : 1;
}
int32 OS_FileClose(void* file) { return std::fclose(static_cast<FILE*>(file)); }
int32 OS_FileSize(void* file) {
    const auto position = std::ftell(static_cast<FILE*>(file));
    std::fseek(static_cast<FILE*>(file), 0, SEEK_END);
    const auto size = std::ftell(static_cast<FILE*>(file));
    std::fseek(static_cast<FILE*>(file), position, SEEK_SET);
    return int32(size);
}
int32 OS_FileRead(void* file, void* data, int32 size) {
    if (std::fread(data, 1, size_t(size), static_cast<FILE*>(file)) != size_t(size)) return 1;
    auto* bytes = static_cast<uint8_t*>(data);
    if (s_RejectEmbeddedCol && size > 32 && bytes[0] == 16 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) {
        bool found = false;
        for (int32 p = 0; p < size - 32; ++p) {
            if (!std::memcmp(bytes + p, "COL3", 4) &&
                (!std::memcmp(bytes + p + 8, "landstal_col", 12) || !std::memcmp(bytes + p + 8, "rustler_col", 11))) {
                bytes[p] = 'X'; // labelled fault injection: unusable source COL
                found = true;
                break;
            }
        }
        Require(found, "fault injection located actual embedded source COL");
    }
    return 0;
}
int32 OS_FileGetPosition(void* file) { return int32(std::ftell(static_cast<FILE*>(file))); }
void OS_FileSetPosition(void* file, int32 position) { std::fseek(static_cast<FILE*>(file), position, SEEK_SET); }
void OS_SetFilePathOffset(const char* path) { s_Game = path; }

int main(int argc, char** argv) try {
    Require(argc == 2, "usage: NativeGeneratedVehicleAssetsProbe /game");
    s_Game = argv[1];
    static_assert(std::is_const_v<NativeGeneratedVehicleAsset::element_type>);
    NativeCarGenerators generators;
    std::string error;
    Require(generators.LoadBeforeWorker(argv[1], 0, error), error);
    Require(generators.ModelDefinitions().size() == 212, "actual 212 IDE definitions");
    NativeCollisionPopulation population;
    population.IncludesStreamed = true;
    for (const auto& definition : generators.ModelDefinitions())
        population.Models.emplace(definition.ModelId, NativeCollisionIde{definition.ModelName, false});
    NativeGeneratedVehicleAsset asset;
    {
        NativeCollisionAssets catalog;
        Require(catalog.Load(argv[1], population, error), error);
        const auto* landstal = generators.FindModel(400);
        Require(landstal, "actual Landstal definition");
        auto result = NativeGeneratedVehicleAssets_Load(argv[1], *landstal, catalog, asset);
        Require(bool(result), result.Detail);
        Require(asset->Definition.ModelId == 400 && asset->Definition.ModelName == "landstal" &&
            asset->Definition.TextureName == "landstal" && asset->DffSource == "gta3.img:landstal.dff" &&
            asset->ModelTxdSource == "gta3.img:landstal.txd", "actual DFF/TXD/IDE identity");
        Require(asset->Collision->Name == "landstal_col" && asset->EmbeddedCollision && !asset->Collision->Empty &&
            !asset->Collision->SourceChunk.empty(), "actual authored COL identity");
        Require(asset->Scene.stats.triangles == 2785 && asset->Scene.meshes.size() == 17 && asset->Frames.size() == 52,
            "actual pristine model counts");
        size_t paint = 0;
        for (size_t m = 0; m < asset->Scene.meshes.size(); ++m) {
            const auto& mesh = asset->Scene.meshes[m];
            Require(std::count(mesh.triImg.begin(), mesh.triImg.end(), -2) == 0, "no missing texture");
            for (size_t t = 0; t < mesh.surfaces.size(); ++t) {
                paint += asset->Paint[m].Slots[t] >= 0;
                Require(mesh.surfaces[t].vehicleColorIndex == -1, "no implicit first carcols selection");
                for (size_t c = 0; c < 3; ++c) Require(mesh.surfaces[t].color[c] == mesh.triCol[t * 3 + c], "source RGB retained");
            }
        }
        Require(paint > 0, "authored paint markers retained");
        const auto original = asset;
        const auto fingerprint = Hash(*asset);
        auto* sentinel = rw::TexDictionary::create();
        auto* sentinelTexture = rw::Texture::create(nullptr);
        std::strcpy(sentinelTexture->name, "parser_sentinel");
        sentinel->add(sentinelTexture);
        rw::TexDictionary::setCurrent(sentinel);
        for (int id : {461, 430, 562}) {
            const auto* definition = generators.FindModel(id);
            Require(definition, "actual inspected IDE definition");
            const auto opens = s_Opens;
            result = NativeGeneratedVehicleAssets_Load(argv[1], *definition, catalog, asset);
            Require(result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && asset == original && s_Opens == opens,
                "unsupported atomic output/no parser or file activity");
            Require(rw::TexDictionary::getCurrent() == sentinel && sentinel->find("parser_sentinel") == sentinelTexture,
                "unsupported preserves parser dictionary");
            std::printf("unsupported model=%d name=%s reason=%s\n", id, definition->ModelName.c_str(), result.Detail.c_str());
        }
        auto mismatch = *landstal;
        mismatch.TextureName = "actual_other_txd";
        result = NativeGeneratedVehicleAssets_Load(argv[1], mismatch, catalog, asset);
        Require(result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && asset == original, "TXD alias rejected atomically");
        NativeCollisionAssets missingCatalog;
        result = NativeGeneratedVehicleAssets_Load(argv[1], *landstal, missingCatalog, asset);
        Require(bool(result) && asset->EmbeddedCollision && asset->Collision->Name == "landstal_col",
            "actual embedded COL owns collision even though world catalog has no vehicle entry");
        asset = original;
        result = NativeGeneratedVehicleAssets_Load("/missing-generated-vehicle-probe", *landstal, catalog, asset);
        Require(result.Status == NativeGeneratedVehicleAssetStatus::Error && asset == original, "I/O failure preserves output");
        s_RejectCommonTxd = true;
        result = NativeGeneratedVehicleAssets_Load(argv[1], *landstal, catalog, asset);
        s_RejectCommonTxd = false;
        Require(result.Status == NativeGeneratedVehicleAssetStatus::Error && !result.Detail.empty() && asset == original &&
            Hash(*asset) == fingerprint && rw::TexDictionary::getCurrent() == sentinel &&
            sentinel->find("parser_sentinel") == sentinelTexture, "post-parser common TXD failure preserves output/dictionary/lifetime");
        s_RejectEmbeddedCol = true;
        result = NativeGeneratedVehicleAssets_Load(argv[1], *landstal, catalog, asset);
        s_RejectEmbeddedCol = false;
        Require(result.Status == NativeGeneratedVehicleAssetStatus::Unsupported && asset == original &&
            rw::TexDictionary::getCurrent() == sentinel, "unusable authored COL cannot become a DFF-bbox collision packet");
        result = NativeGeneratedVehicleAssets_Load(argv[1], *landstal, catalog, asset);
        Require(bool(result), result.Detail);
        Require(rw::TexDictionary::getCurrent() == sentinel && sentinel->find("parser_sentinel") == sentinelTexture,
            "successful reload preserves caller dictionary and texture lifetime");
        Require(Hash(*asset) == fingerprint && Hash(*original) == fingerprint, "reload and old immutable asset lifetime");
        std::vector<NativeGeneratedVehicleFrame> frames = asset->Frames;
        const std::vector<uint8_t> truncated{16, 0, 0, 0, 255, 255, 255, 255};
        Require(!NativeGeneratedVehicleAssets_ReadFrames(truncated, frames, error) && frames.size() == 52 &&
            frames[0].Name == "landstal", "bounded parser failure preserves frame output");
        rw::TexDictionary::setCurrent(nullptr);
        sentinel->destroy();
        std::printf("ready model=400 meshes=%zu tris=%d frames=%zu images=%zu col=%s header=%u validatedHeader=%d paint=%zu hash=%llu\n",
            asset->Scene.meshes.size(), asset->Scene.stats.triangles, asset->Frames.size(), asset->Scene.images.size(),
            asset->Collision->Name.c_str(), asset->Collision->HeaderId, asset->Collision->ValidatedHeaderId, paint,
            static_cast<unsigned long long>(fingerprint));
    } // owned catalog dies before packet use
    const auto fingerprint = Hash(*asset);
    CarPose_Shutdown();
    rw::Engine::stop();
    rw::Engine::close();
    rw::Engine::term();
    Require(Hash(*asset) == fingerprint && !asset->Collision->Faces.empty(), "CPU packet lifetime after catalog/engine destruction");
    std::weak_ptr<const NativeGeneratedVehiclePacket> weak = asset;
    auto retained = asset;
    asset.reset();
    Require(!weak.expired() && Hash(*retained) == fingerprint, "model retention is explicit shared CPU ownership");
    retained.reset();
    Require(weak.expired(), "last owner releases packet");
    std::puts("generated-vehicle-assets-ok unsupported-atomic parser-dictionary owned-after-engine no-spawn");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "generated-vehicle-assets FAIL %s\n", e.what());
    return 1;
}
