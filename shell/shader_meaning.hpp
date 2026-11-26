// What a console shader instruction computes, written down as an interpreter for one lane.
//
// shader_isa.* says what the fields of an instruction are; this file says what the instruction DOES with them,
// as executable code that needs no GPU: a per-lane machine state, and one step function that reads the decoded
// instruction and updates that state.
//
// Scope, and the reason for the shape:
//   * One lane. A console wave runs the same instruction on many lanes at once; what a single lane computes is
//     what a translator has to reproduce, so the state carries a lane number and the two masks that decide what
//     a lane's result means: VCC (a per-lane compare result) and EXEC (the lane-enable mask).
//   * Straight line only. Branches, the wait/clause/prefetch instructions and every memory, image, interpolation
//     and export instruction are outside this step; step() refuses them by format, opcode and name instead of
//     pretending. shader_flow.* (another step) owns control flow.
//   * Nothing is invented. Where the public sources do not state what an instruction does for a particular
//     input, step() refuses THAT input (a StepResult with refused set and a reason) rather than returning a
//     value this project cannot defend. docs/SHADER-MEANINGS.md lists every such place.
//
// The meanings themselves, with the public source for each, are in docs/SHADER-MEANINGS.md. The sources read for
// this file are Mesa's ACO compiler (aco_opcodes.py for the numbers; aco_lower_to_hw_instr.cpp for how an
// instruction is used), LLVM's AMDGPU backend (the VOP1/VOP2/VOP3/VOPC/SOP* instruction definitions and their
// selection patterns; AMDGPUModifierSyntax for clamp and omod) and the instruction-semantics statements in those
// files. No code from any of them is copied here.
#ifndef WHIWHI_SHELL_SHADER_MEANING_HPP
#define WHIWHI_SHELL_SHADER_MEANING_HPP

#include "shader_isa.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace whiwhi::shell {

// The state of ONE lane.
//
// The scalar and vector register files are plain arrays. The scalar codes above 101 are not array entries: they
// are the special registers (VCC, EXEC, M0, SGPR_NULL), kept in their own fields and reached through
// read_scalar_code / write_scalar_code so that a caller can never read them as if they were ordinary registers.
// A 64-bit operand uses the register named by its field as the low word and the next code as the high word.
struct MachineState {
    static constexpr std::uint32_t kScalarRegisters = 128;  // the file; codes 0..101 are ordinary registers
    static constexpr std::uint32_t kVectorRegisters = 256;

    std::uint32_t sgpr[kScalarRegisters] = {};  // ordinary scalar registers, codes 0..101
    std::uint32_t vgpr[kVectorRegisters] = {};  // vector registers, 0..255

    // The 64-bit compare result mask. Bit n is lane n's result; VOPC and the three-operand compares write it
    // (or another mask register, when the three-operand form names one), and v_cndmask_b32 reads it.
    std::uint64_t vcc = 0;
    // The lane-enable mask. A vector instruction writes a lane only when the lane's bit is set.
    std::uint64_t exec = ~0ull;
    // The scalar condition code, set by the compare and carry-producing scalar instructions.
    bool scc = false;
    // Which lane this state is: the bit of VCC and EXEC that belongs to it.
    std::uint32_t lane = 0;
    // M0 (scalar code 124): ordinary storage as far as this step is concerned.
    std::uint32_t m0 = 0;

    // The scalar file at `code` as a 64-bit pair (low word at `code`, high word at the next code).
    std::uint64_t scalar_pair(std::uint32_t code) const {
        return static_cast<std::uint64_t>(sgpr[code]) | (static_cast<std::uint64_t>(sgpr[code + 1]) << 32);
    }
    void set_scalar_pair(std::uint32_t code, std::uint64_t value) {
        sgpr[code] = static_cast<std::uint32_t>(value);
        sgpr[code + 1] = static_cast<std::uint32_t>(value >> 32);
    }
    bool lane_enabled() const { return ((exec >> lane) & 1u) != 0; }
    bool vcc_bit() const { return ((vcc >> lane) & 1u) != 0; }
};

// One 32-bit scalar code: 0..101 is the ordinary file, 106/107 VCC, 126/127 EXEC, 124 M0, 125 SGPR_NULL (reads
// as zero, discards writes). A code this step does not model (the flat-scratch, XNACK and trap-temporary codes)
// answers false with a reason, so that no value is ever made up for it.
bool read_scalar_code(const MachineState& state, std::uint32_t code, std::uint32_t* value, std::string* why);
bool write_scalar_code(MachineState& state, std::uint32_t code, std::uint32_t value, std::string* why);

// A 64-bit scalar source/destination: the pair starting at `code`. Codes 106 and 126 name the VCC and EXEC pairs.
bool read_scalar_pair(const MachineState& state, std::uint32_t code, std::uint64_t* value, std::string* why);
bool write_scalar_pair(MachineState& state, std::uint32_t code, std::uint64_t value, std::string* why);

// The 64-bit patterns a lane can read out of two mask registers is not needed here; these two helpers are what
// the tests and any caller use to look at a mask bit the way an instruction does.
inline bool mask_bit(std::uint64_t mask, std::uint32_t lane) { return ((mask >> lane) & 1u) != 0; }

// What one step did.
struct StepResult {
    // The instruction was the end-of-program word: the program stops here, normally.
    bool done = false;
    // This build does not establish what the instruction computes for this state: nothing was written and the
    // reason says which instruction and which input. A caller must not carry on as if it had executed.
    bool refused = false;
    std::string reason;
};

// Execute one decoded instruction on one lane. Name format and opcode are checked against the covered set;
// anything else, and any covered instruction whose result this step does not establish for this state, comes
// back refused with the format, opcode, public name and the reason in `reason`.
StepResult step(MachineState& state, const Instruction& instruction);

// Execute a straight-line program: every instruction in order until the end-of-program word.
struct RunResult {
    bool reached_end = false;      // an end-of-program instruction was executed
    bool refused = false;          // a step refused, or the body ran out without an end word
    std::size_t executed = 0;      // instructions executed, the end word included
    std::uint32_t refused_pc = 0;  // where the refusal happened (byte offset)
    WalkFormat refused_format = WalkFormat::kUnknown;
    std::uint16_t refused_opcode = 0;
    const char* refused_name = nullptr;  // public name when the shared table names the pair, else null
    std::string reason;
};

RunResult run_straight_line(MachineState& state, const DecodedShader& shader);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_MEANING_HPP