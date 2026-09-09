// ZoneInfo: district-name lookup from the shipped zone rectangles for the
// Linux native track. Round 35 (R6ag): `--zone-at X,Y` answers which named
// district contains a world point, resolving the display name through the
// existing GXT MAIN-table path (GxtText, american.gxt). No game_sa/ linkage,
// no invented rectangles: every record comes from data/info.zon bytes.
//
// Data provenance (read-only reference, NOT linked):
// - data/gta.dat ships `IPL DATA\MAP.ZON` then `IPL DATA\INFO.ZON` with the
//   comment "have to load map.zon before any of the IPLs".
// - source/game_sa/FileLoader.cpp CFileLoader::LoadZone (0x5B4AB0) parses one
//   zone line as 10 tokens: infoLabel type min.x min.y min.z max.x max.y
//   max.z level textLabel (sscanf "%s %d %f %f %f %f %f %f %d %s" == 10).
// - CFileLoader::LoadLine sanitizes each line first: every ',' and every
//   byte < ' ' becomes ' ' (so the on-disk comma format scans as spaces).
// - source/game_sa/TheZones.cpp CTheZones::FindSmallestZoneForPosition
//   (0x572360) picks, among the zones whose box contains the point, the one
//   with the smallest (x2-x1)+(y2-y1) size; ties keep the first record.
// - CTheZones::CreateZone uppercases both labels; the display name is the
//   text label (m_TextLabel) looked up via CText::Get (here: GxtText_Find).
//
// Scope decisions (all logged, all honest):
// - zoneSrc is data/info.zon ONLY (378 records): it holds the ZONE_TYPE_NAVI
//   district rectangles with resolvable display keys. data/map.zon (6
//   ZONE_TYPE_MAP city rectangles, text UNUSED) carries no display names and
//   is excluded; its only consumer is GetLevelFromPosition.
// - Retail info.zon hardcodes level=1 on all 378 rows (verified by counting;
//   the true streaming city level lives in map.zon). The reported level is
//   the record's own level column, so level=1 for every info.zon lookup.
// - The CLI takes X,Y only, so containment is a 2D inclusive slice
//   (x1<=x<=x2 && y1<=y<=y2); the game tests a 3D box against the player
//   height. All three acceptance winners contain z=0 in their file range,
//   so the slice agrees with the 3D test on the acceptance set.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct ZoneRect {
    std::string name; // info label, e.g. "VERO4a" (file bytes)
    std::string key;  // display GXT key, e.g. "VERO" (file bytes)
    int type = 0;     // zone type column (info.zon: 0 = ZONE_TYPE_NAVI)
    float x1 = 0.0f;
    float y1 = 0.0f;
    float z1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float z2 = 0.0f;
    int level = 0; // level column (info.zon: 1 on every shipped row)
};

struct ZoneData {
    std::vector<ZoneRect> zones; // file order (deterministic)
    std::string src;             // zone source path, "data/info.zon"
};

// Loads every zone record from data/info.zon under gameDir via OS_File*.
// Fails honestly when the file is missing, the `zone` header is absent, a
// data line does not scan as exactly 10 tokens, or no records result.
bool ZoneInfo_Load(const char* gameDir, ZoneData& out, char* err, std::size_t errSize);

// Index of the smallest zone (by (x2-x1)+(y2-y1), first wins ties)
// containing (x,y) in a 2D inclusive slice, or -1 when none contains it.
int ZoneInfo_FindSmallest(const ZoneData& data, double x, double y);

// 2D inclusive containment of one record (documents the Z-slice choice).
bool ZoneInfo_Contains2D(const ZoneRect& z, double x, double y);
