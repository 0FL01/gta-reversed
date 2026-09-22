#include "NativeInteractionRuntime.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "interaction-runtime-fail: %s\n", message); std::exit(1); }
}
}

int main() {
    NativeInteractionRuntime runtime;
    std::string error;
    Check(runtime.SetMoney(10000, error) == NativeInteractionStatus::Ok, "set source money");
    Check(runtime.Register("GROVEGAR", NativeInteractionKind::Garage, 0, true, error) == NativeInteractionStatus::Ok &&
        runtime.Register("SAFEHOUSE", NativeInteractionKind::Property, 5000, false, error) == NativeInteractionStatus::Ok &&
        runtime.Register("AMMUNATION", NativeInteractionKind::Shop, 1000, true, error) == NativeInteractionStatus::Ok,
        "register garage property shop");
    const auto revision = runtime.Revision();
    Check(runtime.Use("SAFEHOUSE", error) == NativeInteractionStatus::NotOwned && runtime.Revision() == revision,
        "unowned property use rejection");
    Check(runtime.Buy("SAFEHOUSE", error) == NativeInteractionStatus::Ok && runtime.Money() == 5000 &&
        runtime.Find("SAFEHOUSE")->Owned, "buy property");
    Check(runtime.Use("SAFEHOUSE", error) == NativeInteractionStatus::Ok &&
        runtime.Use("GROVEGAR", error) == NativeInteractionStatus::Ok, "use property and garage");
    Check(runtime.Buy("AMMUNATION", error) == NativeInteractionStatus::Ok && runtime.Money() == 4000 &&
        runtime.Find("AMMUNATION")->Purchases == 1, "shop purchase progression");
    std::vector<std::uint8_t> save;
    Check(runtime.Encode(save, error) == NativeInteractionStatus::Ok && save.size() > 64, "portable interaction save");

    NativeInteractionRuntime restored;
    Check(restored.Restore(save, error) == NativeInteractionStatus::Ok && restored.Money() == 4000 &&
        restored.Find("SAFEHOUSE")->Owned && restored.Find("SAFEHOUSE")->Uses == 1 &&
        restored.Find("GROVEGAR")->Uses == 1 && restored.Find("AMMUNATION")->Purchases == 1,
        "restart load preserves ownership and progression");
    Check(restored.Use("SAFEHOUSE", error) == NativeInteractionStatus::Ok &&
        restored.Find("SAFEHOUSE")->Uses == 2, "post-restart interaction continues");
    const auto retainedMoney = restored.Money();
    auto truncated = save; truncated.pop_back();
    Check(restored.Restore(truncated, error) == NativeInteractionStatus::CorruptSave &&
        restored.Money() == retainedMoney && restored.Find("SAFEHOUSE")->Uses == 2,
        "truncated save rejection retains owner");
    auto corrupt = save; corrupt[12] ^= 1;
    Check(restored.Restore(corrupt, error) == NativeInteractionStatus::CorruptSave &&
        restored.Money() == retainedMoney, "checksum rejection retains owner");

    std::printf("native-interaction-runtime-ok checks=%d garage=used property=bought-used shop=purchased money=4000 save=roundtrip restart=continued corruption=rejected\n",
        g_Checks);
}
