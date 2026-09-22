#pragma once

#include "NativeCollisionAssets.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class NativeWeaponClass : std::uint8_t { Melee, InstantHit, Projectile, AreaEffect, Camera, Use };
struct NativeWeaponDefinition {
    std::string Name;
    NativeWeaponClass Class = NativeWeaponClass::InstantHit;
    std::uint16_t Damage = 0;
    float Speed = 0.0f, Radius = 0.0f, Lifespan = 0.0f, Spread = 0.0f;
};
enum class NativeCombatEntityKind : std::uint8_t { Ped, Vehicle, Object };
struct NativeCombatRef { std::uint32_t Value{}; bool operator==(const NativeCombatRef&) const = default; };
struct NativeCombatEntity {
    NativeCombatRef Reference;
    NativeCombatEntityKind Kind = NativeCombatEntityKind::Ped;
    std::int32_t ModelId = -1;
    float Health = 0.0f, Armour = 0.0f;
    bool Alive = false, Burning = false;
    bool operator==(const NativeCombatEntity&) const = default;
};
struct NativeProjectileState {
    std::uint64_t Sequence = 0;
    NativeWeaponDefinition Weapon;
    NativeCollisionVector Position{}, Velocity{};
    float Remaining = 0.0f;
    bool Active = false;
};
enum class NativeCombatStatus : std::uint8_t {
    Ok, InvalidInput, StaleReference, CapacityExceeded, Unsupported, Overflow,
};

class NativeCombatRuntime {
public:
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    const NativeWeaponDefinition* First(NativeWeaponClass) const noexcept;
    std::span<const NativeWeaponDefinition> Weapons() const { return m_Weapons; }

    NativeCombatStatus Spawn(NativeCombatEntityKind, std::int32_t modelId,
        float health, float armour, NativeCombatRef& out, std::string& error);
    NativeCombatStatus Hit(NativeCombatRef, const NativeWeaponDefinition&, float scale, std::string& error);
    NativeCombatStatus Launch(const NativeWeaponDefinition&, NativeCollisionVector position,
        NativeCollisionVector direction, NativeProjectileState& out, std::string& error);
    NativeCombatStatus AdvanceProjectile(NativeProjectileState&, float timeStep, std::string& error) const;
    NativeCombatStatus Impact(NativeProjectileState&, NativeCombatRef, std::string& error);
    NativeCombatStatus Ignite(NativeCombatRef, const NativeWeaponDefinition&, std::string& error);
    NativeCombatStatus AdvanceFire(NativeCombatRef, const NativeWeaponDefinition&, float timeStep, std::string& error);
    const NativeCombatEntity* Resolve(NativeCombatRef) const noexcept;

private:
    struct Slot { std::uint8_t Generation{}; bool Alive{}; NativeCombatEntity Entity; };
    NativeCombatEntity* ResolveMutable(NativeCombatRef) noexcept;
    NativeCombatStatus ApplyDamage(NativeCombatEntity&, float damage, std::string& error);

    std::vector<NativeWeaponDefinition> m_Weapons;
    std::array<Slot, 64> m_Entities{};
    std::uint64_t m_ProjectileSequence = 0;
};
