#include "app/platform/linux/NativeRadioRuntime.h"

#include <cstdlib>
#include <iostream>

namespace {
unsigned s_Checks;
void Check(bool condition) { ++s_Checks; if (!condition) std::abort(); }
}

int main() {
    Check(NativeRadioRuntime::Stations().size() == 12);
    for (const auto& station : NativeRadioRuntime::Stations())
        Check(station.MusicCount > 0 && station.MusicCount <= station.Music.size() && station.Music[0] != 1922);
    NativeRadioRuntime radio;
    std::string error;
    Check(radio.Start(7, 0, error) == NativeRadioStatus::Ok &&
        radio.State().CurrentTrack == 1259 && radio.State().Queue[1] == 1266);
    Check(radio.Advance(5000, 10000, error) == NativeRadioStatus::Ok &&
        radio.State().PlayTimeMs == 5000 && radio.State().ListenTimeMs[7] == 5000);
    Check(radio.Interrupt(43200, error) == NativeRadioStatus::Ok &&
        radio.State().Mode == NativeRadioMode::Interrupted && radio.State().SavedTrack == 1259);
    std::vector<std::uint8_t> envelope;
    Check(radio.Save(envelope, error) && !envelope.empty());
    NativeRadioRuntime restored;
    Check(restored.Restore(envelope, error) == NativeRadioStatus::Ok && restored.State() == radio.State());
    Check(restored.Resume(error) == NativeRadioStatus::Ok && restored.State().CurrentTrack == 1259 &&
        restored.State().PlayTimeMs == 5000);
    Check(restored.Advance(5000, 10000, error) == NativeRadioStatus::Ok &&
        restored.State().PreviousTrack == 1259 && restored.State().CurrentTrack == 1266 &&
        restored.State().MusicHistory[7][0] == 1259);
    Check(restored.Retune(1, 2, error) == NativeRadioStatus::Ok &&
        restored.State().CurrentTrack == 245 && restored.State().Queue[1] == 252);
    const auto retained = restored.State();
    envelope.pop_back();
    Check(restored.Restore(envelope, error) == NativeRadioStatus::EnvelopeRejected && restored.State() == retained);
    Check(restored.Stop(error) == NativeRadioStatus::Ok && restored.State().Mode == NativeRadioMode::Stopped);
    std::cout << "native-radio-runtime-ok checks=" << s_Checks
              << " stations=12 radio-x=1259,1266 interrupt=43200 save-resume=exact retune=245\n";
}
