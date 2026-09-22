#include "NativeStoryLifecycle.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "story-lifecycle-fail: %s\n", message); std::exit(1); }
}
std::array<char, 8> Name(const char* text) {
    std::array<char, 8> out{};
    for (std::size_t i = 0; i < out.size() && text[i]; ++i) out[i] = text[i];
    return out;
}
}

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    NativeStoryLifecycle story;
    std::string error;
    Check(story.Initialize(gameDir, 2, Name("PROLOG1"), 43200, error) == NativeStoryStatus::Ok, error.c_str());
    Check(story.Start(100, error) == NativeStoryStatus::Ok && story.LastCommitted()->Attempt == 1, "normal start");
    Check(story.StartCutscene(110, error) == NativeStoryStatus::Ok && story.LastCommitted()->CutsceneLoaded, "cutscene start");
    const auto beforeEarly = story.LastCommitted();
    Check(story.ResolveCutscene(111, false, error) == NativeStoryStatus::InvalidPhase && story.LastCommitted() == beforeEarly,
        "early cutscene finish atomic");
    Check(story.ResolveCutscene(112, true, error) == NativeStoryStatus::Ok && story.LastCommitted()->Skips == 1,
        "source skip path");
    Check(story.RegisterResources(3, 1, 1, 2, error) == NativeStoryStatus::Ok, "first attempt resources");
    Check(story.Fail(120, error) == NativeStoryStatus::Ok && story.LastCommitted()->Failures == 1, "failure path");
    Check(story.Cleanup(121, error) == NativeStoryStatus::Ok && story.LastCommitted()->Phase == NativeStoryPhase::RetryAvailable &&
        story.LastCommitted()->Peds == 0 && story.LastCommitted()->Vehicles == 0, "failure cleanup");
    Check(story.Retry(130, error) == NativeStoryStatus::Ok && story.LastCommitted()->Attempt == 2 &&
        story.LastCommitted()->Retries == 1, "retry path");
    Check(story.StartCutscene(140, error) == NativeStoryStatus::Ok, "retry cutscene start");
    Check(story.Advance(23000, error) == NativeStoryStatus::Ok &&
        story.ResolveCutscene(23000, false, error) == NativeStoryStatus::Ok, "authored cutscene completion");
    Check(story.RegisterResources(4, 2, 1, 3, error) == NativeStoryStatus::Ok &&
        story.StartAudio(23100, error) == NativeStoryStatus::Ok, "completion resources and audio");
    const auto beforeAudio = story.LastCommitted();
    Check(story.Complete(23101, error) == NativeStoryStatus::InvalidPhase && story.LastCommitted() == beforeAudio,
        "audio completion barrier");
    Check(story.Advance(50000, error) == NativeStoryStatus::Ok && story.LastCommitted()->AudioFinished,
        "source audio duration completion");
    Check(story.Complete(50001, error) == NativeStoryStatus::Ok && story.LastCommitted()->Completions == 1,
        "mission complete");
    const auto completed = story.LastCommitted();
    Check(story.Cleanup(50002, error) == NativeStoryStatus::Ok && story.LastCommitted()->Phase == NativeStoryPhase::Cleaned &&
        story.LastCommitted()->Peds == 0 && story.LastCommitted()->Vehicles == 0 && story.LastCommitted()->Trains == 0 &&
        story.LastCommitted()->Objects == 0, "completion cleanup");
    Check(completed->Phase == NativeStoryPhase::Completed && completed->Peds == 4 && !NativeStoryLifecycle::PresentationFeedback,
        "immutable completion and no presentation feedback");
    Check(story.LastCommitted()->Events.size() == 11, "ordered lifecycle journal");
    std::printf("native-story-lifecycle-ok checks=%d mission=2 attempts=2 fail=1 retry=1 skip=1 complete=1 "
        "cleanup=2 cutscene=PROLOG1 audio=43200 presentation-feedback=0\n", g_Checks);
}
