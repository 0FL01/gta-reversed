#include "NativeScriptObjects.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "object-lifecycle-fail: %s\n", message); std::exit(1); }
}
NativeScriptObjectSource Source(std::int32_t model) {
    NativeScriptObjectSource source;
    source.ModelId = model;
    std::snprintf(source.Name.data(), source.Name.size(), "fixture%d", model);
    auto collision = std::make_shared<NativeCollisionModel>();
    collision->Name = source.Name.data();
    collision->BoundRadius = 1.0f;
    collision->Spheres.push_back({{}, 1.0f, {}});
    source.Collision = std::move(collision);
    return source;
}
}

int main() {
    NativeScriptObjects objects;
    std::string error;
    std::array<NativeScriptObjectRef, 6> refs;
    constexpr std::array<std::uint8_t, 6> effects{0, 1, 20, 21, 200, 202};
    for (std::size_t i = 0; i < refs.size(); ++i) {
        NativeScriptObjectRequest request{{1, i + 1, 0}, std::int32_t(100 + i), {float(i), 0, 1}, {}};
        const auto created = objects.Create(request, Source(request.ModelId));
        Check(created.Result.Status == NativeScriptServiceStatus::Ready, "create source object");
        refs[i] = created.Reference;
        Check(objects.SetCollisionDamageEffect(refs[i], effects[i], error) == NativeScriptObjectStatus::Ok,
            "set collision damage effect");
    }
    const auto beforeDamage = objects.Publish();
    Check(beforeDamage->Objects.size() == effects.size() && beforeDamage->Epoch == 1,
        "publish complete object matrix");
    for (std::size_t i = 0; i < refs.size(); ++i) {
        Check(objects.ApplyDamage(refs[i], 200.0f, 1.0f, error) == NativeScriptObjectStatus::Ok,
            "apply source object damage");
    }
    Check(objects.Resolve(refs[0])->Health == 800.0f && objects.Resolve(refs[0])->Visible,
        "no-effect object retains world state");
    Check(objects.Resolve(refs[1])->RenderDamaged && objects.Resolve(refs[1])->UsesCollision,
        "change-model damage state");
    Check(!objects.Resolve(refs[2])->Visible && !objects.Resolve(refs[2])->UsesCollision &&
        objects.Resolve(refs[2])->Health == 0.0f, "smash-completely state");
    Check(objects.Resolve(refs[3])->RenderDamaged && objects.Resolve(refs[3])->Visible,
        "change-then-smash first state");
    Check(objects.ApplyDamage(refs[3], 200.0f, 1.0f, error) == NativeScriptObjectStatus::Ok &&
        !objects.Resolve(refs[3])->Visible, "change-then-smash second state");
    Check(objects.Resolve(refs[4])->Broken && objects.Resolve(refs[5])->Broken,
        "breakable states");
    const auto revision = objects.Revision();
    Check(objects.ApplyDamage(refs[0], -1.0f, 1.0f, error) == NativeScriptObjectStatus::InvalidInput &&
        objects.Revision() == revision && objects.Resolve(refs[0])->Health == 800.0f,
        "invalid damage rejection atomic");
    const auto damaged = objects.Publish();
    Check(objects.Reload(1, error) == NativeScriptObjectStatus::InvalidInput && objects.LiveCount() == 6,
        "stale reload rejection");
    Check(objects.Reload(2, error) == NativeScriptObjectStatus::Ok && objects.LiveCount() == 0,
        "reload clears current object owners");
    Check(!objects.Resolve(refs[0]) && damaged->Objects.size() == 6 &&
        damaged->Objects[2].Collision && damaged->Objects[4].Broken,
        "stale refs rejected and held snapshot survives reload");

    std::printf("native-object-lifecycle-ok checks=%d objects=6 effects=0,1,20,21,200,202 damage=source reload=epoch2 generation=stale-safe\n",
        g_Checks);
}
