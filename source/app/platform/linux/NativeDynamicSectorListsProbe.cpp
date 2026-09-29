#include "NativeDynamicSectorLists.h"
#include "NativeWorldGround.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int s_Checks{};
void Check(bool value, const char* message) {
    ++s_Checks;
    if (!value) {
        std::fprintf(stderr, "dynamic-sector-fail: %s\n", message);
        std::exit(1);
    }
}

NativeLiveEntityBound Entity(NativeLiveEntityKind kind, int ref,
    NativeCollisionVector position, float radius = 2.0f) {
    NativeLiveEntityBound entity;
    entity.Kind = kind;
    entity.Reference = ref;
    entity.ModelId = kind == NativeLiveEntityKind::Vehicle ? 400 : 7;
    entity.Transform.Position = position;
    entity.Transform.Basis = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    entity.Model = {NativeLiveBoundsKnowledge::SourceCol, {}, {-1, -1, -1}, {1, 1, 1}, radius};
    entity.WorldCenter = position;
    return entity;
}

NativeDynamicEntityKey Key(const NativeLiveEntityBound& entity) {
    return {entity.Kind, entity.Reference};
}

NativeLiveModelBounds Candidate() {
    return {NativeLiveBoundsKnowledge::SourceCol, {}, {-1, -1, -1}, {1, 1, 1}, 2};
}

NativeDynamicSectorQuery Query(const NativeDynamicSectorLists& lists,
    NativeCollisionVector position = {25, 25, 0}) {
    NativeDynamicSectorQuery out;
    std::string error;
    Check(lists.Query(position, Candidate(), out, error) == NativeDynamicSectorStatus::Ok,
        error.c_str());
    return out;
}

void LiteralCases() {
    std::string error;
    NativeDynamicSectorLists lists;
    const auto car1 = Entity(NativeLiveEntityKind::Vehicle, 1, {25, 25, 0});
    auto car2 = Entity(NativeLiveEntityKind::Vehicle, 2, {25, 25, 0});
    const auto ped = Entity(NativeLiveEntityKind::MissionPed, 1, {25, 25, 0});
    Check(lists.Add(car1, error) == NativeDynamicSectorStatus::Ok, "first source add");
    Check(lists.Add(ped, error) == NativeDynamicSectorStatus::Ok, "ped add");
    Check(lists.Add(car2, error) == NativeDynamicSectorStatus::Ok, "second vehicle add");
    auto query = Query(lists);
    Check(query.Count == 3 && query.Selected[0] == Key(car2) &&
        query.Selected[1] == Key(car1) && query.Selected[2] == Key(ped),
        "head insertion and vehicle-before-ped order");
    Check(query.Blockage.Status == NativeLiveBlockageStatus::Blocked && query.Blockage.ZTests == 1,
        "first overlapping candidate blocks");
    const auto revision = lists.Revision();
    Check(lists.Add(car1, error) == NativeDynamicSectorStatus::DuplicateEntity &&
        lists.Revision() == revision, "duplicate cannot create extra world links");
    Check(lists.Reinsert(car1, error) == NativeDynamicSectorStatus::Ok, "unchanged reinsert");
    query = Query(lists);
    Check(query.Selected[0] == Key(car1) && query.Selected[1] == Key(car2),
        "RemoveAndAdd changes order even inside same sector");
    car2.Transform.Position = {825, 25, 100};
    car2.WorldCenter = car2.Transform.Position;
    Check(lists.Reinsert(car2, error) == NativeDynamicSectorStatus::Ok, "move through repeat alias");
    query = Query(lists);
    Check(query.Count == 2 && query.Selected[0] == Key(car1) && query.Selected[1] == Key(ped),
        "alias list retains spatial exclusion of far entity");
    Check(lists.Remove(Key(car1), error) == NativeDynamicSectorStatus::Ok, "remove exact old membership");
    Check(!lists.Resolve(Key(car1)) && Query(lists).Count == 1, "removed actor is not scanned");
    Check(lists.Remove(Key(car1), error) == NativeDynamicSectorStatus::StaleEntity,
        "second remove rejected");

    NativeDynamicSectorLists capped;
    auto ninth = Entity(NativeLiveEntityKind::Vehicle, 9, {25, 25, 0});
    Check(capped.Add(ninth, error) == NativeDynamicSectorStatus::Ok, "ninth blocker first added");
    for (int i = 1; i <= 8; ++i) {
        Check(capped.Add(Entity(NativeLiveEntityKind::Vehicle, i, {25, 25, 100}), error) ==
            NativeDynamicSectorStatus::Ok, "nonoverlapping Z candidate");
    }
    query = Query(capped);
    Check(query.Count == 8 && query.ScannedEntities == 8 &&
        query.Selected[0].Reference == 8 && query.Selected[7].Reference == 1 &&
        query.Blockage.Status == NativeLiveBlockageStatus::Clear && query.Blockage.ZTests == 8,
        "cap is applied BEFORE ninth Z blocker and before its scan mark");
    Check(capped.Reinsert(ninth, error) == NativeDynamicSectorStatus::Ok, "reinsert ninth at head");
    query = Query(capped);
    Check(query.Count == 8 && query.Selected[0] == Key(ninth) &&
        query.Blockage.Status == NativeLiveBlockageStatus::Blocked,
        "same actors with different source order changes capped result");

    NativeDynamicSectorLists edges;
    auto tangent = Entity(NativeLiveEntityKind::Vehicle, 1, {29, 25, 0});
    Check(edges.Add(tangent, error) == NativeDynamicSectorStatus::Ok, "XY tangent actor");
    Check(Query(edges).Count == 0, "circle equality is excluded");
    tangent.Transform.Position[0] = std::nextafter(29.0f, 25.0f);
    tangent.WorldCenter = tangent.Transform.Position;
    Check(edges.Reinsert(tangent, error) == NativeDynamicSectorStatus::Ok,
        "just inside strict circle");
    Check(Query(edges).Count == 1, "one ULP inside qualifies");
    tangent.Transform.Position = {25, 25, 3};
    tangent.WorldCenter = tangent.Transform.Position;
    Check(edges.Reinsert(tangent, error) == NativeDynamicSectorStatus::Ok,
        "Z tangent actor");
    Check(Query(edges).Blockage.Status == NativeLiveBlockageStatus::Clear,
        "source vertical inequalities remain strict");
    tangent.Transform.Position[2] = std::nextafter(3.0f, 0.0f);
    tangent.WorldCenter = tangent.Transform.Position;
    Check(edges.Reinsert(tangent, error) == NativeDynamicSectorStatus::Ok, "inside Z tangent");
    Check(Query(edges).Blockage.Status == NativeLiveBlockageStatus::Blocked, "inside Z blocks");

    NativeDynamicSectorLists duplicates;
    auto wide = Entity(NativeLiveEntityKind::MissionPed, 8, {50, 50, 100}, 60);
    wide.Model.Min = {-40, -40, -1};
    wide.Model.Max = {40, 40, 1};
    Check(duplicates.Add(wide, error) == NativeDynamicSectorStatus::Ok, "sphere-sector membership");
    NativeDynamicSectorQuery wideQuery;
    auto large = Candidate();
    large.Radius = 75;
    Check(duplicates.Query({50, 50, 0}, large, wideQuery, error) == NativeDynamicSectorStatus::Ok &&
        wideQuery.Count == 1 && wideQuery.ScannedEntities == 1,
        "scan code deduplicates overlapping sectors before distance test");
    Check(duplicates.Remove(Key(wide), error) == NativeDynamicSectorStatus::Ok &&
        duplicates.Query({50, 50, 0}, large, wideQuery, error) == NativeDynamicSectorStatus::Ok &&
        wideQuery.Count == 0, "all membership links are removed");

    NativeDynamicSectorLists negative;
    const auto outside = Entity(NativeLiveEntityKind::Vehicle, 15, {-3025, 25, 100});
    const auto inside = Entity(NativeLiveEntityKind::Vehicle, 11, {-2425, 25, 100});
    Check(negative.Add(outside, error) == NativeDynamicSectorStatus::Ok,
        "physical Add keeps negative grid without base-entity clamp");
    Check(negative.Add(inside, error) == NativeDynamicSectorStatus::Ok, "repeat column eleven");
    auto hugeQuery = Candidate();
    hugeQuery.Radius = 850;
    Check(negative.Query({-2500, 25, 0}, hugeQuery, query, error) == NativeDynamicSectorStatus::Ok &&
        query.Count == 2 && query.Selected[0] == Key(inside) && query.Selected[1] == Key(outside),
        "negative source traversal uses original low bits, not abs mapping");

    NativeDynamicSectorLists aliases;
    auto repeated = Entity(NativeLiveEntityKind::Vehicle, 9, {50, 50, 100}, 450);
    Check(aliases.Add(repeated, error) == NativeDynamicSectorStatus::Ok && aliases.LinkCount() == 361,
        "physical sphere links intentionally alias the sixteen-repeat grid");
    Check(aliases.Query({50, 50, 0}, hugeQuery, query, error) == NativeDynamicSectorStatus::Ok &&
        query.Count == 1 && query.ScannedEntities == 1, "alias links deduplicate by entity scan");
    const auto linkedRevision = aliases.Revision();
    auto excessive = repeated;
    excessive.Model.Radius = 550;
    Check(aliases.Reinsert(excessive, error) == NativeDynamicSectorStatus::CapacityExceeded &&
        aliases.Revision() == linkedRevision && aliases.LinkCount() == 361,
        "source shared link-pool upper bound is not expanded");
    Check(aliases.Remove(Key(repeated), error) == NativeDynamicSectorStatus::Ok &&
        aliases.LinkCount() == 0 && aliases.Size() == 0, "remove all repeated alias links");

    NativeDynamicSectorLists spill;
    const auto broad = Entity(NativeLiveEntityKind::Vehicle, 4, {-1350.25f, 25, 100}, 1);
    const auto tiny = Entity(NativeLiveEntityKind::Vehicle, 5, {-1350.0001220703125f, 25, 0}, 0.00000001f);
    Check(spill.Add(broad, error) == NativeDynamicSectorStatus::Ok &&
        spill.Add(tiny, error) == NativeDynamicSectorStatus::Ok, "original FPU sector boundary actors");
    auto narrow = Candidate();
    narrow.Radius = 0.0002f;
    Check(spill.Query({-1350, 25, 0}, narrow, query, error) == NativeDynamicSectorStatus::Ok &&
        query.Count == 2 && query.Selected[0] == Key(tiny) && query.Selected[1] == Key(broad),
        "double division/add with binary32 spill preserves first visited sector");

    const auto held = Query(capped);
    auto invalid = ninth;
    invalid.Transform.Position[0] = std::numeric_limits<float>::infinity();
    const auto before = capped.Revision();
    Check(capped.Reinsert(invalid, error) == NativeDynamicSectorStatus::InvalidInput &&
        capped.Revision() == before && Query(capped) == held, "invalid reinsert retains links/order/value");
    auto retained = held;
    auto unknown = Candidate();
    unknown.Knowledge = NativeLiveBoundsKnowledge::Unknown;
    Check(capped.Query({25, 25, 0}, unknown, retained, error) == NativeDynamicSectorStatus::InvalidInput &&
        retained == held, "missing model is not a source null fallback");
    unknown.Knowledge = NativeLiveBoundsKnowledge::ProvenSourceNull;
    Check(capped.Query({25, 25, 0}, unknown, retained, error) == NativeDynamicSectorStatus::Ok &&
        retained.Blockage.Status == NativeLiveBlockageStatus::Blocked, "proven source null radius2 Zplusminus1");
    Check(!NativeDynamicSectorLists::SourcePopulationComplete,
        "sector replay does not certify ambient population");

    NativeDynamicSectorLists capacity;
    for (int i = 0; i < 110; ++i) {
        Check(capacity.Add(Entity(NativeLiveEntityKind::Vehicle, i, {25, 25, 100}), error) ==
            NativeDynamicSectorStatus::Ok, "source vehicle capacity slot");
    }
    const auto vehicleRevision = capacity.Revision();
    Check(capacity.Add(Entity(NativeLiveEntityKind::Vehicle, 110, {25, 25, 0}), error) ==
        NativeDynamicSectorStatus::CapacityExceeded && capacity.Size() == 110 &&
        capacity.Revision() == vehicleRevision, "vehicle overflow retains linked census");
    for (int i = 0; i < 140; ++i) {
        Check(capacity.Add(Entity(i == 0 ? NativeLiveEntityKind::Player : NativeLiveEntityKind::MissionPed,
            i, {25, 25, 0}), error) == NativeDynamicSectorStatus::Ok, "ped capacity includes player");
    }
    const auto pedRevision = capacity.Revision();
    Check(capacity.Add(Entity(NativeLiveEntityKind::MissionPed, 140, {25, 25, 0}), error) ==
        NativeDynamicSectorStatus::CapacityExceeded && capacity.Size() == 250 &&
        capacity.Revision() == pedRevision, "ped overflow retains shared player/ped capacity");
    auto outsideMap = Entity(NativeLiveEntityKind::Vehicle, 55, {4000, 25, 0});
    Check(capacity.Reinsert(outsideMap, error) == NativeDynamicSectorStatus::Ok &&
        capacity.Revision() == pedRevision + 1, "physical repeat links remain valid outside static-map clamp");
}
}

#ifdef NATIVE_DYNAMIC_SECTOR_SOURCE_ORACLE
#include "NativeDynamicSectorSourceOracle.inc"
#endif

int main() {
    LiteralCases();
#ifdef NATIVE_DYNAMIC_SECTOR_SOURCE_ORACLE
    SourceOracleCases();
#endif
    std::printf("native-dynamic-sectors-ok checks=%d repeats=16x16 cap=8 order=source-events population-complete=0\n",
        s_Checks);
}
