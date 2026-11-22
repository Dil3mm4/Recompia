// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Modtopia inventory mod: no private Recompia linkage or raw host pointers.
#include "platform.h"
#include "inventory_wire.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
static const MtHost* g_host=nullptr;
static MtMod* g_mod=nullptr;
#define NXLOG_I(tag,...) mt_logf(g_host,g_mod,MT_LOG_INFO,__VA_ARGS__)
#define NXLOG_W(tag,...) mt_logf(g_host,g_mod,MT_LOG_WARN,__VA_ARGS__)
#define NXLOG_E(tag,...) mt_logf(g_host,g_mod,MT_LOG_ERROR,__VA_ARGS__)
namespace nxrt::coop_channel {
struct Message { int from; uint32_t type; const uint8_t* data; size_t size; };
struct MemberEvent { int node; bool joined; bool authority; };
inline int authority() { return g_host->session_authority(); }
inline int self() { return g_host->session_self(); }
inline bool is_open() { return self()>=0 && authority()>=0; }
inline bool is_authority() { return is_open() && self()==authority(); }
inline bool send(int peer,uint32_t type,const void* data,size_t size) {
    return g_host->session_send(g_mod,peer,type,data,size)==MT_OK;
}
}

namespace nxrt::games::pkp::unified {

namespace {
constexpr const char* kTag = "pkp";
constexpr const char* kBuild = "76a489f3bd3642fa5e18bd83d32c98ba";
constexpr uint32_t kChannelType = 0x55494e56;    // UINV; exclusive to this prototype
using wire::kFromVisitor;
using wire::kFromHost;
constexpr size_t kSlots = layout::kBagWholeSlots;
constexpr uint64_t kSlotBytes = items::kSlotSize;
constexpr uint64_t kBagBytes = kSlots * kSlotBytes;
constexpr uint64_t kActiveBagOffset = 0x13d5150;
constexpr uint64_t kInventoryManagerPointer = 0x3483b60;
constexpr double kReapplyGrace = 3.0;
constexpr double kSendGap = 0.10;
constexpr double kRetryGap = 1.0;
constexpr double kEmptyBagSettle = 1.0;
static_assert(kSlots == 130 && kBagBytes == 0x820);

struct HookSite { const char* name; uint64_t offset; uint32_t word; };
constexpr HookSite kSites[] = {
    {"serialize_online", 0x5b382c, 0x9100427au},
    {"serialize_offline", 0x5b3970, 0x910ab29au},
    {"restore_home", 0x5b3aac, 0xb94022a8u},
    {"before_reset", 0x5b08c0, 0xaa0003f3u},
    {"after_reset", 0x5b0a44, 0xa9415ff8u},
    {"arrival_import", 0x572acc, 0xf9400288u},
    {"importer_end", 0x572ae4, 0xaa1503e0u},
    {"resource_before", 0x539f1c, 0x910ec2d7u},
    {"resource_after", 0x539f80, 0x12000018u},
};

enum class Site : uintptr_t {
    OnlineSerialize, OfflineSerialize, HomeRestore, PreReset, PostReset,
    ArrivalImported, ImporterEnd, HostResource, HostResourceResult
};
using wire::Bag;
static_assert(kSlots==wire::kSlots);
struct Peer {
    bool joined = false;
    bool have = false;
    bool seeded = false;
    uint64_t latest_client = 0;
    uint64_t latest_host = 0;
    Bag bag;
};
struct State {
    uint64_t base = 0;
    bool armed = false;
    bool active_visit = false, return_pending = false, have_own = false;
    bool visitor_initialized = false, reapply = false, dirty = false;
    bool host_responded = false;
    bool hosting = false;
    bool in_reset = false;
    bool offline_capture_logged = false;
    uint64_t send_seq = 0, host_seq = 0, last_host_ack = 0;
    Bag own;
    Bag deferred;
    uint64_t deferred_seq = 0, deferred_ack = 0;
    double last_send = -1e9, last_write = -1e9, stage_change_at = -1e9;
    double empty_bag_since = -1e9;
    double last_send_fail = -1e9;
    std::array<Peer, 4> peers;
};
State& state() { static State s; return s; }
double seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

uint64_t save_root() {
    const uint64_t b = state().base;
    return b ? guest::deref(b + layout::kSaveRoot) : 0;
}
uint64_t common() {
    const uint64_t r = save_root();
    return guest::ok(r + layout::kSaveCommon, kBagBytes + layout::kBagOffline) ? r + layout::kSaveCommon : 0;
}
uint8_t session_bits() {
    const uint64_t gm = guest::deref(state().base + layout::kGameState);
    return guest::ok(gm + layout::kStateSession, 1) ? guest::rd<uint8_t>(gm + layout::kStateSession) : 0;
}
bool visitor_now() {
    const uint8_t bits = session_bits();
    return (bits & (layout::kSessionOnline | layout::kSessionVisitor)) ==
           (layout::kSessionOnline | layout::kSessionVisitor);
}
bool host_now() {
    const uint8_t bits = session_bits();
    return (bits & (layout::kSessionOnline | layout::kSessionHost)) ==
           (layout::kSessionOnline | layout::kSessionHost);
}
bool screen_temp() {
    const auto a = state().base + layout::kItemTempFlag;
    return guest::ok(a, 1) && guest::rd<uint8_t>(a) != 0;
}
struct NativeSlot {
    uint64_t vtable;
    uint64_t payload;
};
static_assert(sizeof(NativeSlot)==items::kSlotSize);
bool load_slots(uint64_t bag, std::array<NativeSlot,kSlots>* slots) {
    if (!bag || bag>UINT64_MAX-kBagBytes ||
        guest::host->mem_read(bag,slots->data(),kBagBytes)!=MT_OK) return false;
    const uint64_t vt=state().base+layout::kItemSlotVtable;
    for (const auto& slot:*slots)
        if (slot.vtable!=vt) return false;
    return true;
}
bool slots_ready(uint64_t bag) {
    std::array<NativeSlot,kSlots> slots{};
    return load_slots(bag,&slots);
}
// Pokopia's live player bag is reached through the game-owned inventory
// manager. The save object's ONLINE_OFF is a serializer buffer and can be
// empty while gameplay still has a populated bag; only use the live pointer
// after validating both end-slot vtables against this exact main build.
uint64_t active_bag() {
    State& s=state();
    const uint64_t singleton=guest::deref(s.base+kInventoryManagerPointer);
    const uint64_t manager=singleton ? guest::deref(singleton) : 0;
    if (!manager || manager>UINT64_MAX-kActiveBagOffset-kBagBytes) return 0;
    const uint64_t bag=manager+kActiveBagOffset;
    return slots_ready(bag) ? bag : 0;
}
bool payload_valid(const Bag& b) { return wire::valid(b); }

bool read_bag(uint64_t bag, Bag* out) {
    if (!out) return false;
    std::array<NativeSlot,kSlots> slots{};
    if (!load_slots(bag,&slots)) return false;
    for (size_t i=0;i<kSlots;++i)
        out->slots[i]=items::moved_of(slots[i].payload);
    return payload_valid(*out);
}
bool write_bag(uint64_t bag, const Bag& data) {
    if (!payload_valid(data)) return false;
    std::array<NativeSlot,kSlots> slots{};
    if (!load_slots(bag,&slots)) return false;
    for (size_t i=0;i<kSlots;++i) {
        const auto at=bag+i*kSlotBytes+items::kSlotPayload;
        const uint64_t value=items::moved_into(slots[i].payload,data.slots[i]);
        if (!guest::wr<uint64_t>(at,value)) return false;
    }
    return true;
}
bool write_bag_verified(uint64_t bag, const Bag& data) {
    if (!write_bag(bag,data)) return false;
    Bag check;
    return read_bag(bag,&check) && check==data;
}
uint64_t offline_bag() { const uint64_t c = common(); return c ? c + layout::kBagOffline : 0; }
uint64_t online_bag() { const uint64_t c = common(); return c ? c + layout::kBagOnline : 0; }
uint64_t host_mirror(int node) {
    const uint64_t live=active_bag();
    if (node<1 || node>3 || live<kActiveBagOffset) return 0;
    const uint64_t manager=live-kActiveBagOffset;
    const uint64_t offset=layout::kBagMirror+kBagBytes*static_cast<uint64_t>(node);
    if (manager>UINT64_MAX-offset-kBagBytes) return 0;
    const uint64_t bag=manager+offset;
    return slots_ready(bag) ? bag : 0;
}

uint32_t occupied(const Bag& b) {
    uint32_t n = 0;
    for (uint64_t v : b.slots) n += items::id_of(v) && items::count_of(v) ? 1 : 0;
    return n;
}
bool read_local_bag(Bag* out, const char** source, uint8_t* session,
                    uint32_t* offline_slots, uint32_t* online_slots) {
    Bag offline, online;
    Bag live;
    const bool have_offline=read_bag(offline_bag(),&offline);
    const bool have_online=read_bag(online_bag(),&online);
    const uint64_t live_ptr=active_bag();
    const bool have_live=live_ptr && read_bag(live_ptr,&live);
    const uint8_t bits=session_bits();
    if (session) *session=bits;
    if (offline_slots) *offline_slots=have_offline ? occupied(offline) : 0;
    if (online_slots) *online_slots=have_online ? occupied(online) : 0;
    if (!have_offline && !have_online && !have_live) return false;

    // Pokopia has separate offline and online buffers. During a normal home
    // load the offline buffer is authoritative; if it is empty but the online
    // buffer contains the player's bag, keep that live bag instead of caching
    // an empty one. A host session explicitly uses the online buffer.
    if (have_live) {
        *out=live;
        if (source) *source="live";
        return true;
    }
    const bool prefer_online=(bits & layout::kSessionHost) != 0;
    const bool choose_online=have_online &&
        (prefer_online || !have_offline ||
         (occupied(offline)==0 && occupied(online)>0));
    *out=choose_online ? online : offline;
    if (source) *source=choose_online ? "online" : "offline";
    return true;
}
bool snapshot_offline(const char* why) {
    State& s=state();
    Bag current;
    const char* source="unknown";
    uint8_t bits=0;
    uint32_t offline_count=0,online_count=0;
    if (read_local_bag(&current,&source,&bits,&offline_count,&online_count)) {
        // Travel can clear both live buffers briefly before the visitor bit flips.
        // Keep the last nonempty offline bag through that reset window.
        if (s.have_own && occupied(s.own)>0 && occupied(current)==0) {
            NXLOG_I(kTag,"Unified inventory: kept %u-slot snapshot through empty %s capture at %s",
                    occupied(s.own),source,why);
            return true;
        }
        const bool changed=!s.have_own || current!=s.own;
        s.own=current;s.have_own=true;
        if (changed)
            NXLOG_I(kTag,"Unified inventory: captured %s bag at %s (%u occupied; offline=%u online=%u; session=0x%02x)",
                    source,why,occupied(s.own),offline_count,online_count,bits);
        return true;
    }
    const uint64_t bag=offline_bag();
    NXLOG_W(kTag,"Unified inventory: offline capture failed at %s (save-root=%016llx common=%016llx bag=%016llx slots-ready=%u)",
            why,static_cast<unsigned long long>(save_root()),
            static_cast<unsigned long long>(common()),static_cast<unsigned long long>(bag),
            bag && slots_ready(bag) ? 1u : 0u);
    return false;
}
// A sort or stack merge is not a loss; during landing grace only prevent
// game-owned wipes of items, while accepting pickups (positive item gains).
bool item_loss(const Bag& before, const Bag& after) {
    std::map<uint32_t, int32_t> amounts;
    for (uint64_t v : before.slots) if (auto id=items::id_of(v))
        amounts[id] += static_cast<int32_t>(items::count_of(v));
    for (uint64_t v : after.slots) if (auto id=items::id_of(v))
        amounts[id] -= static_cast<int32_t>(items::count_of(v));
    for (const auto& [_, lost] : amounts) if (lost>0)return true;
    return false;
}
void draw_hud() {
    MtCall regs{};
    g_host->call_guest(state().base + layout::kItemBarRefresh, &regs);
}
bool apply_own(bool home) {
    State& s = state();
    if (!s.have_own || !payload_valid(s.own) || screen_temp()) return false;
    const uint64_t live=active_bag();
    if (!live || !write_bag(live,s.own)) {
        NXLOG_W(kTag,"Unified inventory: refused restore; verified live gameplay bag is unavailable");
        return false;
    }
    // Keep both serialized buffers in step for save/scene transitions, but
    // never mistake either one for the gameplay inventory.
    write_bag(offline_bag(),s.own);
    write_bag(online_bag(),s.own);
    // The game's script-visible native importer completion state: guarded 0/1.
    const uint64_t flag = save_root() + 0x1718430;
    if (!home && guest::ok(flag, 1) && guest::rd<uint8_t>(flag) <= 1)
        guest::wr<uint8_t>(flag, 1);
    s.last_write = seconds();
    s.reapply = false;
    draw_hud();
    NXLOG_I(kTag, "Unified inventory: %s %u occupied slots (%016llx)",
            home ? "home restore" : "visitor landing restore",
            occupied(s.own), static_cast<unsigned long long>(wire::checksum(s.own)));
    return true;
}


void send_own(bool force=false) {
    State& s=state();
    namespace ch=nxrt::coop_channel;
    if (!s.have_own || !visitor_now()) return;
    const int host=ch::authority();
    const bool native_peer=ch::is_open() && !ch::is_authority() && host>=0;
    if (!native_peer) return;
    const double now=seconds();
    if (!force && now-s.last_send<kSendGap) return;
    // Retries use the SAME version. Advancing seq for every HELLO/retry
    // means the newest host acknowledgment can never catch up.
    if (s.send_seq==0 || s.dirty) ++s.send_seq;
    const auto packet=wire::packet(kFromVisitor,s.send_seq,s.host_seq,s.own);
    const bool sent=ch::send(host,kChannelType,packet.data(),packet.size());
    if (sent) {
        const bool first = s.last_send < 0;
        s.last_send=now;s.dirty=false;
        if (first)
            NXLOG_I(kTag,"Unified inventory: sent visitor bag over session (%u slots, seq=%llu, host=%d)",
                    occupied(s.own),
                    static_cast<unsigned long long>(s.send_seq),host);
    } else {
        if (now-s.last_send_fail>=1.0) {
            s.last_send_fail=now;
            NXLOG_W(kTag,"Unified inventory: visitor bag send failed (host=%d)",host);
        }
    }
}
void host_publish(int node, bool initial=false, bool retry=false) {
    State& s=state();
    Peer& p=s.peers[static_cast<size_t>(node)];
    if (!p.have) return;
    if (!retry) ++p.latest_host;
    const auto packet=wire::packet(kFromHost,p.latest_host,p.latest_client,p.bag);
    const bool sent=nxrt::coop_channel::send(node,kChannelType,packet.data(),packet.size());
    if (initial && sent)
        NXLOG_I(kTag,"Unified inventory: acknowledged visitor %d (%u slots)",node,occupied(p.bag));
}
void reset_visit() {
    State& s=state();
    s.active_visit=false;s.return_pending=false;s.visitor_initialized=false;
    s.host_responded=false;s.reapply=false;s.in_reset=false;
    s.empty_bag_since=-1e9;
    s.send_seq=s.host_seq=s.last_host_ack=s.deferred_seq=s.deferred_ack=0;
    s.last_send=-1e9;
    s.peers={};
}
void receive(const nxrt::coop_channel::Message& m) {
    namespace ch=nxrt::coop_channel;
    State& s=state();
    uint8_t kind=0;uint64_t seq=0,ack=0;Bag bag;
    if (!s.armed || !wire::parse(m.data,m.size,&kind,&seq,&ack,&bag)) return;
    if (kind==kFromVisitor) {
        if (!host_now() || !ch::is_authority() || m.from<1 || m.from>3) return;
        Peer& p=s.peers[static_cast<size_t>(m.from)];
        if (!p.joined || seq<p.latest_client) return;
        if (seq==p.latest_client) {
            if (p.seeded) host_publish(m.from,false,true);
            return;
        }
        p.latest_client=seq;p.bag=bag;p.have=true;p.seeded=false;
        // The game checks and mutates this mirror when processing visitor requests.
        const uint64_t target=host_mirror(m.from);
        if (target && write_bag_verified(target,p.bag)) {
            p.seeded=true;
            NXLOG_I(kTag,"Unified inventory: host seeded visitor %d live mirror from seq=%llu (%u slots, digest=%016llx)",
                    m.from,static_cast<unsigned long long>(seq),occupied(p.bag),
                    static_cast<unsigned long long>(wire::checksum(p.bag)));
            host_publish(m.from,true);
        } else {
            NXLOG_W(kTag,"Unified inventory: host rejected visitor %d bag seq=%llu; live mirror unavailable or verification failed (active=%016llx target=%016llx)",
                    m.from,static_cast<unsigned long long>(seq),
                    static_cast<unsigned long long>(active_bag()),
                    static_cast<unsigned long long>(target));
        }
        return;
    }
    if (kind!=kFromHost || !visitor_now() || ch::is_authority() ||
        m.from!=ch::authority())return;
    if (seq<=s.host_seq) return;
    s.last_host_ack=std::max(s.last_host_ack,ack);
    // Do not let an older host snapshot overwrite a more recent local pickup.
    if (ack < s.send_seq || s.dirty) {
        s.deferred=bag;s.deferred_seq=seq;s.deferred_ack=ack;
        return;
    }
    s.own=bag;s.have_own=true;s.host_seq=seq;s.host_responded=true;
    s.visitor_initialized=true;s.reapply=true;
    NXLOG_I(kTag,"Unified inventory: host accepted visitor bag %llu (%u slots)",
            static_cast<unsigned long long>(seq),occupied(bag));
}
void member(const nxrt::coop_channel::MemberEvent& event) {
    State& s=state();
    if (!s.armed || event.node<0)return;
    if (nxrt::coop_channel::is_authority()) {
        if (event.node>0 && event.node<=3) {
            s.peers[static_cast<size_t>(event.node)] = Peer{};
            s.peers[static_cast<size_t>(event.node)].joined=event.joined;
        }
    } else if (event.joined && event.authority && visitor_now()) {
        // Keep the last OFFLINE snapshot until the landing overwrites the online bag.
        if (s.have_own)send_own(true);
    } else if (!event.joined && event.authority && s.active_visit) {
        s.return_pending=true;
    }
}
void host_tick() {
    State& s=state();
    for (int node=1;node<=3;++node) {
        Peer& p=s.peers[static_cast<size_t>(node)];
        if (!p.joined || !p.have || p.seeded)continue;
        const uint64_t ptr=host_mirror(node);
        if (ptr && write_bag_verified(ptr,p.bag)) {
            p.seeded=true;
            host_publish(node,true);
        }
    }
}
void visitor_tick() {
    State& s=state();
    if (s.reapply && s.have_own && !screen_temp() && !s.in_reset)
        apply_own(false);
    if (!s.visitor_initialized && s.have_own && seconds()-s.last_send>kRetryGap)
        send_own(true);  // the host's first join snapshot may precede the game's mirror creation
    if (!s.visitor_initialized || screen_temp() || s.in_reset) return;
    Bag current;
    const uint64_t live=active_bag();
    if (!live || !read_bag(live,&current))return;
    if (current != s.own) {
        // The game clears the live bag during loading and optimistically removes
        // items on a drop. Only the host's native transaction confirms a loss.
        if (item_loss(s.own,current)) {
            if (occupied(current)==0 || seconds()-s.last_write < kReapplyGrace)
                s.reapply=true;
            return;
        }
        s.own=current;s.dirty=true;
        send_own();
    } else if ((s.dirty || s.last_host_ack<s.send_seq) &&
               seconds()-s.last_send>=kRetryGap)send_own(true);
    // A host's deferred snapshot may only be accepted after it acknowledged our local write.
    if (s.deferred_seq>s.host_seq && s.deferred_ack>=s.send_seq && !s.dirty) {
        s.own=s.deferred;s.host_seq=s.deferred_seq;s.reapply=true;
        s.deferred_seq=0;
    }
}
void game_frame() {
    State& s=state();
    if (!s.armed)return;
    namespace ch=nxrt::coop_channel;
    if (host_now()) {
        s.hosting=true;
        if (s.active_visit) {s.return_pending=true;s.active_visit=false;}
        host_tick();
        return;
    }
    if (s.hosting) {
        s.peers={};
        s.hosting=false;
    }
    if (visitor_now()) {
        if (!s.active_visit) {
            // Preserve the departing bag before replacing it with the online bag.
            // Session state can flip before the next ordinary frame snapshot.
            if (!s.have_own) snapshot_offline("visitor entry");
            s.active_visit=true;s.return_pending=false;s.visitor_initialized=false;
            // A visitor can rejoin an island that stayed open, including after
            // restarting the client. Its first version must supersede the old trip.
            s.send_seq=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            s.host_seq=0;s.last_host_ack=0;
            s.deferred_seq=0;s.deferred_ack=0;
            s.dirty=true;s.last_send=-1e9;
            s.empty_bag_since=-1e9;
            s.reapply=s.have_own;s.stage_change_at=seconds();
            NXLOG_I(kTag,"Unified inventory: visitor session started; snapshot=%u carrying %u offline bag slots",
                    s.have_own ? 1u : 0u,occupied(s.own));
            send_own(true);
        }
        visitor_tick();
        return;
    }
    // The guest is offline again. The game's home load may finish after the LDN
    // connection has already disappeared; preserve the authoritative bag until its callback.
    if (s.active_visit) {
        Bag last;
        if (s.visitor_initialized && !s.in_reset && !screen_temp() &&
            read_bag(active_bag(),&last) && !item_loss(s.own,last))
            s.own=last;
        s.active_visit=false;s.return_pending=true;s.stage_change_at=seconds();
        NXLOG_I(kTag,"Unified inventory: left visiting session; waiting for home restore callback");
    }
    if (s.return_pending) {
        // Fallback only after the game has had time to complete the home load.
        if (seconds()-s.stage_change_at>5.0 && apply_own(true)) {
            s.return_pending=false; s.visitor_initialized=false;
        }
        return;
    }
    Bag current;
    const char* source="unknown";
    uint8_t bits=0;
    uint32_t offline_count=0,online_count=0;
    if (read_local_bag(&current,&source,&bits,&offline_count,&online_count)) {
        const bool transient_empty=s.have_own && occupied(s.own)>0 && occupied(current)==0;
        if (transient_empty) {
            if (s.empty_bag_since < 0) s.empty_bag_since=seconds();
            if (seconds()-s.empty_bag_since < kEmptyBagSettle) return;
        } else {
            s.empty_bag_since=-1e9;
        }
        if (s.have_own && current==s.own) return;
        s.own=current;s.have_own=true;
        NXLOG_I(kTag,"Unified inventory: local bag snapshot from %s (%u occupied; offline=%u online=%u; session=0x%02x)",
                source,occupied(s.own),offline_count,online_count,bits);
        s.offline_capture_logged=true;
    }
}
int hook(MtHookCtx* ctx, void* user) {
    State& s=state();
    if (!s.armed)return 0;
    switch (static_cast<Site>(reinterpret_cast<uintptr_t>(user))) {
    case Site::PreReset:
        if (!s.active_visit) snapshot_offline("pre-reset");
        if (s.active_visit) {
            if (!s.have_own) snapshot_offline("visitor pre-reset");
            Bag b;
            if (s.visitor_initialized && !s.in_reset && !screen_temp() &&
                read_bag(active_bag(),&b) && !item_loss(s.own,b) && b!=s.own) {
                s.own=b;s.dirty=true;send_own();
            }
            s.in_reset=true;
            s.reapply=true;
        }
        break;
    case Site::PostReset:
        if (s.active_visit) {
            s.in_reset=false;
            s.reapply=true;
            // Not an assertion that the entire load finished: importer may run afterward.
        }
        break;
    case Site::ArrivalImported:
        if (s.active_visit && active_bag() && write_bag(active_bag(),s.own)) {
            if (g_host->reg_get(ctx,21) && g_host->reg_get(ctx,22)==kBagBytes && slots_ready(g_host->reg_get(ctx,21)))
                write_bag(g_host->reg_get(ctx,21),s.own);
            write_bag(offline_bag(),s.own);
            write_bag(online_bag(),s.own);
            s.reapply=true;
            NXLOG_I(kTag,"Unified inventory: wrote authoritative bag into verified live gameplay inventory");
        }
        break;
    case Site::ImporterEnd:
        if (s.active_visit) s.reapply=true; // also covers no-stream import
        break;
    case Site::HomeRestore:
        if (s.return_pending && g_host->reg_get(ctx,19)==common() && apply_own(true)) {
            s.return_pending=false;s.visitor_initialized=false;
        }
        break;
    case Site::HostResource: {
        if (!host_now())break;
        const int node=static_cast<int>(g_host->reg_get(ctx,19)&0xff);
        if (node>0 && node<=3) {
            const Peer& p=s.peers[static_cast<size_t>(node)];
            const uint64_t target=host_mirror(node);
            if (p.joined && p.have) {
                const bool ok=target && write_bag_verified(target,p.bag);
                NXLOG_I(kTag,"Unified inventory: pre-ResourceUse visitor=%d bag=%s target=%016llx digest=%016llx",
                        node,ok?"verified":"REJECTED",static_cast<unsigned long long>(target),
                        static_cast<unsigned long long>(wire::checksum(p.bag)));
            } else {
                NXLOG_W(kTag,"Unified inventory: pre-ResourceUse has no visitor bag (visitor=%d joined=%u have=%u target=%016llx)",
                        node,p.joined?1u:0u,p.have?1u:0u,static_cast<unsigned long long>(target));
            }
        }
        break;
    }
    case Site::HostResourceResult: {
        // Immediately after the native validator returns: w19 is the
        // authenticated player and w0 is the success bit sent in the response.
        if (!host_now()) break;
        const int node=static_cast<int>(g_host->reg_get(ctx,19)&0xff);
        if (node<1 || node>3) break;
        Peer& p=s.peers[static_cast<size_t>(node)];
        if (!p.joined || !p.have || !p.seeded) break;
        const bool accepted=(g_host->reg_get(ctx,0)&1)!=0;
        Bag result;
        if (accepted && read_bag(host_mirror(node),&result))
            p.bag=result;
        host_publish(node);
        NXLOG_I(kTag,"Unified inventory: native ResourceUse visitor=%d %s (%u slots, digest=%016llx)",
                node,accepted?"accepted":"rejected",occupied(p.bag),
                static_cast<unsigned long long>(wire::checksum(p.bag)));
        break;
    }
    case Site::OnlineSerialize:
        if (!s.have_own) break;
        break;
    case Site::OfflineSerialize:
        // This serializer runs on the home side while travel is preparing the
        // guest save. It is the last stable point before the game swaps bags.
        if (!s.active_visit) snapshot_offline("offline serialize");
        break;
    }
    return 0;
}
// Each hook's expected word is declared in mod.toml and checked by Modtopia.
}  // namespace

void receive_message(MtMod*, int32_t from, uint32_t type, const void* bytes, size_t n, void*) {
    receive(nxrt::coop_channel::Message{from,type,static_cast<const uint8_t*>(bytes),n});
}
void member_event(MtMod*, int32_t node, int joined, int authority, void*) {
    member(nxrt::coop_channel::MemberEvent{node,joined!=0,authority!=0});
}
int frame_event(MtMod*, const MtEvent* ev, void*) {
    if (ev && ev->name && std::strcmp(ev->name,"FrameStart")==0) game_frame();
    return MT_EVENT_CONTINUE;
}
int permissions_hook(MtHookCtx* ctx, void*) {
    if (!state().armed) return MT_CONTINUE;
    if (g_host->reg_get(ctx,22) & 1) return MT_CONTINUE;
    if (g_host->reg_get(ctx,13) != 0x5ffffffeull)
        g_host->reg_set(ctx,13,0x5ffffffeull);
    return MT_CONTINUE;
}
int init_mod(const MtHost* host, MtMod* mod) {
    if (!host || !mod || !MT_HOST_HAS(host,session_authority) ||
        !host->session_subscribe || !host->session_send ||
        !host->session_self || !host->session_authority) return MT_ERR_VERSION;
    g_host=host;g_mod=mod;guest::host=host;
    // The mod's enabled state is the one and only opt-in, so the built-in
    // Modtopia Link Play mod fingerprint cannot hide mismatched settings.
    // Seamless co-op is a different game mode; the launcher marks those launches.
#if defined(_WIN32)
    char* coop_raw=nullptr;
    size_t coop_len=0;
    if (_dupenv_s(&coop_raw,&coop_len,"NXRT_PKP_COOP")!=0) return MT_ERR_STATE;
    std::string coop=coop_raw ? coop_raw : "";
    std::free(coop_raw);
#else
    const char* coop_raw=std::getenv("NXRT_PKP_COOP");
    std::string coop=coop_raw ? coop_raw : "";
#endif
    for (char& c:coop) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (coop=="1" || coop=="true" || coop=="on" || coop=="yes") {
        mt_logf(host,mod,MT_LOG_INFO,"Inventory Link Play mod is inactive in seamless co-op");
        return MT_OK;
    }
    std::string build=host->game_build_id?host->game_build_id:"";
    for (char& c:build) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (build.compare(0,32,kBuild)!=0) {
        mt_logf(host,mod,MT_LOG_ERROR,"Unsupported main build: %s",build.c_str());
        return MT_ERR_VERSION;
    }
    State& s=state();
    s.base=host->module_base("main");
    if (!s.base) return MT_ERR_STATE;
    for (size_t i=0;i<std::size(kSites);++i) {
        if (host->hook(mod,kSites[i].name,&hook,reinterpret_cast<void*>(i))!=MT_OK)
            return MT_ERR_STATE;
    }
    if (host->hook(mod,"permissions",&permissions_hook,nullptr)!=MT_OK)
        return MT_ERR_STATE;
    if (host->subscribe(mod,"FrameStart",MT_PRIORITY_NORMAL,&frame_event,nullptr)!=MT_OK)
        return MT_ERR_STATE;
    s.armed=true;
    const int rc=host->session_subscribe(mod,kChannelType,&receive_message,&member_event,nullptr);
    if (rc!=MT_OK) {
        s.armed=false;
        return rc;
    }
    mt_logf(host,mod,MT_LOG_INFO,"Guest inventory: 10 hooks, 130 slots, session transport available");
    return MT_OK;
}
} // namespace nxrt::games::pkp::unified

MT_DECLARE_MOD()
MT_EXTERN_C MT_EXPORT int mod_init(const MtHost* host,MtMod* mod) {
    return nxrt::games::pkp::unified::init_mod(host,mod);
}
