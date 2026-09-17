#include "app/platform/linux/NativeZonePopulation.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace { std::size_t checks; void Check(bool v,const char* m){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",m);std::exit(1);}} }
int main(int argc,char** argv){
    if(argc!=2)return 2;
    NativeZonePopulation zones; std::string error;
    const bool loaded=zones.LoadBeforeWorker(argv[1],error);
    Check(loaded,error.c_str()); Check(zones.Entries().size()==378,"real info.zon count");
    NativeScriptZonePopulationRequest request; std::memcpy(request.Name.data(),"MARKST",6); request.Value=14;
    Check(zones.SetType(request).Status==NativeScriptServiceStatus::Ready,"source zone type update");
    const auto found=std::ranges::find_if(zones.Entries(),[](const auto& e){return e.Label=="MARKST";});
    Check(found!=zones.Entries().end()&&found->PopulationType==14,"source zone value retained");
    Check(zones.Reset().Status==NativeScriptServiceStatus::Ready&&found->PopulationType==5&&found->Races==15,"source reset defaults");
    std::printf("native-zone-population-ok checks=%zu zones=378 reset=1 type=1 runtime-consumer=0\n",checks);
}
