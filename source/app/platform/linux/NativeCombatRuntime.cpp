#include "NativeCombatRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#ifndef __stdcall
#define __stdcall
#endif
#include "oswrapper/oswrapper.h"

namespace {
bool ParseFloat(const std::string& text, float& out) {
    char* end = nullptr;
    out = std::strtof(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}
bool ParseUnsigned(const std::string& text, std::uint32_t& out) {
    char* end = nullptr;
    const auto value = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}
NativeWeaponClass Class(std::string_view name) {
    if (name == "MELEE") return NativeWeaponClass::Melee;
    if (name == "PROJECTILE") return NativeWeaponClass::Projectile;
    if (name == "AREA_EFFECT") return NativeWeaponClass::AreaEffect;
    if (name == "CAMERA") return NativeWeaponClass::Camera;
    if (name == "USE") return NativeWeaponClass::Use;
    return NativeWeaponClass::InstantHit;
}
bool Finite(const NativeCollisionVector& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
}

bool NativeCombatRuntime::LoadBeforeWorker(const char* gameDir, std::string& error) {
    if (!gameDir || !*gameDir) { error = "combat runtime requires game root"; return false; }
    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "data/weapon.dat", FILE_ACCESS_READ) != 0 || !file) {
        error = "cannot open source weapon data";
        return false;
    }
    const auto size = OS_FileSize(file);
    if (size <= 0 || size > 4 * 1024 * 1024) { OS_FileClose(file); error = "weapon data size is invalid"; return false; }
    std::string text(std::size_t(size), '\0');
    const bool read = OS_FileRead(file, text.data(), int32(text.size())) == 0;
    OS_FileClose(file);
    if (!read) { error = "weapon data read failed"; return false; }
    std::vector<NativeWeaponDefinition> next;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::vector<std::string> tokens;
        for (std::string token; words >> token;) tokens.push_back(token);
        if (tokens.size() >= 3 && tokens[2] == "MELEE") {
            next.push_back({tokens[1], NativeWeaponClass::Melee, 0, 0, 0, 0, 0});
            continue;
        }
        if (tokens.empty() || tokens[0] != "$" || tokens.size() < 26) continue;
        std::uint32_t damage = 0;
        if (!ParseUnsigned(tokens[10], damage) || damage > std::numeric_limits<std::uint16_t>::max()) {
            error = "weapon damage is invalid";
            return false;
        }
        NativeWeaponDefinition definition;
        definition.Name = tokens[1];
        definition.Class = Class(tokens[2]);
        definition.Damage = std::uint16_t(damage);
        if (tokens.size() > 26 && !ParseFloat(tokens[26], definition.Speed)) return false;
        if (tokens.size() > 27 && !ParseFloat(tokens[27], definition.Radius)) return false;
        if (tokens.size() > 28 && !ParseFloat(tokens[28], definition.Lifespan)) return false;
        if (tokens.size() > 29 && !ParseFloat(tokens[29], definition.Spread)) return false;
        next.push_back(std::move(definition));
    }
    if (next.empty()) { error = "weapon data has no source rows"; return false; }
    m_Weapons = std::move(next);
    error.clear();
    return true;
}

const NativeWeaponDefinition* NativeCombatRuntime::First(NativeWeaponClass value) const noexcept {
    const auto found = std::ranges::find(m_Weapons, value, &NativeWeaponDefinition::Class);
    return found == m_Weapons.end() ? nullptr : &*found;
}

NativeCombatStatus NativeCombatRuntime::Spawn(NativeCombatEntityKind kind, std::int32_t modelId,
    float health, float armour, NativeCombatRef& out, std::string& error) {
    if (modelId < 0 || !std::isfinite(health) || health <= 0.0f || !std::isfinite(armour) || armour < 0.0f) {
        error = "combat entity is invalid";
        return NativeCombatStatus::InvalidInput;
    }
    const auto slot = std::ranges::find_if(m_Entities, [](const Slot& value) { return !value.Alive; });
    if (slot == m_Entities.end()) { error = "combat entity capacity exceeded"; return NativeCombatStatus::CapacityExceeded; }
    const auto index = std::size_t(slot - m_Entities.begin());
    slot->Generation = std::uint8_t(slot->Generation + 1u);
    if (!slot->Generation) slot->Generation = 1;
    slot->Alive = true;
    out = {std::uint32_t(index << 8u) | slot->Generation};
    slot->Entity = {out, kind, modelId, health, armour, true, false};
    error.clear();
    return NativeCombatStatus::Ok;
}

NativeCombatEntity* NativeCombatRuntime::ResolveMutable(NativeCombatRef reference) noexcept {
    if (!reference.Value) return nullptr;
    const auto index = std::size_t(reference.Value >> 8u);
    const auto generation = std::uint8_t(reference.Value);
    return index < m_Entities.size() && m_Entities[index].Alive && m_Entities[index].Generation == generation
        ? &m_Entities[index].Entity : nullptr;
}
const NativeCombatEntity* NativeCombatRuntime::Resolve(NativeCombatRef reference) const noexcept {
    return const_cast<NativeCombatRuntime*>(this)->ResolveMutable(reference);
}

NativeCombatStatus NativeCombatRuntime::ApplyDamage(NativeCombatEntity& entity,
    float damage, std::string& error) {
    if (!std::isfinite(damage) || damage < 0.0f) { error = "combat damage is invalid"; return NativeCombatStatus::InvalidInput; }
    if (entity.Kind == NativeCombatEntityKind::Ped) {
        const float absorbed = std::min(entity.Armour, damage);
        entity.Armour -= absorbed;
        damage -= absorbed;
    }
    entity.Health = std::max(0.0f, entity.Health - damage);
    entity.Alive = entity.Health > 0.0f;
    error.clear();
    return NativeCombatStatus::Ok;
}

NativeCombatStatus NativeCombatRuntime::Hit(NativeCombatRef reference,
    const NativeWeaponDefinition& weapon, float scale, std::string& error) {
    auto* entity = ResolveMutable(reference);
    if (!entity) { error = "combat entity reference is stale"; return NativeCombatStatus::StaleReference; }
    if ((weapon.Class == NativeWeaponClass::Camera || weapon.Class == NativeWeaponClass::Use) ||
        !std::isfinite(scale) || scale < 0.0f) {
        error = "weapon cannot produce direct damage";
        return NativeCombatStatus::Unsupported;
    }
    return ApplyDamage(*entity, float(weapon.Damage) * scale, error);
}

NativeCombatStatus NativeCombatRuntime::Launch(const NativeWeaponDefinition& weapon,
    NativeCollisionVector position, NativeCollisionVector direction, NativeProjectileState& out,
    std::string& error) {
    if (weapon.Class != NativeWeaponClass::Projectile || !Finite(position) || !Finite(direction) ||
        weapon.Speed <= 0.0f || weapon.Lifespan <= 0.0f) {
        error = "projectile launch is invalid";
        return NativeCombatStatus::InvalidInput;
    }
    const float magnitude = std::sqrt(direction[0]*direction[0] + direction[1]*direction[1] + direction[2]*direction[2]);
    if (!std::isfinite(magnitude) || magnitude <= 0.0f) { error = "projectile direction is invalid"; return NativeCombatStatus::InvalidInput; }
    NativeProjectileState next;
    next.Sequence = ++m_ProjectileSequence;
    next.Weapon = weapon;
    next.Position = position;
    for (std::size_t axis = 0; axis < 3; ++axis) next.Velocity[axis] = direction[axis] / magnitude * weapon.Speed;
    next.Remaining = weapon.Lifespan;
    next.Active = true;
    out = std::move(next);
    error.clear();
    return NativeCombatStatus::Ok;
}

NativeCombatStatus NativeCombatRuntime::AdvanceProjectile(NativeProjectileState& projectile,
    float timeStep, std::string& error) const {
    if (!projectile.Active || !std::isfinite(timeStep) || timeStep < 0.0f) {
        error = "projectile advance is invalid";
        return NativeCombatStatus::InvalidInput;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) projectile.Position[axis] += projectile.Velocity[axis] * timeStep;
    projectile.Remaining = std::max(0.0f, projectile.Remaining - timeStep);
    projectile.Active = projectile.Remaining > 0.0f;
    if (!Finite(projectile.Position)) { error = "projectile position overflow"; return NativeCombatStatus::Overflow; }
    error.clear();
    return NativeCombatStatus::Ok;
}

NativeCombatStatus NativeCombatRuntime::Impact(NativeProjectileState& projectile,
    NativeCombatRef target, std::string& error) {
    if (!projectile.Active) { error = "projectile is not active"; return NativeCombatStatus::InvalidInput; }
    const auto status = Hit(target, projectile.Weapon, 1.0f, error);
    if (status == NativeCombatStatus::Ok) projectile.Active = false;
    return status;
}

NativeCombatStatus NativeCombatRuntime::Ignite(NativeCombatRef reference,
    const NativeWeaponDefinition& weapon, std::string& error) {
    auto* entity = ResolveMutable(reference);
    if (!entity) { error = "combat entity reference is stale"; return NativeCombatStatus::StaleReference; }
    if (weapon.Class != NativeWeaponClass::AreaEffect) { error = "weapon is not an area fire source"; return NativeCombatStatus::Unsupported; }
    entity->Burning = true;
    error.clear();
    return NativeCombatStatus::Ok;
}

NativeCombatStatus NativeCombatRuntime::AdvanceFire(NativeCombatRef reference,
    const NativeWeaponDefinition& weapon, float timeStep, std::string& error) {
    auto* entity = ResolveMutable(reference);
    if (!entity) { error = "combat entity reference is stale"; return NativeCombatStatus::StaleReference; }
    if (!entity->Burning || weapon.Class != NativeWeaponClass::AreaEffect || !std::isfinite(timeStep) || timeStep < 0.0f) {
        error = "fire advance is invalid";
        return NativeCombatStatus::InvalidInput;
    }
    return ApplyDamage(*entity, float(weapon.Damage) * timeStep, error);
}
