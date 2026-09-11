#include "app/platform/linux/NativeGeneratedVehicleAssets.h"
#include "app/platform/linux/CarPose.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <numbers>

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
using Bytes = std::span<const std::uint8_t>;
struct UnsupportedContract : std::runtime_error { using std::runtime_error::runtime_error; };
void Represented(bool ok, const std::string& message) {
    if (!ok) throw UnsupportedContract(message);
}
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
uint32 U32(Bytes b, size_t p = 0) {
    Check(p <= b.size() && b.size() - p >= 4, "truncated DFF word");
    return uint32(b[p]) | uint32(b[p + 1]) << 8 | uint32(b[p + 2]) << 16 | uint32(b[p + 3]) << 24;
}
struct Chunk { uint32 Type; Bytes Data; };
std::vector<Chunk> Chunks(Bytes bytes) {
    std::vector<Chunk> result;
    while (!bytes.empty()) {
        Check(bytes.size() >= 12, "truncated DFF chunk");
        const size_t size = U32(bytes, 4);
        Check(size <= bytes.size() - 12, "DFF chunk bounds");
        result.push_back({U32(bytes), bytes.subspan(12, size)});
        bytes = bytes.subspan(12 + size);
    }
    return result;
}
Bytes Clump(Bytes bytes) {
    Check(U32(bytes) == 16 && bytes.size() >= 12, "DFF is not a clump");
    const size_t size = U32(bytes, 4);
    Check(size <= bytes.size() - 12, "DFF clump bounds");
    // IMG sector padding can contain stale bytes; never parse beyond the clump.
    return bytes.subspan(12, size);
}
Bytes Unique(const std::vector<Chunk>& chunks, uint32 type) {
    Bytes result;
    bool found = false;
    for (const auto& chunk : chunks) if (chunk.Type == type) {
        Check(!found, "duplicate DFF structural chunk");
        found = true;
        result = chunk.Data;
    }
    Check(found, "missing DFF structural chunk");
    return result;
}
std::vector<NativeGeneratedVehicleFrame> Frames(Bytes bytes) {
    const auto chunks = Chunks(Unique(Chunks(Clump(bytes)), 14));
    Check(!chunks.empty() && chunks.front().Type == 1, "DFF FrameList struct");
    const auto data = chunks.front().Data;
    const auto count = U32(data);
    Check(count && count <= 10000 && data.size() == 4ull + count * 56ull && chunks.size() == count + 1ull,
        "DFF FrameList bounds");
    std::vector<NativeGeneratedVehicleFrame> frames(count);
    for (size_t i = 0; i < count; ++i) {
        auto& frame = frames[i];
        const size_t offset = 4 + i * 56;
        frame.Parent = std::bit_cast<int32>(U32(data, offset + 48));
        Check(i == 0 ? frame.Parent == -1 : frame.Parent >= 0 && size_t(frame.Parent) < i,
            "unsupported DFF parent order/root");
        frame.Flags = U32(data, offset + 52);
        for (size_t j = 0; j < 12; ++j) {
            frame.LocalBind[j] = std::bit_cast<float>(U32(data, offset + j * 4));
            Check(std::isfinite(frame.LocalBind[j]), "nonfinite DFF bind matrix");
        }
        frame.ModelBind = frame.LocalBind;
        if (i) {
            const auto& parent = frames[frame.Parent].ModelBind;
            for (size_t axis = 0; axis < 4; ++axis) for (size_t j = 0; j < 3; ++j) {
                float value = axis == 3 ? parent[9 + j] : 0.0f;
                for (size_t k = 0; k < 3; ++k) value += frame.LocalBind[axis * 3 + k] * parent[k * 3 + j];
                Check(std::isfinite(value), "nonfinite DFF model matrix");
                frame.ModelBind[axis * 3 + j] = value;
            }
        }
        frame.LocalPose = frame.LocalBind;
        frame.ModelPose = frame.ModelBind;
        Check(chunks[i + 1].Type == 3, "DFF frame extension");
        for (const auto& ext : Chunks(chunks[i + 1].Data)) if (ext.Type == 0x253f2fe) {
            Check(frame.Name.empty() && !ext.Data.empty() && ext.Data.size() <= 64, "DFF node name bounds");
            const auto* name = reinterpret_cast<const char*>(ext.Data.data());
            frame.Name.assign(name, strnlen(name, ext.Data.size()));
        }
    }
    return frames;
}
struct File {
    void* Handle = nullptr;
    int32 Size{};
    explicit File(const std::string& path) {
        Check(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &Handle, path.c_str(), FILE_ACCESS_READ) == 0 && Handle,
            "cannot open " + path);
        Size = OS_FileSize(Handle);
    }
    ~File() { if (Handle) OS_FileClose(Handle); }
    File(const File&) = delete;
    Bytes Read(int32 offset, int32 count, std::vector<uint8_t>& buffer) {
        Check(offset >= 0 && count > 0 && offset <= Size && count <= Size - offset, "IMG read bounds");
        OS_FileSetPosition(Handle, offset);
        Check(OS_FileGetPosition(Handle) == offset, "IMG seek");
        buffer.resize(count);
        Check(OS_FileRead(Handle, buffer.data(), count) == 0, "IMG read");
        return buffer;
    }
};
std::vector<uint8_t> ReadDff(const char* gameDir, const std::string& name) {
    File file(std::string(gameDir) + "/models/gta3.img");
    std::vector<uint8_t> buffer;
    const auto header = file.Read(0, 8, buffer);
    Check(!std::memcmp(header.data(), "VER2", 4), "unsupported IMG version");
    const auto count = U32(header, 4);
    Check(count && count <= 300000, "IMG directory count");
    const auto directory = file.Read(8, int32(count * 32), buffer);
    int32 offset = -1, length = 0;
    for (size_t p = 0; p < directory.size(); p += 32) {
        const auto* entry = reinterpret_cast<const char*>(directory.data() + p + 8);
        if (std::string(entry, strnlen(entry, 24)) != name + ".dff") continue;
        Check(offset == -1, "ambiguous IMG DFF name");
        const uint64 off = uint64(U32(directory, p)) * 2048;
        const uint64 size = uint64(U32(directory, p + 4) & 0x7fff) * 2048;
        Check(off >= 8ull + count * 32ull && size && off + size <= uint64(file.Size), "IMG DFF extent");
        offset = int32(off);
        length = int32(size);
    }
    Check(offset >= 0, "missing actual model DFF: " + name);
    file.Read(offset, length, buffer);
    const auto clump = Clump(buffer);
    buffer.resize(clump.size() + 12);
    return buffer;
}

bool Wheel(const std::string& name) {
    return name == "wheel_lf_dummy" || name == "wheel_rf_dummy" ||
        name == "wheel_lb_dummy" || name == "wheel_rb_dummy";
}

// Independently recovered retail constructor 0x6FDB00 -> vtable 0x8BD0BC,
// slot 17 -> 0x6FE1C0. Only the first PARKED/ABANDONED PreRender is audited.
// Resolve descriptor names here; generic flattening only sees source indices.
void RustlerInitialPose(Bytes dff, NativeGeneratedVehiclePacket& packet, const char* gameDir) {
    auto& frames = packet.Frames;
    Represented(frames.size() == 27, "unaudited Rustler frame count");
    const auto frameIndex = [&](const char* name) {
        uint32 index = uint32(frames.size());
        for (uint32 i = 0; i < frames.size(); ++i) if (frames[i].Name == name) {
            Represented(index == frames.size(), "duplicate descriptor frame");
            index = i;
        }
        Represented(index < frames.size(), std::string("missing descriptor frame: ") + name);
        return index;
    };
    // Actual topology, including non-component position/light nodes. Coordinates
    // are always read from DFF, never taken from the numeric proof's output.
    static constexpr const char* names[]{"rustler", "chassis", "static_prop", "gear_l", "aileron_l",
        "rudder", "aileron_r", "gear_r", "elevator_r", "moving_prop", "ped_frontseat", "exhaust",
        "wheel_lb_dummy", "wheel_rb_dummy", "door_lf_dummy", "aileron_pos", "chassis_vlo", "miscpos_a",
        "wingtip_pos", "engine", "door_lf_ok", "wheel_rf_dummy", "wheel", "wheel_lf_dummy", "Omni01", "Omni02", "Omni03"};
    static constexpr int parents[]{-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,7,21,3,1,1,1};
    for (size_t i = 0; i < frames.size(); ++i) {
        Represented(frames[i].Name == names[i] && frames[i].Parent == parents[i], "unaudited Rustler hierarchy");
        if (i < 24) for (size_t j = 0; j < 9; ++j)
            Represented(frames[i].LocalBind[j] == (j % 4 == 0 ? 1.0f : 0.0f), "unaudited Rustler bind basis");
    }
    const auto chunks = Chunks(Clump(dff));
    std::vector<Bytes> geometries;
    for (const auto& chunk : Chunks(Unique(chunks, 26))) if (chunk.Type == 15) geometries.push_back(chunk.Data);
    Represented(geometries.size() == 12, "unaudited Rustler geometry count");
    static constexpr uint32 sourceFrames[]{1,2,3,4,5,6,7,22,8,9,20,16};
    uint32 ordinal = 0;
    for (const auto& chunk : chunks) if (chunk.Type == 20) {
        const auto data = Unique(Chunks(chunk.Data), 1);
        Represented(ordinal < 12 && U32(data) == sourceFrames[ordinal] && U32(data, 4) == ordinal && U32(data, 8) == 5,
            "unaudited Rustler source atomic binding/flags");
        ++ordinal;
    }
    Represented(ordinal == 12, "unaudited Rustler atomic count");
    const auto alpha = [&](uint32 atomic) {
        const auto materials = Chunks(Unique(Chunks(geometries[atomic]), 8));
        std::vector<Bytes> records;
        for (const auto& material : materials) if (material.Type == 7) records.push_back(Unique(Chunks(material.Data), 1));
        Represented(records.size() == 1 && records.front().size() >= 8, "unaudited prop materials");
        return records.front()[7];
    };
    Represented(alpha(1) == 230 && alpha(9) == 255, "unaudited prop authored alpha");
    packet.AtomicOverrides = {
        {1, frameIndex("static_prop"), 1, true, uint8_t(255), {}},
        {9, frameIndex("moving_prop"), 9, false, uint8_t(0), {}},
        {10, frameIndex("door_lf_ok"), 10, {}, {}, frameIndex("door_lf_dummy")},
        {7, frameIndex("wheel"), 7, {}, {}, frameIndex("wheel_rf_dummy")},
    };

    File handling(std::string(gameDir) + "/data/handling.cfg");
    std::vector<uint8_t> buffer;
    Check(handling.Size > 0 && handling.Size < 4 * 1024 * 1024, "handling file bounds");
    handling.Read(0, handling.Size, buffer);
    std::istringstream lines(std::string(buffer.begin(), buffer.end()));
    std::vector<std::string> fields;
    for (std::string line; std::getline(lines, line);) {
        std::istringstream tokens(line);
        std::string name;
        if (!(tokens >> name) || name != packet.Definition.HandlingName) continue;
        Represented(fields.empty(), "ambiguous Rustler handling");
        fields.push_back(name);
        while (tokens >> name) fields.push_back(name);
    }
    Represented(fields.size() >= 33, "missing Rustler suspension handling");
    const float force = std::stof(fields[21]), upper = std::stof(fields[24]), lower = std::stof(fields[25]);
    Represented(force == 2.0f && upper == 0.5f && lower == -0.2f &&
        std::stoul(fields[31], nullptr, 16) == 0x4008108 && std::stoul(fields[32], nullptr, 16) == 0x400020,
        "unaudited Rustler handling flags/suspension");
    const float front = packet.Definition.WheelSizeFront, rear = packet.Definition.WheelSizeRear;
    Represented(front == 0.6f && rear == 0.3f && packet.Definition.Misc == -1, "unaudited Rustler wheel definition");
    const float spring = upper - lower;
    const float start = frames[frameIndex("wheel_lf_dummy")].ModelBind[11] + upper;
    const float springHeight = (1.0f - 1.0f / (force * 4.0f)) * spring;
    const float height = float(double(springHeight) - start + double(front) * 0.5);
    for (const char* name : {"wheel_lf_dummy", "wheel_lb_dummy", "wheel_rf_dummy", "wheel_rb_dummy"}) {
        const auto i = frameIndex(name);
        const bool left = name[6] == 'l', isFront = name[7] == 'f';
        auto matrix = frames[i].LocalBind;
        const float position = (isFront ? front : rear) * 0.5f - height;
        matrix[11] = float(double(position) - frames[i].ModelBind[11] + matrix[11]);
        const float yz = isFront ? 1.0f : rear / front;
        // UpdateWheelMatrix's source wheel width normalization, retail 0x8A2270.
        const float x = (front / 0.7f) * yz;
        const float yaw = left ? std::numbers::pi_v<float> : 0.0f;
        const float si = std::sin(yaw), co = std::cos(yaw);
        matrix[0] = x * co; matrix[1] = x * si; matrix[2] = 0;
        matrix[3] = -yz * si; matrix[4] = yz * co; matrix[5] = 0;
        matrix[6] = 0; matrix[7] = 0; matrix[8] = yz;
        packet.FrameOverrides.push_back({i, matrix});
    }
    // Zero phase and gear reset rotations preserve the loaded translations.
    for (const char* name : {"static_prop", "moving_prop", "gear_l", "gear_r"}) {
        const auto i = frameIndex(name);
        packet.FrameOverrides.push_back({i, frames[i].LocalBind});
    }
    for (const auto& value : packet.FrameOverrides) frames[value.Frame].LocalPose = value.LocalMatrix;
    for (auto& frame : frames) {
        frame.ModelPose = frame.LocalPose;
        if (frame.Parent < 0) continue;
        const auto& parent = frames[frame.Parent].ModelPose;
        for (size_t axis = 0; axis < 4; ++axis) for (size_t j = 0; j < 3; ++j) {
            float value = axis == 3 ? parent[9 + j] : 0.0f;
            for (size_t k = 0; k < 3; ++k) value += frame.LocalPose[axis * 3 + k] * parent[k * 3 + j];
            frame.ModelPose[axis * 3 + j] = value;
        }
    }
    packet.Pose = NativeGeneratedVehiclePacket::PoseContract::FreshParkedAbandoned476;
}
void Components(Bytes dff, NativeGeneratedVehiclePacket& packet) {
    const auto chunks = Chunks(Clump(dff));
    const auto geometryList = Chunks(Unique(chunks, 26));
    std::vector<uint32> triangles;
    std::vector<uint32> formats;
    for (const auto& geometry : geometryList) if (geometry.Type == 15) {
        const auto data = Unique(Chunks(geometry.Data), 1);
        Check(data.size() >= 16, "DFF geometry struct");
        Represented(!(U32(data) & 0x01000000), "native-only vehicle geometry unsupported");
        triangles.push_back(U32(data, 4));
        formats.push_back(U32(data));
    }
    std::vector<NativeGeneratedVehicleComponent> kit;
    uint32 atomicIndex = 0;
    int32 mesh = 0;
    for (const auto& chunk : chunks) if (chunk.Type == 20) {
        const auto data = Unique(Chunks(chunk.Data), 1);
        NativeGeneratedVehicleComponent component;
        component.Atomic = atomicIndex++;
        component.SourceFrame = component.BindFrame = U32(data);
        component.Geometry = U32(data, 4);
        component.Flags = U32(data, 8);
        Check(component.SourceFrame < packet.Frames.size() && component.Geometry < triangles.size(), "DFF atomic index");
        component.RuntimeFlags = component.Flags;
        component.GeometryFlags = component.RuntimeGeometryFlags = formats[component.Geometry];
        const auto* override = CarPose_FindAtomicOverride(packet.AtomicOverrides, component.Atomic);
        if (override) {
            Check(override->SourceFrame == component.SourceFrame && override->SourceGeometry == component.Geometry,
                "component override source identity");
            component.BindFrame = override->BindFrame.value_or(component.BindFrame);
            component.MaterialAlpha = override->MaterialAlpha;
            if (component.MaterialAlpha) component.RuntimeGeometryFlags |= 0x40;
        }
        const auto& name = packet.Frames[component.SourceFrame].Name;
        if (name.find("_dam") != std::string::npos) component.RuntimeFlags &= ~4u;
        Represented(!name.starts_with("extra"), "extra selection not represented");
        if (!CarPose_OverrideVisible(override)) component.Use = NativeGeneratedVehicleComponentUse::AlphaSuppressed;
        else if (name.find("_dam") != std::string::npos) component.Use = NativeGeneratedVehicleComponentUse::DamagedAlternative;
        else if (!(component.Flags & 4)) component.Use = NativeGeneratedVehicleComponentUse::NonRendering;
        else if (name.find("_vlo") != std::string::npos) component.Use = NativeGeneratedVehicleComponentUse::FarLod;
        else {
            int32 ancestor = int32(component.BindFrame);
            while (ancestor >= 0 && !Wheel(packet.Frames[ancestor].Name)) ancestor = packet.Frames[ancestor].Parent;
            if (ancestor >= 0) {
                component.Use = NativeGeneratedVehicleComponentUse::WheelInstance;
                kit.push_back(component);
                continue;
            }
            component.Use = NativeGeneratedVehicleComponentUse::PristineBody;
            component.Mesh = mesh++;
        }
        packet.Components.push_back(component);
    }
    Represented(kit.size() == 1, "unrepresented wheel kit topology");
    size_t wheels = 0;
    for (size_t i = 0; i < packet.Frames.size(); ++i) if (Wheel(packet.Frames[i].Name)) {
        auto component = kit.front();
        component.BindFrame = uint32(i);
        component.Mesh = mesh++;
        packet.Components.push_back(component);
        ++wheels;
    }
    Represented(wheels == 4 && size_t(mesh) == packet.Scene.meshes.size(), "CarPose/component layout mismatch");
    for (const auto& component : packet.Components) if (component.Mesh >= 0) {
        Check(triangles[component.Geometry] == uint32(packet.Scene.meshes[component.Mesh].tris), "CarPose/component triangle mismatch");
    }
}

// CarPose/TexSample parser state is exclusively owned by the caller's startup
// or sole worker phase. Preserve the caller's current dictionary on all paths.
struct ParserScope {
    rw::TexDictionary* Current = rw::Engine::state == rw::Engine::Dead ? nullptr : rw::TexDictionary::getCurrent();
    ~ParserScope() {
        CarPose_Shutdown(); // only destroys this call's private per-model TXDs
        if (rw::Engine::state != rw::Engine::Dead) rw::TexDictionary::setCurrent(Current);
    }
};

NativeGeneratedVehicleAssetResult Unsupported(const std::string& reason) {
    return {NativeGeneratedVehicleAssetStatus::Unsupported, reason};
}
} // namespace

bool NativeGeneratedVehicleAssets_ReadFrames(Bytes dff, std::vector<NativeGeneratedVehicleFrame>& out,
    std::string& error) try {
    auto frames = Frames(dff);
    out = std::move(frames);
    error.clear();
    return true;
} catch (const std::exception& e) { error = e.what(); return false; }

NativeGeneratedVehicleAssetResult NativeGeneratedVehicleAssets_Load(const char* gameDir,
    const NativeCarGeneratorModelDefinition& definition, const NativeCollisionAssets& catalog,
    NativeGeneratedVehicleAsset& out) try {
    const bool rustler = definition.ModelId == 476 && definition.ModelName == "rustler" &&
        definition.Type == NativeVehicleType::Plane && definition.TypeName == "plane" && definition.HandlingName == "RUSTLER";
    const bool landstal = definition.ModelId == 400 && definition.ModelName == "landstal" &&
        definition.Type == NativeVehicleType::Automobile && definition.TypeName == "car" && definition.HandlingName == "LANDSTAL";
    if (!rustler && !landstal) return Unsupported("unaudited model/type/handling identity: " + definition.ModelName);
    if (definition.TextureName != definition.ModelName) return Unsupported(
        "CarPose requires model-named TXD; actual IDE TextureName differs");
    if (definition.ComponentRules || definition.Flags) return Unsupported("unrepresented IDE component rules/flags");
    Check(gameDir && gameDir[0], "missing game directory");

    auto packet = std::make_shared<NativeGeneratedVehiclePacket>();
    packet->Definition = definition;
    const auto dff = ReadDff(gameDir, definition.ModelName);
    packet->Frames = Frames(dff);
    Check(!packet->Frames.empty() && packet->Frames.front().Name == definition.ModelName, "DFF root/model identity mismatch");
    if (rustler) RustlerInitialPose(dff, *packet, gameDir);
    std::string error;
    // Vehicle COL is usually embedded in the actual model clump. The world
    // catalog intentionally scans .col entries, not every DFF in the IMG.
    // CCollisionPlugin::ClumpCollisionStreamRead binds this owned source COL
    // to the current model; retain its authored _col name and stale header ID.
    for (const auto& extension : Chunks(Clump(dff))) if (extension.Type == 3) {
        for (const auto& plugin : Chunks(extension.Data)) if (plugin.Type == 0x253f2fa) {
            Check(!packet->Collision, "multiple embedded vehicle COL chunks");
            auto collision = std::make_shared<NativeCollisionModel>();
            if (!NativeCollisionAssets::Parse(plugin.Data, "gta3.img:" + definition.ModelName + ".dff:0x253f2fa",
                *collision, error)) return Unsupported("authored embedded COL unsupported: " + error);
            Check(collision->Name == definition.ModelName + "_col", "embedded COL/model name mismatch: " + collision->Name);
            collision->ChunkOffset = uint32(plugin.Data.data() - dff.data());
            // HeaderId is provenance only, never used as the vehicle model ID.
            packet->Collision = std::move(collision);
            packet->EmbeddedCollision = true;
        }
    }
    // Use the existing immutable name-binding API with one identity at origin.
    // This is a catalog lookup only, NOT world placement or a DFF-bounds COL.
    if (!packet->Collision) {
        Represented(!rustler, "audited Rustler requires actual embedded COL");
        NativeCollisionPopulation population;
        population.IncludesStreamed = true;
        population.Models.emplace(definition.ModelId, NativeCollisionIde{definition.ModelName, false});
        NativeCollisionPlacement identity;
        identity.Model = definition.ModelName;
        identity.ModelId = definition.ModelId;
        population.Instances.push_back(identity);
        NativeCollisionSnapshot snapshot;
        if (!catalog.Snapshot(population, 0, 0, std::numeric_limits<float>::max() / 4, snapshot, error)) {
            return Unsupported("real authored COL required: " + error);
        }
        Check(snapshot.Instances.size() == 1 && !snapshot.Instances.front().TimeShared, "ambiguous vehicle COL binding");
        packet->Collision = snapshot.Instances.front().Model;
        Check(packet->Collision && packet->Collision->Name == definition.ModelName, "catalog COL/model name mismatch");
    }
    Check(packet->Collision && !packet->Collision->Empty && !packet->Collision->SourceChunk.empty() &&
        (!packet->Collision->Faces.empty() || !packet->Collision->Spheres.empty() || !packet->Collision->Boxes.empty()),
        "missing name-validated authored vehicle COL");
    packet->DffBytes = uint32(dff.size());
    packet->DffFingerprint = 14695981039346656037ull;
    for (auto byte : dff) { packet->DffFingerprint ^= byte; packet->DffFingerprint *= 1099511628211ull; }
    ParserScope parser;
    CarPoseStats stats{};
    CarPoseAudit audit{};
    char message[512]{};
    const bool loaded = CarPose_Init(gameDir, definition.ModelName.c_str(), 0, 0, packet->Scene, stats, audit,
        message, sizeof(message), CarPoseTextures::RealtimeVehicle,
        {CarPoseGeometry::PristineNear, {-1, -1}, packet->AtomicOverrides, packet->FrameOverrides});
    Check(loaded, message);
    Represented(definition.ModelName == stats.model && stats.sharedTextures > 0 && stats.extrasAvailable == 0 &&
        stats.wheels == 4 && audit.bodySame, "CarPose actual model/bind contract mismatch");
    Components(dff, *packet);
    if (rustler) Represented(packet->Scene.meshes.size() == 13 && stats.tris == 2470 && stats.bodyTris == 2062 &&
        stats.wheelTris == 102, "unaudited Rustler near topology");
    packet->DffSource = stats.src;
    packet->ModelTxdSource = stats.txd;
    packet->CommonTxdSource = "models/generic/vehicle.txd";
    for (auto& mesh : packet->Scene.meshes) {
        const auto count = size_t(mesh.tris);
        Check(count && mesh.pos.size() == count * 9 && mesh.nrm.size() == count * 9 && mesh.uv.size() == count * 6 &&
            mesh.triCol.size() == count * 3 && mesh.triImg.size() == count && mesh.surfaces.size() == count &&
            mesh.dayColors.size() == count * 12, "incomplete vehicle soup/material data");
        std::copy_n(mesh.triCol.begin(), 3, mesh.color); // authored fallback, never CarPose's diagnostic mesh palette
        NativeGeneratedVehiclePaint paint;
        for (size_t t = 0; t < count; ++t) {
            Check(mesh.triImg[t] >= -1 && (mesh.triImg[t] < 0 || size_t(mesh.triImg[t]) < packet->Scene.images.size()),
                "missing actual vehicle texture");
            uint32 rgb = 0;
            for (size_t channel = 0; channel < 3; ++channel) {
                const auto authored = mesh.triCol[t * 3 + channel];
                mesh.surfaces[t].color[channel] = authored;
                rgb |= uint32(std::lround(authored * 255.0f)) << (channel * 8);
            }
            // CVehicleModelInfo::SetEditableMaterialsCB marker encoding.
            paint.Slots.push_back(rgb == 0x00ff3c ? 0 : rgb == 0xaf00ff ? 1 : rgb == 0xffff00 ? 2 : rgb == 0xff00ff ? 3 : -1);
            mesh.surfaces[t].vehicleColorIndex = -1;
        }
        packet->Paint.push_back(std::move(paint));
    }
    for (const auto& image : packet->Scene.images) Check(image.w > 0 && image.h > 0 &&
        image.rgba.size() == size_t(image.w) * size_t(image.h) * 4, "incomplete owned vehicle image");
    out = std::move(packet);
    return {NativeGeneratedVehicleAssetStatus::Ready, {}};
} catch (const UnsupportedContract& e) { return Unsupported(e.what()); }
catch (const std::exception& e) { return {NativeGeneratedVehicleAssetStatus::Error, e.what()}; }
