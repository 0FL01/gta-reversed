#include "app/platform/linux/NativeTextFamilies.h"
#include "app/platform/linux/MenuShot.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
unsigned s_Checks;
void Check(bool condition) {
    ++s_Checks;
    if (!condition) {
        std::cerr << "native-text-families-fail check=" << s_Checks << '\n';
        std::abort();
    }
}
}

int main(int argc, char** argv) {
    Check(argc == 2);
    NativeTextFamilies text;
    std::string error;
    Check(text.Load(argv[1], error));
    Check(text.Languages().size() == 5);
    std::size_t totalKeys = 0;
    for (const auto& language : text.Languages()) {
        Check(language.Tables.size() == 127 && language.Tables.front().Name ==
            std::array<char, 8>{'M','A','I','N',0,0,0,0});
        std::string start, mission;
        Check(text.Find(language.Language, "MAIN", "FEP_STG", start, error) && !start.empty());
        const std::array<char, 8> introName{'I','N','T','R','O','1',0,0};
        const auto intro = std::ranges::find(language.Tables, introName, &NativeTextTableFamily::Name);
        Check(intro != language.Tables.end() && !intro->Entries.empty() &&
            text.FindHash(language.Language, "INTRO1", intro->Entries.front().Hash,
                mission, error) && !mission.empty());
        for (const auto& table : language.Tables) {
            std::string tableName(table.Name.begin(),
                std::find(table.Name.begin(), table.Name.end(), '\0'));
            for (const auto& entry : table.Entries) {
                std::string resolved;
                Check(text.FindHash(language.Language, tableName.c_str(), entry.Hash,
                    resolved, error));
                ++totalKeys;
            }
        }
    }
    std::array<std::string, 5> starts;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        Check(text.Find(static_cast<NativeTextLanguage>(i), "MAIN", "FEP_STG", starts[i], error));
    }
    Check(starts[0] != starts[1] && starts[0] != starts[2] && starts[0] != starts[4]);

    NativeTextSubstitutions substitutions;
    substitutions.HasNumber[0] = true;
    substitutions.Numbers[0] = 42;
    substitutions.Controls.emplace("PED_ANSWER_PHONE", "TAB");
    std::string property;
    Check(text.Resolve(NativeTextLanguage::American, "MAIN", "PROP_3", substitutions,
        property, error) && property.find("TAB") != std::string::npos &&
        property.find("~k~~") == std::string::npos);
    const auto retained = property;
    Check(!text.Resolve(NativeTextLanguage::American, "MISSING", "PROP_3", substitutions,
        property, error) && property == retained);

    MenuHudFont font1, font2;
    char fontError[256]{};
    Check(MenuShot_LoadPricedownFont(argv[1], font1, fontError, sizeof(fontError)) &&
        MenuShot_LoadHudFont(argv[1], font2, fontError, sizeof(fontError)));
    Check(std::string(font1.name) == "font1" && std::string(font2.name) == "font2" &&
        font1.ok && font2.ok && !font1.rgba.empty() && !font2.rgba.empty());

    std::cout << "native-text-families-ok checks=" << s_Checks
              << " languages=5 tables=127 main=FEP_STG mission=INTRO1:all-hashes"
              << " keys=" << totalKeys
              << " substitutions=numbers,strings,controls fonts=font1,font2 feedback=0\n";
}
