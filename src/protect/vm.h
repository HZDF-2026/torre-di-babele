// vm.h — the Babele VM (BVM): a tiny fixed-width bytecode interpreter.
//
// Protection layer (PROTOCOL.md §Protection). Security-critical decisions
// (agent key verification) are compiled to BVM bytecode and executed here.
// The shipped binary therefore contains no native cmp/branch sequence at the
// authentication site to flip with a one-byte patch; the decision logic is
// data, walked by this generic decode-dispatch loop. Every fault — bad
// opcode, out-of-bounds access, step budget exhausted, debugger attached —
// fails closed (non-zero exit), never open.
#ifndef gr_GR_PROTECT_VM_H
#define gr_GR_PROTECT_VM_H

#include <cstddef>
#include <cstdint>

namespace gr {

// Opcodes (PROTOCOL.md §Protection). Encoding: 8 bytes per instruction —
// [op][dst][src1][src2][imm32 little-endian].
enum BvmOp : uint8_t {
    BVM_HALT  = 0x01,  // stop; exit code = r[dst]
    BVM_LOADI = 0x02,  // r[dst] = imm32 (zero-extended)
    BVM_MOV   = 0x03,  // r[dst] = r[src1]
    BVM_ADD   = 0x04,  // r[dst] = r[src1] + r[src2]
    BVM_SUB   = 0x05,
    BVM_XOR   = 0x06,
    BVM_AND   = 0x07,
    BVM_OR    = 0x08,
    BVM_NOT   = 0x09,  // r[dst] = ~r[src1]
    BVM_SHL   = 0x0A,
    BVM_SHR   = 0x0B,
    BVM_ADDI  = 0x0C,  // r[dst] = r[src1] + imm32
    BVM_LD8   = 0x0D,  // r[dst] = data[r[src1]]
    BVM_LD32  = 0x0E,  // r[dst] = u32 at data[r[src1]]
    BVM_ST8   = 0x0F,  // data[r[dst]] = (uint8_t)r[src1]
    BVM_CMP   = 0x10,  // flags: eq = (r[src1] == r[src2]); lt = (r[src1] < r[src2])
    BVM_BEQ   = 0x11,  // if eq      pc = imm32
    BVM_BNE   = 0x12,
    BVM_BLTU  = 0x13,  // if lt      pc = imm32
    BVM_BGEU  = 0x14,  // if !lt     pc = imm32
    BVM_JMP   = 0x15,
    BVM_SYS   = 0x16,  // syscall: id = imm32, args in r[src1], r[src2], result in r[dst]
};

// Syscall ids. Host-side primitives the bytecode may call.
enum BvmSys : uint32_t {
    BVM_SYS_DEBUG     = 2,  // r[dst] = 1 if a debugger is attached, else 0
    BVM_SYS_SHA256HEX = 3,  // digest of data[r[src1]..r[src1]+r[src2]) as 64 lowercase hex
                           // bytes written to data[28..91]; r[dst] = 0, or 1 on failure
};

// Runs `code` against `data`. Returns the HALT register value, or -1 on any
// fault (bounds error, unknown opcode, step budget exhausted). data is
// read-write: syscalls and ST8 scratch results into it.
int bvmRun(const uint8_t* code, size_t codeLen, uint8_t* data, size_t dataLen,
           size_t maxSteps);

}  // namespace gr

#endif  // gr_GR_PROTECT_VM_H
