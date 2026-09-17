#pragma once

#include <array>
#include <cstddef>
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
    // One tag0-terminated START_NEW_* parameter. Variable operands carry raw
    // 32-bit union data, so their int/float intent is not encoded in bytecode.
    Argument,
    DebugString,
    DebugString128,
    IgnoredString,
    StringOutput,
};

enum class NativeScriptSemanticCoverage : std::uint8_t {
    Implemented,
    Unsupported,
};

constexpr std::size_t NativeScriptMaxFixedOperands = 18;
// START_NEW_SCRIPT/START_NEW_STREAMED_SCRIPT have one fixed operand followed
// by up to all 32 source local-parameter slots and a tag-zero terminator.
constexpr std::size_t NativeScriptMaxOperands = 33;

struct NativeScriptOpcodeSchema {
    std::uint16_t Opcode = 0;
    std::array<NativeScriptOperandType, NativeScriptMaxFixedOperands> Operands{};
    std::uint8_t OperandCount = 0;
    NativeScriptSemanticCoverage Semantics = NativeScriptSemanticCoverage::Unsupported;
    bool VariadicArguments = false;
};

std::span<const NativeScriptOpcodeSchema> NativeScriptSchemaEntries() noexcept;
const NativeScriptOpcodeSchema* NativeScriptLookupSchema(std::uint16_t opcode) noexcept;
std::string_view NativeScriptSchemaRevision() noexcept;
std::string_view NativeScriptSchemaSha256() noexcept;
std::string_view NativeScriptSchemaVersion() noexcept;
