#include "app/platform/linux/NativeMissionText.h"

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
std::array<char, 8> Name(std::string_view value) {
    std::array<char, 8> out{};
    std::copy(value.begin(), value.end(), out.begin());
    return out;
}
}

int main(int argc, char** argv) {
    Check(argc == 2);
    NativeMissionText text;
    std::string error;
    if (!text.LoadBeforeWorker(argv[1], error)) {
        std::cerr << error << '\n';
        return 1;
    }
    Check(text.Tables().size() == 127 && text.Tables().front().Name == Name("MAIN"));
    Check(text.Select(Name("INTRO1")).Status == NativeScriptServiceStatus::Ready &&
        text.Active() == Name("INTRO1") && text.Revision() == 1);
    Check(text.SetCommandsEnabled(true).Status == NativeScriptServiceStatus::Ready &&
        text.CommandsEnabled() && text.Revision() == 2);
    Check(text.SetDrawBeforeFade(true).Status == NativeScriptServiceStatus::Ready &&
        text.DrawBeforeFade() && text.Revision() == 3);
    Check(text.SetFont(1).Status == NativeScriptServiceStatus::Ready && text.Font() == 1 && text.Revision() == 4);
    Check(text.SetStyle(0x033F,{0.5f,1.25f},{}).Status == NativeScriptServiceStatus::Ready &&
        text.Style().ScaleX == 0.5f && text.Style().ScaleY == 1.25f && text.Revision() == 5);
    const auto active = text.Active();
    Check(text.Select(Name("MISSING")).Status == NativeScriptServiceStatus::Error &&
        text.Active() == active && text.Revision() == 5);
    std::cout << "native-mission-text-ok checks=" << s_Checks
              << " tables=127 active=INTRO1 presentation=0\n";
}
