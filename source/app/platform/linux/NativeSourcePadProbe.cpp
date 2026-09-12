// P2-A03 owned source-pad probe. No file I/O, no assets, no OS clock, no
// device, no thread, no RNG, no framework. Uses error branches (never assert
// for side effects) so Release (NDEBUG) still executes every pad call.
// Owned integrity + digital default profile only: nonzero strictly increasing
// Seq, nondecreasing Tick (equal valid), signed left axes, three Mode0
// digital buttons (Jump=Square 1, Sprint=Cross 2, Enter=Triangle 4). No
// movement/gameplay, no inversion/swap, no physical hardware, no source full
// device state.
#include "NativeSourcePad.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

using Status = NativeSourcePadStatus;
using Sample = NativeSourcePadSample;
using Frame = NativeSourcePadFrame;

std::size_t s_Checks = 0;

void Check(bool condition, const char* message) {
    ++s_Checks;
    if (!condition) {
        std::fprintf(stderr, "native-source-pad FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckStatus(Status got, Status want, const char* message) {
    ++s_Checks;
    if (got != want) {
        std::fprintf(stderr, "native-source-pad FAIL: %s (status %s != %s)\n", message,
            NativeSourcePad::StatusName(got), NativeSourcePad::StatusName(want));
        std::exit(1);
    }
}

Frame MakeSentinel() {
    Frame sentinel;
    sentinel.Sample.Seq = 0xAAAAAAAAAAAAAAAAULL;
    sentinel.Sample.Tick = 0xBBBBBBBBBBBBBBBBULL;
    sentinel.Sample.MoveX = 1234;
    sentinel.Sample.MoveY = -1234;
    sentinel.Sample.Buttons = 0xAA;
    sentinel.Down = 0xBB;
    sentinel.Pressed = 0xCC;
    sentinel.Released = 0xDD;
    return sentinel;
}

void CheckFrameFields(const Frame& frame, std::uint8_t down, std::uint8_t pressed, std::uint8_t released,
    const char* message) {
    Check(frame.Down == down && frame.Pressed == pressed && frame.Released == released, message);
}

void CheckFormatEquals(const Frame& frame, const char* expected, const char* message) {
    const auto buffer = NativeSourcePad::FormatFrame(frame);
    Check(buffer.data()[127] == '\0' || std::strlen(buffer.data()) < 127, message);
    Check(std::strcmp(buffer.data(), expected) == 0, message);
    // No newline, NUL terminated, fits 128.
    const std::size_t len = std::strlen(buffer.data());
    Check(len == std::strlen(expected), message);
    Check(std::memchr(buffer.data(), '\n', len) == nullptr, message);
    Check(buffer[len] == '\0', message);
    // Trailing bytes stay zero (zero-initialised fixed array).
    for (std::size_t i = len + 1; i < buffer.size(); ++i) {
        if (buffer[i] != '\0') {
            Check(false, message);
        }
    }
    ++s_Checks;
}

struct ExpectedRow {
    std::uint64_t Seq;
    std::uint64_t Tick;
    std::int16_t X;
    std::int16_t Y;
    std::uint8_t Buttons;
    std::uint8_t Down;
    std::uint8_t Pressed;
    std::uint8_t Released;
    Status Want;
    const char* Text;
};

} // namespace

int main(int argc, char** argv) {
    bool traceMode = false;
    if (argc == 1) {
        traceMode = false;
    } else if (argc == 2 && argv[1] != nullptr && std::strcmp(argv[1], "--trace") == 0) {
        traceMode = true;
    } else {
        std::fprintf(stderr, "usage: sa_core_input_probe [--trace]\n");
        return 1;
    }

    // Default construction zeroes Old/New and reports no frame.
    {
        NativeSourcePad fresh;
        Check(!fresh.HasFrame(), "fresh HasFrame false");
        Check(fresh.LastFrame() == Frame{}, "fresh LastFrame default zero");
        for (unsigned i = 0; i < 5; ++i) {
            Check(fresh.LastFrame() == Frame{}, "fresh repeated LastFrame same");
        }
    }

    // StatusName lower_snake names for every enumerator.
    Check(std::strcmp(NativeSourcePad::StatusName(Status::Ok), "ok") == 0, "StatusName ok");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::DuplicateIdempotent), "duplicate_idempotent") == 0,
        "StatusName duplicate_idempotent");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::InvalidSequence), "invalid_sequence") == 0,
        "StatusName invalid_sequence");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::DuplicateConflict), "duplicate_conflict") == 0,
        "StatusName duplicate_conflict");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::BackwardTick), "backward_tick") == 0,
        "StatusName backward_tick");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::InvalidButtons), "invalid_buttons") == 0,
        "StatusName invalid_buttons");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::NonFiniteAxis), "non_finite_axis") == 0,
        "StatusName non_finite_axis");
    Check(std::strcmp(NativeSourcePad::StatusName(Status::AxisOutOfRange), "axis_out_of_range") == 0,
        "StatusName axis_out_of_range");

    // QuantizeAxis default profile: finite [-1,1], abs<=0.3 -> 0 else trunc(v*128).
    {
        std::int16_t out = 0;
        Check(NativeSourcePad::QuantizeAxis(0.0f, out) == Status::Ok && out == 0, "quant 0 -> 0");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(0.3f, out) == Status::Ok && out == 0, "quant 0.3 -> 0");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(-0.3f, out) == Status::Ok && out == 0, "quant -0.3 -> 0");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(0.1f, out) == Status::Ok && out == 0, "quant 0.1 -> 0");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(-0.29f, out) == Status::Ok && out == 0, "quant -0.29 -> 0");
        const float justAbove = std::nextafter(0.3f, 1.0f);
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(justAbove, out) == Status::Ok && out == 38,
            "quant nextafter 0.3 -> 38");
        const float justBelow = std::nextafter(-0.3f, -1.0f);
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(justBelow, out) == Status::Ok && out == -38,
            "quant nextafter -0.3 -> -38");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(1.0f, out) == Status::Ok && out == 128, "quant 1 -> 128");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(-1.0f, out) == Status::Ok && out == -128, "quant -1 -> -128");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(0.5f, out) == Status::Ok && out == 64, "quant 0.5 -> 64");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(-0.5f, out) == Status::Ok && out == -64, "quant -0.5 -> -64");
        out = 99;
        Check(NativeSourcePad::QuantizeAxis(0.75f, out) == Status::Ok && out == 96, "quant 0.75 -> 96");
        // Failures leave out unchanged.
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(std::numeric_limits<float>::quiet_NaN(), out) == Status::NonFiniteAxis &&
                out == 1234,
            "quant NaN unchanged");
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(std::numeric_limits<float>::infinity(), out) == Status::NonFiniteAxis &&
                out == 1234,
            "quant +inf unchanged");
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(-std::numeric_limits<float>::infinity(), out) == Status::NonFiniteAxis &&
                out == 1234,
            "quant -inf unchanged");
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(1.5f, out) == Status::AxisOutOfRange && out == 1234,
            "quant 1.5 unchanged");
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(-1.5f, out) == Status::AxisOutOfRange && out == 1234,
            "quant -1.5 unchanged");
        out = 1234;
        Check(NativeSourcePad::QuantizeAxis(2.0f, out) == Status::AxisOutOfRange && out == 1234,
            "quant 2 unchanged");
    }

    // Shared canonical trace: independent literals, never computed.
    const ExpectedRow kTrace[11] = {
        {1, 1000, 0, 0, 0, 0, 0, 0, Status::Ok, "1,1000,0,0,0,0,0"},
        {2, 1010, 64, -128, 1, 1, 1, 0, Status::Ok, "2,1010,64,-128,1,1,0"},
        {2, 1010, 64, -128, 1, 1, 1, 0, Status::DuplicateIdempotent, "2,1010,64,-128,1,1,0"},
        {3, 1010, 64, -128, 1, 1, 0, 0, Status::Ok, "3,1010,64,-128,1,0,0"},
        {4, 1030, 64, -128, 3, 3, 2, 0, Status::Ok, "4,1030,64,-128,3,2,0"},
        {5, 1040, 0, 0, 2, 2, 0, 1, Status::Ok, "5,1040,0,0,2,0,1"},
        {6, 1050, -128, 38, 0, 0, 0, 2, Status::Ok, "6,1050,-128,38,0,0,2"},
        {7, 1060, -128, 38, 0, 0, 0, 0, Status::Ok, "7,1060,-128,38,0,0,0"},
        {8, 1070, -128, 38, 4, 4, 4, 0, Status::Ok, "8,1070,-128,38,4,4,0"},
        {9, 1080, -128, 38, 0, 0, 0, 4, Status::Ok, "9,1080,-128,38,0,0,4"},
        {10, 1080, 0, 0, 0, 0, 0, 0, Status::Ok, "10,1080,0,0,0,0,0"},
    };

    NativeSourcePad pad;
    Frame collected[11] = {};
    for (unsigned i = 0; i < 11; ++i) {
        const ExpectedRow& row = kTrace[i];
        Sample sample{row.Seq, row.Tick, row.X, row.Y, row.Buttons};
        Frame out = MakeSentinel();
        // For the exact-duplicate row the sentinel must be replaced by the
        // same old frame; for Ok rows by the new frame. Either way out is
        // assigned on these two statuses.
        const Status got = pad.SubmitSample(sample, out);
        char message[96];
        std::snprintf(message, sizeof(message), "trace row %u status", i);
        CheckStatus(got, row.Want, message);
        std::snprintf(message, sizeof(message), "trace row %u sample echo", i);
        Check(out.Sample == sample, message);
        std::snprintf(message, sizeof(message), "trace row %u down/pressed/released", i);
        CheckFrameFields(out, row.Down, row.Pressed, row.Released, message);
        std::snprintf(message, sizeof(message), "trace row %u HasFrame", i);
        Check(pad.HasFrame(), message);
        std::snprintf(message, sizeof(message), "trace row %u LastFrame", i);
        Check(pad.LastFrame() == out, message);
        // Repeated LastFrame reads are non-consuming and identical.
        for (unsigned k = 0; k < 3; ++k) {
            std::snprintf(message, sizeof(message), "trace row %u repeat %u", i, k);
            Check(pad.LastFrame() == out, message);
        }
        // No-submit freezes the sample: LastFrame without a new Submit keeps
        // the same Down/Pressed/Released (pressed edges do not expire until a
        // new identical sample arrives, as row 3 proves for holding).
        std::snprintf(message, sizeof(message), "trace row %u format", i);
        CheckFormatEquals(out, row.Text, message);
        CheckFormatEquals(pad.LastFrame(), row.Text, message);
        const auto once = NativeSourcePad::FormatFrame(out);
        const auto twice = NativeSourcePad::FormatFrame(out);
        std::snprintf(message, sizeof(message), "trace row %u format repeat", i);
        Check(once == twice, message);
        collected[i] = out;
        // Exact duplicate returns the SAME old frame with no relatch.
        if (i == 2) {
            std::snprintf(message, sizeof(message), "duplicate returns same old frame");
            Check(out == collected[1], message);
            Check(pad.LastFrame() == collected[1], message);
        }
    }
    // Holding expires pressed edges only via a new identical sample (row 3:
    // same Buttons 1 as row 1/2 but Pressed 0), never via repeated reads.

    // Repeat of the whole trace on a fresh owner agrees exactly.
    {
        NativeSourcePad rerun;
        for (unsigned i = 0; i < 11; ++i) {
            const ExpectedRow& row = kTrace[i];
            Sample sample{row.Seq, row.Tick, row.X, row.Y, row.Buttons};
            Frame out;
            char message[96];
            std::snprintf(message, sizeof(message), "rerun row %u status", i);
            CheckStatus(rerun.SubmitSample(sample, out), row.Want, message);
            std::snprintf(message, sizeof(message), "rerun row %u equal", i);
            Check(out == collected[i], message);
            std::snprintf(message, sizeof(message), "rerun row %u format equal", i);
            Check(NativeSourcePad::FormatFrame(out) == NativeSourcePad::FormatFrame(collected[i]), message);
        }
        Check(rerun.LastFrame() == pad.LastFrame(), "rerun whole trace equal");
    }

    // Failure atomicity on the traced owner: every rejection leaves owner AND
    // out unchanged. Stable priority: duplicate gating precedes all other
    // monotonic/value checks.
    {
        const Frame before = pad.LastFrame();
        const bool beforeHas = pad.HasFrame();
        auto expectReject = [&](Sample bad, Status want, const char* message) {
            Frame out = MakeSentinel();
            const Frame sentinel = out;
            CheckStatus(pad.SubmitSample(bad, out), want, message);
            Check(out == sentinel, message);
            Check(pad.LastFrame() == before, message);
            Check(pad.HasFrame() == beforeHas, message);
        };
        expectReject(Sample{0, 2000, 0, 0, 0}, Status::InvalidSequence, "bad seq 0");
        expectReject(Sample{5, 2000, 0, 0, 0}, Status::InvalidSequence, "bad lower seq");
        // Same Seq with changed payload is a conflict even when the changed
        // Tick/buttons/axes would otherwise report a different status.
        expectReject(Sample{10, 1080, 1, 0, 0}, Status::DuplicateConflict, "conflict movex");
        expectReject(Sample{10, 900, 0, 0, 0}, Status::DuplicateConflict, "conflict priority over backward tick");
        expectReject(Sample{10, 1080, 0, 0, 8}, Status::DuplicateConflict, "conflict priority over buttons");
        expectReject(Sample{10, 1080, 129, 0, 0}, Status::DuplicateConflict, "conflict priority over axis");
        expectReject(Sample{11, 1079, 0, 0, 0}, Status::BackwardTick, "backward tick");
        expectReject(Sample{11, 1080, 0, 0, 8}, Status::InvalidButtons, "unknown bits 8");
        expectReject(Sample{12, 1080, 0, 0, 255}, Status::InvalidButtons, "unknown bits 255");
        expectReject(Sample{11, 1080, 129, 0, 0}, Status::AxisOutOfRange, "axis x 129");
        expectReject(Sample{11, 1080, -129, 0, 0}, Status::AxisOutOfRange, "axis x -129");
        expectReject(Sample{11, 1080, 0, 129, 0}, Status::AxisOutOfRange, "axis y 129");
        expectReject(Sample{11, 1080, 0, -129, 0}, Status::AxisOutOfRange, "axis y -129");
        expectReject(Sample{11, 1080, 32767, 0, 0}, Status::AxisOutOfRange, "axis x max");
        expectReject(Sample{11, 1080, 0, -32768, 0}, Status::AxisOutOfRange, "axis y min");
    }

    // Fresh-owner rejections also freeze HasFrame false with default frame.
    {
        NativeSourcePad fresh;
        Frame out = MakeSentinel();
        const Frame sentinel = out;
        CheckStatus(fresh.SubmitSample(Sample{0, 1000, 0, 0, 0}, out), Status::InvalidSequence,
            "fresh seq 0");
        Check(out == sentinel, "fresh seq 0 out unchanged");
        Check(!fresh.HasFrame(), "fresh seq 0 still no frame");
        Check(fresh.LastFrame() == Frame{}, "fresh seq 0 default frame");
        out = MakeSentinel();
        CheckStatus(fresh.SubmitSample(Sample{1, 1000, 0, 0, 8}, out), Status::InvalidButtons,
            "fresh bad buttons");
        Check(!fresh.HasFrame(), "fresh bad buttons still no frame");
        out = MakeSentinel();
        CheckStatus(fresh.SubmitSample(Sample{1, 1000, 129, 0, 0}, out), Status::AxisOutOfRange,
            "fresh bad axis");
        Check(!fresh.HasFrame(), "fresh bad axis still no frame");
    }

    // Equal Tick with a new Seq is valid (already proven by trace rows 3 and
    // 10; this isolated pair pins the rule on a fresh owner).
    {
        NativeSourcePad eq;
        Frame first;
        Frame second;
        CheckStatus(eq.SubmitSample(Sample{1, 500, 0, 0, 0}, first), Status::Ok, "equal-time first");
        CheckFrameFields(first, 0, 0, 0, "equal-time first fields");
        CheckStatus(eq.SubmitSample(Sample{2, 500, 0, 0, 1}, second), Status::Ok, "equal-time second");
        CheckFrameFields(second, 1, 1, 0, "equal-time second fields");
    }

    // Valid boundaries on fresh owners: Buttons 7 and axes +-128.
    {
        NativeSourcePad b7;
        Frame out;
        CheckStatus(b7.SubmitSample(Sample{1, 1000, 0, 0, 7}, out), Status::Ok, "buttons 7 valid");
        CheckFrameFields(out, 7, 7, 0, "buttons 7 fields");
        CheckFormatEquals(out, "1,1000,0,0,7,7,0", "buttons 7 format");
    }
    {
        NativeSourcePad ax;
        Frame out;
        CheckStatus(ax.SubmitSample(Sample{1, 1000, 128, 128, 0}, out), Status::Ok, "axes 128 valid");
        CheckFormatEquals(out, "1,1000,128,128,0,0,0", "axes 128 format");
    }
    {
        NativeSourcePad ax;
        Frame out;
        CheckStatus(ax.SubmitSample(Sample{1, 1000, -128, -128, 0}, out), Status::Ok, "axes -128 valid");
        CheckFormatEquals(out, "1,1000,-128,-128,0,0,0", "axes -128 format");
    }

    // Repeated LastFrame queries freeze the sample without expiry.
    {
        const Frame frozen = pad.LastFrame();
        for (unsigned i = 0; i < 100; ++i) {
            char message[64];
            std::snprintf(message, sizeof(message), "freeze %u", i);
            Check(pad.LastFrame() == frozen, message);
        }
    }

    // Recovery after rejections still advances normally.
    {
        Frame out;
        CheckStatus(pad.SubmitSample(Sample{11, 1090, 0, 0, 1}, out), Status::Ok, "recovery ok");
        CheckFrameFields(out, 1, 1, 0, "recovery fields");
        Check(pad.LastFrame() == out, "recovery last");
    }

    if (traceMode) {
        std::printf("native-source-pad-trace-v1\n");
        for (unsigned i = 0; i < 11; ++i) {
            const auto buffer = NativeSourcePad::FormatFrame(collected[i]);
            std::printf("%s\n", buffer.data());
        }
        return 0;
    }

    std::printf("native-source-pad-ok checks=%zu profile=Pad38-43,215/Pad402-409,880,1022/"
                "ControllerState/owned-integrity/digital-default-deadzone0.3-trunc128/"
                "no-hardware/no-full-device\n",
        s_Checks);
    return 0;
}
