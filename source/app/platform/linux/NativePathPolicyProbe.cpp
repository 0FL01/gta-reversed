#include "app/platform/linux/NativePathPolicy.h"
#include <cstdio>
int main(){NativePathPolicy p;NativeScriptPathPolicyRequest r;r.Kind=NativePathPolicyKind::VehicleOff;r.Coordinates={2,4,6,-2,-4,-6};
if(p.Add(r).Status!=NativeScriptServiceStatus::Ready||p.Entries().size()!=1||p.Entries()[0].Min!=std::array<float,3>{-2,-4,-6})return 1;
std::printf("native-path-policy-ok checks=3 ordered=1 consumer=0\n");}
