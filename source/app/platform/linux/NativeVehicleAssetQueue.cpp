#include "app/platform/linux/NativeVehicleAssetQueue.h"

NativeGeneratedVehicleAssetResult NativeVehicleAssetSource::Load(
    const NativeCarGeneratorModelDefinition& definition, NativeGeneratedVehicleAsset& out) const {
    if (!Catalog || GameDir.empty()) return {NativeGeneratedVehicleAssetStatus::Error, "missing startup game directory/catalog owner"};
    // This virtual boundary is called ONLY by the existing parser worker. The
    // default source always invokes the committed real DFF/TXD/COL/frames loader.
    return NativeGeneratedVehicleAssets_Load(GameDir.c_str(), definition, *Catalog, out);
}
