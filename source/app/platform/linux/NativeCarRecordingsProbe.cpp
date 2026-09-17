#include "app/platform/linux/NativeCarRecordings.h"
#include <cstdio>
#include <stdexcept>
int main(int argc,char** argv){try{if(argc!=2)throw std::runtime_error("usage: probe GAME");NativeCarRecordings owner;std::string error;if(!owner.LoadBeforeWorker(argv[1],error))throw std::runtime_error(error);if(owner.Count()!=426||owner.IsLoaded(1))throw std::runtime_error("archive census");if(owner.Request(1).Status!=NativeScriptServiceStatus::Ready||!owner.IsLoaded(1)||owner.Request(0).Status!=NativeScriptServiceStatus::Error)throw std::runtime_error("request state");std::printf("native-car-recordings-ok checks=4 entries=%zu loaded=%zu playback=0\n",owner.Count(),owner.LoadedCount());return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
