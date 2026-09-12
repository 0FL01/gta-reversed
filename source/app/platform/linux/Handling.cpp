// Handling implementation: one handling.cfg row via OS_File*.
// See Handling.h for the spec contract. Token indices follow
// tHandlingData::InitFromData (cHandlingDataMgr.cpp) after the name token:
//   1 mass  2 turnMass  3 drag  4/5/6 centreOfMass xyz  7 percentSubmerged
//   8 tractionMult  9 tractionLoss  10 tractionBias  11 nGears
//   12 fMaxVelocity(N)  13 fEngineAcceleration(O)  14 fEngineInertia
//   15 driveType(Q)  16 engineType(R)  17 brakeDecel ...
// Only the fields needed for the R6p log + integrator are kept; the rest
// are validated for presence (a short row is a fail, never a guess).

#include "app/platform/linux/Handling.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

#ifndef __stdcall
#define __stdcall
#endif

#include "oswrapper/oswrapper.h"

namespace {

void SetErr(char* err, std::size_t errSize, const char* msg) {
    if (!err || errSize == 0) {
        return;
    }
    (void)std::snprintf(err, errSize, "%s", msg ? msg : "handling error");
}

void UpperInPlace(char* s, std::size_t n) {
    for (std::size_t i = 0; i < n && s[i]; ++i) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] = static_cast<char>(s[i] - 32);
        }
    }
}

std::string UpperCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 32);
        }
    }
    return o;
}

bool ParseDouble(const std::string& tok, double& out) {
    if (tok.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end && *end == '\0';
}

bool ParseInt(const std::string& tok, int& out) {
    if (tok.empty()) {
        return false;
    }
    char* end = nullptr;
    long v = std::strtol(tok.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

} // namespace

bool Handling_Load(const char* gameDir, const char* model, HandlingParams& out, char* err,
                   std::size_t errSize) {
    out = HandlingParams{};
    if (!gameDir || !gameDir[0]) {
        SetErr(err, errSize, "no game dir");
        return false;
    }
    if (!model || !model[0]) {
        SetErr(err, errSize, "no model");
        return false;
    }
    std::string want = UpperCopy(model);
    if (want.empty() || want.size() >= sizeof(out.model)) {
        SetErr(err, errSize, "bad model name");
        return false;
    }
    (void)std::snprintf(out.requested, sizeof(out.requested), "%s", model);

    OS_SetFilePathOffset(gameDir);
    void* file = nullptr;
    if (OS_FileOpen(FILE_DATA_AREA_DEFAULT, &file, "data/handling.cfg", FILE_ACCESS_READ) != 0 ||
        !file) {
        SetErr(err, errSize, "cannot open data/handling.cfg");
        return false;
    }
    int32 size = OS_FileSize(file);
    if (size <= 0) {
        OS_FileClose(file);
        SetErr(err, errSize, "empty data/handling.cfg");
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(size));
    int32 rc = OS_FileRead(file, buf.data(), size);
    OS_FileClose(file);
    if (rc != 0) {
        SetErr(err, errSize, "cannot read data/handling.cfg");
        return false;
    }
    // Split into lines (keep raw bytes; strip trailing CR only).
    bool found = false;
    std::vector<std::string> matchToks;
    size_t pos = 0;
    while (pos < buf.size() && !found) {
        size_t end = pos;
        while (end < buf.size() && buf[end] != '\n') {
            ++end;
        }
        std::string line(buf.data() + pos, end - pos);
        pos = end < buf.size() ? end + 1 : buf.size();
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) {
            ++b;
        }
        if (b >= line.size()) {
            continue; // blank
        }
        char c0 = line[b];
        if (c0 == ';') {
            continue; // comment / units header / field docs / ";the end"
        }
        if (c0 == '!' || c0 == '$' || c0 == '%' || c0 == '^') {
            continue; // bike/flying/boat/animgroup rows (not vehicle handling)
        }
        // Tokenize on whitespace (spaces/tabs, like the game loader).
        std::vector<std::string> toks;
        {
            size_t i = b;
            while (i < line.size()) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                    ++i;
                }
                if (i >= line.size()) {
                    break;
                }
                size_t j = i;
                while (j < line.size() && line[j] != ' ' && line[j] != '\t') {
                    ++j;
                }
                toks.emplace_back(line.substr(i, j - i));
                i = j;
            }
        }
        if (toks.empty()) {
            continue;
        }
        if (UpperCopy(toks[0]) != want) {
            continue;
        }
        // Vehicle row needs the complete source automobile handling columns,
        // including model/handling flags (af/ag at indices 31/32).
        if (toks.size() < 33) {
            SetErr(err, errSize, "short handling row");
            return false;
        }
        matchToks = toks;
        found = true;
    }
    if (!found) {
        char msg[128];
        (void)std::snprintf(msg, sizeof(msg), "model '%s' not found in handling.cfg",
                            want.c_str());
        SetErr(err, errSize, msg);
        return false;
    }
    // Column map (see file header above): toks[0]=name.
    double mass = 0, turnMass = 0, drag = 0, com[3]{}, submerged = 0, traction[3]{},
        vmax = 0, accel = 0, inertia = 0, brake=0, brakeBias=0, steering=0;
    int gears = 0, abs=0;
    if (!ParseDouble(matchToks[1], mass) || !ParseDouble(matchToks[2], turnMass) ||
        !ParseDouble(matchToks[3], drag) || !ParseDouble(matchToks[4],com[0]) ||
        !ParseDouble(matchToks[5],com[1]) || !ParseDouble(matchToks[6],com[2]) ||
        !ParseDouble(matchToks[7],submerged) || !ParseDouble(matchToks[8],traction[0]) ||
        !ParseDouble(matchToks[9],traction[1]) || !ParseDouble(matchToks[10],traction[2]) ||
        !ParseInt(matchToks[11], gears) ||
        !ParseDouble(matchToks[12], vmax) || !ParseDouble(matchToks[13], accel) ||
        !ParseDouble(matchToks[14], inertia) || !ParseDouble(matchToks[17],brake) ||
        !ParseDouble(matchToks[18],brakeBias) || !ParseInt(matchToks[19],abs) ||
        !ParseDouble(matchToks[20],steering)) {
        SetErr(err, errSize, "bad handling numbers");
        return false;
    }
    if (matchToks[15].size() != 1 || matchToks[16].size() != 1) {
        SetErr(err, errSize, "bad handling drive/engine type");
        return false;
    }
    if (!(mass > 0.0) || !(turnMass > 0.0) || drag < 0 || submerged < 0 ||
        !(traction[0] > 0) || !(traction[1] > 0) || traction[2] < 0 || traction[2] > 1 ||
        !(vmax > 0.0) || !(accel > 0.0) || !(brake > 0) || brakeBias < 0 || brakeBias > 1 ||
        (abs!=0 && abs!=1) || !(steering > 0) || steering > 90 || gears < 1 || gears > 6) {
        SetErr(err, errSize, "handling values out of range");
        return false;
    }
    (void)std::snprintf(out.model, sizeof(out.model), "%s", want.c_str());
    out.mass = mass;
    out.turnMass = turnMass;
    out.drag = drag;
    out.gears = gears;
    out.vmaxFileKmh = vmax;
    out.accelFile = accel;
    out.inertia = inertia;
    out.driveType = matchToks[15][0];
    out.engineType = matchToks[16][0];
    std::copy_n(com,3,out.centreOfMass);
    out.percentSubmerged=submerged; out.tractionMult=traction[0];
    out.tractionLoss=traction[1]; out.tractionBias=traction[2];
    out.brakeDeceleration=brake; out.brakeBias=brakeBias; out.Abs=abs!=0;
    out.steeringLockDegrees=steering;
    // SI: km/h -> m/s via the game's own 0.277778 (1000/3600) factor;
    // accel file is already ms-2 (handling.cfg units header).
    out.vmaxMs = vmax * (1000.0 / 3600.0);
    out.accelSi = accel;
    (void)std::snprintf(out.massTok, sizeof(out.massTok), "%s", matchToks[1].c_str());
    (void)std::snprintf(out.dragTok, sizeof(out.dragTok), "%s", matchToks[3].c_str());
    (void)std::snprintf(out.vmaxTok, sizeof(out.vmaxTok), "%s", matchToks[12].c_str());
    (void)std::snprintf(out.accelTok, sizeof(out.accelTok), "%s", matchToks[13].c_str());
    (void)std::snprintf(out.gearsTok, sizeof(out.gearsTok), "%s", matchToks[11].c_str());
    char* end=nullptr;
    const auto flags=std::strtoul(matchToks[31].c_str(),&end,16);
    if (!end || *end!='\0') { SetErr(err,errSize,"bad handling flags"); return false; }
    (void)std::snprintf(out.handlingFlagsTok, sizeof(out.handlingFlagsTok), "%s", matchToks[31].c_str());
    out.HandlingFlags=std::uint32_t(flags);
    (void)UpperInPlace(out.model, sizeof(out.model));
    return true;
}
