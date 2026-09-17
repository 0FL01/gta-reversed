#include "app/platform/linux/NativeScriptModelAnims.h"

#include <cstdio>

int main() {
    NativeScriptModelAnims bindings;
    NativeScriptModelAnimRequest request;
    request.ModelId = 7;
    request.IfpName = {'A', 'T', 'T', 'R', 'A', 'C', 'T'};
    if (bindings.Add(request).Status != NativeScriptServiceStatus::Ready || bindings.Entries().size() != 1 ||
        bindings.Add(request).Status != NativeScriptServiceStatus::Ready || bindings.Entries().size() != 1) {
        return 1;
    }
    request.ModelId = -1;
    if (bindings.Add(request).Status != NativeScriptServiceStatus::Error || bindings.Entries().size() != 1) return 1;
    std::printf("native-script-model-anims-ok checks=5 capacity=8 application=0\n");
}
