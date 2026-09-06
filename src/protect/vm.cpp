// vm.cpp — the BVM interpreter (see vm.h). Decode-dispatch, 16 x uint64
// registers, unsigned compare flags. One hard rule: every anomaly fails
// closed with -1, so a tampered binary cannot fail open by accident.
#include "protect/vm.h"

#include <cstring>
#include <string>

#include "sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gr {

namespace {

inline bool ldU32(const uint8_t* p, uint32_t& out) {
    std::memcpy(&out, p, 4);
    return true;
}

bool syscallDebug(uint64_t& out) {
#ifdef _WIN32
    out = IsDebuggerPresent() ? 1 : 0;
    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) && remote)
        out = 1;
#else
    out = 0;
#endif
    return true;
}

bool syscallSha256Hex(uint8_t* data, size_t dataLen, uint64_t off,
                      uint64_t len, uint64_t& out) {
    // Digest scratch is fixed: data[28..91] (64 bytes).
    if (off > dataLen || len > dataLen - off || 92 > dataLen) return false;
    std::string bytes(reinterpret_cast<const char*>(data + off),
                      static_cast<size_t>(len));
    std::string hex = sha256Hex(bytes);
    if (hex.size() != 64) return false;
    std::memcpy(data + 28, hex.data(), 64);
    out = 0;
    return true;
}

}  // namespace

int bvmRun(const uint8_t* code, size_t codeLen, uint8_t* data, size_t dataLen,
           size_t maxSteps) {
    uint64_t r[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bool eq = false, lt = false;
    size_t pc = 0;
    size_t steps = 0;

    for (;;) {
        if (++steps > maxSteps) return -1;
        if (pc + 8 > codeLen) return -1;
        uint8_t op = code[pc];
        uint8_t dst = code[pc + 1], s1 = code[pc + 2], s2 = code[pc + 3];
        uint32_t imm = 0;
        ldU32(code + pc + 4, imm);
        if (dst > 15 || s1 > 15 || s2 > 15) return -1;

        switch (op) {
            case BVM_HALT:
                return static_cast<int>(r[dst]);
            case BVM_LOADI:
                r[dst] = imm;
                break;
            case BVM_MOV:
                r[dst] = r[s1];
                break;
            case BVM_ADD:
                r[dst] = r[s1] + r[s2];
                break;
            case BVM_SUB:
                r[dst] = r[s1] - r[s2];
                break;
            case BVM_XOR:
                r[dst] = r[s1] ^ r[s2];
                break;
            case BVM_AND:
                r[dst] = r[s1] & r[s2];
                break;
            case BVM_OR:
                r[dst] = r[s1] | r[s2];
                break;
            case BVM_NOT:
                r[dst] = ~r[s1];
                break;
            case BVM_SHL:
                r[dst] = r[s1] << (r[s2] & 63);
                break;
            case BVM_SHR:
                r[dst] = r[s1] >> (r[s2] & 63);
                break;
            case BVM_ADDI:
                r[dst] = r[s1] + imm;
                break;
            case BVM_LD8: {
                if (r[s1] >= dataLen) return -1;
                r[dst] = data[r[s1]];
                break;
            }
            case BVM_LD32: {
                if (r[s1] + 4 > dataLen || r[s1] > dataLen) return -1;
                uint32_t v = 0;
                std::memcpy(&v, data + r[s1], 4);
                r[dst] = v;
                break;
            }
            case BVM_ST8: {
                if (r[dst] >= dataLen) return -1;
                data[r[dst]] = static_cast<uint8_t>(r[s1]);
                break;
            }
            case BVM_CMP:
                eq = (r[s1] == r[s2]);
                lt = (r[s1] < r[s2]);
                break;
            case BVM_BEQ:
                if (eq) {
                    pc = imm;
                    continue;  // taken: pc already absolute, do not advance
                }
                break;
            case BVM_BNE:
                if (!eq) {
                    pc = imm;
                    continue;
                }
                break;
            case BVM_BLTU:
                if (lt) {
                    pc = imm;
                    continue;
                }
                break;
            case BVM_BGEU:
                if (!lt) {
                    pc = imm;
                    continue;
                }
                break;
            case BVM_JMP:
                pc = imm;
                continue;
            case BVM_SYS: {
                uint64_t res = 0;
                bool ok = true;
                switch (imm) {
                    case BVM_SYS_DEBUG:
                        ok = syscallDebug(res);
                        break;
                    case BVM_SYS_SHA256HEX:
                        ok = syscallSha256Hex(data, dataLen, r[s1], r[s2], res);
                        break;
                    default:
                        ok = false;
                }
                if (!ok) return -1;
                r[dst] = res;
                break;
            }
            default:
                return -1;
        }
        pc += 8;
    }
}

}  // namespace gr
