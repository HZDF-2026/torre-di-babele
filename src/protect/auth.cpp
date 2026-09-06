// auth.cpp — the BVM auth program and its host-side loader.
//
// Data buffer layout (built by bvmAgentCheck, parsed by the bytecode):
//   [0..3]   u32 magic 0xBABE1E01 — bytecode refuses anything else
//   [4..7]   u32 nameOff
//   [8..11]  u32 nameLen
//   [12..15] u32 keyOff
//   [16..19] u32 keyLen
//   [20..23] u32 agentsOff
//   [24..27] u32 agentCount
//   [28..91] 64-byte digest scratch, filled by SYS_SHA256HEX
//   [92..]   payload: name bytes, key bytes, then per agent
//            u32 nameLen, name bytes, 64-byte keyHash hex
//
// Verdict: exit 0 = authenticated; 2 = no such agent; 1 = corrupt input;
// 0xF0 = debugger attached. The interpreter turns every fault into non-zero,
// so the check fails closed on tampering of code, data, or process.
#include "protect/auth.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "protect/vm.h"

namespace gr {
namespace {

constexpr uint32_t kAuthMagic = 0xBABE1E01u;

struct Asm {
    std::vector<uint8_t> code;

    size_t emit(uint8_t op, uint8_t dst, uint8_t s1, uint8_t s2, uint32_t imm) {
        size_t at = code.size();
        code.push_back(op);
        code.push_back(dst);
        code.push_back(s1);
        code.push_back(s2);
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>(imm >> (8 * i)));
        return at;
    }
    size_t here() const { return code.size(); }
    void jumpTo(size_t at, uint32_t target) {
        for (int i = 0; i < 4; ++i)
            code[at + 4 + i] = static_cast<uint8_t>(target >> (8 * i));
    }
};

// Registers: r0 = 0, r1 = scratch, r2 = header base, r3 = agents cursor p,
// r4 = agentCount, r5 = nameOff, r6 = nameLen, r7 = keyOff (later digest
// cursor), r8 = keyLen (later end pointer), r9 = agent name length,
// r10 = agent index, r11 = loop index, r12 = scratch byte,
// r14 = mismatch accumulator, r15 = hash pointer.
std::vector<uint8_t> authProgram() {
    Asm a;

    a.emit(BVM_LOADI, 0, 0, 0, 0);                       // r0 = 0
    a.emit(BVM_SYS, 1, 0, 0, BVM_SYS_DEBUG);             // debugger?
    a.emit(BVM_CMP, 0, 1, 0, 0);
    size_t jDbg = a.emit(BVM_BNE, 0, 0, 0, 0);
    a.emit(BVM_LOADI, 2, 0, 0, 0);                       // r2 = header base
    a.emit(BVM_LD32, 1, 2, 0, 0);                        // magic
    a.emit(BVM_LOADI, 3, 0, 0, kAuthMagic);
    a.emit(BVM_CMP, 0, 1, 3, 0);
    size_t jBad = a.emit(BVM_BNE, 0, 0, 0, 0);

    // header fields -> r5..r10: nameOff nameLen keyOff keyLen agentsOff agentCount
    for (uint8_t i = 0; i < 6; ++i) {
        a.emit(BVM_ADDI, 1, 2, 0, static_cast<uint32_t>(4 + 4 * i));
        a.emit(BVM_LD32, static_cast<uint8_t>(5 + i), 1, 0, 0);
    }
    a.emit(BVM_MOV, 4, 10, 0, 0);                        // r4 = agentCount
    a.emit(BVM_MOV, 3, 9, 0, 0);                         // r3 = p = agentsOff
    a.emit(BVM_SYS, 1, 7, 8, BVM_SYS_SHA256HEX);         // digest -> data[28]
    a.emit(BVM_CMP, 0, 1, 0, 0);
    size_t jBad2 = a.emit(BVM_BNE, 0, 0, 0, 0);

    a.emit(BVM_LOADI, 10, 0, 0, 0);                      // i = 0
    size_t scan = a.here();
    a.emit(BVM_CMP, 0, 10, 4, 0);
    size_t jNotFound = a.emit(BVM_BGEU, 0, 0, 0, 0);
    a.emit(BVM_LD32, 9, 3, 0, 0);                        // anLen
    a.emit(BVM_CMP, 0, 9, 6, 0);                        // anLen == nameLen?
    size_t jNext1 = a.emit(BVM_BNE, 0, 0, 0, 0);
    a.emit(BVM_LOADI, 14, 0, 0, 0);                      // mismatch = 0
    a.emit(BVM_LOADI, 11, 0, 0, 0);                      // j = 0
    size_t nameCmp = a.here();
    a.emit(BVM_CMP, 0, 11, 6, 0);
    size_t jNameCmpDone = a.emit(BVM_BGEU, 0, 0, 0, 0);
    a.emit(BVM_ADDI, 1, 3, 0, 4);                       // r1 = p + 4
    a.emit(BVM_ADD, 1, 1, 11, 0);                       // r1 = p + 4 + j
    a.emit(BVM_LD8, 12, 1, 0, 0);                       // agent name byte
    a.emit(BVM_ADD, 1, 5, 11, 0);                       // r1 = nameOff + j
    a.emit(BVM_LD8, 1, 1, 0, 0);                        // presented name byte
    a.emit(BVM_XOR, 1, 12, 1, 0);
    a.emit(BVM_OR, 14, 14, 1, 0);                       // mismatch |= a^b
    a.emit(BVM_ADDI, 11, 11, 0, 1);
    size_t jmpNameCmp = a.emit(BVM_JMP, 0, 0, 0, 0);
    a.jumpTo(jmpNameCmp, static_cast<uint32_t>(nameCmp));
    size_t nameDone = a.here();                           // mismatch check
    a.emit(BVM_CMP, 0, 14, 0, 0);
    size_t jNext2 = a.emit(BVM_BNE, 0, 0, 0, 0);

    // name matched: 64 hash bytes at p+4+anLen vs digest at data[28]
    a.emit(BVM_ADD, 15, 3, 9, 0);                       // hp = p + anLen
    a.emit(BVM_ADDI, 15, 15, 0, 4);                     // hp += 4
    a.emit(BVM_ADDI, 7, 0, 0, 28);                      // q = 28
    a.emit(BVM_ADD, 8, 15, 0, 0);                       // r8 = hp
    a.emit(BVM_ADDI, 8, 8, 0, 64);                      // end = hp + 64
    a.emit(BVM_LOADI, 14, 0, 0, 0);                     // diff = 0
    size_t hashCmp = a.here();
    a.emit(BVM_CMP, 0, 15, 8, 0);
    size_t jHashDone = a.emit(BVM_BGEU, 0, 0, 0, 0);
    a.emit(BVM_LD8, 12, 15, 0, 0);
    a.emit(BVM_LD8, 1, 7, 0, 0);
    a.emit(BVM_XOR, 1, 12, 1, 0);
    a.emit(BVM_OR, 14, 14, 1, 0);
    a.emit(BVM_ADDI, 15, 15, 0, 1);
    a.emit(BVM_ADDI, 7, 7, 0, 1);
    size_t jmpHashCmp = a.emit(BVM_JMP, 0, 0, 0, 0);
    a.jumpTo(jmpHashCmp, static_cast<uint32_t>(hashCmp));
    size_t hashDone = a.here();                           // diff check
    a.emit(BVM_CMP, 0, 14, 0, 0);
    size_t jOk = a.emit(BVM_BEQ, 0, 0, 0, 0);
    size_t jNext3 = a.emit(BVM_JMP, 0, 0, 0, 0);
    size_t ok = a.here();
    a.emit(BVM_HALT, 0, 0, 0, 0);                        // exit 0 = authenticated

    // next agent: p += 4 + anLen + 64; i++
    size_t next = a.here();
    a.emit(BVM_ADDI, 3, 3, 0, 4);
    a.emit(BVM_ADD, 3, 3, 9, 0);
    a.emit(BVM_ADDI, 3, 3, 0, 64);
    a.emit(BVM_ADDI, 10, 10, 0, 1);
    size_t jmpScan = a.emit(BVM_JMP, 0, 0, 0, 0);
    a.jumpTo(jmpScan, static_cast<uint32_t>(scan));

    size_t notFound = a.here();
    a.emit(BVM_LOADI, 1, 0, 0, 2);
    a.emit(BVM_HALT, 1, 0, 0, 0);
    size_t dbg = a.here();
    a.emit(BVM_LOADI, 1, 0, 0, 0xF0);
    a.emit(BVM_HALT, 1, 0, 0, 0);
    size_t bad = a.here();
    a.emit(BVM_LOADI, 1, 0, 0, 1);
    a.emit(BVM_HALT, 1, 0, 0, 0);

    a.jumpTo(jDbg, static_cast<uint32_t>(dbg));
    a.jumpTo(jBad, static_cast<uint32_t>(bad));
    a.jumpTo(jBad2, static_cast<uint32_t>(bad));
    a.jumpTo(jNotFound, static_cast<uint32_t>(notFound));
    a.jumpTo(jNameCmpDone, static_cast<uint32_t>(nameDone));
    a.jumpTo(jNext1, static_cast<uint32_t>(next));
    a.jumpTo(jNext2, static_cast<uint32_t>(next));
    a.jumpTo(jNext3, static_cast<uint32_t>(next));
    a.jumpTo(jHashDone, static_cast<uint32_t>(hashDone));
    a.jumpTo(jOk, static_cast<uint32_t>(ok));
    return a.code;
}

const std::vector<uint8_t>& authBytecode() {
    static const std::vector<uint8_t> code = authProgram();
    return code;
}

void putU32At(std::vector<uint8_t>& d, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace

bool bvmAgentCheck(const std::vector<std::pair<std::string, std::string>>& registry,
                   const std::string& name, const std::string& key) {
    if (name.empty() || key.empty()) return false;
    size_t agentsBytes = 0;
    for (const auto& e : registry) {
        if (e.second.size() != 64) return false;  // malformed registry: fail closed
        agentsBytes += 4 + e.first.size() + 64;
    }
    std::vector<uint8_t> data(92 + name.size() + key.size() + agentsBytes, 0);
    uint32_t nameOff = 92;
    uint32_t keyOff = nameOff + static_cast<uint32_t>(name.size());
    uint32_t agentsOff = keyOff + static_cast<uint32_t>(key.size());
    putU32At(data, 0, kAuthMagic);
    putU32At(data, 4, nameOff);
    putU32At(data, 8, static_cast<uint32_t>(name.size()));
    putU32At(data, 12, keyOff);
    putU32At(data, 16, static_cast<uint32_t>(key.size()));
    putU32At(data, 20, agentsOff);
    putU32At(data, 24, static_cast<uint32_t>(registry.size()));
    std::memcpy(data.data() + nameOff, name.data(), name.size());
    std::memcpy(data.data() + keyOff, key.data(), key.size());
    uint32_t p = agentsOff;
    for (const auto& e : registry) {
        putU32At(data, p, static_cast<uint32_t>(e.first.size()));
        std::memcpy(data.data() + p + 4, e.first.data(), e.first.size());
        std::memcpy(data.data() + p + 4 + e.first.size(), e.second.data(), 64);
        p += 4 + static_cast<uint32_t>(e.first.size()) + 64;
    }

    const std::vector<uint8_t>& code = authBytecode();
    return bvmRun(code.data(), code.size(), data.data(), data.size(), 1u << 20) == 0;
}

}  // namespace gr
