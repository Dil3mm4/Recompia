// SPDX-License-Identifier: GPL-3.0-or-later
// Pokopia's cloud-island worker entry, lifted as a host override so the SDK can start it through call_indirect.
#include "nxrt/dispatch.h"
#include "nxrt/log.h"

#include <cstdint>
#include <mutex>

namespace {
constexpr uint64_t kMainBase = 0x7100000000ull;

uint64_t read_guest_word(uint64_t address) {
    return *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(address));
}
}  // namespace

extern "C" void h_cloud_island_thread_entry(Ctx* c) {
    static std::once_flag logged;
    std::call_once(logged, [] { NXLOG_I("net", "cloud: main+0x1769584 (cloud island thread entry) answered on the host"); });

    const uint64_t object = c->x[0];
    const uint64_t lr = c->x[30];
    c->x[0] = read_guest_word(object + 0x1c8);
    call_indirect(c, read_guest_word(object + 0x1c0));
    if (read_guest_word(object + 0x1c0) != read_guest_word(kMainBase + 0x34ae828)) {
        c->x[0] = object;
        call_indirect(c, kMainBase + 0x17698f8);
    }
    c->x[30] = lr;
}
