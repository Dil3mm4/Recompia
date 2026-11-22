// SPDX-License-Identifier: GPL-3.0-or-later
// Recompia-only unified inventory payload, pure/testable and independent of runtime.
// This is intentionally a different packet format from NX2Apple.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxrt::games::pkp::unified::wire {
constexpr size_t kSlots = 130;
constexpr size_t kHeader = 28;
constexpr size_t kPacketBytes = kHeader + kSlots * 8;
constexpr uint8_t kProtocol = 1;
constexpr uint8_t kFromVisitor = 1, kFromHost = 2;
constexpr uint64_t kMoved = uint64_t{0x7fffffff} | (uint64_t{0xff} << 32);
struct Bag {
    std::array<uint64_t,kSlots> slots{};
    bool operator==(const Bag&) const = default;
};
inline bool valid(const Bag& bag) {
    for (uint64_t p : bag.slots) {
        const uint32_t id = static_cast<uint32_t>(p) & 0x1fff;
        const uint32_t count = static_cast<uint32_t>((p >> 32) & 0xff);
        if ((id==0 && count!=0) || (id!=0 && count==0) || (p & ~kMoved))
            return false;
    }
    return true;
}
inline uint64_t checksum(const Bag& b) {
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    for (uint64_t v:b.slots) for (int i=0;i<8;++i) {
        h^=static_cast<uint8_t>(v>>(i*8));
        h*=UINT64_C(0x100000001b3);
    }
    return h;
}
inline void append64(std::vector<uint8_t>& b,uint64_t v) {
    for (int i=0;i<8;++i)b.push_back(static_cast<uint8_t>(v>>(i*8)));
}
inline uint64_t load64(const uint8_t* b) {
    uint64_t v=0;
    for (int i=0;i<8;++i)v|=uint64_t{b[i]}<<(i*8);
    return v;
}
inline std::vector<uint8_t> packet(uint8_t kind,uint64_t seq,uint64_t ack,const Bag& bag) {
    if ((kind!=kFromVisitor && kind!=kFromHost) || seq==0 || !valid(bag))return {};
    std::vector<uint8_t> b;
    b.reserve(kPacketBytes);
    b.insert(b.end(),{'U','I',kProtocol,kind});
    append64(b,seq);append64(b,ack);append64(b,checksum(bag));
    for (uint64_t p:bag.slots)append64(b,p);
    return b;
}
inline bool parse(const uint8_t* data,size_t length,uint8_t* kind,uint64_t* seq,
                  uint64_t* ack,Bag* out) {
    if (!data || length!=kPacketBytes || data[0]!='U' || data[1]!='I' ||
        data[2]!=kProtocol || (data[3]!=kFromVisitor && data[3]!=kFromHost) ||
        !kind || !seq || !ack || !out) return false;
    const uint64_t s=load64(data+4),a=load64(data+12),hash=load64(data+20);
    if (s==0)return false;
    Bag b;
    for (size_t i=0;i<kSlots;++i)b.slots[i]=load64(data+kHeader+i*8);
    if (!valid(b) || checksum(b)!=hash)return false;
    *kind=data[3];*seq=s;*ack=a;*out=b;
    return true;
}
} // namespace nxrt::games::pkp::unified::wire
