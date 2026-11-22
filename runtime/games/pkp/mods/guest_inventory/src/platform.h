// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Pokopia 2.0.1 memory layout used only by the standalone inventory mod.
// Every guest access goes through the public Modtopia ABI, never a host pointer.
#include <modtopia/modtopia.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nxrt::games::pkp {
namespace layout {
constexpr uint64_t kSaveRoot = 0x3561930;
constexpr uint64_t kGameState = 0x3561710;
constexpr uint64_t kStateSession = 0x90;
constexpr uint8_t kSessionOnline = 0x01, kSessionHost = 0x02, kSessionVisitor = 0x04;
constexpr uint64_t kSaveCommon = 0x13d4920;
constexpr uint64_t kBagOffline = 0x830;
constexpr uint64_t kBagOnline = 0x10;
constexpr uint64_t kBagMirror = 0x17154a8;
constexpr size_t kBagWholeSlots = 130;
constexpr uint64_t kItemSlotVtable = 0x322c1d8;
constexpr uint64_t kItemTempFlag = 0x36bbfd8;
constexpr uint64_t kItemBarRefresh = 0xa8e724;
}
namespace items {
constexpr uint64_t kSlotSize = 16, kSlotPayload = 8;
constexpr uint64_t kMoved = uint64_t{0x7fffffff} | (uint64_t{0xff} << 32);
constexpr uint32_t id_of(uint64_t p) { return static_cast<uint32_t>(p) & 0x1fff; }
constexpr uint32_t count_of(uint64_t p) { return static_cast<uint32_t>((p >> 32) & 0xff); }
constexpr uint64_t moved_of(uint64_t p) { return p & kMoved; }
constexpr uint64_t moved_into(uint64_t dst, uint64_t src) {
    return (dst & ~kMoved) | (src & kMoved);
}
}
namespace guest {
inline const MtHost* host = nullptr;
inline bool ok(uint64_t a, size_t n) {
    if (!host || !a || n > 0x10000 || a > UINT64_MAX-n) return false;
    std::array<uint8_t,8192> buffer{};
    if (n<=buffer.size()) return host->mem_read(a, buffer.data(), n) == MT_OK;
    std::vector<uint8_t> bytes(n);
    return host->mem_read(a, bytes.data(), n) == MT_OK;
}
template<class T> T rd(uint64_t a) {
    T value{};
    if (host && a) host->mem_read(a, &value, sizeof value);
    return value;
}
template<class T> bool wr(uint64_t a, T value) {
    return host && a && host->mem_write(a, &value, sizeof value) == MT_OK;
}
inline uint64_t deref(uint64_t a) { return rd<uint64_t>(a); }
}
}
