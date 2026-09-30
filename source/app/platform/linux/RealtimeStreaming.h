// Native --play's single CPU producer. No GL or SDL calls in this file.
#pragma once

#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeGameplay.h"
#include "app/platform/linux/NativeVehicleAssetQueue.h"
#include "app/platform/linux/NativeScriptEntities.h"
#include "app/platform/linux/NativePedAssets.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace realtime_streaming {
inline double Milliseconds() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Center {
    float X{}, Y{}, Z{};
};
struct StaticModelRequest {
    std::uint64_t Ticket = 0;
    std::string GameDir, Model, Texture;
    bool Vehicle = false;
};
struct StaticModelCompletion {
    std::uint64_t Ticket = 0;
    std::shared_ptr<const WorldShotScene> Scene;
    std::shared_ptr<const NativeCollisionModel> Collision;
    std::string Error;
};
using StaticModelLoader = std::function<bool(const StaticModelRequest&, WorldShotScene&,
    std::shared_ptr<const NativeCollisionModel>&, std::string&)>;
struct ScriptTextureRequest {
    std::uint64_t Ticket = 0;
    std::string GameDir, Name;
};
struct ScriptTextureCompletion {
    std::uint64_t Ticket = 0;
    std::shared_ptr<const NativeScriptTextureDictionaryPacket> Packet;
    std::string Error;
};
using ScriptTextureLoader = std::function<bool(const ScriptTextureRequest&,
    NativeScriptTextureDictionaryPacket&, std::string&)>;
struct PedAssetRequest {
    std::uint64_t Ticket = 0;
    std::string GameDir;
    NativeWorldPedModelInfo Model;
};
struct PedAssetCompletion {
    std::uint64_t Ticket = 0;
    std::shared_ptr<const NativePedAssets> Packet;
    std::string Error;
};
using PedAssetLoader = std::function<bool(const PedAssetRequest&, NativePedAssets&, std::string&)>;

struct CpuWorld {
    Center Position;
    uint64_t Generation{};
    WorldShotScene Scene;
    RealtimeGameplayWorld Collision;
    // Startup may adopt the host's already-built query world. All consumers
    // use QueryWorld(), so diagnostics and physics share that exact owner.
    std::shared_ptr<const RealtimeGameplayWorld> BorrowedCollision;
    std::shared_ptr<const NativeCollisionSnapshot> SourceCollision;
    std::shared_ptr<const NativePlacementOverrides> Overrides;
    E2EPagerFrame Frame{};
    std::string Error;
    double Started{}, PagerMs{}, CollisionMs{};

    const RealtimeGameplayWorld& QueryWorld() const {
        return BorrowedCollision ? *BorrowedCollision : Collision;
    }

    // Without a context, retain the legacy render fixture path. Runtime physics always
    // supplies a source context; both outputs belong to Position/Generation.
    void Build(bool collision, const std::shared_ptr<const NativeCollisionContext>& context = {},
               std::shared_ptr<const NativePlacementOverrides> overrides = {}) {
        Overrides = std::move(overrides);
        BorrowedCollision.reset();
        Started = Milliseconds();
        char error[512]{};
        if (!StreamPager_Update(Position.X, Position.Y, Position.Z, Scene, Frame, error, sizeof(error), Overrides)) {
            Error = error;
            return;
        }
        PagerMs = Milliseconds() - Started;
        if (collision) {
            if (context) {
                auto snapshot = std::make_shared<NativeCollisionSnapshot>();
                auto collision = std::make_shared<RealtimeGameplayWorld>();
                if (!context->Snapshot(Position.X, Position.Y, *snapshot, Error, Overrides) ||
                    !collision->Rebuild(*snapshot, Error)) return;
                SourceCollision = std::move(snapshot);
                BorrowedCollision = std::move(collision);
            } else {
                Collision.Rebuild(Scene, Error);
            }
        }
        CollisionMs = Milliseconds() - Started - PagerMs;
        if (SourceCollision) {
            std::printf("source-col-world generation=%llu center=%.3f,%.3f,%.3f pagerMs=%.2f collisionMs=%.2f instances=%zu triangles=%zu spheres=%zu boxes=%zu collapsed=%zu missing=%zu empty=%zu\n",
                static_cast<unsigned long long>(Generation), Position.X, Position.Y, Position.Z, PagerMs, CollisionMs,
                SourceCollision->Instances.size(), QueryWorld().TriangleCount(), QueryWorld().SphereCount(), QueryWorld().BoxCount(),
                QueryWorld().CollapsedTriangleCount(), SourceCollision->MissingModels, SourceCollision->EmptyModels);
        }
    }
};

// Start ONLY after all startup parsers (including gameplay/CarPose/IfpAnim)
// have returned. Until Stop joins, ONLY this thread may call StreamPager,
// TexSample/librw parsers or touch their globals. Freeze the startup file-path
// offset value (CarPose repeats its setter on the worker with that same path).
// Actor Tick/Draw use owned CPU snapshots, not librw. Even separate TXDs are
// not independent: librw current dictionary, plugins, frame lists are global.
class Worker {
public:
    explicit Worker(bool collision, std::shared_ptr<const NativeCollisionContext> context = {},
                     std::shared_ptr<const NativePlacementOverrides> overrides = {}, uint64_t initialGeneration = 1,
                     std::shared_ptr<const NativeVehicleAssetSource> vehicleSource = {},
                     StaticModelLoader modelLoader = {}, ScriptTextureLoader textureLoader = {},
                     PedAssetLoader pedLoader = {})
        : m_Collision(collision), m_Context(std::move(context)), m_Overrides(std::move(overrides)),
          m_Generation(initialGeneration), m_VehicleSource(std::move(vehicleSource)),
           m_StaticModelLoader(std::move(modelLoader)), m_ScriptTextureLoader(std::move(textureLoader)),
           m_PedAssetLoader(std::move(pedLoader)),
           m_Thread([this] { Run(); }) {}
    ~Worker() { Stop({}, {}); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Latest-center mailbox, not a queue. Returning inside the loaded threshold
    // cancels unstarted requests; an in-flight result is allowed to finish.
    void Request(Center center, bool wanted) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop) return;
        m_Center = center;
        m_Wanted = wanted;
        m_Wake.notify_one();
    }

    NativeVehicleAssetSubmission RequestVehicle(const NativeVehicleAssetRequest& request) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop) return {NativeVehicleAssetAdmission::Stopped, {}};
        auto result = m_Vehicles.Submit(request, bool(m_VehicleSource));
        m_Wake.notify_one();
        return result;
    }

    NativeVehicleAssetPhase VehiclePhase(const NativeVehicleAssetTicket& ticket) {
        std::lock_guard lock(m_Mutex);
        return m_Stop ? NativeVehicleAssetPhase::Stopped : m_Vehicles.Phase(ticket);
    }

    bool CancelVehicle(const NativeVehicleAssetTicket& ticket) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop) return false;
        const bool result = m_Vehicles.Cancel(ticket);
        m_Wake.notify_one();
        return result;
    }

    std::shared_ptr<const NativeVehicleAssetCompletion> TakeVehicleReady(const NativeVehicleAssetTicket& ticket) {
        std::lock_guard lock(m_Mutex);
        return m_Stop ? nullptr : m_Vehicles.Take(ticket);
    }

    bool RequestStaticModel(const StaticModelRequest& request) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop || !request.Ticket || request.GameDir.empty() || request.Model.empty() || request.Texture.empty()) return false;
        // Moving Waiting into the worker does not free the one-slot producer:
        // a second ticket must not overwrite its still-unconsumed completion.
        if (m_StaticModelInFlight) return m_StaticModelInFlight == request.Ticket;
        if ((m_StaticModelWaiting && m_StaticModelWaiting->Ticket != request.Ticket) ||
            (m_StaticModelReady && m_StaticModelReady->Ticket != request.Ticket)) return false;
        if (!m_StaticModelWaiting && !m_StaticModelReady) m_StaticModelWaiting = request;
        m_Wake.notify_one();
        return true;
    }

    std::optional<StaticModelCompletion> TakeStaticModel(std::uint64_t ticket) {
        std::lock_guard lock(m_Mutex);
        if (!m_StaticModelReady || m_StaticModelReady->Ticket != ticket) return std::nullopt;
        auto result = std::move(m_StaticModelReady);
        m_StaticModelReady.reset();
        return result;
    }
    bool RequestScriptTexture(const ScriptTextureRequest& request) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop || !request.Ticket || request.GameDir.empty() || request.Name.empty()) return false;
        if (m_ScriptTextureInFlight) return m_ScriptTextureInFlight == request.Ticket;
        if ((m_ScriptTextureWaiting && m_ScriptTextureWaiting->Ticket != request.Ticket) ||
            (m_ScriptTextureReady && m_ScriptTextureReady->Ticket != request.Ticket)) return false;
        if (!m_ScriptTextureWaiting && !m_ScriptTextureReady) m_ScriptTextureWaiting = request;
        m_Wake.notify_one();
        return true;
    }
    std::optional<ScriptTextureCompletion> TakeScriptTexture(std::uint64_t ticket) {
        std::lock_guard lock(m_Mutex);
        if (!m_ScriptTextureReady || m_ScriptTextureReady->Ticket != ticket) return std::nullopt;
        auto result = std::move(m_ScriptTextureReady);
        m_ScriptTextureReady.reset();
        return result;
    }

    // One unconsumed ticket, including during parsing. This queue uses the SAME
    // exclusive parser thread as world, vehicle, static model and texture jobs.
    bool RequestPedAsset(const PedAssetRequest& request) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop || !request.Ticket || request.GameDir.empty() || request.Model.ModelId <= 0 ||
            request.Model.ModelId >= 20000 || request.Model.Name.empty() || request.Model.TxdName.empty()) return false;
        if (m_PedAssetAdmitted) {
            const auto& old = *m_PedAssetAdmitted;
            const auto identity = [](const NativeWorldPedModelInfo& model) {
                return std::tie(model.ModelId, model.Name, model.TxdName, model.Ide.Source, model.Ide.Line,
                    model.PedTypeName, model.StatName, model.AnimationGroupName, model.AnimationFileName,
                    model.AudioTypeName, model.VoiceMinName, model.VoiceMaxName, model.CarsCanDriveMask,
                    model.PedFlags, model.Radio1, model.Radio2);
            };
            return old.Ticket == request.Ticket && old.GameDir == request.GameDir && identity(old.Model) == identity(request.Model);
        }
        m_PedAssetAdmitted = request;
        m_PedAssetWaiting = request;
        m_Wake.notify_one();
        return true;
    }
    std::optional<PedAssetCompletion> TakePedAsset(std::uint64_t ticket) {
        std::lock_guard lock(m_Mutex);
        if (m_Stop || !m_PedAssetReady || m_PedAssetReady->Ticket != ticket) return std::nullopt;
        auto result = std::move(m_PedAssetReady);
        m_PedAssetReady.reset();
        m_PedAssetAdmitted.reset();
        return result;
    }

    std::unique_ptr<CpuWorld> TakeReady() {
        std::lock_guard lock(m_Mutex);
        return std::move(m_Ready);
    }
    void Discard(std::unique_ptr<CpuWorld> world) {
        std::lock_guard lock(m_Mutex);
        assert(m_Busy && !m_Retired && !m_Ready);
        m_Retired = std::move(world);
        m_Busy = false;
        m_Wake.notify_one();
    }

    bool Building() {
        std::lock_guard lock(m_Mutex);
        return m_Building;
    }

    void Retire(std::unique_ptr<CpuWorld> world) {
        std::lock_guard lock(m_Mutex);
        assert(m_Busy && !m_Retired);
        m_Retired = std::move(world);
        m_Wake.notify_one();
    }

    // Called after publication AND incremental GL retirement. CPU retirement
    // is drained before another build, so there are at most two world packets
    // (active + building/ready/uploading/retiring), plus the bounded pager cache.
    void Release() {
        std::lock_guard lock(m_Mutex);
        assert(m_Busy && !m_Ready);
        m_Busy = false;
        m_Wake.notify_one();
    }

    void Stop(std::unique_ptr<CpuWorld> active, std::unique_ptr<CpuWorld> pending) {
        if (!m_Thread.joinable()) {
            return;
        }
        {
            std::lock_guard lock(m_Mutex);
            m_StopActive = std::move(active);
            m_StopPending = std::move(pending);
            m_Stop = true;
            m_Wake.notify_one();
        }
        // A parser/BVH build is not interruptible. Finish it before pager
        // shutdown; destroy all large CPU payloads on the worker, even on quit.
        m_Thread.join();
    }

private:
    void Run() {
        std::unique_lock lock(m_Mutex);
        for (;;) {
            m_Wake.wait(lock, [&] { return m_Stop || m_Retired || m_Vehicles.Retiring() ||
                m_Vehicles.Waiting() || m_StaticModelWaiting || m_ScriptTextureWaiting || m_PedAssetWaiting ||
                (!m_Busy && m_Wanted); });
            if (m_Stop) {
                auto ready = std::move(m_Ready);
                auto retired = std::move(m_Retired);
                auto active = std::move(m_StopActive);
                auto pending = std::move(m_StopPending);
                auto vehicle = m_Vehicles.Stop();
                auto ped = std::move(m_PedAssetReady);
                lock.unlock();
                return;
            }
            if (m_Retired) {
                auto retired = std::move(m_Retired);
                lock.unlock();
                retired.reset();
                lock.lock();
                continue;
            }
            if (m_Vehicles.Retiring()) {
                auto vehicle = m_Vehicles.Retire();
                lock.unlock();
                vehicle.reset();
                lock.lock();
                continue;
            }
            if (m_StaticModelWaiting) {
                auto request = std::move(*m_StaticModelWaiting);
                m_StaticModelWaiting.reset();
                m_StaticModelInFlight = request.Ticket;
                lock.unlock();
                StaticModelCompletion completion{.Ticket=request.Ticket,.Scene={},.Collision={},.Error={}};
                try {
                    auto scene = std::make_shared<WorldShotScene>();
                    if (m_StaticModelLoader && m_StaticModelLoader(request,*scene,completion.Collision,completion.Error))
                        completion.Scene = std::move(scene);
                    else if (!m_StaticModelLoader) completion.Error = "script model loader is unavailable";
                    else completion.Error = request.Model + ": " + completion.Error;
                } catch (const std::exception& e) {
                    completion.Error = request.Model + ": " + e.what();
                } catch (...) {
                    completion.Error = request.Model + ": unknown static-model parser exception";
                }
                lock.lock();
                m_StaticModelInFlight = 0;
                m_StaticModelReady = std::move(completion);
                continue;
            }
            if (m_ScriptTextureWaiting) {
                auto request = std::move(*m_ScriptTextureWaiting);
                m_ScriptTextureWaiting.reset();
                m_ScriptTextureInFlight = request.Ticket;
                lock.unlock();
                ScriptTextureCompletion completion{.Ticket=request.Ticket,.Packet={},.Error={}};
                try {
                    auto packet = std::make_shared<NativeScriptTextureDictionaryPacket>();
                    if (m_ScriptTextureLoader && m_ScriptTextureLoader(request,*packet,completion.Error))
                        completion.Packet = std::move(packet);
                    else if (!m_ScriptTextureLoader) completion.Error = "script texture loader is unavailable";
                    else completion.Error = request.Name + ": " + completion.Error;
                } catch (const std::exception& error) {
                    completion.Error = request.Name + ": " + error.what();
                } catch (...) {
                    completion.Error = request.Name + ": unknown texture parser exception";
                }
                lock.lock();
                m_ScriptTextureInFlight = 0;
                m_ScriptTextureReady = std::move(completion);
                continue;
            }
            if (m_PedAssetWaiting && ((!m_Vehicles.Waiting() && (m_Busy || !m_Wanted)) || !m_LastWasPed)) {
                auto request = std::move(*m_PedAssetWaiting);
                m_PedAssetWaiting.reset();
                m_PedAssetInFlight = request.Ticket;
                m_LastWasPed = true;
                lock.unlock();
                PedAssetCompletion completion{.Ticket=request.Ticket,.Packet={},.Error={}};
                try {
                    auto packet = std::make_shared<NativePedAssets>();
                    if (m_PedAssetLoader && m_PedAssetLoader(request, *packet, completion.Error)) {
                        if (packet->Geometries.empty() || packet->Model.ModelId != request.Model.ModelId ||
                            packet->Model.Name != request.Model.Name || packet->Model.TxdName != request.Model.TxdName)
                            completion.Error = "ped parser returned an empty or mismatched packet";
                        else if (completion.Error.empty()) completion.Packet = std::move(packet);
                    } else if (!m_PedAssetLoader) completion.Error = "ped asset loader is unavailable";
                } catch (const std::exception& error) {
                    completion.Error = request.Model.Name + ": " + error.what();
                } catch (...) {
                    completion.Error = request.Model.Name + ": unknown ped parser exception";
                }
                if (!completion.Packet && completion.Error.empty()) completion.Error = "ped parser failed without a packet";
                lock.lock();
                m_PedAssetInFlight = 0;
                m_PedAssetReady = std::move(completion);
                continue;
            }
            // Alternate eligible parser jobs, without waiting for world GPU
            // upload/retirement. Neither queue can starve the other; world CPU
            // retirement above retains its original priority and memory bound.
            if (m_Vehicles.Waiting() && (m_Busy || !m_Wanted || !m_LastWasVehicle)) {
                const auto ticket = m_Vehicles.Begin();
                const auto generation = m_Generation;
                m_LastWasVehicle = true;
                m_LastWasPed = false;
                lock.unlock();
                auto next = std::make_shared<NativeVehicleAssetCompletion>();
                next->Ticket = ticket;
                next->WorkerGeneration = generation;
                try {
                    next->Result = m_VehicleSource->Load(ticket.Identity->Definition, next->Asset);
                } catch (const std::exception& error) {
                    next->Result = {NativeGeneratedVehicleAssetStatus::Error, error.what()};
                } catch (...) {
                    next->Result = {NativeGeneratedVehicleAssetStatus::Error, "unknown vehicle parser exception"};
                }
                if (next->Result && !next->Asset) {
                    next->Result = {NativeGeneratedVehicleAssetStatus::Error,
                        "vehicle asset source returned Ready without a CPU packet"};
                } else if (!next->Result) {
                    next->Asset.reset();
                }
                std::shared_ptr<const NativeVehicleAssetCompletion> result = std::move(next);
                lock.lock();
                // Stop rejects new requests immediately, then this loop drains
                // all results on the worker before join permits RW shutdown.
                m_Vehicles.Finish(result);
                lock.unlock();
                result.reset();
                lock.lock();
                continue;
            }
            m_LastWasVehicle = false;
            m_LastWasPed = false;
            const auto center = m_Center;
            m_Wanted = false;
            m_Busy = true;
            m_Building = true;
            lock.unlock();
            auto next = std::make_unique<CpuWorld>();
            next->Position = center;
            next->Generation = ++m_Generation;
            try {
                next->Build(m_Collision, m_Context, m_Overrides);
            } catch (const std::exception& error) {
                next->Error = error.what();
            }
            lock.lock();
            m_Building = false;
            assert(!m_Ready);
            m_Ready = std::move(next);
        }
    }

    bool m_Collision;
    std::shared_ptr<const NativeCollisionContext> m_Context;
    const std::shared_ptr<const NativePlacementOverrides> m_Overrides;
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false, m_Wanted = false, m_Busy = false, m_Building = false;
    Center m_Center;
    uint64_t m_Generation = 1;
    const std::shared_ptr<const NativeVehicleAssetSource> m_VehicleSource;
    StaticModelLoader m_StaticModelLoader;
    ScriptTextureLoader m_ScriptTextureLoader;
    PedAssetLoader m_PedAssetLoader;
    NativeVehicleAssetQueue m_Vehicles;
    std::optional<StaticModelRequest> m_StaticModelWaiting;
    std::optional<StaticModelCompletion> m_StaticModelReady;
    std::uint64_t m_StaticModelInFlight = 0;
    std::optional<ScriptTextureRequest> m_ScriptTextureWaiting;
    std::optional<ScriptTextureCompletion> m_ScriptTextureReady;
    std::uint64_t m_ScriptTextureInFlight = 0;
    std::optional<PedAssetRequest> m_PedAssetWaiting;
    std::optional<PedAssetRequest> m_PedAssetAdmitted; // Immutable identity through waiting/parsing/ready.
    std::optional<PedAssetCompletion> m_PedAssetReady;
    std::uint64_t m_PedAssetInFlight = 0;
    bool m_LastWasVehicle = false;
    bool m_LastWasPed = false; // Bounded rotation with vehicle and pending world jobs.
    std::unique_ptr<CpuWorld> m_Ready, m_Retired, m_StopActive, m_StopPending;
    // Last: every field above is initialized before Run can observe it.
    std::thread m_Thread;
};
} // namespace realtime_streaming
