#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include "apivpn/platform/hook_backend.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace apivpn::platform {

namespace {

struct DecodedInsn {
    uint8_t length          = 0;     // 0 means "not decoded / refuse to hook"
    bool    is_rel_jmp_call = false; // E8, E9, EB, 70..7F, 0F 80..8F
    bool    is_cond_jcc     = false;
    uint8_t jcc_cc          = 0;     // 0..15 condition code
    bool    is_call         = false;
    int64_t rel_target      = 0;     // Absolute target address of relative branch
    bool    is_rip_relative = false; // x64 ModRM mod==00 && rm==101
    uint8_t disp_offset     = 0;     // Offset of 32-bit displacement inside instruction
    bool    is_ff25_jmp     = false; // FF 25 disp32 / addr32 import thunk
};

// Lightweight x86/x64 instruction length & branch decoder for function prologues.
// It FAILS CLOSED: any opcode it does not explicitly understand returns length 0,
// so installDetour refuses to hook rather than corrupting code.
//
// Supported instruction forms:
//   Prefixes: 0x66 (operand size), 0x67, 0xF0, 0xF2, 0xF3, segment overrides
//             (0x2E/0x3E/0x26/0x36/0x64/0x65), and REX (0x40..0x4F) in 64-bit mode.
//   No-ModRM, no-imm: push/pop reg (0x50..0x5F); 32-bit inc/dec (0x40..0x4F, only
//             reachable in 32-bit mode since those bytes are REX on x64); nop/xchg
//             eax,eax (0x90); cwde/cdqe (0x98); cdq/cqo (0x99); pushf/popf
//             (0x9C/0x9D); ret (0xC3); leave (0xC9); int3 (0xCC).
//   ret imm16 (0xC2).
//   push imm8/imm16/imm32 (0x6A / 0x68).
//   mov reg,imm (0xB0..0xBF, incl. movabs imm64 with REX.W).
//   mov AL/eAX moffs (0xA0..0xA3); test AL/eAX,imm (0xA8/0xA9).
//   ALU AL/eAX,imm (0x04/0x05 families, i.e. add/or/adc/sbb/and/sub/xor/cmp).
//   Short jmp/jcc rel8 (0xEB, 0x70..0x7F) and near call/jmp rel32 (0xE8/0xE9),
//             re-encoded on relocation; near jcc rel32 (0F 80..8F).
//   ModR/M ALU reg/mem (0x00..0x3B families); movsxd (0x63); test (0x84/0x85); xchg (0x86/0x87);
//             mov (0x88..0x8E), lea (0x8D), pop r/m (0x8F); shift (0xC0/0xC1 imm8,
//             0xD0..0xD3); mov r/m,imm (0xC6/0xC7); imul imm (0x69/0x6B);
//             group1 (0x80/0x81/0x82/0x83); group3 test/not/neg/mul... (0xF6/0xF7,
//             with imm only for the test sub-opcodes /0 and /1); inc/dec r/m
//             (0xFE); group5 inc/dec/call/jmp/push (0xFF, incl. the FF 25 import
//             thunk and FF /2,/4 RIP-relative forms).
//   Two-byte: endbr/reserved-nop (0F 1E / 0F 1F), movzx/movsx (0F B6/B7/BE/BF).
// Near operand-size-prefixed branches (0x66 + E8/E9/0F8x) are refused (fail closed),
// as a 16-bit relative branch is not something a prologue detour needs to relocate.
DecodedInsn decodeInsn(const uint8_t* code) {
    DecodedInsn out{};
    size_t i = 0;

    bool operand_size_16 = false;
    bool rex_w = false;
    (void)rex_w;

    // 1. Legacy prefixes & REX prefix (x64)
    while (i < 15) {
        uint8_t b = code[i];
        if (b == 0x66) {
            operand_size_16 = true;
            ++i;
        } else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                   b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
                   b == 0x64 || b == 0x65) {
            ++i;
        }
#if defined(_M_X64) || defined(__x86_64__)
        else if ((b & 0xF0) == 0x40) {
            rex_w = (b & 0x08) != 0;
            ++i;
        }
#endif
        else {
            break;
        }
    }

    uint8_t op = code[i++];

    // --- One-byte opcodes with no ModR/M and no immediate ---
    if ((op >= 0x50 && op <= 0x5F) || // push/pop reg
        (op >= 0x40 && op <= 0x4F) || // 32-bit inc/dec (on x64 these are REX, consumed above)
        op == 0x90 ||                 // nop / xchg eax,eax (incl. hot-patch 'mov edi,edi' handled as mov below)
        op == 0x98 || op == 0x99 ||   // cwde/cdqe, cdq/cqo
        op == 0x9C || op == 0x9D ||   // pushf/popf
        op == 0xC3 ||                 // ret
        op == 0xC9 ||                 // leave
        op == 0xCC) {                 // int3
        out.length = static_cast<uint8_t>(i);
        return out;
    }

    // RET imm16 (C2 iw)
    if (op == 0xC2) {
        out.length = static_cast<uint8_t>(i + 2);
        return out;
    }

    // Short relative jumps: EB rel8, 70..7F rel8
    if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) {
        int8_t rel8 = static_cast<int8_t>(code[i++]);
        out.length = static_cast<uint8_t>(i);
        out.is_rel_jmp_call = true;
        out.is_cond_jcc = (op != 0xEB);
        out.jcc_cc = op & 0x0F;
        out.rel_target = reinterpret_cast<intptr_t>(code) + out.length + rel8;
        return out;
    }

    // Near relative call/jmp: E8 rel32, E9 rel32
    if (op == 0xE8 || op == 0xE9) {
        if (operand_size_16) return out; // 16-bit rel branch: refuse (fail closed)
        int32_t rel32 = 0;
        std::memcpy(&rel32, code + i, 4);
        i += 4;
        out.length = static_cast<uint8_t>(i);
        out.is_rel_jmp_call = true;
        out.is_call = (op == 0xE8);
        out.rel_target = reinterpret_cast<intptr_t>(code) + out.length + rel32;
        return out;
    }

    // Push imm8 / imm16 / imm32
    if (op == 0x6A) {
        out.length = static_cast<uint8_t>(i + 1);
        return out;
    }
    if (op == 0x68) {
        out.length = static_cast<uint8_t>(i + (operand_size_16 ? 2 : 4));
        return out;
    }

    // MOV reg, imm8 (B0..B7)
    if (op >= 0xB0 && op <= 0xB7) {
        out.length = static_cast<uint8_t>(i + 1);
        return out;
    }
    // MOV reg, imm16/32/64 (B8..BF)
    if (op >= 0xB8 && op <= 0xBF) {
        size_t imm_len = rex_w ? 8 : (operand_size_16 ? 2 : 4);
        out.length = static_cast<uint8_t>(i + imm_len);
        return out;
    }

    // MOV AL/EAX/RAX, moffs (A0..A3)
    if (op >= 0xA0 && op <= 0xA3) {
        out.length = static_cast<uint8_t>(i + sizeof(void*));
        return out;
    }
    // TEST AL, imm8 (A8) / TEST eAX, imm32 (A9)
    if (op == 0xA8) {
        out.length = static_cast<uint8_t>(i + 1);
        return out;
    }
    if (op == 0xA9) {
        out.length = static_cast<uint8_t>(i + (operand_size_16 ? 2 : 4));
        return out;
    }

    // ALU AL, imm8 (04, 0C, 14, 1C, 24, 2C, 34, 3C)
    if ((op & 0xC7) == 0x04) {
        out.length = static_cast<uint8_t>(i + 1);
        return out;
    }
    // ALU eAX, imm32 (05, 0D, 15, 1D, 25, 2D, 35, 3D)
    if ((op & 0xC7) == 0x05) {
        out.length = static_cast<uint8_t>(i + (operand_size_16 ? 2 : 4));
        return out;
    }

    // --- Instructions that use a ModR/M byte ---
    bool two_byte = false;
    int imm_size = -1; // >=0 once we recognise a ModR/M instruction

    if (op == 0x0F) {
        two_byte = true;
        uint8_t op2 = code[i++];
        // Near conditional jump: 0F 80..8F rel32
        if (op2 >= 0x80 && op2 <= 0x8F) {
            if (operand_size_16) return out; // 16-bit rel branch: refuse
            int32_t rel32 = 0;
            std::memcpy(&rel32, code + i, 4);
            i += 4;
            out.length = static_cast<uint8_t>(i);
            out.is_rel_jmp_call = true;
            out.is_cond_jcc = true;
            out.jcc_cc = op2 & 0x0F;
            out.rel_target = reinterpret_cast<intptr_t>(code) + out.length + rel32;
            return out;
        }
        switch (op2) {
            case 0x1E: case 0x1F:            // endbr64/endbr32 (with F3), reserved NOP
            case 0xB6: case 0xB7:            // movzx
            case 0xBE: case 0xBF:            // movsx
                imm_size = 0;
                break;
            default:
                return out;                  // unknown two-byte opcode: fail closed
        }
        op = op2;
    } else if ((op & 0xC0) == 0x00 && (op & 0x07) <= 0x03) {
        // ALU reg/mem forms: add/or/adc/sbb/and/sub/xor/cmp r/m,reg and reg,r/m
        imm_size = 0;
    } else {
        switch (op) {
            case 0x63:                       // movsxd reg, r/m32 (x64) / arpl (x86): plain ModR/M
            case 0x84: case 0x85:            // test r/m, reg
            case 0x86: case 0x87:            // xchg r/m, reg
            case 0x88: case 0x89:            // mov r/m, reg
            case 0x8A: case 0x8B:            // mov reg, r/m (incl. hot-patch 'mov edi,edi' 8B FF)
            case 0x8C: case 0x8E:            // mov r/m, seg / seg, r/m
            case 0x8D:                       // lea
            case 0x8F:                       // pop r/m
            case 0xD0: case 0xD1:            // shift r/m, 1
            case 0xD2: case 0xD3:            // shift r/m, cl
            case 0xFE:                       // inc/dec r/m8
            case 0xFF:                       // group5: inc/dec/call/jmp/push
                imm_size = 0;
                break;
            case 0x80: case 0x82: case 0x83: // group1 r/m, imm8
            case 0xC0: case 0xC1:            // shift r/m, imm8
            case 0xC6:                       // mov r/m8, imm8
            case 0x6B:                       // imul reg, r/m, imm8
                imm_size = 1;
                break;
            case 0x81:                       // group1 r/m, imm16/32
            case 0xC7:                       // mov r/m, imm16/32
            case 0x69:                       // imul reg, r/m, imm16/32
                imm_size = operand_size_16 ? 2 : 4;
                break;
            case 0xF6: {                     // group3 r/m8: imm8 only for test (/0, /1)
                uint8_t r = (code[i] >> 3) & 7;
                imm_size = (r == 0 || r == 1) ? 1 : 0;
                break;
            }
            case 0xF7: {                     // group3 r/m: imm16/32 only for test (/0, /1)
                uint8_t r = (code[i] >> 3) & 7;
                imm_size = (r == 0 || r == 1) ? (operand_size_16 ? 2 : 4) : 0;
                break;
            }
            default:
                return out;                  // unknown one-byte opcode: fail closed
        }
    }

    // Common ModR/M (+ SIB + displacement + immediate) length computation
    uint8_t modrm = code[i++];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;

    if (mod != 3 && rm == 4) {
        // SIB byte present
        uint8_t sib = code[i++];
        uint8_t base = sib & 7;
        if (mod == 0 && base == 5) {
            i += 4; // disp32 with no base
        }
    }

    if (mod == 0 && rm == 5) {
        out.disp_offset = static_cast<uint8_t>(i);
#if defined(_M_X64) || defined(__x86_64__)
        out.is_rip_relative = true;
        if (!two_byte && op == 0xFF && reg == 4) {
            out.is_ff25_jmp = true;
        }
#else
        (void)reg;
        (void)two_byte;
#endif
        i += 4;
    } else if (mod == 1) {
        i += 1; // disp8
    } else if (mod == 2) {
        i += 4; // disp32
    }

    i += static_cast<size_t>(imm_size);
    out.length = static_cast<uint8_t>(i);
    return out;
}

// Allocate executable trampoline memory, preferring within +-1.5GB of target on x64
uint8_t* allocateTrampolineNear(void* target_func, size_t size) {
#if defined(_M_X64) || defined(__x86_64__)
    uintptr_t target = reinterpret_cast<uintptr_t>(target_func);
    const uintptr_t max_delta = 0x5FFF0000ULL; // ~1.5 GB
    uintptr_t min_addr = (target > max_delta) ? (target - max_delta) : 0x10000ULL;
    uintptr_t max_addr = target + max_delta;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;

    // Search upward from target first, then downward
    for (uintptr_t addr = (target & ~(gran - 1)); addr < max_addr; ) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            uintptr_t candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (gran - 1)) & ~(gran - 1);
            if (candidate >= min_addr && candidate < max_addr) {
                void* p = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) return static_cast<uint8_t*>(p);
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    for (uintptr_t addr = (target & ~(gran - 1)); addr > min_addr; ) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            uintptr_t candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (gran - 1)) & ~(gran - 1);
            if (candidate >= min_addr && candidate < max_addr) {
                void* p = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) return static_cast<uint8_t*>(p);
            }
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (base <= min_addr + gran) break;
        addr = base - gran;
    }
#endif
    return static_cast<uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}

void writeAbsoluteJump(uint8_t*& dst, void* target) {
#if defined(_M_X64) || defined(__x86_64__)
    // FF 25 00 00 00 00 <64-bit target>
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    uint64_t addr = reinterpret_cast<uint64_t>(target);
    std::memcpy(dst + 6, &addr, 8);
    dst += 14;
#else
    // E9 <rel32>
    dst[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(target) - (reinterpret_cast<uintptr_t>(dst) + 5));
    std::memcpy(dst + 1, &rel, 4);
    dst += 5;
#endif
}

// Suspend every other thread of this process (toolhelp snapshot) so the live code
// can be patched without another thread executing through the half-written bytes.
std::vector<HANDLE> suspendOtherThreads() {
    DWORD self_pid = GetCurrentProcessId();
    DWORD self_tid = GetCurrentThreadId();

    // Phase 1: collect the thread IDs (all heap allocation happens here, before any
    // thread is suspended, so we cannot deadlock against a suspended thread that
    // holds the heap lock).
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD)) {
                    continue;
                }
                if (te.th32OwnerProcessID == self_pid && te.th32ThreadID != self_tid) {
                    tids.push_back(te.th32ThreadID);
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    // Phase 2: suspend. The handle vector is reserved up front so no reallocation
    // (and therefore no heap access) happens while any thread is suspended.
    std::vector<HANDLE> handles;
    handles.reserve(tids.size());
    for (DWORD tid : tids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                              FALSE, tid);
        if (!h) continue;
        if (SuspendThread(h) == static_cast<DWORD>(-1)) {
            CloseHandle(h);
            continue;
        }
        handles.push_back(h);
    }
    return handles;
}

void resumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    handles.clear();
}

// Returns true if any suspended thread's instruction pointer lies within [lo, hi).
bool anyThreadIpInRange(const std::vector<HANDLE>& handles, uintptr_t lo, uintptr_t hi) {
    for (HANDLE h : handles) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(h, &ctx)) continue;
#if defined(_M_X64) || defined(__x86_64__)
        uintptr_t ip = static_cast<uintptr_t>(ctx.Rip);
#else
        uintptr_t ip = static_cast<uintptr_t>(ctx.Eip);
#endif
        if (ip >= lo && ip < hi) return true;
    }
    return false;
}

} // namespace

bool HookBackend::installDetour(void* target_func, void* detour_func, void** out_trampoline, DetourEntry* out_record) {
    if (!target_func || !detour_func || !out_trampoline) return false;

    auto* src = static_cast<uint8_t*>(target_func);
    constexpr size_t kTrampBlock = 128;
    uint8_t* tramp_block = allocateTrampolineNear(target_func, kTrampBlock);
    if (!tramp_block) return false;

    // Relay stub (x64 5-byte patch) sits near the end of the block; the trampoline
    // body grows upward from the start and must not reach it.
    uint8_t* relay_stub = tramp_block + 104;
    const uint8_t* tramp_body_limit = tramp_block + 100;

    intptr_t delta_to_relay = reinterpret_cast<intptr_t>(relay_stub) - (reinterpret_cast<intptr_t>(src) + 5);
    bool use_5byte_patch = (delta_to_relay >= INT32_MIN && delta_to_relay <= INT32_MAX);
#if !(defined(_M_X64) || defined(__x86_64__))
    // On 32-bit there is no 14-byte absolute jump form; a relay out of rel32 range
    // cannot be reached, so refuse rather than write a bad jump.
    if (!use_5byte_patch) {
        VirtualFree(tramp_block, 0, MEM_RELEASE);
        return false;
    }
#endif
    size_t required_patch_size = use_5byte_patch ? 5 : 14;

    if (use_5byte_patch) {
        uint8_t* p = relay_stub;
        writeAbsoluteJump(p, detour_func);
    }

    // --- Pass 1: decode the prologue instructions we will steal ---
    struct Stolen {
        DecodedInsn    insn;
        const uint8_t* ptr;
    };
    std::vector<Stolen> stolen;
    size_t total_copied = 0;
    while (total_copied < required_patch_size) {
        const uint8_t* insn_ptr = src + total_copied;
        DecodedInsn insn = decodeInsn(insn_ptr);
        if (insn.length == 0 || total_copied + insn.length > 32) {
            VirtualFree(tramp_block, 0, MEM_RELEASE);
            return false;
        }
        stolen.push_back({insn, insn_ptr});
        total_copied += insn.length;
    }
    const size_t stolen_len = total_copied;
    const uintptr_t stolen_lo = reinterpret_cast<uintptr_t>(src);
    const uintptr_t stolen_hi = stolen_lo + stolen_len;

    // Refuse if any relative branch in the stolen bytes jumps into the stolen
    // range: after relocation that target no longer exists as decoded.
    for (const Stolen& s : stolen) {
        if (s.insn.is_rel_jmp_call) {
            uintptr_t tgt = static_cast<uintptr_t>(s.insn.rel_target);
            if (tgt >= stolen_lo && tgt < stolen_hi) {
                VirtualFree(tramp_block, 0, MEM_RELEASE);
                return false;
            }
        }
    }

    // --- Pass 2: emit the relocated prologue into the trampoline ---
    uint8_t* tramp_cursor = tramp_block;
    for (const Stolen& s : stolen) {
        const DecodedInsn& insn = s.insn;
        const uint8_t* insn_ptr = s.ptr;

        if (tramp_cursor + 14 > tramp_body_limit) { // 14 = largest single emission
            VirtualFree(tramp_block, 0, MEM_RELEASE);
            return false;
        }

        if (insn.is_ff25_jmp) {
            // x64: FF 25 disp32 -> indirect jump through [rip + disp32]
            int32_t disp = 0;
            std::memcpy(&disp, insn_ptr + insn.disp_offset, 4);
            const auto* ptr_slot = reinterpret_cast<void* const*>(insn_ptr + insn.length + disp);
            void* final_target = *ptr_slot;
            writeAbsoluteJump(tramp_cursor, final_target);
        } else if (insn.is_rel_jmp_call) {
            intptr_t target_addr = static_cast<intptr_t>(insn.rel_target);
            if (insn.is_cond_jcc) {
                // Re-encode (rel8 or rel32) as a 6-byte 0F 8x rel32
                intptr_t rel = target_addr - (reinterpret_cast<intptr_t>(tramp_cursor) + 6);
                if (rel < INT32_MIN || rel > INT32_MAX) {
                    VirtualFree(tramp_block, 0, MEM_RELEASE);
                    return false;
                }
                int32_t rel32 = static_cast<int32_t>(rel);
                tramp_cursor[0] = 0x0F;
                tramp_cursor[1] = static_cast<uint8_t>(0x80 | insn.jcc_cc);
                std::memcpy(tramp_cursor + 2, &rel32, 4);
                tramp_cursor += 6;
            } else if (insn.is_call) {
                intptr_t rel = target_addr - (reinterpret_cast<intptr_t>(tramp_cursor) + 5);
                if (rel < INT32_MIN || rel > INT32_MAX) {
                    VirtualFree(tramp_block, 0, MEM_RELEASE);
                    return false;
                }
                int32_t rel32 = static_cast<int32_t>(rel);
                tramp_cursor[0] = 0xE8;
                std::memcpy(tramp_cursor + 1, &rel32, 4);
                tramp_cursor += 5;
            } else {
                // Unconditional jmp (rel8/rel32) -> absolute form (always in range)
                writeAbsoluteJump(tramp_cursor, reinterpret_cast<void*>(target_addr));
            }
        } else if (insn.is_rip_relative) {
            std::memcpy(tramp_cursor, insn_ptr, insn.length);
            int32_t orig_disp = 0;
            std::memcpy(&orig_disp, insn_ptr + insn.disp_offset, 4);
            intptr_t target_mem = reinterpret_cast<intptr_t>(insn_ptr) + insn.length + orig_disp;
            intptr_t new_disp = target_mem - (reinterpret_cast<intptr_t>(tramp_cursor) + insn.length);
            if (new_disp < INT32_MIN || new_disp > INT32_MAX) {
                VirtualFree(tramp_block, 0, MEM_RELEASE);
                return false;
            }
            int32_t new_disp32 = static_cast<int32_t>(new_disp);
            std::memcpy(tramp_cursor + insn.disp_offset, &new_disp32, 4);
            tramp_cursor += insn.length;
        } else {
            std::memcpy(tramp_cursor, insn_ptr, insn.length);
            tramp_cursor += insn.length;
        }
    }

    // Append jump from end of trampoline back to target_func + stolen_len
    if (tramp_cursor + 14 > tramp_body_limit) {
        VirtualFree(tramp_block, 0, MEM_RELEASE);
        return false;
    }
    writeAbsoluteJump(tramp_cursor, src + stolen_len);

    // --- Patch the live prologue under thread suspension ---
    std::vector<HANDLE> suspended = suspendOtherThreads();

    // Relocating a thread IP across re-encoded instructions is not generally
    // possible, so if any other thread is stopped inside the bytes we are about
    // to overwrite, refuse the install rather than corrupt that thread.
    if (anyThreadIpInRange(suspended, stolen_lo, stolen_hi)) {
        resumeThreads(suspended);
        VirtualFree(tramp_block, 0, MEM_RELEASE);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(src, stolen_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        resumeThreads(suspended);
        VirtualFree(tramp_block, 0, MEM_RELEASE);
        return false;
    }

    if (out_record) {
        out_record->target_func = target_func;
        out_record->detour_func = detour_func;
        out_record->trampoline_func = tramp_block;
        out_record->patch_size = stolen_len;
        std::memcpy(out_record->orig_bytes, src, stolen_len);
        out_record->active = true;
    }

    // Publish the trampoline pointer BEFORE the jump is written, so any thread that
    // enters the detour can always reach the original function through it. The
    // barrier keeps the compiler/CPU from reordering the store past the patch.
    *out_trampoline = tramp_block;
    MemoryBarrier();

    uint8_t patch[32];
    std::memset(patch, 0x90, stolen_len); // NOP padding beyond the jump
    if (use_5byte_patch) {
        patch[0] = 0xE9;
        int32_t rel32 = static_cast<int32_t>(
            reinterpret_cast<intptr_t>(
#if defined(_M_X64) || defined(__x86_64__)
                relay_stub
#else
                detour_func
#endif
            ) - (reinterpret_cast<intptr_t>(src) + 5));
        std::memcpy(patch + 1, &rel32, 4);
    } else {
        uint8_t* p = patch;
        writeAbsoluteJump(p, detour_func);
    }
    std::memcpy(src, patch, stolen_len);

    DWORD dummy = 0;
    VirtualProtect(src, stolen_len, old_protect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), tramp_block, kTrampBlock);
    FlushInstructionCache(GetCurrentProcess(), src, stolen_len);

    resumeThreads(suspended);
    return true;
}

bool HookBackend::removeDetour(DetourEntry& record) {
    if (!record.active || !record.target_func || record.patch_size == 0) return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(record.target_func, record.patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(record.target_func, record.orig_bytes, record.patch_size);
    DWORD dummy = 0;
    VirtualProtect(record.target_func, record.patch_size, old_protect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), record.target_func, record.patch_size);
    record.active = false;
    return true;
}

} // namespace apivpn::platform
