#include "NativeScriptContentLedger.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "script-content-fail: %s\n", message); std::exit(1); }
}
}

int main(int argc, char** argv) {
    const char* gameDir = argc > 1 ? argv[1] : "/game";
    NativeScriptContentLedger ledger;
    std::string error;
    Check(ledger.Load(gameDir, error) == NativeScriptContentStatus::Ok, error.c_str());
    const auto held = ledger.Snapshot();
    Check(held && held->Complete && !held->UnknownReachableBehavior, "complete classified corpus");
    Check(held->Groups == 215 && held->ExecutableGroups == 214 && held->Sites == 416669 &&
        held->MainSites == 16587 && held->MissionSites == 343852 && held->StreamedSites == 56230,
        "complete main mission streamed site census");
    Check(held->Opcodes == 1570 && held->OperandForms == 4388 && held->ImplementedSites == 346905 &&
        held->RuntimeUnsupportedSites == 40882 && held->ManifestUnsupportedSites == 28882 &&
        held->ManifestUnsupportedOpcodes == 971, "coverage census");
    std::uint32_t manifestUnsupported = 0;
    for (std::uint32_t opcode = 0; opcode <= 0x0A7F; ++opcode)
        manifestUnsupported += ledger.Classify(std::uint16_t(opcode)) == NativeScriptSiteCoverage::ManifestUnsupported;
    Check(manifestUnsupported == 971, "manifest opcode bitset");
    Check(ledger.Classify(0x0050) == NativeScriptSiteCoverage::Implemented &&
        ledger.Classify(0x0A4B) == NativeScriptSiteCoverage::ManifestUnsupported,
        "implemented and strict manifest examples");
    const auto before = ledger.Snapshot();
    Check(ledger.Load("/missing-script-corpus", error) == NativeScriptContentStatus::InvalidInput &&
        ledger.Snapshot() == before, "failed reload retains publication");
    Check(ledger.Load(gameDir, error) == NativeScriptContentStatus::Ok && ledger.Snapshot()->Revision == 2 &&
        held->Revision == 1 && held->SiteFingerprint == 0x54B67C3B1B6BD9E5ULL,
        "immutable prior publication");
    std::printf("native-script-content-ok checks=%d groups=215 sites=416669 main=16587 missions=343852 "
        "streamed=56230 opcodes=1570 forms=4388 implemented-sites=346905 strict-sites=69764 "
        "unknown-reachable=0 fingerprint=54B67C3B1B6BD9E5 schema=%s\n",
        g_Checks, NativeScriptContentLedger::SchemaRevision);
}
