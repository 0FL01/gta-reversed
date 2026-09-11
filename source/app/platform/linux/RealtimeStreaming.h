// Native --play's single CPU producer. No GL or SDL calls in this file.
#pragma once

#include "app/platform/linux/StreamPager.h"
#include "app/platform/linux/RealtimeGameplay.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace realtime_streaming {
inline double Milliseconds() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Center {
    float X{}, Y{}, Z{};
};

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
                if (!context->Snapshot(Position.X, Position.Y, *snapshot, Error, Overrides) ||
                    !Collision.Rebuild(*snapshot, Error)) return;
                SourceCollision = std::move(snapshot);
            } else {
                Collision.Rebuild(Scene, Error);
            }
        }
        CollisionMs = Milliseconds() - Started - PagerMs;
        if (SourceCollision) {
            std::printf("source-col-world generation=%llu center=%.3f,%.3f,%.3f pagerMs=%.2f collisionMs=%.2f instances=%zu triangles=%zu spheres=%zu boxes=%zu collapsed=%zu missing=%zu empty=%zu\n",
                static_cast<unsigned long long>(Generation), Position.X, Position.Y, Position.Z, PagerMs, CollisionMs,
                SourceCollision->Instances.size(), Collision.TriangleCount(), Collision.SphereCount(), Collision.BoxCount(),
                Collision.CollapsedTriangleCount(), SourceCollision->MissingModels, SourceCollision->EmptyModels);
        }
    }
};

// Start ONLY after all startup parsers (including gameplay/CarPose/IfpAnim)
// have returned. Until Stop joins, ONLY this thread may call StreamPager,
// TexSample/librw parsers or touch their globals. Freeze OS_SetFilePathOffset.
// Actor Tick/Draw use owned CPU snapshots, not librw. Even separate TXDs are
// not independent: librw current dictionary, plugins, frame lists are global.
class Worker {
public:
    explicit Worker(bool collision, std::shared_ptr<const NativeCollisionContext> context = {},
                    std::shared_ptr<const NativePlacementOverrides> overrides = {}, uint64_t initialGeneration = 1)
        : m_Collision(collision), m_Context(std::move(context)), m_Overrides(std::move(overrides)),
          m_Generation(initialGeneration), m_Thread([this] { Run(); }) {}
    ~Worker() { Stop({}, {}); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Latest-center mailbox, not a queue. Returning inside the loaded threshold
    // cancels unstarted requests; an in-flight result is allowed to finish.
    void Request(Center center, bool wanted) {
        std::lock_guard lock(m_Mutex);
        m_Center = center;
        m_Wanted = wanted;
        m_Wake.notify_one();
    }

    std::unique_ptr<CpuWorld> TakeReady() {
        std::lock_guard lock(m_Mutex);
        return std::move(m_Ready);
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
            m_Wake.wait(lock, [&] { return m_Stop || m_Retired || (!m_Busy && m_Wanted); });
            if (m_Stop) {
                auto ready = std::move(m_Ready);
                auto retired = std::move(m_Retired);
                auto active = std::move(m_StopActive);
                auto pending = std::move(m_StopPending);
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
    std::unique_ptr<CpuWorld> m_Ready, m_Retired, m_StopActive, m_StopPending;
    // Last: every field above is initialized before Run can observe it.
    std::thread m_Thread;
};
} // namespace realtime_streaming
