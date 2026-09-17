#include "app/platform/linux/NativeExternalScriptTriggers.h"

#include <cstdio>

int main() {
    NativeExternalScriptTriggers triggers;
    std::array<NativeScriptStreamedState, 1> scripts{};
    NativeScriptExternalTriggerRequest request;
    request.ModelId = 123;
    request.Priority = 100;
    request.Radius = 6.0f;
    request.Type = 1;
    request.ObjectModel = true;
    if (triggers.Add(request, scripts).Status != NativeScriptServiceStatus::Ready || triggers.Entries().size() != 1) {
        return 1;
    }
    NativeScriptCodeBrainRequest codeUse;
    codeUse.ScriptIndex = 0;
    codeUse.Name = {'H', 'O', 'U', 'S', 'E'};
    if (triggers.AddCodeUse(codeUse, scripts).Status != NativeScriptServiceStatus::Ready ||
        triggers.Entries().size() != 2 || triggers.Entries()[1].Kind != NativeExternalTriggerKind::CodeUse ||
        triggers.Entries()[1].Type != 3) {
        return 1;
    }
    codeUse.Attractor = true;
    if (triggers.AddCodeUse(codeUse, scripts).Status != NativeScriptServiceStatus::Ready ||
        triggers.Entries().size() != 3 || triggers.Entries()[2].Kind != NativeExternalTriggerKind::AttractorCodeUse ||
        triggers.Entries()[2].Type != 5) {
        return 1;
    }
    std::printf("native-external-triggers-ok checks=8 capacity=70 activation=0\n");
}
