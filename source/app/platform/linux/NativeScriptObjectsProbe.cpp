#include "app/platform/linux/NativeScriptObjects.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {
std::size_t s_Checks = 0;
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) { std::fprintf(stderr, "native-script-objects FAIL: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeScriptObjects objects;
    auto collision = std::make_shared<NativeCollisionModel>();
    collision->Name = "trdcsgrgdoor_lvs";
    collision->Spheres.push_back({{}, 1, {}});
    NativeScriptObjectSource source{3084, {'t','r','d','c','s','g','r','g','d','o','o','r','_','l','v','s'}, collision};
    NativeScriptObjectRequest request{{1, 1, 10}, -64, {1903.383f, 967.62f, 11.438f}, source.Name};
    std::string error;
    const auto created = objects.Create(request, source);
    Check(created.Result.Status == NativeScriptServiceStatus::Ready && created.Reference.Value >= 0,
        "source object registration returns an opaque generation reference");
    const auto* object = objects.Resolve(created.Reference);
    Check(object && object->ModelOperand == -64 && object->ModelId == 3084 && object->Position == request.Position &&
        object->Mission && object->InWorld && object->Collision == collision, "object retains exact identity and position");
    Check(objects.SetVelocity(created.Reference, {1, 2, 3}, error) == NativeScriptObjectStatus::Ok &&
        objects.Resolve(created.Reference)->Velocity == NativeScriptPosition{1, 2, 3},
        "source object velocity is value-owned without moving the object");
    Check(objects.SetHeading(created.Reference, 90, error) == NativeScriptObjectStatus::Ok,
        "source object heading accepts the source degree setter");
    NativeScriptPosition offset{};
    Check(objects.GetOffsetInWorld(created.Reference, {1, 0, 0}, offset, error) == NativeScriptObjectStatus::Ok &&
        std::abs(offset.X - request.Position.X) < 0.001f && std::abs(offset.Y - (request.Position.Y + 1)) < 0.001f,
        "zero-euler source object offset applies the owned heading transform");
    Check(objects.SetRotation(created.Reference, {1, 0, 0}, false, error) == NativeScriptObjectStatus::Ok &&
        objects.GetOffsetInWorld(created.Reference, {}, offset, error) == NativeScriptObjectStatus::Unsupported,
        "unowned full 3D object matrix remains a strict offset frontier");
    Check(objects.SetRotation(created.Reference, {}, false, error) == NativeScriptObjectStatus::Ok &&
        objects.SetVelocity(created.Reference, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, error) == NativeScriptObjectStatus::InvalidInput &&
        objects.Resolve(created.Reference)->Velocity == NativeScriptPosition{1, 2, 3},
        "invalid velocity retains the prior object publication");
    const auto retained = *objects.Resolve(created.Reference);
    request.Position.X = std::numeric_limits<float>::quiet_NaN();
    const auto bad = objects.Create(request, source);
    Check(bad.Result.Status == NativeScriptServiceStatus::Error && objects.Resolve(created.Reference) &&
        *objects.Resolve(created.Reference) == retained, "invalid object request retains the live owner");
    Check(objects.Remove(created.Reference, error) == NativeScriptObjectStatus::Ok && !objects.Resolve(created.Reference),
        "release invalidates the generation reference");
    Check(objects.Remove(created.Reference, error) == NativeScriptObjectStatus::InvalidInput,
        "stale object reference is rejected");
    std::printf("native-script-objects-ok checks=%zu capacity=350 model=3084 source=trdcsgrgdoor_lvs generation=stale-safe\n", s_Checks);
}
