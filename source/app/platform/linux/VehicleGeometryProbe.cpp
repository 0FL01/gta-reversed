// Real DFF component selection and wheel-layout audit.
#include "app/platform/linux/CarPose.cpp"
#include <algorithm>
#include <cstdlib>

void VehicleGeometryGpuProbe(const char* game, const WorldShotScene& before, const WorldShotScene& after);

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "vehicle-geometry FAIL %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    const char* game = argc > 1 ? argv[1] : "/game";
    char error[512]{};
    WorldShotScene scene, pristine, spin, steer;
    CarPoseStats stats{};
    CarPoseAudit audit{};
    Require(CarPose_Init(game, "landstal", 0, 0, scene, stats, audit, error, sizeof(error),
        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::StoredAtomics}), error);
    ImgIndex index;
    std::vector<uint8> bytes;
    Require(BuildImgIndex("models/gta3.img", index) && ImgReadBytesStd(index, "landstal.dff", bytes), "read actual landstal DFF");
    auto linked = TexSample_LinkedParse(bytes.data(), bytes.size(), nullptr, nullptr, 0);
    Require(linked.clump, "parse actual landstal clump");
    std::vector<RawFrame> raw;
    Require(ParseFrameNames(bytes, raw, error, sizeof(error)), error);
    std::vector<rw::Frame*> frames(raw.size());
    Require(PairFrames(raw, 0, linked.clump->getFrame(), frames, error, sizeof(error)), error);
    std::vector<std::string> storedNames;
    int damagedTris = 0, damaged = 0, lodTris = 0;
    FORLIST(link, linked.clump->atomics) {
        auto* atomic = rw::Atomic::fromClump(link);
        const auto i = std::find(frames.begin(), frames.end(), atomic->getFrame()) - frames.begin();
        Require(i < raw.size(), "atomic paired frame");
        std::printf("atomic frame=%zu name=%s parent=%s flags=0x%x tris=%d\n", i, raw[i].name.c_str(),
            raw[i].parent < 0 ? "root" : raw[raw[i].parent].name.c_str(), atomic->getFlags(), atomic->geometry->numTriangles);
        Require(atomic->getFlags() == 5, "stored Landstal atomic flags include RENDER even on damaged components");
        if (raw[i].name != "wheel") storedNames.push_back(raw[i].name);
        if (raw[i].name.find("_dam") != std::string::npos) { ++damaged; damagedTris += atomic->geometry->numTriangles; }
        if (raw[i].name == "chassis_vlo") lodTris += atomic->geometry->numTriangles;
    }
    TexSample_FreeLinked(linked);
    Require(damaged == 11 && damagedTris == 717 && lodTris == 111, "actual alternatives regression control");
    CarPoseStats pristineStats{}, spinStats{}, steerStats{};
    Require(CarPose_Init(game, "landstal", 0, 0, pristine, pristineStats, audit, error, sizeof(error), CarPoseTextures::RealtimeVehicle), error);
    Require(CarPose_Init(game, "landstal", 0, 180, spin, spinStats, audit, error, sizeof(error), CarPoseTextures::RealtimeVehicle), error);
    Require(audit.bodySame && audit.before[9] == audit.after[9] && audit.before[10] == audit.after[10] && audit.before[11] == audit.after[11], "spin frame pivot/body invariant");
    Require(CarPose_Init(game, "landstal", 180, 0, steer, steerStats, audit, error, sizeof(error), CarPoseTextures::RealtimeVehicle), error);
    Require(audit.bodySame && audit.before[9] == audit.after[9] && audit.before[10] == audit.after[10] && audit.before[11] == audit.after[11], "steer frame pivot/body invariant");
    Require(pristineStats.geoms == 13 && pristineStats.tris == 2785 && pristine.meshes.size() == 17 &&
        pristineStats.bodyTris == 2221 && pristineStats.wheelTris == 141 && pristineStats.wheels == 4 &&
        pristineStats.damagedAtomicsSkipped == 11 && pristineStats.lodAtomicsSkipped == 1 &&
        pristineStats.extrasAvailable == 0 && pristineStats.extrasSelected == 0, "pristine actual model counts");
    Require(pristineStats.geoms == spinStats.geoms && pristineStats.geoms == steerStats.geoms &&
        pristine.meshes.size() == spin.meshes.size() && pristine.meshes.size() == steer.meshes.size(), "bind/spin/steer layout agreement");
    // Explicit actual component manifest: no simultaneous damaged counterparts,
    // no VLO shell. Compare survivors to their original DFF-derived geometry.
    const std::vector<std::string> expected{"door_lf_ok", "door_rf_ok", "door_lr_ok", "bonnet_ok", "boot_ok",
        "door_rr_ok", "chassis", "exhaust_ok", "windscreen_ok", "bump_rear_ok", "plate_rear_ok", "bump_front_ok", "plate_front_ok"};
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto oldIndex = std::find(storedNames.begin(), storedNames.end(), expected[i]) - storedNames.begin();
        Require(oldIndex < storedNames.size(), "expected source component exists");
        const auto& before = scene.meshes[oldIndex];
        const auto& after = pristine.meshes[i];
        Require(before.pos == after.pos && before.nrm == after.nrm && before.uv == after.uv &&
            before.triCol == after.triCol && before.dayColors == after.dayColors, "selected geometry/normals/UV/prelight/material unchanged");
        Require(after.pos == spin.meshes[i].pos && after.pos == steer.meshes[i].pos, "all body components stationary during wheel poses");
        for (size_t t = 0; t < after.surfaces.size(); ++t) {
            Require(after.surfaces[t].color == before.surfaces[t].color &&
                after.surfaces[t].ambient == before.surfaces[t].ambient && after.surfaces[t].diffuse == before.surfaces[t].diffuse,
                "authored paint/alpha/surface coefficients preserved");
        }
        std::printf("selected mesh=%zu component=%s tris=%d\n", i, expected[i].c_str(), after.tris);
    }
    for (size_t w = 0; w < 4; ++w) {
        const size_t m = pristineStats.geoms + w;
        const auto& bind = pristine.meshes[m];
        Require(bind.pos == scene.meshes[stats.geoms + w].pos, "wheel bind geometry unchanged");
        const auto f = std::find_if(raw.begin(), raw.end(), [&](const auto& frame) { return frame.name == pristineStats.wheelNames[w]; });
        Require(f != raw.end(), "wheel stored frame");
        const float pivot[3]{(bind.pos[0] + steer.meshes[m].pos[0]) * 0.5f,
            (bind.pos[1] + spin.meshes[m].pos[1]) * 0.5f, (bind.pos[2] + spin.meshes[m].pos[2]) * 0.5f};
        Require(std::abs(pivot[1] - f->pos[1]) < 1e-5f && std::abs(pivot[2] - f->pos[2]) < 1e-5f, "gameplay Y/Z pivot inference equals actual DFF dummy");
        const bool front = ClassifyWheel(f->name) == 1;
        Require(!front || std::abs(pivot[0] - f->pos[0]) < 1e-5f, "gameplay front X pivot inference equals DFF dummy");
        Require(front || bind.pos == steer.meshes[m].pos, "rear wheels do not steer");
        Require(bind.pos != spin.meshes[m].pos, "each wheel spins");
        std::printf("wheel=%s pivot=%.6f,%.6f,%.6f front=%d\n", f->name.c_str(), pivot[0], pivot[1], pivot[2], front);
    }
    for (const auto& mesh : pristine.meshes) Require(std::count(mesh.triImg.begin(), mesh.triImg.end(), -2) == 0, "pristine texture completeness");
    for (const auto& image : pristine.images) {
        const auto original = std::find_if(scene.images.begin(), scene.images.end(), [&](const auto& candidate) { return !std::strcmp(candidate.name, image.name); });
        Require(original != scene.images.end() && image.rgba == original->rgba && image.filter == original->filter, "surviving texture/sampler bytes preserved");
    }
    // ZR350 is a real extra-bearing car (the stored optional rear spoiler).
    WorldShotScene noExtra, withExtra;
    CarPoseStats noExtraStats{}, withExtraStats{};
    Require(CarPose_Init(game, "zr350", 0, 0, noExtra, noExtraStats, audit, error, sizeof(error), CarPoseTextures::RealtimeVehicle), error);
    Require(CarPose_Init(game, "zr350", 0, 0, withExtra, withExtraStats, audit, error, sizeof(error),
        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::PristineNear, {0, -1}}), error);
    Require(noExtraStats.extrasAvailable == 1 && noExtraStats.extrasSelected == 0 && withExtraStats.extrasSelected == 1 &&
        withExtraStats.geoms == noExtraStats.geoms + 1 && withExtraStats.tris > noExtraStats.tris, "actual optional extra excluded/selected as one instance");
    WorldShotScene storedExtras, secondSlot;
    CarPoseStats storedExtrasStats{}, secondSlotStats{};
    Require(CarPose_Init(game, "zr350", 0, 0, storedExtras, storedExtrasStats, audit, error, sizeof(error),
        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::StoredAtomics}), error);
    Require(ImgReadBytesStd(index, "zr350.dff", bytes), "read actual extra-bearing DFF");
    linked = TexSample_LinkedParse(bytes.data(), bytes.size(), nullptr, nullptr, 0);
    Require(linked.clump && ParseFrameNames(bytes, raw, error, sizeof(error)), "parse extra-bearing DFF");
    frames.assign(raw.size(), nullptr);
    Require(PairFrames(raw, 0, linked.clump->getFrame(), frames, error, sizeof(error)), error);
    int bodyIndex = 0, matchedExtras = 0;
    FORLIST(link, linked.clump->atomics) {
        auto* atomic = rw::Atomic::fromClump(link);
        const auto i = std::find(frames.begin(), frames.end(), atomic->getFrame()) - frames.begin();
        Require(i < raw.size(), "extra-bearing atomic frame");
        if (raw[i].name == "wheel") continue;
        if (raw[i].name == "extra1") {
            Require(raw[i].parent >= 0 && raw[raw[i].parent].name == "chassis_dummy", "actual extra's instance parent");
            const auto& source = storedExtras.meshes[bodyIndex];
            const auto& selected = withExtra.meshes[noExtraStats.geoms];
            Require(source.tris == 140 && selected.pos == source.pos && selected.nrm == source.nrm &&
                selected.uv == source.uv && selected.dayColors == source.dayColors, "real spoiler geometry and chassis-relative transform");
            ++matchedExtras;
        }
        ++bodyIndex;
    }
    Require(matchedExtras == 1, "exactly one real extra1");
    TexSample_FreeLinked(linked);
    Require(CarPose_Init(game, "zr350", 0, 0, secondSlot, secondSlotStats, audit, error, sizeof(error),
        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::PristineNear, {-1, 0}}), error);
    Require(secondSlot.meshes.size() == withExtra.meshes.size() && secondSlotStats.extrasSelected == 1, "second forced component slot");
    for (size_t i = 0; i < withExtra.meshes.size(); ++i) Require(withExtra.meshes[i].pos == secondSlot.meshes[i].pos, "stable forced extra geometry/order");
    Require(!CarPose_Init(game, "zr350", 0, 0, secondSlot, secondSlotStats, audit, error, sizeof(error),
        CarPoseTextures::RealtimeVehicle, {CarPoseGeometry::PristineNear, {1, -1}}), "invalid forced component rejected");
    std::printf("extras model=zr350 available=%d noneTris=%d selectedTris=%d selected=%d\n", noExtraStats.extrasAvailable,
        noExtraStats.tris, withExtraStats.tris, withExtraStats.extrasSelected);
    CarPose_Shutdown();
    VehicleGeometryGpuProbe(game, scene, pristine);
    std::printf("vehicle-geometry PASS storedTris=%d pristineTris=%d bodyMeshes=%d damagedSkipped=%d lodSkipped=%d images=%zu\n",
        stats.tris, pristineStats.tris, pristineStats.geoms, pristineStats.damagedAtomicsSkipped, pristineStats.lodAtomicsSkipped, pristine.images.size());
}
