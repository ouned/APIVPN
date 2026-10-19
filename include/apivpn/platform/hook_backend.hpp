#pragma once

#include <cstdint>
#include <vector>

namespace apivpn::platform {

struct DetourEntry {
    void*   target_func     = nullptr;
    void*   detour_func     = nullptr;
    void*   trampoline_func = nullptr;
    uint8_t orig_bytes[32]  = {};
    size_t  patch_size      = 0;
    bool    active          = false;
};

class HookBackend {
public:
    // Install an inline detour on target_func redirecting to detour_func.
    //
    // Contract:
    //  - On success: returns true and writes a callable trampoline pointer (the
    //    relocated original prologue, ending in a jump back to the function body)
    //    to *out_trampoline. If out_record is non-null it is fully populated and
    //    its .active flag is set, so it can later be passed to removeDetour.
    //  - On failure: returns false and leaves *out_trampoline UNCHANGED (callers
    //    must initialise it to null and treat a false return as "not hooked",
    //    falling back to the raw GetProcAddress pointer). out_record is left
    //    untouched. No bytes of target_func are modified, so the target stays
    //    callable. The decoder fails closed: if any prologue instruction is not
    //    explicitly understood, or a relative branch cannot be relocated into
    //    range, the install is refused rather than risking code corruption.
    //  - target_func, detour_func and out_trampoline must all be non-null.
    static bool installDetour(void* target_func, void* detour_func, void** out_trampoline, DetourEntry* out_record = nullptr);

    // Restore original prologue bytes and free trampoline.
    // Returns false if the record is inactive/empty or the restore failed.
    static bool removeDetour(DetourEntry& record);
};

} // namespace apivpn::platform
