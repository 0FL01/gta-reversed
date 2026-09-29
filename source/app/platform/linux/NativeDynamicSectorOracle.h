// Test-only shells for Physical's documented linked-node implementation and
// the actual World query bodies. The runner records disabled-body extraction;
// read-only retail RE independently confirms virtual sphere/no-clamp semantics.
#include <cassert>
#include <functional>
#include <type_traits>
#include <utility>

namespace dynamic_source_oracle {
using int16 = std::int16_t;
using int32 = std::int32_t;

struct CVector { float x{}, y{}, z{}; };
float DistanceBetweenPoints2D(CVector a, CVector b) {
    const float x = a.x - b.x, y = a.y - b.y;
    return std::sqrt(x * x + y * y);
}
float DistanceBetweenPoints(CVector a, CVector b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}
struct CRect {
    float left{}, bottom{}, right{}, top{};
    CRect() = default;
    CRect(float a, float b, float c, float d) : left(a), bottom(b), right(c), top(d) {}
    CRect(CVector p, float radius) : left(p.x - radius), bottom(p.y - radius),
        right(p.x + radius), top(p.y + radius) {}
};
struct CColModel { float Radius{}; float GetBoundRadius() const { return Radius; } };
struct ModelInfo { CColModel* Model{}; CColModel* GetColModel() const { return Model; } };
struct CModelInfo {
    inline static std::array<ModelInfo, 256> Models;
    static ModelInfo* GetModelInfo(int id) { return &Models.at(std::size_t(id)); }
};
enum { ENTITY_TYPE_DUMMY, ENTITY_TYPE_VEHICLE, ENTITY_TYPE_PED, ENTITY_TYPE_OBJECT, ENTITY_TYPE_BUILDING };
class CEntity;
struct Link {
    CEntity* Value{};
    Link* Next{};
    Link* Prev{};
};
class List {
public:
    using ItemType = CEntity*;
    using NodeType = Link;
    Link* Head{};
    struct Iterator {
        Link* Node{};
        CEntity* operator*() const { return Node->Value; }
        Iterator& operator++() { Node = Node->Next; return *this; }
        bool operator!=(const Iterator& other) const { return Node != other.Node; }
    };
    Iterator begin() const { return {Head}; }
    Iterator end() const { return {}; }
    Link* AddNode(Link* node) {
        node->Prev = nullptr;
        node->Next = std::exchange(Head, node);
        if (node->Next) node->Next->Prev = node;
        return node;
    }
    Link* AddItem(CEntity* item) { return AddNode(new Link{item}); }
    void UnlinkNode(Link* node) {
        if (node->Prev) node->Prev->Next = node->Next;
        else { assert(Head == node); Head = node->Next; }
        if (node->Next) node->Next->Prev = node->Prev;
    }
    void DeleteNode(Link* node) { UnlinkNode(node); delete node; }
};
using CPtrListDoubleLink = List;
struct CSector { List Dummies, Buildings; };
struct CRepeatSector { List Vehicles, Peds, Objects; };
class CWorld {
public:
    inline static std::array<CRepeatSector, 16 * 16> Repeats;
    inline static CSector UnusedStatic;
    inline static std::uint64_t Scan{};
    static float GetSectorfX(float x) { return float(double(x) / 50.0 + 60.0); }
    static float GetSectorfY(float y) { return float(double(y) / 50.0 + 60.0); }
    // SOURCE_DYNAMIC_GRID_INSERT
    static CSector& GetSector(int32, int32) { return UnusedStatic; }
    static CRepeatSector& GetRepeatSector(int32 x, int32 y) {
        return Repeats[(static_cast<std::uint32_t>(y) & 15u) * 16u +
            (static_cast<std::uint32_t>(x) & 15u)];
    }
    static void AdvanceCurrentScanCode() { ++Scan; }
    template<class PtrListType> static void FindObjectsKindaCollidingSectorList(
        PtrListType&, const CVector&, float, bool, int16*, int16, CEntity**);
    static void FindObjectsKindaColliding(const CVector&, float, bool, int16*, int16,
        CEntity**, bool, bool, bool, bool, bool);
};
CRepeatSector* GetRepeatSector(int32 x, int32 y) { return &CWorld::GetRepeatSector(x, y); }
class CEntity {
public:
    NativeDynamicEntityKey Key;
    CVector Position, Centre;
    CColModel Col;
    int m_nModelIndex{};
    int m_nType{};
    bool m_bIsBIGBuilding = false;
    std::uint64_t LastScan{};
    CVector GetBoundCentre() const { return Centre; }
    bool IsScanCodeCurrent() const { return LastScan == CWorld::Scan; }
    void SetCurrentScanCode() { LastScan = CWorld::Scan; }
    void Add() { std::abort(); } // BigBuilding is outside this dynamic profile
    void Remove() { std::abort(); }
};
struct CEntryInfoNode {
    List* m_doubleLinkList{};
    Link* m_doubleLink{};
    CRepeatSector* m_repeatSector{};
    CEntryInfoNode* m_prev{};
    CEntryInfoNode* m_next{};
    void AddToList(CEntryInfoNode* head) {
        m_prev = nullptr;
        m_next = head;
        if (head) head->m_prev = this;
    }
};
struct CollisionList {
    CEntryInfoNode* m_node{};
    void DeleteNode(CEntryInfoNode* node) {
        if (node->m_prev) node->m_prev->m_next = node->m_next;
        else { assert(m_node == node); m_node = node->m_next; }
        if (node->m_next) node->m_next->m_prev = node->m_prev;
        delete node;
    }
};
class CPhysical : public CEntity {
public:
    CollisionList m_pCollisionList;
    CRect GetBoundRect() const;
    void Add();
    void Remove();
    void RemoveAndAdd();
};

// SOURCE_DYNAMIC_BODIES_INSERT

void Bind(CPhysical& entity, const NativeLiveEntityBound& bound, int modelIndex) {
    entity.Key = Key(bound);
    entity.m_nType = bound.Kind == NativeLiveEntityKind::Vehicle ? ENTITY_TYPE_VEHICLE : ENTITY_TYPE_PED;
    entity.Position = {bound.Transform.Position[0], bound.Transform.Position[1], bound.Transform.Position[2]};
    entity.Centre = {bound.WorldCenter[0], bound.WorldCenter[1], bound.WorldCenter[2]};
    entity.Col.Radius = bound.Model.Radius;
    entity.m_nModelIndex = modelIndex;
    CModelInfo::Models.at(std::size_t(modelIndex)).Model = &entity.Col;
}

void Compare(NativeDynamicSectorLists& lists, NativeCollisionVector position, float radius) {
    NativeDynamicSectorQuery actual;
    auto candidate = Candidate();
    candidate.Radius = radius;
    std::string error;
    Check(lists.Query(position, candidate, actual, error) == NativeDynamicSectorStatus::Ok,
        "owned oracle query is valid");
    CEntity* entities[8]{};
    int16 count{};
    CWorld::FindObjectsKindaColliding({position[0], position[1], position[2]}, radius, true,
        &count, 8, entities, false, true, true, false, false);
    Check(actual.Count == static_cast<std::size_t>(count), "extracted source capped count");
    auto status = NativeLiveBlockageStatus::Clear;
    std::size_t zTests{};
    for (int i = 0; i < count; ++i) {
        Check(actual.Selected[std::size_t(i)] == entities[i]->Key, "extracted source exact selected order");
        const auto* bound = lists.Resolve(entities[i]->Key);
        Check(bound != nullptr, "source pointer maps to exact owned identity");
        if (status == NativeLiveBlockageStatus::Clear) {
            ++zTests;
            if (entities[i]->Position.z + bound->Model.Max[2] + 1.0f > position[2] + candidate.Min[2] &&
                entities[i]->Position.z + bound->Model.Min[2] - 1.0f < position[2] + candidate.Max[2]) {
                status = NativeLiveBlockageStatus::Blocked;
            }
        }
    }
    Check(actual.Blockage.Status == status && actual.Blockage.ZTests == zTests,
        "source-selected order drives unrotated Z tests");
}
}

static void SourceOracleCases() {
    using namespace dynamic_source_oracle;
    for (const float pivot : {-3025.f, -2990.f, -1350.0001220703125f, -800.5f, -50.f,
        -.5f, 0.f, 49.5f, 50.f, 799.5f, 2990.f}) {
        CWorld::Scan = 0;
        NativeDynamicSectorLists lists;
        std::array<CPhysical, 16> source;
        std::array<NativeLiveEntityBound, 16> bounds;
        std::string error;
        for (std::size_t i = 0; i < source.size(); ++i) {
            bounds[i] = Entity(i < 10 ? NativeLiveEntityKind::Vehicle : NativeLiveEntityKind::MissionPed,
                int(i + 1), {pivot + float(int(i % 5) - 2), 50.f + float(int(i % 3) - 1),
                    i % 4 == 0 ? 0.f : 100.f}, 4.f);
            Check(lists.Add(bounds[i], error) == NativeDynamicSectorStatus::Ok, "oracle add");
            Bind(source[i], bounds[i], int(i));
            source[i].Add();
        }
        for (const float radius : {2.f, 60.f, 850.f}) Compare(lists, {pivot, 50, 0}, radius);
        for (std::size_t i = 0; i < source.size(); ++i) {
            bounds[i].Transform.Position[0] += i % 2 ? 1.f : -1.f;
            bounds[i].Transform.Position[2] = i % 2 ? 100.f : 0.f;
            bounds[i].WorldCenter = bounds[i].Transform.Position;
            Check(lists.Reinsert(bounds[i], error) == NativeDynamicSectorStatus::Ok, "oracle reinsert");
            Bind(source[i], bounds[i], int(i));
            source[i].RemoveAndAdd(); // source node reuse, NOT test remove-all/add
            Compare(lists, {pivot, 50, 0}, 3.f);
        }
        for (std::size_t i = 0; i < source.size(); ++i) {
            source[i].Remove();
            Check(lists.Remove(Key(bounds[i]), error) == NativeDynamicSectorStatus::Ok, "oracle remove");
            Compare(lists, {pivot, 50, 0}, 3.f);
        }
        Check(lists.LinkCount() == 0, "no replay links after all source removals");
        for (const auto& sector : CWorld::Repeats)
            Check(!sector.Vehicles.Head && !sector.Peds.Head, "source entry/list node cleanup");
    }
    {
        NativeDynamicSectorLists lists;
        CPhysical broadSource, tinySource;
        auto broad = Entity(NativeLiveEntityKind::Vehicle, 4, {-1350.25f, 25, 100}, 1);
        auto tiny = Entity(NativeLiveEntityKind::Vehicle, 5, {-1350.0001220703125f, 25, 0}, 0.00000001f);
        std::string error;
        Check(lists.Add(broad, error) == NativeDynamicSectorStatus::Ok &&
            lists.Add(tiny, error) == NativeDynamicSectorStatus::Ok, "sector-spill oracle setup");
        Bind(broadSource, broad, 0); Bind(tinySource, tiny, 1);
        broadSource.Add(); tinySource.Add();
        Compare(lists, {-1350, 25, 0}, .0002f);
        broadSource.Remove(); tinySource.Remove();
    }
    {
        NativeDynamicSectorLists lists;
        CPhysical source;
        auto bound = Entity(NativeLiveEntityKind::Vehicle, 8, {50, 50, 100}, 450);
        std::string error;
        Check(lists.Add(bound, error) == NativeDynamicSectorStatus::Ok, "alias node-reuse oracle setup");
        Bind(source, bound, 0);
        source.Add();
        Compare(lists, {50, 50, 0}, 850);
        bound.Model.Radius = 40;
        Check(lists.Reinsert(bound, error) == NativeDynamicSectorStatus::Ok &&
            lists.LinkCount() == 4, "large-to-small source membership");
        Bind(source, bound, 0);
        source.RemoveAndAdd();
        Compare(lists, {50, 50, 0}, 850);
        source.Remove();
        Check(lists.Remove(Key(bound), error) == NativeDynamicSectorStatus::Ok,
            "source large alias retirement");
    }
    std::printf("dynamic-sector-source-oracle-ok sequences=13 source-bodies=physical-add,remove,reuse,sphere,scan,capped-traversal\n");
}
