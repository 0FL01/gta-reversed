#include "app/platform/linux/NativeCarGenerators.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;
constexpr float RadiansPerAngleUnit = 2.0f * std::numbers::pi_v<float> / 256.0f;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string Lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
    }
    return value;
}

std::string Clean(std::string value, bool commas = false) {
    if (const auto comment = value.find('#'); comment != value.npos) {
        value.resize(comment);
    }
    if (commas) {
        std::replace(value.begin(), value.end(), ',', ' ');
    }
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

template<typename T>
T Number(std::string_view text, int base = 10) {
    T value{};
    const auto result = [&] {
        if constexpr (std::is_floating_point_v<T>) {
            return std::from_chars(text.data(), text.data() + text.size(), value);
        } else {
            return std::from_chars(text.data(), text.data() + text.size(), value, base);
        }
    }();
    const auto [end, ec] = result;
    Require(ec == std::errc{} && end == text.data() + text.size(), "invalid car-generator numeric field");
    if constexpr (std::is_floating_point_v<T>) {
        Require(std::isfinite(value), "nonfinite car-generator numeric field");
    }
    return value;
}

std::vector<std::string> Tokens(std::string row) {
    std::replace(row.begin(), row.end(), ',', ' ');
    std::istringstream stream(row);
    std::vector<std::string> result;
    for (std::string token; stream >> token;) {
        result.push_back(std::move(token));
    }
    return result;
}

std::string AssetPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    Require(!path.empty() && path.front() != '/' && path.find(':') == path.npos &&
        path.find("..") == path.npos, "invalid car-generator asset path");
    return path;
}

std::string ResolvePath(const char* gameDir, const std::string& relative) {
    auto path = std::filesystem::absolute(gameDir);
    for (const auto& component : std::filesystem::path(AssetPath(relative))) {
        if (component == ".") {
            continue;
        }
        std::filesystem::path match;
        for (const auto& child : std::filesystem::directory_iterator(path)) {
            if (Lower(child.path().filename().string()) != Lower(component.string())) {
                continue;
            }
            Require(match.empty(), "ambiguous case-insensitive car-generator asset path: " + relative);
            match = child.path();
        }
        Require(!match.empty(), "missing car-generator asset path: " + relative);
        path = std::move(match);
    }
    return path.string();
}

struct File {
    void* Handle = nullptr;
    int32 Size = -1;
    explicit File(const std::string& path) {
        Require(OS_FileOpen(FILE_DATA_AREA_DEFAULT, &Handle, path.c_str(), FILE_ACCESS_READ) == 0 && Handle,
            "car-generator open: " + path);
        Size = OS_FileSize(Handle);
        Require(Size >= 0, "car-generator size: " + path);
    }
    ~File() {
        if (Handle) {
            OS_FileClose(Handle);
        }
    }
    std::vector<uint8> Read(uint64 offset, std::size_t size) {
        Require(offset <= uint64(Size) && size <= uint64(Size) - offset &&
            offset <= uint64(std::numeric_limits<int32>::max()) && size <= std::size_t(std::numeric_limits<int32>::max()),
            "car-generator file range");
        OS_FileSetPosition(Handle, int32(offset));
        Require(OS_FileGetPosition(Handle) == int32(offset), "car-generator seek");
        std::vector<uint8> bytes(size);
        Require(!size || OS_FileRead(Handle, bytes.data(), int32(size)) == 0, "car-generator read");
        return bytes;
    }
};

std::string ReadText(const std::string& path) {
    File file(path);
    Require(file.Size <= 16 * 1024 * 1024, "car-generator text file bound: " + path);
    const auto bytes = file.Read(0, std::size_t(file.Size));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

uint16 U16(const uint8* bytes) {
    return uint16(bytes[0]) | uint16(uint16(bytes[1]) << 8);
}

uint32 U32(const uint8* bytes) {
    return uint32(bytes[0]) | (uint32(bytes[1]) << 8) | (uint32(bytes[2]) << 16) | (uint32(bytes[3]) << 24);
}

float F32(const uint8* bytes) {
    return std::bit_cast<float>(U32(bytes));
}

NativeVehicleType VehicleType(std::string_view name) {
    static constexpr std::pair<std::string_view, NativeVehicleType> Types[]{
        {"car", NativeVehicleType::Automobile}, {"mtruck", NativeVehicleType::MonsterTruck},
        {"quad", NativeVehicleType::Quad}, {"heli", NativeVehicleType::Helicopter},
        {"plane", NativeVehicleType::Plane}, {"boat", NativeVehicleType::Boat},
        {"train", NativeVehicleType::Train}, {"f_heli", NativeVehicleType::FakeHelicopter},
        {"f_plane", NativeVehicleType::FakePlane}, {"bike", NativeVehicleType::Bike},
        {"bmx", NativeVehicleType::Bmx}, {"trailer", NativeVehicleType::Trailer},
    };
    const auto found = std::ranges::find_if(Types, [&](const auto& type) { return type.first == name; });
    return found == std::end(Types) ? NativeVehicleType::Unsupported : found->second;
}

NativeCarGeneratorModelDefinition ParseModel(std::string_view row, const std::string& source, uint32 line) {
    const auto fields = Tokens(std::string(row));
    Require(fields.size() >= 11, "vehicle IDE row requires at least 11 fields");
    NativeCarGeneratorModelDefinition model;
    model.ModelId = Number<int32>(fields[0]);
    model.ModelName = Lower(fields[1]);
    model.TextureName = Lower(fields[2]);
    model.TypeName = Lower(fields[3]);
    model.Type = VehicleType(model.TypeName);
    Require(model.Type != NativeVehicleType::Unsupported, "unsupported vehicle IDE type");
    model.HandlingName = fields[4];
    model.GameName = fields[5];
    model.AnimationGroup = fields[6];
    model.ClassName = fields[7];
    model.Frequency = Number<uint32>(fields[8]);
    model.Flags = Number<uint32>(fields[9]);
    model.ComponentRules = Number<uint32>(fields[10], 16);
    if (fields.size() > 11) model.Misc = Number<int32>(fields[11]);
    if (fields.size() > 12) model.WheelSizeFront = Number<float>(fields[12]);
    if (fields.size() > 13) model.WheelSizeRear = Number<float>(fields[13]);
    if (fields.size() > 14) model.WheelUpgradeClass = Number<int32>(fields[14]);
    model.Source = source;
    model.Line = line;
    return model;
}

bool Finite(NativeScriptPosition position) {
    return std::isfinite(position.X) && std::isfinite(position.Y) && std::isfinite(position.Z);
}

std::int16_t CompressPosition(float value) {
    const float scaled = value * 8.0f;
    Require(std::isfinite(scaled) && scaled >= std::numeric_limits<std::int16_t>::min() &&
        scaled <= std::numeric_limits<std::int16_t>::max(), "car-generator position exceeds source fixed-point range");
    return std::int16_t(scaled);
}

std::int8_t LowByte(int32 value) {
    return std::bit_cast<std::int8_t>(uint8(value));
}

std::int8_t CompressAngle(float degrees) {
    const float scaled = degrees * (256.0f / 360.0f);
    Require(std::isfinite(scaled) && scaled >= float(std::numeric_limits<int32>::min()) &&
        scaled <= float(std::numeric_limits<int32>::max()), "car-generator angle exceeds source conversion range");
    return LowByte(int32(scaled));
}

std::uint16_t LowWord(int32 value) {
    return uint16(uint32(value));
}

const NativeCarGeneratorObservation* Observation(const NativeCarGeneratorProcessInput& input,
    NativeCarGeneratorRef generator) {
    const auto found = std::ranges::find_if(input.Observations,
        [&](const auto& value) { return value.Generator == generator; });
    return found == input.Observations.end() ? nullptr : &*found;
}

const NativeCarGeneratorModelRuntime* RuntimeModel(const NativeCarGeneratorProcessInput& input, int32 model) {
    const auto found = std::ranges::find_if(input.Models, [&](const auto& value) { return value.ModelId == model; });
    return found == input.Models.end() ? nullptr : &*found;
}

NativeScriptServiceResult Ready(std::string message = {}) {
    return {NativeScriptServiceStatus::Ready, std::move(message)};
}

NativeScriptServiceResult Error(std::string message) {
    return {NativeScriptServiceStatus::Error, std::move(message)};
}

NativeScriptServiceResult Pending(std::string message) {
    return {NativeScriptServiceStatus::Pending, std::move(message)};
}

NativeScriptServiceResult Unsupported(std::string message) {
    return {NativeScriptServiceStatus::Unsupported, std::move(message)};
}

void PromoteResult(NativeCarGeneratorProcessFrame& frame, const NativeScriptServiceResult& result) {
    const auto rank = [](NativeScriptServiceStatus status) {
        switch (status) {
        case NativeScriptServiceStatus::Ready: return 0;
        case NativeScriptServiceStatus::Pending: return 1;
        case NativeScriptServiceStatus::Unsupported: return 2;
        case NativeScriptServiceStatus::Error: return 3;
        }
        return 3;
    };
    if (rank(result.Status) > rank(frame.Result.Status)) {
        frame.Result = result;
    }
}
} // namespace

NativeScriptPosition NativeCarGeneratorState::Position() const {
    return {float(CompressedPosition[0]) / 8.0f, float(CompressedPosition[1]) / 8.0f,
        float(CompressedPosition[2]) / 8.0f};
}

float NativeCarGeneratorState::HeadingRadians() const {
    return float(Angle) * RadiansPerAngleUnit;
}

bool NativeCarGenerators::ParseTextRecord(std::string_view row, NativeCarGeneratorFileRecord& out,
    std::string& error) try {
    const auto fields = Tokens(Clean(std::string(row), true));
    Require(fields.size() >= 12, "IPL cars row requires 12 fields");
    NativeCarGeneratorFileRecord record;
    record.Position = {Number<float>(fields[0]), Number<float>(fields[1]), Number<float>(fields[2])};
    record.AngleRadians = Number<float>(fields[3]);
    record.ModelId = Number<int32>(fields[4]);
    record.PrimaryColor = Number<int32>(fields[5]);
    record.SecondaryColor = Number<int32>(fields[6]);
    record.Flags = uint32(Number<int32>(fields[7]));
    record.AlarmChance = Number<int32>(fields[8]);
    record.DoorLockChance = Number<int32>(fields[9]);
    record.MinDelay = Number<int32>(fields[10]);
    record.MaxDelay = Number<int32>(fields[11]);
    out = record;
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

bool NativeCarGenerators::ParseBinaryIpl(std::span<const uint8> bytes, std::string_view source,
    uint64 containerByteOffset, std::vector<NativeCarGeneratorAssetRecord>& out, std::string& error) try {
    Require(bytes.size() >= 0x4c && !std::memcmp(bytes.data(), "bnry", 4), "invalid binary IPL header");
    static constexpr std::array<std::size_t, 6> CountOffsets{4, 8, 12, 16, 20, 24};
    static constexpr std::array<std::size_t, 6> OffsetOffsets{28, 36, 44, 52, 60, 68};
    for (std::size_t i = 0; i < CountOffsets.size(); ++i) {
        const uint32 count = U32(bytes.data() + CountOffsets[i]);
        const uint32 offset = U32(bytes.data() + OffsetOffsets[i]);
        const uint32 size = U32(bytes.data() + OffsetOffsets[i] + 4);
        if (!count && !size) {
            continue;
        }
        Require(offset >= 0x4c && offset <= bytes.size() && size <= bytes.size() - offset,
            "binary IPL section outside entry");
    }
    const uint32 instanceCount = U32(bytes.data() + 4);
    const uint32 instanceOffset = U32(bytes.data() + 28);
    Require(!instanceCount || (instanceOffset >= 0x4c && instanceOffset <= bytes.size() &&
        uint64(instanceCount) * 0x28 <= bytes.size() - instanceOffset),
        "binary IPL instance records out of bounds");
    const uint32 count = U32(bytes.data() + 20);
    const uint32 offset = U32(bytes.data() + 60);
    Require(!count || (offset >= 0x4c && offset <= bytes.size() &&
        uint64(count) * 0x30 <= bytes.size() - offset),
        "binary IPL car-generator records out of bounds");
    std::vector<NativeCarGeneratorAssetRecord> parsed;
    parsed.reserve(count);
    for (uint32 i = 0; i < count; ++i) {
        const auto rowOffset = uint64(offset) + uint64(i) * 0x30;
        const auto* row = bytes.data() + rowOffset;
        NativeCarGeneratorAssetRecord asset;
        asset.Authored.Position = {F32(row), F32(row + 4), F32(row + 8)};
        asset.Authored.AngleRadians = F32(row + 12);
        Require(Finite(asset.Authored.Position) && std::isfinite(asset.Authored.AngleRadians),
            "nonfinite binary IPL car-generator field");
        asset.Authored.ModelId = std::bit_cast<int32>(U32(row + 16));
        asset.Authored.PrimaryColor = std::bit_cast<int32>(U32(row + 20));
        asset.Authored.SecondaryColor = std::bit_cast<int32>(U32(row + 24));
        asset.Authored.Flags = U32(row + 28);
        asset.Authored.AlarmChance = std::bit_cast<int32>(U32(row + 32));
        asset.Authored.DoorLockChance = std::bit_cast<int32>(U32(row + 36));
        asset.Authored.MinDelay = std::bit_cast<int32>(U32(row + 40));
        asset.Authored.MaxDelay = std::bit_cast<int32>(U32(row + 44));
        asset.Provenance = {NativeCarGeneratorSourceKind::BinaryIpl, std::string(source), 0, i + 1,
            containerByteOffset, rowOffset, 0x30};
        asset.Phase = NativeCarGeneratorAssetPhase::DeferredStreaming;
        parsed.push_back(std::move(asset));
    }
    out.insert(out.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

const NativeCarGeneratorEvent* NativeCarGenerators::FindEvent(NativeScriptRequestId id) const {
    const auto found = std::ranges::find_if(m_Events, [&](const auto& event) { return event.Id == id; });
    return found == m_Events.end() ? nullptr : &*found;
}

NativeScriptReferenceResult<NativeCarGeneratorRef> NativeCarGenerators::CreateInternal(
    const NativeCarGeneratorCreateRequest& request, uint32 timeMs,
    const NativeCarGeneratorProvenance& provenance, bool journal,
    NativeCarGeneratorRegistrationStatus* registration, NativeCarGeneratorEventKind kind) try {
    if (journal) {
        if (const auto* event = FindEvent(request.Id)) {
            if (event->Kind != kind || event->Create != request) {
                return {Error("car-generator request ID reused by another operation or payload"), {}};
            }
            return {event->Result, event->Generator};
        }
    }
    NativeScriptReferenceResult<NativeCarGeneratorRef> result{Ready(), {}};
    if (!IsSourceModelInRange(request.ModelId)) {
        if (registration) *registration = NativeCarGeneratorRegistrationStatus::RejectedModelRange;
        result.Result.Message = "source CreateCarGenerator returned -1 for model range";
    } else {
        Require(Finite(request.Position), "nonfinite car-generator position");
        const auto slot = std::ranges::find_if(m_Entries, [](const auto& entry) { return !entry.Used; });
        if (slot == m_Entries.end()) {
            if (registration) *registration = NativeCarGeneratorRegistrationStatus::CapacityExceeded;
            result.Result.Message = "source CreateCarGenerator returned -1 for capacity 500";
        } else {
            const auto index = std::size_t(std::distance(m_Entries.begin(), slot));
            const bool activeFlag = slot->ActiveFlag; // Setup does not write this source bit.
            NativeCarGeneratorState state;
            state.ModelId = std::int16_t(request.ModelId);
            state.PrimaryColor = LowByte(request.PrimaryColor);
            state.SecondaryColor = LowByte(request.SecondaryColor);
            state.CompressedPosition = {CompressPosition(request.Position.X), CompressPosition(request.Position.Y),
                CompressPosition(request.Position.Z)};
            state.Angle = CompressAngle(request.AngleDegrees);
            state.AlarmChance = uint8(request.AlarmChance);
            state.DoorLockChance = uint8(request.DoorLockChance);
            state.WaitUntilFarFromPlayer = false;
            state.HighPriority = uint8(request.ForceSpawn) != 0;
            state.ActiveFlag = activeFlag;
            state.PlayerHasAlreadyOwnedCar = false;
            state.IgnorePopulationLimit = request.IgnorePopulationLimit;
            state.MinDelay = LowWord(request.MinDelay);
            state.MaxDelay = LowWord(request.MaxDelay);
            state.NextGenerationTime = timeMs + 1;
            state.Vehicle = {-1};
            state.GenerateCount = 0;
            state.IplId = request.IplId;
            state.Used = true;
            state.Provenance = provenance;
            *slot = std::move(state);
            ++m_Registered;
            ++m_Revision;
            result.Reference = {int32(index)};
            if (request.PlateText[0] && m_PlateCount < m_Plates.size()) {
                m_Plates[m_PlateCount++] = {result.Reference, request.PlateText};
            }
            if (registration) *registration = NativeCarGeneratorRegistrationStatus::Registered;
        }
    }
    if (journal) {
        m_Events.push_back({m_Events.size() + 1, kind,
            request.Id, result.Reference, request, 0, result.Result});
    }
    return result;
} catch (const std::exception& exception) {
    if (registration) *registration = NativeCarGeneratorRegistrationStatus::InvalidMetadata;
    NativeScriptReferenceResult<NativeCarGeneratorRef> result{Error(exception.what()), {}};
    if (journal) {
        m_Events.push_back({m_Events.size() + 1, kind,
            request.Id, result.Reference, request, 0, result.Result});
    }
    return result;
}

NativeScriptReferenceResult<NativeCarGeneratorRef> NativeCarGenerators::Create(
    const NativeCarGeneratorCreateRequest& request, uint32 timeMs) {
    return CreateInternal(request, timeMs,
        {NativeCarGeneratorSourceKind::Script014B, "main.scm", 0, 0, 0, request.Id.IP, 0}, true);
}

NativeScriptReferenceResult<NativeCarGeneratorRef> NativeCarGenerators::CreateWithPlate(
    const NativeCarGeneratorCreateRequest& request, uint32 timeMs) {
    return CreateInternal(request, timeMs,
        {NativeCarGeneratorSourceKind::Script014B, "main.scm", 0, 0, 0, request.Id.IP, 0}, true,
        nullptr, NativeCarGeneratorEventKind::Create09E2);
}

NativeScriptServiceResult NativeCarGenerators::Switch(const NativeCarGeneratorSwitchRequest& request,
    uint32 timeMs) {
    if (const auto* event = FindEvent(request.Id)) {
        if (event->Kind != NativeCarGeneratorEventKind::Switch014C || event->Generator != request.Generator ||
            event->Count != request.Count) {
            return Error("car-generator request ID reused by another operation or payload");
        }
        return event->Result;
    }
    auto* generator = Resolve(request.Generator);
    NativeScriptServiceResult result;
    if (!generator) {
        result = Error("014C car-generator reference is outside the live source registry");
    } else {
        if (request.Count) {
            generator->GenerateCount = std::numeric_limits<uint16>::max();
            generator->NextGenerationTime = timeMs + 4;
            if (request.Count <= 100) {
                generator->GenerateCount = LowWord(request.Count);
            }
        } else {
            generator->GenerateCount = 0;
        }
        ++m_Revision;
        result = Ready();
    }
    m_Events.push_back({m_Events.size() + 1, NativeCarGeneratorEventKind::Switch014C,
        request.Id, request.Generator, {}, request.Count, result});
    return result;
}

NativeScriptServiceResult NativeCarGenerators::SetPlayerOwned(
    NativeCarGeneratorRef reference, bool owned, std::string& error) {
    auto* generator = Resolve(reference);
    if (!generator) {
        error = "0A17 car-generator reference is outside the live source registry";
        return {NativeScriptServiceStatus::Error, error};
    }
    generator->PlayerHasAlreadyOwnedCar = owned;
    ++m_Revision;
    error.clear();
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeCarGenerators::RegisterAsset(NativeCarGeneratorAssetRecord& asset,
    uint8 iplId, uint32 timeMs) {
    NativeCarGeneratorCreateRequest request;
    request.Position = asset.Authored.Position;
    request.AngleDegrees = asset.Authored.AngleRadians * DegreesPerRadian;
    request.ModelId = asset.Authored.ModelId;
    request.PrimaryColor = asset.Authored.PrimaryColor;
    request.SecondaryColor = asset.Authored.SecondaryColor;
    request.ForceSpawn = int32(asset.Authored.Flags & 1);
    request.AlarmChance = asset.Authored.AlarmChance;
    request.DoorLockChance = asset.Authored.DoorLockChance;
    request.MinDelay = asset.Authored.MinDelay;
    request.MaxDelay = asset.Authored.MaxDelay;
    request.IplId = iplId;
    request.IgnorePopulationLimit = (asset.Authored.Flags & 2) != 0;
    const auto created = CreateInternal(request, timeMs, asset.Provenance, false, &asset.RegistrationStatus);
    asset.RegistrationIplId = iplId;
    asset.RegistrationDetail = created.Result.Message;
    ++m_Revision;
    if (created.Result.Status == NativeScriptServiceStatus::Ready && created.Reference.Value >= 0) {
        auto& state = m_Entries[std::size_t(created.Reference.Value)];
        state.GenerateCount = std::numeric_limits<uint16>::max();
        state.NextGenerationTime = timeMs + 4;
        ++m_Revision;
    }
    asset.RegistryIndex = created.Reference.Value;
    if (asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::CapacityExceeded) {
        return Error(created.Result.Message);
    }
    return created.Result;
}

bool NativeCarGenerators::LoadBeforeWorker(const char* gameDir, uint32 timeMs, std::string& error) try {
    Require(!m_SourceLoaded && !m_Sealed && !m_Registered && m_Events.empty(),
        "car-generator source loading must be first and before startup seal");
    Require(gameDir && *gameDir, "car-generator game directory");
    OS_SetFilePathOffset(gameDir);
    NativeCarGenerators prepared;
    for (const auto* dat : {"data/default.dat", "data/gta.dat"}) {
        const auto datText = ReadText(ResolvePath(gameDir, dat));
        std::size_t position = 0;
        while (position <= datText.size()) {
            const auto end = datText.find('\n', position);
            auto line = Clean(datText.substr(position, end == std::string::npos ? std::string::npos : end - position));
            position = end == std::string::npos ? datText.size() + 1 : end + 1;
            if (line.empty()) continue;
            std::istringstream directive(line);
            std::string kind, relative, extra;
            if (!(directive >> kind >> relative) || (kind != "IDE" && kind != "IPL")) continue;
            Require(!(directive >> extra), std::string(dat) + ": car-generator DAT directive field count");
            relative = AssetPath(relative);
            const auto sourceText = ReadText(ResolvePath(gameDir, relative));
            bool section = false;
            uint32 lineNumber = 0, rowNumber = 0;
            std::size_t byteOffset = 0;
            while (byteOffset <= sourceText.size()) {
                const auto lineEnd = sourceText.find('\n', byteOffset);
                const auto byteSize = (lineEnd == std::string::npos ? sourceText.size() : lineEnd) - byteOffset;
                auto sourceLine = Clean(sourceText.substr(byteOffset, byteSize));
                const auto sourceByteOffset = byteOffset;
                byteOffset = lineEnd == std::string::npos ? sourceText.size() + 1 : lineEnd + 1;
                ++lineNumber;
                if (sourceLine.empty()) continue;
                const auto lower = Lower(sourceLine);
                if (lower == "end") { section = false; continue; }
                if (!section) { section = lower == "cars"; rowNumber = 0; continue; }
                ++rowNumber;
                if (kind == "IDE") {
                    auto model = ParseModel(sourceLine, relative, lineNumber);
                    const auto duplicate = std::ranges::find_if(prepared.m_Models,
                        [&](const auto& existing) { return existing.ModelId == model.ModelId; });
                    Require(duplicate == prepared.m_Models.end(), "duplicate vehicle IDE model ID");
                    prepared.m_Models.push_back(std::move(model));
                } else {
                    NativeCarGeneratorAssetRecord asset;
                    std::string parseError;
                    Require(ParseTextRecord(sourceLine, asset.Authored, parseError),
                        relative + ":" + std::to_string(lineNumber) + ": " + parseError);
                    asset.Provenance = {NativeCarGeneratorSourceKind::TextIpl, relative, lineNumber, rowNumber,
                        0, sourceByteOffset, uint32(byteSize)};
                    asset.Phase = NativeCarGeneratorAssetPhase::StartupStaticLoaded;
                    const auto* definition = asset.Authored.ModelId == -1 ? nullptr : prepared.FindModel(asset.Authored.ModelId);
                    asset.ModelDefinitionResolved = asset.Authored.ModelId == -1 || definition;
                    prepared.m_Assets.push_back(std::move(asset));
                    const auto registered = prepared.RegisterAsset(prepared.m_Assets.back(), 0, timeMs);
                    Require(registered.Status == NativeScriptServiceStatus::Ready, registered.Message);
                }
            }
            if (kind == "IPL") ++prepared.m_TextIplFiles;
        }
    }
    Require(!prepared.m_Models.empty(), "no vehicle IDE definitions before car-generator startup");
    const auto startupRecords = std::ranges::count_if(prepared.m_Assets, [](const auto& asset) {
        return asset.Phase == NativeCarGeneratorAssetPhase::StartupStaticLoaded;
    });

    std::set<std::string> binaryNames;
    for (const auto* relative : {"models/gta3.img", "models/gta_int.img"}) {
        std::string absolute;
        try {
            absolute = ResolvePath(gameDir, relative);
        } catch (const std::exception&) {
            continue;
        }
        File image(absolute);
        Require(image.Size >= 8, "car-generator IMG header");
        const auto header = image.Read(0, 8);
        Require(!std::memcmp(header.data(), "VER2", 4), "car-generator IMG is not VER2");
        const uint32 count = U32(header.data() + 4);
        Require(count && count <= 300000 && uint64(count) * 32 <= uint64(image.Size) - 8,
            "car-generator IMG directory bound");
        const auto directory = image.Read(8, std::size_t(count) * 32);
        for (uint32 i = 0; i < count; ++i) {
            const auto* entry = directory.data() + std::size_t(i) * 32;
            const auto length = ::strnlen(reinterpret_cast<const char*>(entry + 8), 24);
            auto name = Lower(std::string(reinterpret_cast<const char*>(entry + 8), length));
            if (!name.ends_with(".ipl") || !binaryNames.insert(name).second) continue;
            const uint32 sector = U32(entry);
            const uint16 archivedSectors = U16(entry + 6);
            const uint16 streamingSectors = U16(entry + 4);
            const uint32 sectors = archivedSectors ? archivedSectors : streamingSectors;
            Require(sectors && uint64(sector) * 2048 <= uint64(image.Size) &&
                uint64(sectors) * 2048 <= uint64(image.Size) - uint64(sector) * 2048,
                "binary IPL IMG range");
            const uint64 containerOffset = uint64(sector) * 2048;
            const auto bytes = image.Read(containerOffset, std::size_t(sectors) * 2048);
            const auto before = prepared.m_Assets.size();
            std::string parseError;
            if (!ParseBinaryIpl(bytes, std::string(relative) + ":" + name, containerOffset,
                prepared.m_Assets, parseError)) {
                throw std::runtime_error(std::string(relative) + ":" + name + ": " + parseError);
            }
            for (auto& asset : std::span(prepared.m_Assets).subspan(before)) {
                asset.ModelDefinitionResolved = asset.Authored.ModelId == -1 || prepared.FindModel(asset.Authored.ModelId);
            }
            ++prepared.m_BinaryIplFiles;
        }
    }
    prepared.m_SourceLoaded = true;
    prepared.m_StartupOrderProven = true;
    // LoadScene handles the DAT text rows before scripts. IMG rows belong to
    // CIplStore::LoadIpl and are removed again by RemoveIpl when that streamed
    // IPL leaves; cataloguing them is not startup registration.
    prepared.m_StartupDisposition = startupRecords ?
        NativeCarGeneratorStartupDisposition::TextIplRecordsRegistered :
        NativeCarGeneratorStartupDisposition::ProvenEmptyTextIplsBinaryDeferred;
    *this = std::move(prepared);
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

bool NativeCarGenerators::ActivateStreamedIpl(std::string_view source, uint8 iplId, uint32 timeMs,
    std::string& error) try {
    Require(m_SourceLoaded, "streamed IPL activation requires source catalog");
    Require(iplId != 0, "streamed IPL requires nonzero source slot ID");
    bool found = false;
    for (auto& asset : m_Assets) {
        if (asset.Phase != NativeCarGeneratorAssetPhase::DeferredStreaming || asset.Provenance.Source != source) continue;
        found = true;
        if (asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::Registered ||
            asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::RejectedModelRange) {
            Require(asset.RegistrationIplId == iplId, "streamed IPL already registered with another ID");
            continue;
        }
        const auto registered = RegisterAsset(asset, iplId, timeMs);
        Require(registered.Status == NativeScriptServiceStatus::Ready,
            asset.Provenance.Source + ":record=" + std::to_string(asset.Provenance.Record) + ": " + registered.Message);
    }
    Require(found, "streamed IPL has no catalogued car-generator records");
    error.clear();
    return true;
} catch (const std::exception& exception) {
    error = exception.what();
    return false;
}

std::size_t NativeCarGenerators::RemoveIpl(uint8 iplId) {
    std::size_t removed = 0;
    bool assetChanged = false;
    for (std::size_t i = 0; i < m_Entries.size(); ++i) {
        auto& generator = m_Entries[i];
        if (!generator.Used || generator.IplId != iplId) continue;
        generator.IplId = 0;
        generator.Used = false;
        generator.Vehicle = {-1};
        --m_Registered;
        ++removed;
    }
    for (auto& asset : m_Assets) {
        if (asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::NotAttempted ||
            asset.RegistrationIplId != iplId) continue;
        asset.RegistryIndex = -1;
        asset.RegistrationStatus = NativeCarGeneratorRegistrationStatus::NotAttempted;
        asset.RegistrationIplId = 0;
        asset.RegistrationDetail.clear();
        assetChanged = true;
    }
    if (removed || assetChanged) ++m_Revision;
    return removed;
}

const NativeCarGeneratorState* NativeCarGenerators::Resolve(NativeCarGeneratorRef reference) const {
    return reference.Value >= 0 && std::size_t(reference.Value) < m_Entries.size() &&
        m_Entries[std::size_t(reference.Value)].Used ? &m_Entries[std::size_t(reference.Value)] : nullptr;
}

NativeCarGeneratorState* NativeCarGenerators::Resolve(NativeCarGeneratorRef reference) {
    return const_cast<NativeCarGeneratorState*>(std::as_const(*this).Resolve(reference));
}

const NativeCarGeneratorModelDefinition* NativeCarGenerators::FindModel(int32 modelId) const {
    const auto found = std::ranges::find_if(m_Models, [&](const auto& model) { return model.ModelId == modelId; });
    return found == m_Models.end() ? nullptr : &*found;
}

NativeCarGeneratorSourceCensus NativeCarGenerators::Census() const {
    NativeCarGeneratorSourceCensus census;
    census.Definitions = m_Models.size();
    census.TextIplFiles = m_TextIplFiles;
    census.BinaryIplFiles = m_BinaryIplFiles;
    census.StartupAssetRecords = std::ranges::count_if(m_Assets, [](const auto& asset) {
        return asset.Phase == NativeCarGeneratorAssetPhase::StartupStaticLoaded;
    });
    census.DeferredAssetRecords = m_Assets.size() - census.StartupAssetRecords;
    census.Registered = m_Registered;
    for (const auto& asset : m_Assets) {
        census.RegisteredAssetRecords += asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::Registered;
        census.RejectedAssetRecords += asset.RegistrationStatus == NativeCarGeneratorRegistrationStatus::RejectedModelRange;
    }
    census.StartupOrderProven = m_StartupOrderProven;
    census.StartupDisposition = m_StartupDisposition;
    return census;
}

NativeCarGeneratorProcessFrame NativeCarGenerators::Process(const NativeCarGeneratorProcessInput& input) {
    NativeCarGeneratorProcessFrame frame;
    frame.ProcessCounterBefore = m_ProcessCounter;
    frame.GenerateCloseCounterBefore = m_GenerateCloseCounter;
    if (!Finite(input.PlayerCenter) || !Finite(input.Camera) ||
        !std::ranges::all_of(input.PlayerSpeed, [](float value) { return std::isfinite(value); }) ||
        !std::isfinite(input.GenerationDistanceMultiplier) || input.GenerationDistanceMultiplier < 0.0f) {
        frame.Result = Error("invalid car-generator player/camera/runtime input");
        frame.ProcessCounterAfter = m_ProcessCounter;
        frame.GenerateCloseCounterAfter = m_GenerateCloseCounter;
        return frame;
    }
    if (input.PlayerInTrain || input.Cutscene || input.ReplayPlayback) {
        frame.ProcessCounterAfter = m_ProcessCounter;
        frame.GenerateCloseCounterAfter = m_GenerateCloseCounter;
        return frame;
    }
    if (++m_ProcessCounter == 4) m_ProcessCounter = 0;
    auto action = [&](NativeCarGeneratorRef reference, NativeCarGeneratorDecision decision,
        NativeCarGeneratorRequirement requirement, NativeScriptServiceResult result,
        NativeCarGeneratorConsumerRequest request = {}) {
        request.Generator = reference;
        frame.Actions.push_back({reference, decision, requirement, std::move(result), std::move(request)});
        PromoteResult(frame, frame.Actions.back().Result);
    };
    for (std::size_t i = m_ProcessCounter; i < m_Entries.size(); i += 4) {
        auto& generator = m_Entries[i];
        if (!generator.Used) continue;
        ++frame.Visited;
        const NativeCarGeneratorRef reference{int32(i)};
        NativeCarGeneratorConsumerRequest request;
        request.Generator = reference;
        request.Position = generator.Position();
        request.Camera = input.Camera;
        request.PrimaryColor = generator.PrimaryColor;
        request.SecondaryColor = generator.SecondaryColor;
        request.AlarmChance = generator.AlarmChance;
        request.DoorLockChance = generator.DoorLockChance;
        request.HighPriority = generator.HighPriority;
        request.PlayerAlreadyOwned = generator.PlayerHasAlreadyOwnedCar;
        if (generator.Vehicle.Value != -1) {
            if (!input.Vehicles) {
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::VehiclePool,
                    Unsupported("resolve the generator's NativeVehicleRef through the owned vehicle pool"), request);
                continue;
            }
            const auto* vehicle = input.Vehicles->Resolve(generator.Vehicle);
            if (!vehicle) {
                generator.Vehicle = {-1};
                ++m_Revision;
                action(reference, NativeCarGeneratorDecision::VehicleReferenceExpired,
                    NativeCarGeneratorRequirement::None, Ready(), request);
            } else if (vehicle->State.Status == NativeVehicleStatus::Player) {
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::PlayerVehicleTransition,
                    Unsupported("set actual vehicle extended-removal-range to zero, then CommitPlayerVehicleTransition"), request);
            } else {
                action(reference, NativeCarGeneratorDecision::HasVehicle,
                    NativeCarGeneratorRequirement::None, Ready(), request);
            }
            continue;
        }
        if (!m_GenerateCloseCounter && generator.NextGenerationTime > input.TimeMs) {
            action(reference, NativeCarGeneratorDecision::Timer, NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        if (!generator.GenerateCount) {
            action(reference, NativeCarGeneratorDecision::Disabled, NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        const float dx = input.PlayerCenter.X - request.Position.X;
        const float dy = input.PlayerCenter.Y - request.Position.Y;
        const float dz = input.PlayerCenter.Z - request.Position.Z;
        if (std::fabs(dz) > 50.0f) {
            action(reference, NativeCarGeneratorDecision::VerticalRange, NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance)) {
            action(reference, NativeCarGeneratorDecision::ConsumerRequired, NativeCarGeneratorRequirement::None,
                Error("car-generator player distance overflow"), request);
            continue;
        }
        const int32 stateModel = generator.ModelId;
        const int32 candidateModel = stateModel < -1 ? -stateModel : stateModel;
        const auto* runtime = candidateModel >= 0 ? RuntimeModel(input, candidateModel) : nullptr;
        auto type = runtime ? runtime->Type : NativeVehicleType::Unsupported;
        if (type == NativeVehicleType::Unsupported) {
            if (const auto* definition = candidateModel >= 0 ? FindModel(candidateModel) : nullptr) type = definition->Type;
        }
        if (stateModel > 0 && type == NativeVehicleType::Unsupported) {
            request.ModelId = stateModel;
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::ModelDefinition,
                Unsupported("provide the source vehicles.ide definition for the fixed generator model"), request);
            continue;
        }
        bool visible = false;
        if (stateModel > 0 && type == NativeVehicleType::Boat &&
            distance < input.GenerationDistanceMultiplier * 240.0f) {
            const auto* observed = Observation(input, reference);
            if (!observed || observed->Visibility == NativeCarGeneratorVisibility::Unknown) {
                request.ModelId = stateModel;
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::BoatVisibilityAndOcclusion,
                    Pending("evaluate actual camera sphere visibility and source occlusion at the generator"), request);
                continue;
            }
            visible = observed->Visibility == NativeCarGeneratorVisibility::VisibleUnoccluded;
        }
        if (distance >= input.GenerationDistanceMultiplier * 160.0f && !visible) {
            if (generator.WaitUntilFarFromPlayer) {
                generator.WaitUntilFarFromPlayer = false;
                ++m_Revision;
            }
            action(reference, NativeCarGeneratorDecision::TooFar, NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        if (!m_GenerateCloseCounter) {
            if (generator.WaitUntilFarFromPlayer) {
                action(reference, NativeCarGeneratorDecision::WaitingUntilFar,
                    NativeCarGeneratorRequirement::None, Ready(), request);
                continue;
            }
            const bool matchingArea = (input.CanSeeOutside && request.Position.Z < 950.0f) ||
                (!input.CanSeeOutside && request.Position.Z >= 950.0f);
            if (!matchingArea) {
                action(reference, NativeCarGeneratorDecision::AreaMismatch,
                    NativeCarGeneratorRequirement::None, Ready(), request);
                continue;
            }
            if (distance < input.GenerationDistanceMultiplier * 160.0f - 20.0f && !generator.HighPriority) {
                action(reference, NativeCarGeneratorDecision::DistanceOrPriority,
                    NativeCarGeneratorRequirement::None, Ready(), request);
                continue;
            }
            if (input.PlayerSpeed[0] * dx + input.PlayerSpeed[1] * dy > 0.0f) {
                action(reference, NativeCarGeneratorDecision::PlayerApproaching,
                    NativeCarGeneratorRequirement::None, Ready(), request);
                continue;
            }
        }
        // This deliberately mirrors the currently reversed ClockHoursInRange
        // expression used by CarGenerator.cpp, including its strict comparisons.
        const bool nightTime = input.ClockHour > 21 && input.ClockHour < 7;
        if (!generator.IgnorePopulationLimit &&
            ((nightTime && input.NumParkedCars >= 10) || (!nightTime && input.NumParkedCars >= 5))) {
            action(reference, NativeCarGeneratorDecision::PopulationLimit,
                NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        if (stateModel < 0 && (stateModel == -1 || !runtime || !runtime->Loaded)) {
            request.ModelId = stateModel;
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::RandomPopulationSelection,
                Unsupported("run source zone/AppropriateLoadedCars selection on the shared source RNG state, then CommitRandomPopulationSelection; no fixed fallback model"), request);
            continue;
        }
        const int32 actualModel = stateModel < 0 ? -stateModel : stateModel;
        if (type == NativeVehicleType::Unsupported) {
            request.ModelId = actualModel;
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::ModelDefinition,
                Unsupported("provide the source vehicles.ide definition for the actual generator model"), request);
            continue;
        }
        request.ModelId = actualModel;
        request.VehicleType = type;
        const auto* observed = Observation(input, reference);
        if (!observed || observed->Blockage == NativeCarGeneratorBlockage::Unknown) {
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::CollisionBlockage,
                Pending("query actual source model COL against buildings/vehicles/objects at the generator"), request);
            continue;
        }
        if (observed->Blockage == NativeCarGeneratorBlockage::Blocked) {
            generator.WaitUntilFarFromPlayer = true;
            generator.NextGenerationTime += 4;
            ++m_Revision;
            action(reference, NativeCarGeneratorDecision::Blocked,
                NativeCarGeneratorRequirement::None, Ready(), request);
            continue;
        }
        if (stateModel >= 0) {
            request.ModelsToRequest[0] = actualModel;
            request.ModelRequestCount = 1;
            const auto* modelRuntime = RuntimeModel(input, actualModel);
            bool modelsReady = modelRuntime && modelRuntime->KeepInMemoryRequested && modelRuntime->Loaded;
            if (actualModel == 588) {
                request.ModelsToRequest[1] = 168;
                request.ModelRequestCount = 2;
                const auto* vendor = RuntimeModel(input, 168);
                modelsReady = modelsReady && vendor && vendor->KeepInMemoryRequested && vendor->Loaded;
            }
            if (!modelsReady) {
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::ModelKeepInMemory,
                    Pending("request listed source model IDs KEEP_IN_MEMORY and report actual residency"), request);
                continue;
            }
        }
        const bool direct = type == NativeVehicleType::Boat || actualModel == 417 || actualModel == 447 || actualModel == 460;
        request.Placement = direct ? (request.Position.Z <= -100.0f ?
            NativeCarGeneratorPlacement::DirectGroundLookup : NativeCarGeneratorPlacement::Direct) :
            NativeCarGeneratorPlacement::VerticalLine;
        if (request.Placement != NativeCarGeneratorPlacement::Direct) {
            if (observed->Ground == NativeCarGeneratorGround::Unknown) {
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::GroundCollision,
                    Pending(request.Placement == NativeCarGeneratorPlacement::VerticalLine ?
                        "run the source vertical-line ground query" : "run source FindGroundZForCoord"), request);
                continue;
            }
            if (request.Placement == NativeCarGeneratorPlacement::VerticalLine &&
                observed->Ground == NativeCarGeneratorGround::Miss) {
                action(reference, NativeCarGeneratorDecision::GroundMiss,
                    NativeCarGeneratorRequirement::None, Ready(), request);
                continue;
            }
            if (observed->Ground != NativeCarGeneratorGround::Hit || !std::isfinite(observed->GroundZ)) {
                action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                    NativeCarGeneratorRequirement::GroundCollision,
                    Error("invalid completed car-generator ground query"), request);
                continue;
            }
            request.GroundZ = observed->GroundZ;
        }
        if (!input.Vehicles) {
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::VehiclePool,
                Unsupported("provide the authoritative NativeVehiclePool before parked-vehicle construction"), request);
            continue;
        }
        const auto& producers = input.Vehicles->ProducerExtent();
        if (!producers.NativeHostComplete ||
            !producers.Owned[std::size_t(NativeVehicleProducer::CarGenerator)]) {
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::VehicleProducerBinding,
                Unsupported("bind NativeVehicleProducer::CarGenerator before sealing/publishing the vehicle pool"), request);
            continue;
        }
        if (input.Vehicles->Census().Alive >= NativeVehiclePool::Capacity) {
            action(reference, NativeCarGeneratorDecision::ConsumerRequired,
                NativeCarGeneratorRequirement::VehiclePoolCapacity,
                Pending("wait for an actual owned vehicle slot; do not fabricate or discard a parked vehicle"), request);
            continue;
        }
        action(reference, NativeCarGeneratorDecision::ConsumerRequired,
            NativeCarGeneratorRequirement::VehicleConstructionAndWorldInsertion,
            Unsupported("construct the typed parked vehicle, apply ground/base/light/status/ownership/alpha and shared rand15 effects, insert it, allocate the source pool record, then AttachSpawnedVehicle"), request);
    }
    if (m_GenerateCloseCounter) --m_GenerateCloseCounter;
    frame.ProcessCounterAfter = m_ProcessCounter;
    frame.GenerateCloseCounterAfter = m_GenerateCloseCounter;
    return frame;
}

bool NativeCarGenerators::AttachSpawnedVehicle(NativeCarGeneratorRef generatorRef, NativeVehicleRef vehicleRef,
    int32 actualModelId, std::int8_t primaryColor, std::int8_t secondaryColor, uint32 timeMs,
    const NativeVehiclePool& pool, std::string& error) {
    auto* generator = Resolve(generatorRef);
    const auto* vehicle = pool.Resolve(vehicleRef);
    if (!generator || generator->Vehicle.Value != -1 || !vehicle ||
        vehicle->Producer != NativeVehicleProducer::CarGenerator || vehicle->ProducerIndex != generatorRef.Value ||
        vehicle->State.ModelId != actualModelId || actualModelId < 0 ||
        (generator->ModelId >= 0 && generator->ModelId != actualModelId)) {
        error = "spawned car-generator vehicle does not match registry/pool ownership";
        return false;
    }
    if (generator->ModelId < -1) {
        if (-generator->ModelId != actualModelId) {
            error = "cached random car-generator model mismatch";
            return false;
        }
    } else if (generator->ModelId == -1) {
        error = "random car-generator selection must be committed before spawned-vehicle attachment";
        return false;
    }
    if (generator->ModelId < -1) {
        generator->PrimaryColor = primaryColor;
        generator->SecondaryColor = secondaryColor;
    }
    generator->Vehicle = vehicleRef;
    // The original unsigned-vs-signed comparison never decrements the count.
    generator->NextGenerationTime = timeMs + 4;
    ++m_Revision;
    error.clear();
    return true;
}

bool NativeCarGenerators::CommitRandomPopulationSelection(NativeCarGeneratorRef generatorRef,
    int32 actualModelId, NativeVehicleType type, float sourceBoundLength, std::string& error) {
    auto* generator = Resolve(generatorRef);
    if (!generator || generator->Vehicle.Value != -1 || generator->ModelId >= 0 ||
        actualModelId < 400 || actualModelId > 611 ||
        type == NativeVehicleType::Unsupported || type == NativeVehicleType::Boat ||
        !std::isfinite(sourceBoundLength) || sourceBoundLength > 8.0f || sourceBoundLength < 0.0f) {
        error = "random car-generator selection fails source model/type/bound-length filters";
        return false;
    }
    generator->ModelId = std::int16_t(-actualModelId);
    generator->PrimaryColor = -1;
    generator->SecondaryColor = -1;
    ++m_Revision;
    error.clear();
    return true;
}

bool NativeCarGenerators::CommitPlayerVehicleTransition(NativeCarGeneratorRef generatorRef,
    const NativeVehiclePool& pool, std::string& error) {
    auto* generator = Resolve(generatorRef);
    const auto* vehicle = generator ? pool.Resolve(generator->Vehicle) : nullptr;
    if (!generator || !vehicle || vehicle->State.Status != NativeVehicleStatus::Player) {
        error = "car-generator player transition requires its live player-status vehicle";
        return false;
    }
    generator->NextGenerationTime += 60000;
    generator->Vehicle = {-1};
    generator->WaitUntilFarFromPlayer = true;
    if (generator->ModelId < 0) generator->ModelId = -1;
    ++m_Revision;
    error.clear();
    return true;
}
