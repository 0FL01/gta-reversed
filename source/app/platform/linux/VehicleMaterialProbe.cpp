// Actual DFF material evidence; separate TU from the GL-only renderer probe.
#include "app/platform/linux/CarPose.cpp"
#include <algorithm>
#include <cstdlib>

void VehicleMaterialGpuProbe(const char* game, const WorldShotScene& scene);
static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "vehicle-material FAIL %s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    WorldShotScene scene;
    CarPoseStats stats{};
    CarPoseAudit audit{};
    char error[512]{};
    Require(CarPose_Init(game, "landstal", 0, 0, scene, stats, audit, error, sizeof(error), CarPoseTextures::RealtimeVehicle), error);
    ImgIndex index;
    std::vector<uint8> bytes;
    Require(BuildImgIndex("models/gta3.img", index) && ImgReadBytesStd(index, "landstal.dff", bytes), "actual DFF read");
    auto linked = TexSample_LinkedParse(bytes.data(), bytes.size(), nullptr, nullptr, 0);
    Require(linked.clump, "actual DFF parse");
    std::vector<RawFrame> raw;
    Require(ParseFrameNames(bytes, raw, error, sizeof(error)), error);
    std::vector<rw::Frame*> frames(raw.size());
    Require(PairFrames(raw, 0, linked.clump->getFrame(), frames, error, sizeof(error)), error);
    int glass = 0;
    size_t selected = 0;
    int authoredAlphaTriangles = 0;
    FORLIST(link, linked.clump->atomics) {
        auto* atomic = rw::Atomic::fromClump(link);
        const auto i = std::find(frames.begin(), frames.end(), atomic->getFrame()) - frames.begin();
        Require(i < raw.size(), "atomic source frame");
        const auto* geo = atomic->geometry;
        const auto& name = raw[i].name;
        if (name != "wheel" && name.find("_dam") == std::string::npos && name.find("_vlo") == std::string::npos) {
            const auto& mesh = scene.meshes.at(selected++);
            Require(mesh.tris == geo->numTriangles, "source triangle topology");
            for (int t = 0; t < geo->numTriangles; ++t) {
                const auto* material = geo->matList.materials[geo->triangles[t].matId];
                Require(mesh.surfaces[t].color[3] == material->color.alpha / 255.0f, "GPU alpha equals actual DFF alpha");
                if (material->color.alpha != 255) {
                    ++authoredAlphaTriangles;
                    Require(mesh.surfaces[t].vehicleAlpha, "actual translucent triangle reaches ordered GPU pass");
                }
                if (name == "windscreen_ok") {
                    Require(material->color.red == 255 && material->color.green == 255 && material->color.blue == 255 &&
                        material->color.alpha == 128 && material->texture && !std::strcmp(material->texture->name, "vehiclegeneric256"), "actual windscreen material source");
                }
            }
        }
        for (int m = 0; m < geo->matList.numMaterials; ++m) {
            const auto* mat = geo->matList.materials[m];
            const auto c = mat->color;
            if (c.alpha == 255) continue;
            ++glass;
            std::printf("source frame=%s material=%d rgba=%u,%u,%u,%u texture=%s geometryFlags=0x%x\n",
                raw[i].name.c_str(), m, c.red, c.green, c.blue, c.alpha,
                mat->texture ? mat->texture->name : "none", geo->flags);
        }
    }
    Require(glass > 0, "actual authored translucent material");
    Require(selected == static_cast<size_t>(stats.geoms) && authoredAlphaTriangles > 0, "all pristine body materials audited");
    std::printf("material-source PASS bodyMeshes=%zu authoredAlphaTriangles=%d windscreen=RGBA255,255,255,128\n", selected, authoredAlphaTriangles);
    for (const auto& image : scene.images) {
        int low = 255, high = 0, fractional = 0;
        for (size_t i = 3; i < image.rgba.size(); i += 4) {
            const int a = image.rgba[i]; low = std::min(low, a); high = std::max(high, a);
            fractional += a > 0 && a < 255;
        }
        std::printf("texture name=%s alphaMin=%d alphaMax=%d fractional=%d\n", image.name, low, high, fractional);
    }
    TexSample_FreeLinked(linked);
    WorldShotScene offline;
    Require(CarPose_Init(game, "landstal", 0, 0, offline, stats, audit, error, sizeof(error)), error);
    for (const auto& mesh : offline.meshes) {
        for (const auto& surface : mesh.surfaces) Require(!surface.vehicleAlpha, "default ModelOnly does not opt into realtime alpha pass");
    }
    CarPose_Shutdown();
    VehicleMaterialGpuProbe(game, scene);
}
