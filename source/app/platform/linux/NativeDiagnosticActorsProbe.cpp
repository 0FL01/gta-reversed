#include "NativeDiagnosticActors.h"
#include "StreamPager.h"
#include <cstdio>
#include <limits>
#include <rw.h>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: sa_diagnostic_actors_probe <owned-game-dir>\n"); return 2; }
    char error[512]{}; E2ELoadInfo load{};
    if (!StreamPager_Init(argv[1], load, error, sizeof(error))) { std::fprintf(stderr, "%s\n", error); return 1; }
    bool ok = true;
    NativeDiagnosticActorPose retained;
    {
        NativeDiagnosticActors actors; std::string failure;
        ok = actors.Initialize(argv[1], failure);
        if (ok) std::puts(actors.Current().Trace.c_str());
        for (int i = 0; ok && i < 26; ++i) {
            RealtimeGameplayInput input; input.Forward = i < 8 ? 1.0f : 0.0f; input.Sprint = i >= 4 && i < 8;
            input.Interact = i == 12;
            if (i > 12) { input.Forward = 1; input.Side = 0.25f; }
            const auto previous = actors.Current();
            ok = actors.Tick(1.0 / 30.0, input, failure);
            if (!ok) break;
            const auto before = actors.Current().Trace;
            NativeDiagnosticActorPose zero, one, half;
            ok = actors.Present(0, zero, failure) && actors.Present(1, one, failure) && actors.Present(0.5, half, failure);
            ok = ok && one.Positions == actors.Current().Positions && actors.Current().Trace == before;
            for (std::size_t m = 0; ok && m < half.Positions.size(); ++m) {
                if (previous.Triangles[m] != one.Triangles[m]) continue;
                ok = zero.Positions[m] == previous.Positions[m];
                for (std::size_t v = 0; ok && v < half.Positions[m].size(); ++v)
                    ok = half.Positions[m][v] == float((double(previous.Positions[m][v]) + double(one.Positions[m][v])) * 0.5);
            }
            ok = ok && !actors.Present(std::numeric_limits<double>::quiet_NaN(), half, failure) && half.Trace == before;
            ok = ok && !actors.Tick(-1, input, failure) && actors.Current().Trace == before;
            retained = std::move(half);
            std::puts(before.c_str());
        }
        ok = ok && actors.Current().InVehicle && actors.Current().CarSpeed > 0;
        if (!ok) std::fprintf(stderr, "diagnostic-actors FAIL: %s\n", failure.c_str());
    }
    StreamPager_Shutdown();
    ok = ok && rw::Raster::numAllocated == 0 && rw::Texture::numAllocated == 0;
    ok = ok && !retained.Positions.empty() && retained.Tick > 0;
    if (ok) std::puts("diagnostic-actors-ok synthetic-floor=true source-gameplay=false teardown=owned");
    return ok ? 0 : 1;
}
