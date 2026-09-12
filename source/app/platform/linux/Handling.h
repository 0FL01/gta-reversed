// Handling: handling.cfg vehicle row for the Linux native track (R6p).
// Parses data/handling.cfg through OS_File* (single vehicle row, model name
// matched UPPERCASE) and exposes the file columns per the game spec
// (read-only, NOT linked): game_sa/tHandlingData.h column order +
// game_sa/cHandlingDataMgr.cpp InitFromData sscanf order (mass, turnMass,
// dragMult, centreOfMass xyz, percentSubmerged, tractionMult/Loss/Bias,
// nNumberOfGears, fMaxVelocity (N, km/h), fEngineAcceleration (O, ms-2),
// fEngineInertia, driveType, engineType, ...) and the units header inside
// handling.cfg itself ("velocity in Km/h", "acceleration/deceleration in
// ms-2"). SI conversion for the drive integrator: VMAXms = VMAXkmh *
// (1000/3600); the 0.277778 factor is the game's own
// (cHandlingDataMgr.cpp:11 VELOCITY_CONST = 0.277778f / 50.f — the /50 is
// per-frame game-unit scaling, unused in the SI integrator). Acceleration
// is used as-is (file is already ms-2). No hardcoded vehicle numbers in
// this TU: every value below comes from handling bytes.
#pragma once

#include <cstddef>
#include <cstdint>

struct HandlingParams {
    char model[64] = {}; // UPPERCASE row name as matched (e.g. "LANDSTAL")
    char requested[64] = {}; // argv model as passed (e.g. "landstal")
    // File values (raw columns N/B/D/M/O...).
    double mass = 0.0; // (B) fMass, Kg
    double turnMass = 0.0; // (C) fTurnMass
    double drag = 0.0; // (D) fDragMult
    double vmaxFileKmh = 0.0; // (N) TransmissionData.fMaxVelocity, Km/h
    double accelFile = 0.0; // (O) TransmissionData.fEngineAcceleration, ms-2
    double inertia = 0.0; // (P) fEngineInertia
    int gears = 0; // (M) nNumberOfGears
    char driveType = '\0'; // (Q) F/R/4
    char engineType = '\0'; // (R) P/D/E
    double centreOfMass[3]{}; // (E/F/G)
    double percentSubmerged = 0.0; // (H)
    double tractionMult = 0.0, tractionLoss = 0.0, tractionBias = 0.0; // (I/J/K)
    double brakeDeceleration = 0.0, brakeBias = 0.0; // (S/T)
    bool Abs = false; // (U)
    double steeringLockDegrees = 0.0; // (V)
    std::uint32_t HandlingFlags = 0; // (ag)
    double suspensionForce=0, suspensionDamping=0, suspensionHighSpeedDamping=0;
    double suspensionUpper=0, suspensionLower=0, suspensionBias=0, suspensionAntiDive=0;
    double seatOffset=0, collisionDamageMultiplier=0;
    std::uint32_t ModelFlags = 0; // (af)
    // SI values for the integrator (derived, formula logged by caller).
    double vmaxMs = 0.0; // vmaxFileKmh * (1000/3600)
    double accelSi = 0.0; // == accelFile (ms-2)
    // Verbatim file tokens (byte proof that the log matches grep).
    char massTok[32] = {};
    char dragTok[32] = {};
    char vmaxTok[32] = {};
    char accelTok[32] = {};
    char gearsTok[32] = {};
    char handlingFlagsTok[32] = {}; // (ag), validated hex source token
};

// Loads the handling row for `model` (case-insensitive, matched UPPERCASE)
// relative to gameDir (sets the OS_File path offset itself, like TimeCycle).
// Returns false with a message in err on any failure (missing file/row,
// bad columns). Never invents values.
bool Handling_Load(const char* gameDir, const char* model, HandlingParams& out, char* err,
                   std::size_t errSize);
