#include "NativeActivityLifecycle.h"
#include "NativeScriptSession.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "activity-lifecycle-fail: %s\n", message); std::exit(1); }
}
}

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    NativeScriptSession scripts;
    std::string error;
    Check(scripts.LoadMain(gameDir, error), error.c_str());
    NativeActivityLifecycle lifecycle;
    const auto& definitions = NativeActivityLifecycle::Definitions();
    Check(definitions.size() == 17, "discovered family census");
    std::set<NativeActivityFamily> families;
    std::set<NativeActivityCategory> categories;
    std::uint32_t time = 100;
    for (const auto& definition : definitions) {
        Check(families.insert(definition.Family).second, "unique activity family");
        categories.insert(definition.Category);
        if (definition.StreamedScript >= 0) {
            const auto& source = scripts.Metadata().StreamedDefinitions[std::size_t(definition.StreamedScript)].Name;
            Check(std::equal(definition.Name.begin(), definition.Name.end(), source.begin()),
                "exact streamed activity identity");
        }
        Check(lifecycle.Start(definition.Family, time, error) == NativeActivityStatus::Ok, "activity start");
        const auto running = lifecycle.LastCommitted();
        Check(lifecycle.Finish(true, 1000 + std::int32_t(std::size_t(definition.Family)), time + 50, error) ==
            NativeActivityStatus::Ok, "activity pass");
        Check(lifecycle.Cleanup(time + 51, error) == NativeActivityStatus::Ok &&
            lifecycle.LastCommitted()->Phase == NativeActivityPhase::Cleaned, "activity cleanup");
        Check(running->Phase == NativeActivityPhase::Running, "immutable running publication");
        time += 100;
    }
    Check(categories.size() == 5, "service race school minigame other categories");
    Check(lifecycle.Start(NativeActivityFamily::Taxi, time, error) == NativeActivityStatus::Ok &&
        lifecycle.Finish(false, 0, time + 1, error) == NativeActivityStatus::Ok &&
        lifecycle.Cleanup(time + 2, error) == NativeActivityStatus::Ok, "failure cleanup route");
    const auto before = lifecycle.LastCommitted();
    Check(lifecycle.Cleanup(time + 3, error) == NativeActivityStatus::InvalidPhase &&
        lifecycle.LastCommitted() == before, "invalid cleanup atomic");
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& stats = lifecycle.LastCommitted()->Stats[i];
        Check(stats.Passes == 1 && stats.Cleanups == 1 + (i == std::size_t(NativeActivityFamily::Taxi)) &&
            stats.Starts == 1 + (i == std::size_t(NativeActivityFamily::Taxi)), "family route statistics");
    }
    Check(lifecycle.LastCommitted()->Events.size() == 17 * 3 + 3 && !NativeActivityLifecycle::GameplayRulesComplete,
        "ordered route journal and bounded rule claim");
    std::printf("native-activity-lifecycle-ok checks=%d families=17 categories=service,race,school,minigame,other "
        "routes=start,result,cleanup failure-retry=taxi gameplay-rules-complete=0\n", g_Checks);
}
