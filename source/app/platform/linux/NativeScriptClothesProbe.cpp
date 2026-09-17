#include "app/platform/linux/NativeScriptClothes.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
unsigned s_Checks;
void Check(bool condition) {
    ++s_Checks;
    if (!condition) std::abort();
}
std::array<char, 16> Name(std::string_view value) {
    std::array<char, 16> out{};
    std::copy(value.begin(), value.end(), out.begin());
    return out;
}
}

int main() {
    NativeScriptClothes clothes;
    Check(clothes.Give({{}, 0, Name("VEST"), Name("VEST"), 0}).Status == NativeScriptServiceStatus::Ready);
    Check(clothes.State().Parts[0].Texture == Name("VEST") && clothes.State().Revision == 1);
    Check(clothes.Build(0).Status == NativeScriptServiceStatus::Ready && clothes.State().BuildRevision == 1);
    Check(clothes.Store().Status == NativeScriptServiceStatus::Ready && clothes.State().HasStoredState &&
        clothes.State().StoredParts == clothes.State().Parts && clothes.State().StoreRevision == 1);
    const auto retained = clothes.State();
    Check(clothes.Give({{}, 1, Name("VEST"), Name("VEST"), 0}).Status == NativeScriptServiceStatus::Error &&
        clothes.State() == retained);
    Check(clothes.Give({{}, 0, {}, Name("VEST"), 0}).Status == NativeScriptServiceStatus::Error &&
        clothes.State() == retained);
    Check(clothes.Give({{}, 0, Name("VEST"), Name("VEST"), 18}).Status == NativeScriptServiceStatus::Error &&
        clothes.State() == retained);
    Check(clothes.Build(1).Status == NativeScriptServiceStatus::Error && clothes.State() == retained);
    std::cout << "native-script-clothes-ok checks=" << s_Checks
              << " player=0 parts=18 runtime-application=0\n";
}
