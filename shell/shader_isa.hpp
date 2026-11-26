// Reading a console (RDNA2-class) shader: the instruction words and their fields.
//
// A console shader is the GPU's own machine code, not a PC shader bytecode. This reader sits on top of
// shader_walk.*: the walk says where each instruction starts, which format it is, its opcode number and how
// many words it has; this file then reads the FIELDS of that instruction for every format the walk knows.
// Nothing here is guessed: a field whose position this project could not establish from a public source for
// this GPU generation is left raw and marked, and the translator refuses instructions it does not implement.
//
// Where the layouts come from (public sources, read for this work; cited per format in shader_isa.cpp):
//   * LLVM's AMDGPU backend, the encoding classes for the GFX10 generation: VOP1Instructions.td,
//     VOP2Instructions.td, VOPCInstructions.td, VOPInstructions.td (VOP3a_gfx10, VOP3Pe_gfx10), SOPInstructions.td
//     (SOP1_Real, SOP2_Real32, SOPK_Real32, SOPC_Real, SOPP_Real_32), DSInstructions.td
//     (Base_DS_Real_gfx6_gfx7_gfx10_gfx11_gfx12_gfx13), FLATInstructions.td (FLAT_Real, FLAT_Real_gfx10),
//     BUFInstructions.td (Base_MUBUF_Real_gfx6_gfx7_gfx10_gfx11, MTBUF_Real_gfx10), SMInstructions.td
//     (SMEM_Real_10Plus_common), MIMGInstructions.td (MIMGe_gfx10), and EXP in SIInstrFormats.td (EXPe).
//     LLVM is Apache-2.0/MIT-compatible; no LLVM code is copied here, only the published bit positions.
//   * The operand numbering in AMDGPUDisassembler.cpp: a 9-bit source field with bit 8 set names a VGPR
//     (256 + register); otherwise 0..SGPR_MAX names a scalar register, 128..254 an inline constant and 255 the
//     literal constant word that follows. decodeSpecialReg32 there names the scalar codes 102..111 and 124..127
//     (flat scratch, VCC, TBA/TMA, M0, SGPR_NULL, EXEC); 112..123 are the trap temporaries.
//   * Inline constant values from the same file (getInlineImmVal32, decodeIntImmed) and AMDGPUBaseInfo.cpp.
//
// The formats the walk reports with a top six-bit value that is no format of this generation stay unknown and
// stop a decode; they are never approximated.
#ifndef WHIWHI_SHELL_SHADER_ISA_HPP
#define WHIWHI_SHELL_SHADER_ISA_HPP

#include "shader_walk.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

// What a shader is for. Read from the shader's own table first; the translator needs it for the SPIR-V entry
// point, and a stage it cannot translate is refused with a reason.
enum class ShaderStage {
    kUnknown,
    kCompute,
    kVertex,
    kHull,
    kDomain,
    kGeometry,
    kPixel,
};

const char* shader_stage_name(ShaderStage stage);

// The format's name ("SOP2", "VOP3", ...). Same names as walk_format_name; kept as its own entry point because
// callers that only include this header should not have to know about the walk.
const char* isa_format_name(WalkFormat format);

// Where an operand's value comes from. The number ranges are the public source operand encoding described above.
enum class OperandKind {
    kNone,
    kSgpr,             // a scalar register in the ordinary file: codes 0..101 on this generation
    kVgpr,             // a vector register: codes 256..511, kept as 0..255
    kInline,           // an inline constant carried in the instruction word: codes 128..254
    kSpecialRegister,  // a scalar code naming a special register (102..127), kept as its raw number
    kLiteral,          // code 255: the value is the constant word that follows the instruction
};

struct Operand {
    OperandKind kind = OperandKind::kNone;
    std::uint32_t index = 0;  // register number, or the raw inline/special code
    // The 32-bit pattern the operand contributes: the inline constant's value, or the literal word. Left 0 and
    // value_known false for an inline code this build does not name; the translator refuses those.
    std::uint32_t value = 0;
    bool value_known = false;
};

// ---- per-format extra fields -----------------------------------------------------------------------------
//
// Which word a field lives in is written next to it. Every struct is plain data; a decoder fills only the
// struct that belongs to the instruction's format.

// VOP3 (encoding 110101) and VOP3P (encoding 110011), both two words. The destination and the modifiers are in
// the first word, the three 9-bit sources in the second (LLVM VOP3a_gfx10 / VOP3Pe_gfx10).
struct Vop3Fields {
    // word 0 bits 7..0: the destination field. A vector register number, except for a compare (VOP3 opcode
    // below 0x100), where it is a scalar code: the register pair, VCC (106) or EXEC (126) that gets the mask.
    std::uint32_t vdst = 0;
    // How many of the three source fields the instruction uses, from the public operand lists
    // (vop3_source_count). The fields it does not use are not counted in Instruction::source_count.
    // `sources_established` is false for an opcode the public table of this generation does not list; all
    // three fields are then kept as they stand.
    std::uint8_t sources_used = 3;
    bool sources_established = false;
    // The VOP3 instructions with a second, scalar result (carry out; the mask of v_div_scale; the high part
    // of v_mad_*64): word 0 bits 14..8 hold that result's scalar code, and are then NOT abs / op_sel
    // (LLVM VOP3be: Inst{14-8} = sdst).
    bool has_scalar_result = false;
    std::uint8_t scalar_result = 0;
    bool clamp = false;      // word 0 bit 15
    // word 0 bits 10..8, one bit per source (bit 8 = src0): VOP3 = absolute-value modifier;
    // VOP3P = negate-the-high-half modifier.
    std::uint8_t abs_or_neg_hi = 0;
    // word 0 bits 14..11: VOP3 = op_sel[3:0] for the instructions that have it (0 otherwise);
    // VOP3P = op_sel[2:0] in bits 2..0 and op_sel_hi[2] in bit 3.
    std::uint8_t op_sel = 0;
    // word 1 bits 28..27: VOP3 = output modifier (omod); VOP3P = op_sel_hi[1:0].
    std::uint8_t omod_or_opsel_hi = 0;
    // word 1 bits 31..29, one bit per source (bit 29 = src0): negate the source.
    std::uint8_t neg = 0;
};

// DS (110110), two words: local data share.
struct DsFields {
    std::uint32_t offset0 = 0;  // word 0 bits 7..0
    std::uint32_t offset1 = 0;  // word 0 bits 15..8
    bool gds = false;           // word 0 bit 17
    std::uint32_t addr = 0;     // word 1 bits 7..0: address VGPR
    std::uint32_t data0 = 0;    // word 1 bits 15..8
    std::uint32_t data1 = 0;    // word 1 bits 23..16
    std::uint32_t vdst = 0;     // word 1 bits 31..24: destination VGPR (for a load)
};

// FLAT (110111), two words: flat / global / scratch memory.
struct FlatFields {
    std::uint32_t offset = 0;  // word 0 bits 11..0 (12-bit unsigned offset on this generation)
    bool dlc = false;          // word 0 bit 12
    std::uint32_t seg = 0;     // word 0 bits 15..14: address segment
    bool glc = false;          // word 0 bit 16
    bool slc = false;          // word 0 bit 17
    std::uint32_t vaddr = 0;   // word 1 bits 7..0
    std::uint32_t vdata = 0;   // word 1 bits 15..8
    std::uint32_t saddr = 0;   // word 1 bits 22..16
    bool acc = false;          // word 1 bit 23
    std::uint32_t vdst = 0;    // word 1 bits 31..24
};

// MUBUF (111000), two words: untyped buffer.
struct MubufFields {
    std::uint32_t offset = 0;  // word 0 bits 11..0
    bool offen = false;        // word 0 bit 12
    bool idxen = false;        // word 0 bit 13
    bool glc = false;          // word 0 bit 14
    bool dlc = false;          // word 0 bit 15
    bool lds = false;          // word 0 bit 16
    std::uint32_t vaddr = 0;   // word 1 bits 7..0
    std::uint32_t vdata = 0;   // word 1 bits 15..8
    // word 1 bits 20..16: the encoded resource field, srsrc >> 2 (a resource base SGPR index is a multiple of 4)
    std::uint32_t srsrc = 0;
    bool slc = false;          // word 1 bit 22
    bool tfe = false;          // word 1 bit 23
    std::uint32_t soffset = 0; // word 1 bits 31..24
};

// MTBUF (111010), two words: typed buffer. The 7-bit combined format field replaces the separate data/number
// format fields of the older generation.
struct MtbufFields {
    std::uint32_t offset = 0;  // word 0 bits 11..0
    bool offen = false;        // word 0 bit 12
    bool idxen = false;        // word 0 bit 13
    bool glc = false;          // word 0 bit 14
    bool dlc = false;          // word 0 bit 15
    bool slc = false;          // word 1 bit 22
    bool tfe = false;          // word 1 bit 23
    std::uint32_t format = 0;  // word 0 bits 25..19
    std::uint32_t vaddr = 0;   // word 1 bits 7..0
    std::uint32_t vdata = 0;   // word 1 bits 15..8
    std::uint32_t srsrc = 0;   // word 1 bits 20..16 (srsrc >> 2)
    std::uint32_t soffset = 0; // word 1 bits 31..24
};

// SMEM (111101), two words: scalar memory. The 7-bit field at word 0 bits 12..6 is the destination for a load
// and the data source for a store, depending on the opcode; it is exposed as the raw field.
struct SmemFields {
    std::uint32_t sbase = 0;   // word 0 bits 5..0 (sbase >> 1)
    std::uint32_t sdst = 0;    // word 0 bits 12..6
    bool dlc = false;          // word 0 bit 14
    bool glc = false;          // word 0 bit 16
    std::uint32_t offset = 0;  // word 1 bits 20..0 (21-bit offset)
    std::uint32_t soffset = 0; // word 1 bits 31..25
};

// EXP (111110), two words: export to the colour/parameter interface.
struct ExpFields {
    std::uint32_t en = 0;   // word 0 bits 3..0: export enable mask
    std::uint32_t tgt = 0;  // word 0 bits 9..4: export target
    bool compr = false;     // word 0 bit 10
    bool done = false;      // word 0 bit 11
    bool vm = false;        // word 0 bit 12
    std::uint32_t src[4] = {};  // word 1 bits 7..0, 15..8, 23..16, 31..24
};

// MIMG (111100): two words plus nsa extra address words (nsa counted in word 0 bits 2..1).
struct MimgFields {
    std::uint32_t nsa = 0;     // word 0 bits 2..1: number of extra address words
    std::uint32_t dim = 0;     // word 0 bits 5..3
    bool dlc = false;          // word 0 bit 7
    std::uint32_t dmask = 0;   // word 0 bits 11..8
    bool unorm = false;        // word 0 bit 12
    bool glc = false;          // word 0 bit 13
    bool r128 = false;         // word 0 bit 15
    bool tfe = false;          // word 0 bit 16
    bool lwe = false;          // word 0 bit 17
    bool slc = false;          // word 0 bit 25
    std::uint32_t vaddr = 0;   // word 1 bits 7..0: first address VGPR
    std::uint32_t vdata = 0;   // word 1 bits 15..8
    std::uint32_t srsrc = 0;   // word 1 bits 20..16 (srsrc >> 2)
    std::uint32_t ssamp = 0;   // word 1 bits 25..21 (ssamp >> 2)
    bool a16 = false;          // word 1 bit 30
    bool d16 = false;          // word 1 bit 31
};

// VINTRP (110010), one word: attribute interpolation.
//
// The source field means two different things. In the two interpolation steps (opcodes 0 and 1) it is a vector
// register number, and the instruction's src[0] is that register. In the parameter move (opcode 2) it is not a
// register at all but the number that selects which of the attribute's three values is taken, 0, 1 or 2, written
// straight into the field (Mesa ACO aco_assembler.cpp emit_vintrp_instruction; LLVM AMDGPUInstPrinter.cpp
// printInterpSlot prints 0 as p10, 1 as p20, 2 as p0). The move therefore has no source operand.
struct VintrpFields {
    std::uint32_t vsrc = 0;     // word 0 bits 7..0: a vector register number, or the move's selector
    std::uint32_t attrchan = 0; // word 0 bits 9..8
    std::uint32_t attr = 0;     // word 0 bits 15..10
    std::uint32_t vdst = 0;     // word 0 bits 25..18
    bool vsrc_is_selector = false;  // opcode 2: vsrc is the selector, not a register
};

// One decoded instruction.
struct Instruction {
    std::uint32_t pc = 0;         // byte offset of the instruction inside the body
    std::uint8_t word_count = 0;  // 1 to 5, as the walk reports it
    WalkFormat format = WalkFormat::kUnknown;
    std::uint16_t opcode = 0;     // opcode number inside the format, as the walk reports it
    std::uint32_t words[5] = {};  // the instruction's own words, first word first
    const char* name = nullptr;   // mnemonic when this build names the opcode from a public table, else null

    bool has_destination = false;
    Operand dst;
    Operand src[3];
    std::uint8_t source_count = 0;

    // A 16-bit immediate in the instruction word itself (SOPK, SOPP). Not a constant word.
    bool has_immediate = false;
    std::uint32_t immediate = 0;

    // A 32-bit constant word that follows the instruction, present when the walk's length says so (a source
    // field held 255, or the opcode always carries one). All sources that held 255 read this word.
    bool has_constant_word = false;
    std::uint32_t constant_word = 0;

    // MIMG's extra address words. Kept raw: this reader does not establish from its sources which bits hold the
    // register number, so no field is decoded from them.
    std::uint8_t address_word_count = 0;
    std::uint32_t address_words[3] = {};

    Vop3Fields vop3;
    DsFields ds;
    FlatFields flat;
    MubufFields mubuf;
    MtbufFields mtbuf;
    SmemFields smem;
    ExpFields exp;
    MimgFields mimg;
    VintrpFields vintrp;
};

// One decoded shader body.
struct DecodedShader {
    bool ok = false;
    ShaderStage stage = ShaderStage::kUnknown;
    std::vector<Instruction> instructions;
    // The first place the reader could not go on, if any.
    bool has_bad_instruction = false;
    std::uint32_t bad_pc = 0;
    std::uint32_t bad_word = 0;
    std::string bad_reason;
    std::uint32_t entry_pc = 0;
    // What the program walk found: did it reach an end-of-program word, and where.
    bool walk_landed = false;
    std::size_t end_word_index = 0;
};

// Decode every instruction of a shader body. `code` is the shader's instruction stream (the caller has already
// found where the stream starts and how long it is); bytes are little-endian 32-bit words. The body is decoded
// only when the program walk from the first word lands on an end-of-program word that is the body's last word;
// anything else is a bad instruction with the reason.
DecodedShader decode_shader(const std::uint8_t* code, std::size_t size, ShaderStage stage,
                            std::uint32_t entry_pc = 0);

// The mnemonic for a format and opcode, when this build names it from a public opcode table; null otherwise.
const char* instruction_name(WalkFormat format, std::uint16_t opcode);

// The name of a scalar source code that names a special register on this generation (102..127); null for a code
// this build does not name (the number is still kept as it is).
const char* special_scalar_register_name(std::uint32_t code);

// A human-readable one-line description built from the decoded fields (never from the bytes as text). Used in
// diagnostics and tests; the shell never writes it to a file.
std::string describe_instruction(const Instruction& instruction);

// Translate an inline-constant code into the 32-bit pattern it supplies. Returns false for a code this build
// does not name (the caller then refuses the shader rather than guessing).
bool inline_constant(std::uint32_t code, std::uint32_t* value);

// What the public instruction table of this GPU generation says about a VOP3 opcode (Mesa ACO's opcode table,
// the gfx10 column, with its rule for the two-word form: a compare keeps its number, a two-source instruction
// is its number + 0x100, a one-source instruction its number + 0x180; the rest are VOP3's own).
//
// vop3_is_compare: the opcode is one of the compares (below 0x100): two sources, and the destination field
// names the scalar register pair, VCC or EXEC that receives the lane mask.
bool vop3_is_compare(std::uint32_t opcode);
// vop3_source_count: how many of the three source fields the instruction uses (0..3), or -1 when the table
// does not list the opcode. The select (0x101) and the three carry-in adds (0x128..0x12A) use the third field
// for a lane mask; the other re-encoded two-source instructions use two.
int vop3_source_count(std::uint32_t opcode);
// vop3_has_scalar_result: the instruction has a second, scalar result in word 0 bits 14..8.
bool vop3_has_scalar_result(std::uint32_t opcode);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_ISA_HPP