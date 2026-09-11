#include "NativeWorldGround.h"
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace {
using V = NativeCollisionVector;
using Known = NativeSourceGroundKnown;
using Class = NativeSourceGroundClass;
using Issue = NativeWorldGroundIssue;
using Key = std::tuple<std::string, uint32_t, bool>;
static Key IdentityKey(const NativeCollisionPlacement& p) { return {p.Ipl, p.Record, p.Binary}; }
static bool Finite(V v) { return std::ranges::all_of(v, [](float f) { return std::isfinite(f); }); }
static V Point(const NativeSourceGroundTransform& t, V p) {
    V out{};
    for (size_t j = 0; j < 3; ++j)
        out[j] = ((t.Basis[0][j]*p[0] + t.Basis[1][j]*p[1]) + t.Basis[2][j]*p[2]) + t.Position[j];
    return out;
}
static NativeWorldGroundDiagnostic Diagnostic(Issue issue, const NativePlacementIdentity& id, const char* reason) {
    return {issue, id, reason};
}
}

int NativeWorldGround::Sector(float coordinate) {
    // World.h:GetSectorfX/GetSectorX: division BEFORE adding half the map.
    return static_cast<int>(std::floor(coordinate / 50.0f + 60.0f));
}

bool NativeWorldGround::SourceTransform(const NativeCollisionPlacement& p, NativeSourceGroundTransform& t) {
    if (!Finite(p.Position) || !std::ranges::all_of(p.Quaternion, [](float f) { return std::isfinite(f); })) return false;
    // FileLoader.cpp:1036-1052; unlike NativeCollisionAssets::Basis, NO normalize.
    const auto& q = p.Quaternion;
    t.Position = p.Position;
    if (std::abs(q[0]) > 0.05f || std::abs(q[1]) > 0.05f || ((p.Flags & 512u) && q[0] != 0 && q[1] != 0)) {
        const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
        const float xx = (x+x)*x, yx = (y+y)*x, zx = (z+z)*x;
        const float yy = (y+y)*y, zy = (z+z)*y, zz = (z+z)*z;
        const float xw = (x+x)*w, yw = (y+y)*w, zw = (z+z)*w;
        t.Basis = {V{1.0f-(zz+yy), zw+yx, zx-yw}, V{yx-zw, 1.0f-(zz+xx), xw+zy}, V{yw+zx, zy-xw, 1.0f-(yy+xx)}};
    } else {
        if (q[3] < -1.0f || q[3] > 1.0f) return false;
        const auto heading = std::acos(q[3]) * (q[2] < 0.0f ? 2.0f : -2.0f);
        // Matrix.cpp:SetRotateZOnly uses unqualified double sin/cos, then float.
        const float c = std::cos(static_cast<double>(heading)), s = std::sin(static_cast<double>(heading));
        t.Basis = {V{c,s,0}, V{-s,c,0}, V{0,0,1}};
    }
    // FileLoader's train-crossing special transform needs ModelIndices authority.
    // Identity is model-name provenance, not a geometry/name classification rule.
    if (p.Model == "traincross2") return false; // ModelIndices.cpp:476
    return std::ranges::all_of(t.Basis, Finite);
}

std::array<float, 4> NativeWorldGround::SourceRect(const NativeCollisionModel& m, const NativeSourceGroundTransform& t,
    const NativeCollisionPlacement* initialPlacement) {
    auto a = m.Min, b = m.Max;
    std::array<float, 4> rect{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    const auto stretch = [&](V v) {
        auto p = Point(t,v);
        // Before GetMatrix allocation, GetBoundRect -> TransformFromObjectSpace
        // uses common.cpp:38's simple transform (float trig and no zero terms).
        if (initialPlacement) {
            const auto& q = initialPlacement->Quaternion;
            if (!(std::abs(q[0]) > 0.05f || std::abs(q[1]) > 0.05f || ((initialPlacement->Flags & 512u) && q[0] != 0 && q[1] != 0))) {
                const auto heading = std::acos(q[3]) * (q[2] < 0 ? 2.0f : -2.0f);
                const auto c = std::cos(heading), s = std::sin(heading);
                p = {c*v[0]-s*v[1]+t.Position[0], s*v[0]+c*v[1]+t.Position[1], v[2]+t.Position[2]};
            }
        }
        rect[0] = std::min(rect[0],p[0]); rect[1] = std::min(rect[1],p[1]);
        rect[2] = std::max(rect[2],p[0]); rect[3] = std::max(rect[3],p[1]);
    };
    stretch(a); stretch(b); std::swap(a[0],b[0]); stretch(a); stretch(b);
    return rect;
}

std::shared_ptr<const NativeWorldGround> NativeWorldGround::Prepare(
    const NativeCollisionContext& context, const NativeWorldEntityInfo& metadata, std::string& error, uint64_t metadataRevision) {
    NativeCollisionSnapshot all;
    // Full outdoor census: cannot window unresolved geometry by IPL origin.
    // No overrides here: disabled doors must still have a constructor binding.
    if (!context.Assets.Snapshot(context.Population,0,0,std::numeric_limits<float>::max(),all,error)) return {};
    auto next = std::const_pointer_cast<NativeWorldGround>(PreparePopulation(context.Population,metadata,all,error,metadataRevision));
    if (!next) return {};
    // This inference is confined to our own unwindowed, override-free catalog
    // census, NEVER an imported or committed window's MissingModels counter.
    for (auto& e : next->m_Entities) {
        if (e.Target.Model || e.BindingMayChange || e.Target.InWorld != Known::Yes || !e.Target.VerifiedClassification) continue;
        auto name = e.Placement.Model;
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
        if (!all.KnownAbsence.contains(name)) {
            e.Diagnostic = Diagnostic(Issue::CollisionBinding,e.Target.Identity,
                "full catalog proves primitive-empty COL omitted by Assets::Snapshot; original header/bounds unavailable (V2+ HasCollisionVolumes is flags&2, NOT primitive count)");
        } else {
            e.Diagnostic = Diagnostic(Issue::CollisionBinding,e.Target.Identity,
                "full COL catalog absence; objs/tobj/anim Init starts null, hier LoadClumpObject alone binds TempColBBox; GetBoundRect/GetColModel dereference forbids inventing normal-sector null-model membership");
        }
    }
    return next;
}

std::shared_ptr<const NativeWorldGround> NativeWorldGround::PreparePopulation(
    const NativeCollisionPopulation& population, const NativeWorldEntityInfo& metadata,
    const NativeCollisionSnapshot& bindings, std::string& error, uint64_t metadataRevision) {
    auto next = std::make_shared<NativeWorldGround>();
    next->m_CompletePopulation = population.IncludesStreamed;
    next->m_MetadataRevision = metadataRevision;
    std::map<Key, const NativeCollisionInstance*> models;
    for (const auto& i : bindings.Instances) {
        if (!models.emplace(IdentityKey(i.Placement), &i).second) { error = "duplicate COL binding identity"; return {}; }
    }
    std::set<Key> seen;
    // Without CIplStore's staticIdx arrays, any text record matching an authored
    // LOD ordinal is a possible parent. Model-wide LinkLods::SetColModel can also
    // affect binary placements sharing that model/COL. Conservative superset;
    // never spatially cull such an unresolved candidate by its pre-link bounds.
    std::set<uint32_t> lodOrdinals;
    std::set<int> possibleLodModels;
    std::set<const NativeCollisionModel*> possibleLodBindings;
    for (const auto& p : population.Instances) if (p.Lod >= 0) lodOrdinals.insert(static_cast<uint32_t>(p.Lod));
    for (const auto& p : population.Instances) if (!p.Binary && lodOrdinals.contains(p.Record)) {
        possibleLodModels.insert(p.ModelId);
        if (const auto it = models.find(IdentityKey(p)); it != models.end() && it->second->Model)
            possibleLodBindings.insert(it->second->Model.get());
    }
    for (const auto& p : population.Instances) {
        if (!seen.insert(IdentityKey(p)).second) { error = "duplicate population source/record/binary identity"; return {}; }
        Entity e; e.Placement = p;
        auto& t = e.Target;
        t.Identity = NativePlacementIdentity::From(p);
        const auto info = metadata.Query(p);
        NativeCollisionInstance initial; initial.Placement = p;
        if (const auto it = models.find(IdentityKey(p)); it != models.end()) {
            if (!t.Identity.Matches(it->second->Placement)) { error = "COL binding model identity mismatch"; return {}; }
            initial.Model = it->second->Model;
        }
        t = NativeSourceGround::BindInitialMetadata(initial,info,true);
        if (info.Model) e.DrawDistance = info.Model->DrawDistance;
        t.SourceEffectiveTransformKnown = SourceTransform(p,t.Transform);
        t.CollisionModelKnown = t.Model && t.Model->Unsupported.empty() && t.Model->Version >= 1 && t.Model->Version <= 4;
        e.BindingMayChange = possibleLodModels.contains(p.ModelId) || (t.Model && possibleLodBindings.contains(t.Model.get()));
        if (e.BindingMayChange) t.CollisionModelKnown = false;
        // Native initial-world definition: outdoors only, no converted dummies,
        // ignored entity or mutable class changes. Source line code has NO area
        // test; exclusion is world membership, never a query-time area shortcut.
        t.InWorld = info.Placement && info.Placement->Area ? (*info.Placement->Area == 0 ? Known::Yes : Known::No) : Known::Unknown;
        t.Ignored = Known::No;
        // CEntity zeroes flags; CBuilding sets UsesCollision. LoadObjectInstance
        // only disables it when an actual cm has !m_bHasCollisionVolumes.
        const auto hasVolumes = t.Model && (t.Model->Version == 1 ?
            (!t.Model->Spheres.empty() || !t.Model->Boxes.empty() || !t.Model->Faces.empty()) : (t.Model->Flags & 2u) != 0);
        const auto sourceHeaderKnown = t.Model && t.Model->Unsupported.empty() && t.Model->Version >= 1 && t.Model->Version <= 4;
        t.UsesCollision = sourceHeaderKnown ? (hasVolumes ? Known::Yes : Known::No) : Known::Unknown;
        t.BigBuilding = p.Binary ? Known::No : Known::Unknown;
        t.NormalSector = p.Binary ? Known::Yes : Known::Unknown;
        // LinkLods is deliberately not guessed from names, bounds, Lod==-1 or
        // draw distance alone. Inbound children matter even for Lod==-1 parents.
        if (!t.VerifiedClassification)
            e.Diagnostic = Diagnostic(Issue::Metadata,t.Identity,"CFileLoader::LoadObjectInstance initial Object.dat class/type word unavailable");
        else if (!t.SourceEffectiveTransformKnown)
            e.Diagnostic = Diagnostic(Issue::Transform,t.Identity,"CFileLoader::LoadObjectInstance effective transform unresolved (including train crossing special case)");
        else if (e.BindingMayChange)
            e.Diagnostic = Diagnostic(Issue::TextLodState,t.Identity,"possible text LOD parent's model-wide LinkLods::SetColModel binding unresolved; pre-link bounds cannot exclude candidate");
        else if (!t.CollisionModelKnown)
            e.Diagnostic = Diagnostic(Issue::CollisionBinding,t.Identity,"COL census has no nonempty binding: BaseModelInfo::Init null versus clump TempCol/empty/LOD rebinding unresolved; origin cannot bound candidate");
        next->m_Entities.push_back(std::move(e));
    }
    if (models.size() > seen.size() || std::ranges::any_of(models, [&](const auto& item) { return !seen.contains(item.first); })) {
        error = "COL binding outside full population"; return {};
    }
    error.clear(); return next;
}

NativeWorldGroundPublication NativeWorldGround::Publish(const NativeWorldGroundCommit& commit, float x, float y) const {
    NativeWorldGroundPublication publication;
    auto snapshot = std::make_shared<NativeSourceGroundSnapshot>();
    publication.Snapshot = snapshot;
    publication.SourceCollision = commit.SourceCollision;
    snapshot->WorldGeneration = commit.WorldGeneration; snapshot->MetadataRevision = commit.MetadataRevision;
    const auto fail = [&](Issue issue, const char* why) { publication.Diagnostics.push_back(Diagnostic(issue,{},why)); return publication; };
    if (commit.MetadataRevision != m_MetadataRevision)
        return fail(Issue::StaleCommit,"metadata revision differs from immutable prepared population metadata");
    if (!commit.SourceCollision || !std::isfinite(x) || !std::isfinite(y) || x < -3000 || x >= 3000 || y < -3000 || y >= 3000 ||
        !std::isfinite(commit.X) || !std::isfinite(commit.Y) || !std::isfinite(commit.Radius) || commit.Radius <= 0 ||
        (commit.LodDistanceMultiplier && (!std::isfinite(*commit.LodDistanceMultiplier) || *commit.LodDistanceMultiplier <= 0)))
        return fail(Issue::InvalidCommit,"finite committed source square and in-world sector required");
    const int sx = Sector(x), sy = Sector(y);
    snapshot->MinXY = {float(sx-60)*50,float(sy-60)*50};
    snapshot->MaxXY = {snapshot->MinXY[0]+50,snapshot->MinXY[1]+50};
    if (snapshot->MinXY[0] < commit.X-commit.Radius || snapshot->MaxXY[0] > commit.X+commit.Radius ||
        snapshot->MinXY[1] < commit.Y-commit.Radius || snapshot->MaxXY[1] > commit.Y+commit.Radius)
        return fail(Issue::CommittedGeometry,"whole source 50m sector is outside committed COL square");
    if (!m_CompletePopulation) return fail(Issue::PopulationIncomplete,"full StreamPager text/binary population provenance missing");
    if (commit.SourceCollision->Overrides) for (const auto& entry : commit.SourceCollision->Overrides->Entries()) {
        if (!Finite(entry.Position) || !std::ranges::all_of(entry.Basis,Finite) ||
            !std::ranges::any_of(m_Entities,[&](const auto& e) { return entry.Identity.Matches(e.Placement); }))
            return fail(Issue::InvalidCommit,"nonfinite or orphan committed override identity");
    }
    std::map<Key, const NativeCollisionInstance*> committed;
    for (const auto& i : commit.SourceCollision->Instances) {
        if (!committed.emplace(IdentityKey(i.Placement), &i).second)
            return fail(Issue::DuplicateIdentity,"duplicate committed COL candidate");
    }
    // Enumeration is not claimed to be retail insertion order. One identity per
    // single-sector list performs the source scan-code dedup structurally.
    for (size_t index = m_Entities.size(); index-- > 0;) {
        const auto& e = m_Entities[index]; auto t = e.Target; auto diagnostic = e.Diagnostic;
        if (t.VerifiedClassification && t.EffectiveClass == Class::Other) continue;
        if (t.InWorld == Known::No) continue;
        const auto* override = commit.SourceCollision->Overrides ? commit.SourceCollision->Overrides->Find(e.Placement) : nullptr;
        if (override) {
            t.Transform = {override->Position,override->Basis}; t.SourceEffectiveTransformKnown = true;
            // An override can disable initial collision; it cannot undo source
            // empty-COL or SetupBigBuilding flags by merely saying enabled=true.
            if (!override->CollisionEnabled) t.UsesCollision = Known::No;
        }
        if (!e.Placement.Binary && commit.LodDistanceMultiplier && e.DrawDistance &&
            *commit.LodDistanceMultiplier * *e.DrawDistance > 300.0f) {
            t.BigBuilding = Known::Yes; t.NormalSector = Known::No; t.UsesCollision = Known::No;
        }
        if (t.UsesCollision == Known::No || t.BigBuilding == Known::Yes) continue;
        if (t.Model && t.Model->Unsupported.empty() && t.Model->Version >= 1 && t.Model->Version <= 4 &&
            !e.BindingMayChange && t.SourceEffectiveTransformKnown) {
            auto rect = SourceRect(*t.Model,t.Transform,override ? nullptr : &e.Placement);
            if (!std::ranges::all_of(rect, [](float f) { return std::isfinite(f); })) {
                t.SourceEffectiveTransformKnown = false;
                diagnostic = Diagnostic(Issue::Transform,t.Identity,"nonfinite source GetBoundRect");
            } else {
                // Entity::Add asymmetric clamps, inclusive sector maxima.
                rect[0] = std::max(rect[0],-3000.0f); rect[1] = std::max(rect[1],-3000.0f);
                if (rect[2] >= 3000) rect[2] = 2999;
                if (rect[3] >= 3000) rect[3] = 2999;
                if (rect[0] > rect[2] || rect[1] > rect[3] || rect[2] < -3000 || rect[3] < -3000 || rect[0] >= 3000 || rect[1] >= 3000) continue;
                if (sx < Sector(rect[0]) || sx > Sector(rect[2]) || sy < Sector(rect[1]) || sy > Sector(rect[3])) continue;
            }
        }
        if (!e.Placement.Binary && !e.BindingMayChange && commit.LodDistanceMultiplier && e.DrawDistance &&
            *commit.LodDistanceMultiplier * *e.DrawDistance <= 300.0f) {
            // No incoming ordinal even in the conservative all-file superset.
            t.BigBuilding = Known::No; t.NormalSector = Known::Yes;
        }
        if (diagnostic.Issue == Issue::None && t.BigBuilding == Known::Unknown)
            diagnostic = Diagnostic(Issue::TextLodState,t.Identity,"FileLoader::LinkLods inbound children/collision-model reassignment and camera multiplier not committed; Lod==-1 is not proof of normal sector");
        const auto found = committed.find(IdentityKey(e.Placement));
        if (t.Model && (found == committed.end() || !t.Identity.Matches(found->second->Placement) || found->second->Model != t.Model)) {
            // Full-source transform membership can differ from normalized COL
            // window membership. Never borrow uncommitted geometry for a hit.
            t.CollisionModelKnown = false;
            if (diagnostic.Issue == Issue::None)
                diagnostic = Diagnostic(Issue::CommittedGeometry,t.Identity,"potential source-sector building lacks identical committed COL binding");
        }
        snapshot->Targets.push_back(std::move(t));
        if (diagnostic.Issue != Issue::None) publication.Diagnostics.push_back(std::move(diagnostic));
    }
    if (!snapshot->Targets.empty()) {
        const auto& first = snapshot->Targets.front().Identity;
        const bool oneSource = first.Binary && std::ranges::all_of(snapshot->Targets, [&](const auto& t) {
            return t.Identity.Binary && t.Identity.Ipl == first.Ipl;
        });
        if (oneSource && (!commit.SourceCollision->Overrides || commit.SourceCollision->Overrides->Entries().empty()))
            for (auto& t : snapshot->Targets) t.SourceListOrdinal = uint64_t(UINT32_MAX)-t.Identity.Record;
    }
    // Completeness certifies the enumerated native potential-candidate set, not
    // eligibility of each member. Unresolved members remain Unknown above.
    snapshot->CompleteNormalSector = true;
    snapshot->MembershipAndOverridesVerified = true;
    snapshot->Deduplicated = true;
    return publication;
}

bool NativeWorldGround::Publish(const NativeWorldGroundCommit& commit, float x, float y,
    NativeWorldGroundPublication& current, std::string& error) const {
    if (current.Snapshot && (commit.WorldGeneration < current.Snapshot->WorldGeneration ||
        commit.MetadataRevision < current.Snapshot->MetadataRevision ||
        (commit.WorldGeneration == current.Snapshot->WorldGeneration && current.SourceCollision != commit.SourceCollision))) {
        error = "stale native world generation/metadata revision"; return false;
    }
    auto next = Publish(commit,x,y);
    if (!next.Snapshot->CompleteNormalSector) {
        error = next.Diagnostics.empty() ? "native sector authority unavailable" : next.Diagnostics.front().SourceReason; return false;
    }
    current = std::move(next); error.clear(); return true;
}

NativeSourceGroundResult NativeWorldGround::Query(const NativeWorldGroundPublication& publication,
    V stored, uint64_t generation, uint64_t revision) {
    if (!publication.Snapshot) { NativeSourceGroundResult r; r.Reason = NativeSourceGroundReason::UnknownCoverage; return r; }
    return NativeSourceGround::QueryBuildings(*publication.Snapshot,stored,generation,revision);
}
