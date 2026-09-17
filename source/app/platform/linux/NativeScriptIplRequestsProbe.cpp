#include "app/platform/linux/NativeScriptIplRequests.h"

#include <cstdio>

int main() {
    NativeScriptIplRequests requests;
    NativeScriptIplRequest request;
    request.Name = {'C', 'R', 'A', 'C', 'K'};
    request.Requested = true;
    if (requests.Set(request).Status != NativeScriptServiceStatus::Ready || requests.Entries().size() != 1 ||
        !requests.Entries()[0].Requested) return 1;
    request.Requested = false;
    if (requests.Set(request).Status != NativeScriptServiceStatus::Ready || requests.Entries()[0].Requested) return 1;
    std::printf("native-script-ipl-requests-ok checks=5 capacity=256 streaming=0\n");
}
