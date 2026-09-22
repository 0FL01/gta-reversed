#include "NativeCombatRuntime.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const std::string& message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "combat-runtime-fail: %s\n", message.c_str()); std::exit(1); }
}
const NativeWeaponDefinition* Damaging(const NativeCombatRuntime& runtime, NativeWeaponClass kind) {
    for (const auto& weapon : runtime.Weapons()) {
        if (weapon.Class == kind && weapon.Damage > 0 &&
            (kind != NativeWeaponClass::Projectile || (weapon.Speed > 0.0f && weapon.Lifespan > 0.0f))) return &weapon;
    }
    return nullptr;
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "usage: probe GAME_DIR");
    NativeCombatRuntime combat;
    std::string error;
    Check(combat.LoadBeforeWorker(argv[1], error), error);
    const auto* melee = combat.First(NativeWeaponClass::Melee);
    const auto* instant = Damaging(combat, NativeWeaponClass::InstantHit);
    const auto* projectile = Damaging(combat, NativeWeaponClass::Projectile);
    const auto* area = Damaging(combat, NativeWeaponClass::AreaEffect);
    Check(melee && instant && projectile && area, "representative source weapon classes");
    Check(combat.First(NativeWeaponClass::Camera) && combat.First(NativeWeaponClass::Use),
        "non-damage source classes retained");

    NativeCombatRef ped, vehicle, object;
    Check(combat.Spawn(NativeCombatEntityKind::Ped, 7, 100.0f, 25.0f, ped, error) == NativeCombatStatus::Ok &&
        combat.Spawn(NativeCombatEntityKind::Vehicle, 400, 1000.0f, 0.0f, vehicle, error) == NativeCombatStatus::Ok &&
        combat.Spawn(NativeCombatEntityKind::Object, 3084, 1000.0f, 0.0f, object, error) == NativeCombatStatus::Ok,
        "spawn representative source model classes");
    Check(combat.Hit(ped, *instant, 1.0f, error) == NativeCombatStatus::Ok &&
        combat.Resolve(ped)->Armour < 25.0f, "instant hit consumes ped armour first");
    Check(combat.Hit(vehicle, *instant, 2.0f, error) == NativeCombatStatus::Ok &&
        combat.Resolve(vehicle)->Health < 1000.0f, "instant damage transition on vehicle");
    const auto vehicleHealth = combat.Resolve(vehicle)->Health;
    Check(combat.Hit(vehicle, *melee, 1.0f, error) == NativeCombatStatus::Ok &&
        combat.Resolve(vehicle)->Health == vehicleHealth, "melee class defers combo damage amount");

    NativeProjectileState shot;
    Check(combat.Launch(*projectile, {}, {1, 0, 0}, shot, error) == NativeCombatStatus::Ok &&
        shot.Active && shot.Sequence == 1, "projectile launch");
    const auto before = shot.Position;
    Check(combat.AdvanceProjectile(shot, std::min(0.1f, shot.Remaining * 0.5f), error) == NativeCombatStatus::Ok &&
        shot.Position != before, "projectile movement");
    Check(combat.Impact(shot, object, error) == NativeCombatStatus::Ok && !shot.Active &&
        combat.Resolve(object)->Health < 1000.0f, "projectile impact damage");

    Check(combat.Ignite(ped, *area, error) == NativeCombatStatus::Ok && combat.Resolve(ped)->Burning,
        "area fire ignition");
    const auto health = combat.Resolve(ped)->Health;
    Check(combat.AdvanceFire(ped, *area, 1.0f, error) == NativeCombatStatus::Ok &&
        combat.Resolve(ped)->Health < health, "source fire damage tick");
    Check(combat.Hit(ped, *instant, 1000.0f, error) == NativeCombatStatus::Ok &&
        !combat.Resolve(ped)->Alive && combat.Resolve(ped)->Health == 0.0f, "ped death transition");
    const auto retained = *combat.Resolve(vehicle);
    Check(combat.Hit(vehicle, *combat.First(NativeWeaponClass::Camera), 1.0f, error) ==
        NativeCombatStatus::Unsupported && *combat.Resolve(vehicle) == retained,
        "non-damage class rejection retains target");

    std::printf("native-combat-runtime-ok checks=%d weapons=%zu classes=melee,instant,projectile,area,camera,use ped=dead vehicle=damaged object=damaged fire=ticked\n",
        g_Checks, combat.Weapons().size());
}
