// Standalone real-asset probe; includes CarPose to inspect its actual TXD loader.
// Build/run with VehicleTextureProbe.sh from /workspace in the dev container.
#include "app/platform/linux/CarPose.cpp"
#include <algorithm>
#include <cstdlib>

void VehicleTextureGpuProbe(const char* game, const WorldShotScene& offline, const WorldShotScene& realtime);

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "vehicle-texture FAIL %s\n", message); std::exit(1); }
}

static int Missing(const WorldShotScene& scene) {
    int count = 0;
    for (const auto& mesh : scene.meshes) count += std::count(mesh.triImg.begin(), mesh.triImg.end(), -2);
    return count;
}

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{};
    WorldShotScene offline, realtime;
    CarPoseStats oldStats{}, stats{};
    CarPoseAudit audit{};
    Require(CarPose_Init(game, "landstal", 0, 0, offline, oldStats, audit, error, sizeof(error)), error);
    Require(CarPose_Init(game, "landstal", 0, 0, realtime, stats, audit, error, sizeof(error),
                        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::StoredAtomics}), error);
    std::printf("landstal triangles=%d offlineMissing=%d realtimeMissing=%d modelTextures=%d sharedTextures=%d totalTextures=%d images=%zu\n",
        stats.tris, Missing(offline), Missing(realtime), oldStats.textures, stats.sharedTextures, stats.textures, realtime.images.size());
    Require(stats.textures == oldStats.textures + stats.sharedTextures, "total dictionary texture stats");
    Require(stats.tris == oldStats.tris && offline.meshes.size() == realtime.meshes.size(), "geometry unchanged");
    for (size_t i = 0; i < offline.meshes.size(); ++i) {
        Require(offline.meshes[i].pos == realtime.meshes[i].pos && offline.meshes[i].uv == realtime.meshes[i].uv &&
                offline.meshes[i].triCol == realtime.meshes[i].triCol, "positions/UV/material colors unchanged");
    }

    auto* shared = LoadVehicleTxd();
    Require(shared && !s_txds.empty(), "actual dictionaries loaded");
    auto* model = s_txds.front();
    int decodedCount = 0;
    FORLIST(link, shared->textures) {
        auto* texture = rw::Texture::fromDict(link);
        TexImage image;
        const bool decoded = TexSample_Decode(texture, image);
        auto* native = GETD3DRASTEREXT(texture->raster);
        std::printf("shared name=%s format=0x%x raster=0x%x depth=%d size=%dx%d decoded=%d\n", texture->name,
            native->format, texture->raster->format, texture->raster->depth,
            texture->raster->width, texture->raster->height, decoded);
        decodedCount += decoded;
        Require(decoded && (native->format == 21 || native->format == 22), "actual vehicle TXD raw 32-bit formats");
        auto* raster = texture->raster;
        const auto* raw = raster->lock(0, rw::Raster::LOCKREAD);
        Require(raw, "lock actual native texels");
        for (int y = 0; y < image.h; ++y) for (int x = 0; x < image.w; ++x) {
            const auto* bgra = raw + y * raster->stride + x * 4;
            const auto* rgba = image.rgba.data() + (y * image.w + x) * 4;
            Require(rgba[0] == bgra[2] && rgba[1] == bgra[1] && rgba[2] == bgra[0] &&
                    rgba[3] == (native->format == 21 ? bgra[3] : 255), "exact BGRA-to-RGBA and A8/X8 alpha");
        }
        raster->unlock(0);
    }
    ImgIndex index;
    std::vector<uint8> bytes;
    Require(BuildImgIndex("models/gta3.img", index) && ImgReadBytesStd(index, "landstal.dff", bytes), "actual landstal DFF read");
    auto linked = TexSample_LinkedParse(bytes.data(), bytes.size(), model, nullptr, 0, shared);
    Require(linked.clump, "landstal linked clump");
    std::set<std::string> names;
    int missingNames = 0;
    for (const auto& [dummy, resolved] : linked.resolved) {
        Require(resolved.filter == dummy->filterAddressing, "DFF sampler metadata preserved");
        if (!names.insert(resolved.name).second) continue;
        const auto* expected = shared->find(resolved.name) ? shared->find(resolved.name) : model->find(resolved.name);
        Require(resolved.real == expected, "common-before-model actual material resolution");
        TexImage image;
        const bool decoded = TexSample_Decode(resolved.real, image);
        missingNames += !decoded;
        const auto flattened = std::find_if(realtime.images.begin(), realtime.images.end(),
            [&](const auto& candidate) { return std::strcmp(candidate.name, resolved.name) == 0; });
        Require(flattened != realtime.images.end() && flattened->rgba == image.rgba && flattened->filter == resolved.filter,
                "scene owns the resolved material texels and DFF sampler");
        int alphaMin = 255, alphaMax = 0;
        for (size_t i = 3; i < image.rgba.size(); i += 4) {
            alphaMin = std::min(alphaMin, int(image.rgba[i]));
            alphaMax = std::max(alphaMax, int(image.rgba[i]));
        }
        std::printf("material name=%s source=%s decoded=%d size=%dx%d alpha=%d..%d filter=0x%x\n",
            resolved.name, shared->find(resolved.name) ? "vehicle.txd" : "landstal.txd", decoded,
            image.w, image.h, alphaMin, alphaMax, resolved.filter);
    }
    TexSample_FreeLinked(linked);

    // In-memory negative controls use actual raster-backed textures, no invented texels.
    auto* local = rw::Texture::fromDict(model->textures.link.next);
    auto* common = rw::Texture::fromDict(shared->textures.link.next);
    char original[32]; std::memcpy(original, local->name, sizeof(original));
    std::memcpy(local->name, common->name, sizeof(local->name));
    Require(TexSample_FindVehicleTexture(common->name, model, shared) == common, "common wins duplicate name");
    std::string upper(common->name);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
    Require(TexSample_FindVehicleTexture(upper.c_str(), model, shared) == common, "case-insensitive common lookup");
    std::snprintf(local->name, sizeof(local->name), "remap_probe");
    Require(TexSample_FindVehicleTexture("remap_probe", model, shared) == local && local->name[0] == '#', "remap rename");
    Require(TexSample_FindVehicleTexture("remap_probe", model, shared) == local, "repeat remap alias lookup");
    Require(TexSample_FindVehicleTexture("#emap_probe", model, shared) == local, "explicit remap alias");
    Require(!TexSample_FindVehicleTexture("missing_probe", model, shared), "missing stays missing");
    std::memcpy(local->name, original, sizeof(original));
    shared->destroy();
    CarPose_Shutdown();
    const auto savedImages = realtime.images;
    WorldShotScene spun;
    Require(CarPose_Init(game, "landstal", 0, 180, spun, oldStats, audit, error, sizeof(error)), error);
    CarPose_Shutdown();
    for (size_t i = 0; i < savedImages.size(); ++i) Require(savedImages[i].rgba == realtime.images[i].rgba, "scene RGBA lifetime across reinit/shutdown");
    std::printf("audit decodedShared=%d/%d materialNames=%zu missingNames=%d missingTriangles=%d\n",
        decodedCount, stats.sharedTextures, names.size(), missingNames, Missing(realtime));
    Require(missingNames == 0 && Missing(realtime) == 0, "all landstal textures decoded");
    Require(Missing(offline) == 2797 && stats.tris == 3613, "original missing-texture negative control");
    VehicleTextureGpuProbe(game, offline, realtime);
    std::puts("vehicle-texture PASS precedence remap RGBA lifetime actual-materials GPU");
}
