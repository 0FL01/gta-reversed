#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// Pinned GTA:SA 1.0 default command schema. This describes bytecode forms;
// NativeScriptSemanticCoverage separately says whether this owned VM has the
// command behavior. A known form is never permission to execute it as a NOP.
enum class NativeScriptOperandType : std::uint8_t {
    Integer,
    Float,
    String,
    Output,
    FloatOutput,
    InOutInteger,
    InOutFloat,
};

enum class NativeScriptSemanticCoverage : std::uint8_t {
    Implemented,
    Unsupported,
};

struct NativeScriptOpcodeSchema {
    std::uint16_t Opcode = 0;
    std::array<NativeScriptOperandType, 16> Operands{};
    std::uint8_t OperandCount = 0;
    NativeScriptSemanticCoverage Semantics = NativeScriptSemanticCoverage::Unsupported;
};

std::span<const NativeScriptOpcodeSchema> NativeScriptSchemaEntries() noexcept;
const NativeScriptOpcodeSchema* NativeScriptLookupSchema(std::uint16_t opcode) noexcept;
std::string_view NativeScriptSchemaRevision() noexcept;
std::string_view NativeScriptSchemaSha256() noexcept;
std::string_view NativeScriptSchemaVersion() noexcept;
