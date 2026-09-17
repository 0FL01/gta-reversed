#include "app/platform/linux/NativeSetPieces.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
std::size_t s_Checks;
void Check(bool value, const char* message) { ++s_Checks; if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); } }
}

int main() {
    NativeSetPieces pieces;
    NativeScriptSetPieceRequest request;
    request.Type = 3;
    request.Coordinates = {2435.2f,-1740.0f,2453.9f,-1723.4f,2432.0f,-1707.0f,
        2448.0f,-1715.0f,2432.0f,-1707.0f,2448.0f,-1715.0f};
    std::string error;
    Check(pieces.Add(request,error)==NativeSetPieceStatus::Ok,"source set-piece registration");
    Check(pieces.Entries().size()==1 && pieces.Entries()[0].Type==3,"source set-piece count/type");
    Check(pieces.Entries()[0].CornerMin==NativeSetPiecePoint{2435.0f,-1740.0f} &&
        pieces.Entries()[0].CornerMax==NativeSetPiecePoint{2453.75f,-1723.25f},"source quarter-unit area");
    const auto prior=pieces.Entries()[0]; request.Coordinates[0]=std::numeric_limits<float>::quiet_NaN();
    Check(pieces.Add(request,error)==NativeSetPieceStatus::InvalidInput && pieces.Entries()[0]==prior,"invalid registration atomic");
    std::printf("native-set-pieces-ok checks=%zu capacity=210 registration=1 runtime-update=0\n",s_Checks);
}
