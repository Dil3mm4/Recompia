// SPDX-License-Identifier: GPL-3.0-or-later
// Modtopia API 0.8: per-mod messages on the game's current Link Play session.
// The Modtopia loader and native mod libraries remain mapped until process exit.
#include "mods_internal.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#include "nxrt/coop_channel.h"

namespace nxrt::mods::detail {
namespace {
struct Subscription {
    MtMod* mod;
    MtSessionMessageFn message;
    MtSessionMemberFn member;
    void* user;
};
struct Registry {
    std::mutex mu;
    std::unordered_map<uint32_t, Subscription> types;
    bool opened = false;
};
Registry& registry() { static Registry s; return s; }

void notify_member(const coop_channel::MemberEvent& e) {
    std::vector<Subscription> listeners;
    {
        Registry& r = registry();
        std::lock_guard lock(r.mu);
        for (const auto& [type, sub] : r.types)
            if (sub.member) listeners.push_back(sub);
    }
    for (const Subscription& sub : listeners)
        sub.member(sub.mod, e.node, e.joined ? 1 : 0, e.authority ? 1 : 0, sub.user);
}
} // namespace

int session_subscribe(MtMod* mod, uint32_t type, MtSessionMessageFn message,
                      MtSessionMemberFn member, void* user) {
    if (!mod || !type || !message) return MT_ERR;
    Registry& r = registry();
    std::lock_guard lock(r.mu);
    if (r.types.contains(type)) return MT_ERR_STATE;
    r.types.emplace(type, Subscription{mod, message, member, user});
    coop_channel::on_message(type, [type](const coop_channel::Message& e) {
        Subscription sub{};
        {
            Registry& r = registry();
            std::lock_guard lock(r.mu);
            const auto it = r.types.find(type);
            if (it == r.types.end()) return;
            sub = it->second;
        }
        sub.message(sub.mod, e.from, e.type, e.data, e.size, sub.user);
    });
    if (!r.opened) {
        coop_channel::on_member(&notify_member);
        if (!coop_channel::open()) {
            coop_channel::on_message(type, {});
            coop_channel::on_member({});
            r.types.erase(type);
            return MT_ERR_STATE;
        }
        r.opened = true;
    }
    return MT_OK;
}

int session_send(MtMod* mod, int32_t peer, uint32_t type, const void* data, size_t size) {
    if (!mod || !type || (!data && size) || size > 1048576) return MT_ERR;
    {
        Registry& r = registry();
        std::lock_guard lock(r.mu);
        const auto it = r.types.find(type);
        if (it == r.types.end() || it->second.mod != mod) return MT_ERR_STATE;
    }
    if (peer < 0 || !coop_channel::is_open()) return MT_ERR_STATE;
    return coop_channel::send(peer, type, data, size) ? MT_OK : MT_ERR_STATE;
}
int32_t session_self() { return coop_channel::self(); }
int32_t session_authority() { return coop_channel::authority(); }
void session_drain() {
    if (!registry().opened) return;
    // ldn_reset() closes the session channel while keeping its registrations.
    // Reopen it after a reset so a second visit or a changed transport can join.
    if (!coop_channel::is_open()) coop_channel::open();
    coop_channel::drain(128);
}
} // namespace nxrt::mods::detail
