#include "app/platform/linux/NativeSourceSurfaces.h"
#include "app/platform/linux/NativeSourcePhysical.h"
#include "game_sa/SurfaceNameLookup.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::size_t s_Checks{};
void Check(bool value, const char* reason) {
    ++s_Checks;
    if (!value) throw std::runtime_error(reason);
}

std::string Matrix() {
    std::string text = " ; source comments, commas and controls\r\n";
    for (int row = 0; row < 6; ++row) {
        text += "ignored_label\t";
        for (int column = 0; column <= row; ++column) text += std::to_string(row * 10 + column) + ",";
        text += "ignored_tail\n";
    }
    return text;
}

std::string Material(const std::string& name, const std::string& group) {
    std::string text = name + " " + group + " +1 -0.5 DEFAULT NONE";
    for (int field = 0; field < 29; ++field) text += " 0";
    return text + " NONE ignored_tail\n";
}
}

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(eSurfaceType) == 1);
        using Status = NativeSourceSurfaceStatus;
        NativeSourceSurfaces surfaces;
        std::string error;
        float limit = 123;
        Check(surfaces.AdhesiveLimit(0, 0, limit) == Status::NotLoaded && limit == 123, "unloaded lookup retention");
        Check(GetSourceSurfaceIdFromName("PED") == 62 && GetSourceSurfaceIdFromName("RAILTRACK") == 178,
              "shared source material IDs");
        Check(GetSourceSurfaceIdFromName("ped") == SURFACE_DEFAULT && GetSourceSurfaceIdFromName("") == SURFACE_DEFAULT,
              "source case sensitivity and default alias");
        constexpr const char* names[]{"DEFAULT", "TARMAC", "TARMAC_FUCKED", "TARMAC_REALLYFUCKED", "PAVEMENT", "PAVEMENT_FUCKED"};
        constexpr const char* groups[]{"RUBBER", "HARD", "ROAD", "LOOSE", "SAND", "WET"};
        std::string materials = "# owned fixture, not shipped game coefficients\n";
        for (int i = 0; i < 6; ++i) materials += Material(names[i], groups[i]);
        Check(surfaces.LoadBytes(Matrix(), materials, error) && error.empty(), "complete generated matrix and material binding");
        const auto first = surfaces.Snapshot();
        Check(first.MaterialRows == 6 && first.Loaded, "owned material row count");
        for (int a = 0; a < 6; ++a) {
            Check(first.AdhesionGroups[a] == a, "source adhesion group ordering");
            for (int b = 0; b < 6; ++b) {
                Check(surfaces.AdhesiveLimit(a, b, limit) == Status::Ok && limit == std::max(a, b) * 10 + std::min(a, b),
                      "all mirrored matrix entries bind through material IDs");
            }
        }
        limit = 123;
        Check(surfaces.AdhesiveLimit(179, 0, limit) == Status::InvalidMaterial && limit == 123, "out of ordinary material range");
        Check(surfaces.AdhesiveLimit(62, 255, limit) == Status::InvalidMaterial && limit == 123, "NONE material is not a friction surface");

        auto rejected = [&](std::string matrix, std::string rows) {
            const auto before = surfaces.Snapshot();
            Check(!surfaces.LoadBytes(matrix, rows, error) && !error.empty() && surfaces.Snapshot() == before,
                  "malformed reload retains both complete tables");
        };
        rejected("x 1\n", materials);
        rejected(Matrix() + "seventh 1\n", materials);
        auto nonfinite = Matrix();
        nonfinite.replace(nonfinite.find("0,"), 2, "nan,");
        rejected(nonfinite, materials);
        auto overflow = Matrix();
        overflow.replace(overflow.find("0,"), 2, "1e40,");
        rejected(overflow, materials);
        auto badSign = Matrix();
        badSign.replace(badSign.find("0,"), 2, "+-1,");
        rejected(badSign, materials);
        rejected(Matrix(), "PED HARD 1 1\n");
        rejected(Matrix(), Material(std::string(64, 'x'), "HARD"));
        rejected(Matrix(), Material("PED", std::string(32, 'x')));
        rejected(Matrix(), std::string(512, ' ') + "\n");
        rejected(Matrix(), std::string("PED\0HARD", 8));
        auto badNumber = Material("PED", "HARD");
        badNumber.replace(badNumber.find(" 0"), 2, " overflow");
        rejected(Matrix(), badNumber);
        auto badIntegerSign = Material("PED", "HARD");
        badIntegerSign.replace(badIntegerSign.find(" 0"), 2, " +-1");
        rejected(Matrix(), badIntegerSign);

        auto dash = Matrix();
        dash.replace(dash.find("0,"), 2, "-9.5,");
        Check(surfaces.LoadBytes(dash, Material("unrecognised_material", "WET") + Material("TARMAC", "unknown_group"), error),
              "source dash-prefix/default alias/unknown-group reload");
        const auto changed = surfaces.Snapshot();
        Check(changed.AdhesiveLimits[0][0] == 0 && changed.AdhesionGroups[0] == 5 && changed.AdhesionGroups[1] == 1,
              "negative prefix zero, alias DEFAULT and retain unknown group");
        Check(first.AdhesionGroups[0] == 0 && first.MaterialRows == 6, "retained value snapshot detached from reload");
        Check(surfaces.LoadBytes(Matrix(), Material("PED", "HARD") + Material("PED", "ROAD"), error), "duplicate material source order");
        Check(surfaces.Snapshot().AdhesionGroups[62] == 2, "last known group wins");

        Check(surfaces.AdhesiveLimit(62, 1, limit) == Status::Ok && limit == 21, "material-bound physical coefficient");
        NativeSourcePhysicalState body;
        body.Mass = 70;
        body.IsPed = body.DisableTurnForce = true;
        body.MoveSpeed = {10, 0, 0};
        NativeSourcePhysicalContact contact;
        contact.Normal = {0, 0, 1};
        bool applied = false;
        Check(NativeSourceApplyPedFriction(body, limit, 1, contact, applied) == NativeSourcePhysicalStatus::Ok && applied
              && std::abs(body.FrictionMoveSpeed[0] + 0.3f) < 0.000001f && body.MoveSpeed[0] == 10,
              "source lookup feeds separate friction accumulation");
        if (argc > 1) {
            const bool loaded = surfaces.Load(argv[1], error);
            Check(loaded, error.c_str());
            const auto real = surfaces.Snapshot();
            Check(real.MaterialRows == 179 && real.Loaded, "real source material rows");
            for (const auto group : real.AdhesionGroups) Check(group < 6, "real material group range");
            for (std::uint16_t a = 0; a < 179; ++a) {
                Check(surfaces.AdhesiveLimit(62, a, limit) == Status::Ok && std::isfinite(limit) && limit >= 0,
                      "real ped-to-material coefficient coverage");
            }
            Check(!surfaces.Load(std::string(argv[1]) + "/missing-surface-fixture", error) && surfaces.Snapshot() == real,
                  "missing file reload preserves real tables");
        }
        std::cout << "source-surfaces-ok checks=" << s_Checks << " canonical-source-names ordered-mirrored-adhesion\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "source-surfaces-failed: " << e.what() << '\n';
        return 1;
    }
}
