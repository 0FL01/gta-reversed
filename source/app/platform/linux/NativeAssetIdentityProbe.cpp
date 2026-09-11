#include "app/platform/linux/NativeAssetIdentity.h"

#include <cassert>
#include <map>
#include <string>

int main() {
    using namespace NativeAssetIdentity;

    const ArchiveMember dff{"models/gta3.img", "fixture.dff"};
    const Model model{dff, 100};
    const Geometry geometry{model, 0};

    std::map<Model, int> models;
    models[model] = 1;
    models[Model{dff, 100}] = 2;
    assert(models.size() == 1 && models.at(model) == 2);
    models[Model{dff, 101}] = 3;
    assert(models.size() == 2);

    std::map<Geometry, int> geometries;
    geometries[geometry] = 1;
    geometries[Geometry{model, 0}] = 2;
    assert(geometries.size() == 1 && geometries.at(geometry) == 2);
    geometries[Geometry{model, 1}] = 3;
    geometries[Geometry{Model{dff, 101}, 0}] = 4;
    assert(geometries.size() == 3);

    const Material material{geometry, 0};
    std::map<Material, int> materials;
    materials[material] = 1;
    materials[Material{geometry, 0}] = 2;
    assert(materials.size() == 1 && materials.at(material) == 2);
    materials[Material{geometry, 1}] = 3;
    materials[Material{Geometry{model, 1}, 0}] = 4;
    assert(materials.size() == 3);

    const ArchiveMember commonTxd{"models/gta3.img", "common.txd"};
    const ArchiveMember modelTxd{"models/gta3.img", "fixture.txd"};
    const ArchiveMember otherTxd{"models/gta3.img", "other.txd"};
    const Texture texture{{commonTxd, modelTxd}, commonTxd, "detail", 0x1101u};

    std::map<Texture, int> textures;
    textures[texture] = 1;
    textures[Texture{{commonTxd, modelTxd}, commonTxd, "detail", 0x1101u}] = 2;
    assert(textures.size() == 1 && textures.at(texture) == 2);
    textures[Texture{{commonTxd, modelTxd}, modelTxd, "detail", 0x1101u}] = 3;
    textures[Texture{{otherTxd, modelTxd}, commonTxd, "detail", 0x1101u}] = 4;
    textures[Texture{{commonTxd, modelTxd}, commonTxd, "detail", 0x1102u}] = 5;
    assert(textures.size() == 4);

    std::map<Texture, int> nonAscii;
    nonAscii[Texture{{modelTxd}, modelTxd, "\xC3\x84", 0}] = 1;
    nonAscii[Texture{{modelTxd}, modelTxd, "\xC3\xA4", 0}] = 2;
    assert(nonAscii.size() == 2);

    Texture owned;
    {
        const std::string archive = "models/gta3.img";
        const std::string member = "temporary.txd";
        const std::string name = "temporary_texture";
        const std::vector<ArchiveMember> lineage{{archive, member}};
        owned = Texture{lineage, lineage.front(), name, 0x2201u};
    }
    assert((owned.lineage == std::vector<ArchiveMember>{{"models/gta3.img", "temporary.txd"}}));
    assert(owned.owner == owned.lineage.front());
    assert(owned.name == "temporary_texture" && owned.filter == 0x2201u);

    return 0;
}
