#include "app/platform/linux/NativeGaragesRuntime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "vehicle-pool: %s\n", message.c_str());
        std::exit(2);
    }
}

struct PoolFixture {
    NativeVehiclePool Pool;
    std::uint64_t Frame = 0;

    PoolFixture() {
        std::string error;
        Require(Pool.BindProducer(NativeVehicleProducer::NativeScm, error), error);
        Require(Pool.BindProducer(NativeVehicleProducer::NativeGameplayController, error), error);
        Require(Pool.SealProducerExtent(error), error);
    }

    std::shared_ptr<const NativeVehiclePoolSnapshot> Publish() {
        std::string error;
        auto snapshot = Pool.Publish(Frame++, error);
        Require(bool(snapshot), error);
        return snapshot;
    }
};

NativeGarageEntry Garage() {
    NativeGarageEntry garage;
    garage.Origin = {0.0f, 0.0f, 0.0f};
    garage.DirectionA = {1.0f, 0.0f};
    garage.DirectionB = {0.0f, 1.0f};
    garage.Width = garage.Height = 10.0f;
    garage.Top = 10.0f;
    garage.Rect = {0.0f, 10.0f, 0.0f, 10.0f};
    return garage;
}

NativeVehicleState State(NativeVehicleType type = NativeVehicleType::Automobile) {
    NativeVehicleState state;
    state.ModelId = 400;
    state.Type = type;
    state.SubType = 0;
    state.Status = NativeVehicleStatus::Physics;
    state.CreatedBy = NativeVehicleCreatedBy::Random;
    state.Matrix.Position = {5.0f, 5.0f, 5.0f};
    return state;
}

NativeVehicleRef Allocate(NativeVehiclePool& pool, const NativeVehicleState& state,
    NativeVehicleProducer producer = NativeVehicleProducer::NativeScm) {
    const auto result = pool.Allocate({.Producer = producer, .ScriptRequest = {}, .ProducerIndex = -1, .State = state});
    Require(result.Result.Status == NativeScriptServiceStatus::Ready, result.Result.Message);
    return result.Reference;
}

NativeGarageTidyPlan Plan(const NativeGarageEntry& garage, const NativeVehiclePoolSnapshot& snapshot) {
    return NativeGaragesRuntime::PlanTidy(garage, {71, 1}, snapshot.Frame(), false, snapshot);
}

void ProducerAndIdentityTests() {
    std::string error;
    NativeVehiclePool incomplete;
    Require(!incomplete.SealProducerExtent(error), "empty producer extent must not seal");
    Require(incomplete.BindProducer(NativeVehicleProducer::NativeScm, error), error);
    Require(!incomplete.BindProducer(NativeVehicleProducer::NativeScm, error), "duplicate producer bind must fail");
    auto incompleteSnapshot = incomplete.Publish(0, error);
    Require(incompleteSnapshot && !incompleteSnapshot->Census().Producers.NativeHostComplete &&
        !incompleteSnapshot->Census().Producers.SourceParityComplete,
        "unsealed publication must remain explicitly incomplete");
    const auto incompletePlan = Plan(Garage(), *incompleteSnapshot);
    Require(incompletePlan.Status == NativeScriptServiceStatus::Error &&
        incompletePlan.Requirement == NativeGarageTidyRequirement::VehicleAuthority,
        "planner cannot promote an incomplete empty snapshot to Ready");
    Require(!incomplete.SealProducerExtent(error), "published producer extent must be immutable");

    PoolFixture fixture;
    const auto initial = fixture.Publish();
    Require(initial->Owner() == fixture.Pool.Owner() && initial->Generation() == 1 && initial->Frame() == 0,
        "first immutable publication identity");
    Require(initial->Census().Alive == 0 && initial->Census().CreatedEvents == 0 &&
        initial->Census().Producers.NativeHostComplete && !initial->Census().Producers.SourceParityComplete,
        "sealed native-empty census has bounded claim scope");

    const auto later = fixture.Pool.Publish(2, error);
    Require(later && later->Generation() == 2, error);
    const auto generation = fixture.Pool.PublicationGeneration();
    Require(!fixture.Pool.Publish(2, error) && fixture.Pool.PublicationGeneration() == generation,
        "duplicate snapshot frame fails transactionally");
    Require(!fixture.Pool.Publish(1, error) && fixture.Pool.PublicationGeneration() == generation,
        "stale snapshot frame fails transactionally");
}

void TransactionTests() {
    PoolFixture fixture;
    std::string error;
    auto invalid = State();
    invalid.Matrix.Position[0] = std::numeric_limits<float>::quiet_NaN();
    const auto failed = fixture.Pool.Allocate({.Producer = NativeVehicleProducer::NativeScm, .ScriptRequest = {}, .ProducerIndex = -1, .State = invalid});
    Require(failed.Result.Status == NativeScriptServiceStatus::Error && fixture.Pool.Revision() == 0 && fixture.Pool.Events().empty(),
        "failed allocation leaves revision and journal unchanged");
    const auto unbound = fixture.Pool.Allocate({.Producer = NativeVehicleProducer::RandomTraffic, .ScriptRequest = {}, .ProducerIndex = -1, .State = State()});
    Require(unbound.Result.Status == NativeScriptServiceStatus::Error && fixture.Pool.Revision() == 0,
        "unbound producer cannot create an authoritative record");

    const auto first = Allocate(fixture.Pool, State());
    Require(first.Value == 1 && fixture.Pool.AtSlot(0) == fixture.Pool.Resolve(first),
        "first source slot and seven-bit generation");
    const auto before = fixture.Publish();
    auto moved = State();
    moved.Matrix.Position[0] = 7.0f;
    Require(fixture.Pool.Update(first, moved, error), error);
    const auto after = fixture.Publish();
    Require(before->AtSlot(0)->State.Matrix.Position[0] == 5.0f && after->AtSlot(0)->State.Matrix.Position[0] == 7.0f &&
        before->Revision() + 1 == after->Revision(), "snapshot is immutable across a real update");

    const auto revision = fixture.Pool.Revision();
    const auto events = fixture.Pool.Events().size();
    invalid = moved;
    invalid.Matrix.Basis[2][2] = std::numeric_limits<float>::infinity();
    Require(!fixture.Pool.Update(first, invalid, error) && fixture.Pool.Revision() == revision &&
        fixture.Pool.Events().size() == events && fixture.Pool.Resolve(first)->State == moved,
        "failed update is atomic");

    Require(fixture.Pool.Release(first, error) && !fixture.Pool.Resolve(first), error);
    const auto second = Allocate(fixture.Pool, State(NativeVehicleType::Bike));
    Require(second.Value == 2 && !fixture.Pool.Resolve(first) && fixture.Pool.Resolve(second),
        "released source slot is reused with a new generation and stale ref rejected");
    const auto staleRevision = fixture.Pool.Revision();
    const auto staleEvents = fixture.Pool.Events().size();
    Require(!fixture.Pool.Update(first, moved, error) && !fixture.Pool.Release(first, error) &&
        fixture.Pool.Revision() == staleRevision && fixture.Pool.Events().size() == staleEvents,
        "stale update/release cannot mutate the reused slot");
    const auto census = fixture.Pool.Census();
    Require(census.Alive == 1 && census.CreatedEvents == 2 && census.UpdatedEvents == 1 && census.ReleasedEvents == 1,
        "typed lifecycle census");
}

void PlannerTests() {
    PoolFixture fixture;
    const auto garage = Garage();
    std::string error;
    auto candidate = State();
    candidate.Status = NativeVehicleStatus::Wrecked;
    const auto slot0 = Allocate(fixture.Pool, candidate);
    const auto slot1 = Allocate(fixture.Pool, State());
    Require(slot0.Value >> 8 == 0 && slot1.Value >> 8 == 1, "source slot fixture allocation");

    auto snapshot = fixture.Publish();
    auto plan = Plan(garage, *snapshot);
    Require(plan.Status == NativeScriptServiceStatus::Ready && plan.Examined == 109 && plan.Candidates.empty() &&
        plan.FirstSlot == 109 && plan.LastSlot == 1 && plan.Destroyed == 0,
        "slot zero is excluded from the exact 109..1 scan");

    const auto check = [&](NativeVehicleState state, bool expected, const char* message) {
        Require(fixture.Pool.Update(slot1, state, error), error);
        const auto current = fixture.Publish();
        const auto currentPlan = Plan(garage, *current);
        Require((currentPlan.Candidates.size() == 1) == expected, message);
        return currentPlan;
    };

    auto state = candidate;
    state.Type = NativeVehicleType::Boat;
    check(state, false, "non-automobile/bike type excluded");
    state.Type = NativeVehicleType::Automobile;
    state.Matrix.Position = {0.0f, 10.0f, 10.0f};
    check(state, true, "vehicle center on inclusive garage boundary");
    state.Matrix.Position[0] = std::nextafter(0.0f, -1.0f);
    check(state, false, "vehicle center one float outside garage boundary");
    state = State(NativeVehicleType::Bike);
    state.Status = NativeVehicleStatus::Wrecked;
    check(state, true, "bike kind and wrecked status selected");
    state = State();
    state.Matrix.Basis[2][2] = 0.5f;
    check(state, false, "up Z exactly one half retained");
    state.Matrix.Basis[2][2] = std::nextafter(0.5f, 0.0f);
    auto overturned = check(state, true, "up Z one float below one half selected");
    Require(overturned.Candidates[0].Reason == NativeGarageTidyReason::UpVectorBelowHalf,
        "overturned candidate has typed reason");
    state.Matrix.Basis[2][2] = 1.0f;
    state.Status = NativeVehicleStatus::Wrecked;
    auto wrecked = check(state, true, "wrecked upright automobile selected");
    Require(wrecked.Candidates[0].Reason == NativeGarageTidyReason::Wrecked,
        "wrecked candidate has typed reason");

    state = State();
    state.Matrix.Basis[2][2] = 0.0f;
    state.CreatedBy = NativeVehicleCreatedBy::Mission;
    state.MissionCleanupRegistered = true;
    state.ScriptLocked = true;
    const auto mission = check(state, true, "mission ownership does not protect a tidy candidate");
    Require(mission.Candidates[0].CreatedBy == NativeVehicleCreatedBy::Mission &&
        mission.Candidates[0].MissionCleanupRegistered && mission.Candidates[0].ScriptLocked,
        "mission ownership facts survive in the destruction plan");

    const auto nonCandidate = State(NativeVehicleType::Boat);
    for (std::size_t slot = 2; slot < NativeVehiclePool::Capacity - 1; ++slot) {
        const auto reference = Allocate(fixture.Pool, nonCandidate);
        Require(static_cast<std::size_t>(reference.Value >> 8) == slot, "contiguous source-index allocation");
    }
    auto high = State(NativeVehicleType::Bike);
    high.Status = NativeVehicleStatus::Wrecked;
    const auto slot109 = Allocate(fixture.Pool, high);
    Require(slot109.Value >> 8 == 109, "highest source vehicle slot fixture");
    snapshot = fixture.Publish();
    const auto revision = fixture.Pool.Revision();
    const auto events = fixture.Pool.Events().size();
    const auto reverse = Plan(garage, *snapshot);
    Require(reverse.Candidates.size() == 2 && reverse.Candidates[0].SourceSlot == 109 &&
        reverse.Candidates[1].SourceSlot == 1 && reverse.Status == NativeScriptServiceStatus::Unsupported &&
        reverse.Requirement == NativeGarageTidyRequirement::VehicleDestruction && reverse.Destroyed == 0,
        "candidate plan preserves reverse source-slot order without destruction");
    Require(fixture.Pool.Revision() == revision && fixture.Pool.Events().size() == events &&
        fixture.Pool.Census().Alive == NativeVehiclePool::Capacity &&
        fixture.Pool.Allocate({.Producer = NativeVehicleProducer::NativeScm, .ScriptRequest = {}, .ProducerIndex = -1, .State = State()}).Result.Status == NativeScriptServiceStatus::Error &&
        fixture.Pool.Revision() == revision && fixture.Pool.Events().size() == events,
        "capacity failure and planning leave the authoritative pool unchanged");
    const auto near = NativeGaragesRuntime::PlanTidy(garage, {71, 1}, snapshot->Frame(), true, *snapshot);
    Require(near.Status == NativeScriptServiceStatus::Unsupported && near.Examined == 0 &&
        near.Requirement == NativeGarageTidyRequirement::NearVehicleCollision,
        "near tidy remains a typed source-COL dependency");
}
} // namespace

int main() {
    ProducerAndIdentityTests();
    TransactionTests();
    PlannerTests();
    std::printf("NativeVehiclePoolProbe PASS capacity110 refs7bit slot0excluded reverse109to1 farType0or9 center wreckedOrUpZltHalf missionUnprotected\n");
}
