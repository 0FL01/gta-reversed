#include "app/platform/linux/NativeWorldObjectOverrides.h"

#include <cstdio>

int main() {
    NativeCollisionPopulation population;
    NativeCollisionPlacement placement;
    placement.ModelId = 100;
    placement.Model = "fixture";
    placement.Ipl = "fixture.ipl";
    placement.Position = {1.0f, 2.0f, 3.0f};
    population.Instances.push_back(placement);
    NativeScriptWorldObjectVisibilityRequest request;
    request.ModelId = 100;
    request.Position = {1.0f, 2.0f, 3.0f};
    request.Radius = 2.0f;
    request.Visible = false;
    NativeWorldObjectOverrides overrides;
    if (overrides.SetClosestVisibility(request, population).Status != NativeScriptServiceStatus::Ready ||
        overrides.Entries().size() != 1 || overrides.Entries()[0].Visible) return 1;
    request.ModelId = -1;
    if (overrides.SetClosestVisibility(request, population).Status != NativeScriptServiceStatus::Error ||
        overrides.Entries().size() != 1) return 1;
    std::printf("native-world-object-overrides-ok checks=5 capacity=128 presentation=0\n");
}
