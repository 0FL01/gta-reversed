#include "app/platform/linux/NativeAudioFamilies.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
unsigned s_Checks;
void Check(bool condition) { ++s_Checks; if (!condition) std::abort(); }
}

int main(int argc, char** argv) {
    Check(argc == 2);
    NativeAudioFamilies audio;
    std::string error;
    Check(audio.Load(argv[1], error));
    const auto& sfx = audio.Assets()[0];
    const auto& speech = audio.Assets()[1];
    const auto& environment = audio.Assets()[2];
    Check(sfx.Family == NativeAudioFamily::Sfx && sfx.Bank == 138 && sfx.Sound == 40 &&
        sfx.Rate == 20000 && sfx.Hash == 12126532606915493261ull && sfx.Payload->size() == 11220);
    Check(speech.Family == NativeAudioFamily::Speech && speech.SourceEvent == 43200 &&
        speech.Encoding == NativeAudioEncoding::OggVorbis && speech.DurationMs > 25000 &&
        !std::memcmp(speech.Payload->data(), "OggS", 4));
    Check(environment.Family == NativeAudioFamily::Environment && environment.Bank == 105 &&
        environment.Sound == 0 && environment.Rate > 0 && environment.DurationMs > 0 &&
        environment.Payload && !environment.Payload->empty());
    const auto retained = audio.Assets();
    Check(!audio.Load(argv[1], error) && audio.Assets()[0].Payload == retained[0].Payload);
    Check(!NativeAudioFamilies::OutputFeedback);
    std::cout << "native-audio-families-ok checks=" << s_Checks
              << " sfx=80/138/40 speech=43200 environment=105/0"
              << " payloads=pcm16,ogg,pcm16 output-feedback=0\n";
}
