#include "app/platform/linux/NativeScriptSchema.h"

namespace {
using O = NativeScriptOperandType;
using C = NativeScriptSemanticCoverage;

// Sanny Builder Library sa/sa.json, default extension, pinned rather than read
// at runtime. Behavioral ownership is verified independently against game_sa.
constexpr NativeScriptOpcodeSchema s_Schema[]{
    {0x0000, {}, 0, C::Implemented},
    {0x0001, {O::Integer}, 1, C::Implemented}, {0x0002, {O::Integer}, 1, C::Implemented},
    {0x0004, {O::Output, O::Integer}, 2, C::Implemented}, {0x0005, {O::FloatOutput, O::Float}, 2, C::Implemented},
    {0x0006, {O::Output, O::Integer}, 2, C::Implemented}, {0x0007, {O::FloatOutput, O::Float}, 2, C::Implemented},
    {0x0086, {O::FloatOutput, O::Float}, 2, C::Implemented},
    {0x0008, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x0009, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x000A, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x000B, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x000C, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x000D, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x000E, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x000F, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x0010, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x0011, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x0012, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x0013, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x0014, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x0015, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x0016, {O::InOutInteger, O::Integer}, 2, C::Implemented}, {0x0017, {O::InOutFloat, O::Float}, 2, C::Implemented},
    {0x001A, {O::Integer, O::Integer}, 2, C::Implemented},
    {0x004D, {O::Integer}, 1, C::Implemented}, {0x004E, {}, 0, C::Implemented},
    {0x00D6, {O::Integer}, 1, C::Implemented}, {0x03A4, {O::String}, 1, C::Implemented},
    {0x016A, {O::Integer, O::Integer}, 2, C::Implemented},
    {0x042C, {O::Integer}, 1, C::Implemented}, {0x030D, {O::Integer}, 1, C::Implemented},
    {0x0997, {O::Integer}, 1, C::Implemented}, {0x01F0, {O::Integer}, 1, C::Implemented},
    {0x0111, {O::Integer}, 1, C::Implemented}, {0x00C0, {O::Integer, O::Integer}, 2, C::Implemented},
    {0x04E4, {O::Float, O::Float}, 2, C::Implemented},
    {0x03CB, {O::Float, O::Float, O::Float}, 3, C::Implemented},
    {0x062A, {O::Integer, O::Float}, 2, C::Implemented},
    {0x0629, {O::Integer, O::Integer}, 2, C::Implemented},
    {0x0053, {O::Integer, O::Float, O::Float, O::Float, O::Output}, 5, C::Implemented},
    {0x06CF, {O::Integer}, 1, C::Implemented},
    {0x0746, {O::Integer, O::Integer, O::Integer}, 3, C::Implemented},
    {0x07AF, {O::Integer, O::Output}, 2, C::Implemented},
    {0x01F5, {O::Integer, O::Output}, 2, C::Implemented},
    {0x0373, {}, 0, C::Implemented}, {0x0173, {O::Integer, O::Float}, 2, C::Implemented},
    {0x0417, {O::Integer}, 1, C::Implemented}, {0x06C8, {O::Integer}, 1, C::Implemented},
    {0x0517, {O::Float, O::Float, O::Float, O::String, O::Output}, 5, C::Implemented},
    {0x0518, {O::Float, O::Float, O::Float, O::Integer, O::String, O::Output}, 6, C::Implemented},
    {0x0570, {O::Float, O::Float, O::Float, O::Integer, O::Output}, 5, C::Implemented},
    {0x04CE, {O::Float, O::Float, O::Float, O::Integer, O::Output}, 5, C::Implemented},
    {0x018B, {O::Integer, O::Integer}, 2, C::Implemented},
    {0x09B4, {O::Float, O::Float, O::Float, O::Integer, O::Integer}, 5, C::Implemented},
    {0x02B9, {O::String}, 1, C::Implemented},
    {0x016C, {O::Float, O::Float, O::Float, O::Float, O::Integer}, 5, C::Implemented},
    {0x016D, {O::Float, O::Float, O::Float, O::Float, O::Integer}, 5, C::Implemented},
    {0x0213, {O::Integer, O::Integer, O::Float, O::Float, O::Float, O::Output}, 6, C::Implemented},
    {0x0214, {O::Integer}, 1, C::Implemented}, {0x0215, {O::Integer}, 1, C::Implemented},
    {0x014B, {O::Float, O::Float, O::Float, O::Float, O::Integer, O::Integer, O::Integer,
              O::Integer, O::Integer, O::Integer, O::Integer, O::Integer, O::Output}, 13, C::Implemented},
    {0x014C, {O::Integer, O::Integer}, 2, C::Implemented},
    // First strict P4 frontier. Full form is classified, but World::AddStuntJump
    // and its registry/runtime owner are deliberately not fabricated here.
    {0x0814, {O::Float, O::Float, O::Float, O::Float, O::Float, O::Float,
              O::Float, O::Float, O::Float, O::Float, O::Float, O::Float,
              O::Float, O::Float, O::Float, O::Integer}, 16, C::Unsupported},
};
}

std::span<const NativeScriptOpcodeSchema> NativeScriptSchemaEntries() noexcept { return s_Schema; }

const NativeScriptOpcodeSchema* NativeScriptLookupSchema(std::uint16_t opcode) noexcept {
    for (const auto& schema : s_Schema) {
        if (schema.Opcode == opcode) return &schema;
    }
    return nullptr;
}

std::string_view NativeScriptSchemaRevision() noexcept { return "53ed1c2561bf6ca70dc16afca5d8f3a406066158"; }
std::string_view NativeScriptSchemaSha256() noexcept { return "797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671"; }
std::string_view NativeScriptSchemaVersion() noexcept { return "1.65"; }
