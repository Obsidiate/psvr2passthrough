#include "layer_dispatch.h"
#include "layer_session.h"
#include "logging.h"

#include <cstring>

namespace psvr2pt {

LayerState& LayerState::get() {
    static LayerState s;
    return s;
}

InstanceState* LayerState::add(XrInstance instance) {
    std::lock_guard lock(mu_);
    auto s = std::make_unique<InstanceState>();
    s->instance = instance;
    auto* raw = s.get();
    instances_[instance] = std::move(s);
    return raw;
}

InstanceState* LayerState::find(XrInstance instance) {
    std::lock_guard lock(mu_);
    auto it = instances_.find(instance);
    return it == instances_.end() ? nullptr : it->second.get();
}

void LayerState::remove(XrInstance instance) {
    std::lock_guard lock(mu_);
    // Also clear any session->instance mappings that pointed at this instance.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        auto inst_it = instances_.find(instance);
        if (inst_it != instances_.end() && it->second == inst_it->second.get()) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    instances_.erase(instance);
}

InstanceState* LayerState::find_for_session(XrSession session) {
    std::lock_guard lock(mu_);
    auto it = sessions_.find(session);
    return it == sessions_.end() ? nullptr : it->second;
}

void LayerState::register_session(XrSession session, InstanceState* inst) {
    std::lock_guard lock(mu_);
    sessions_[session] = inst;
}

void LayerState::unregister_session(XrSession session) {
    std::lock_guard lock(mu_);
    sessions_.erase(session);
}

}  // namespace psvr2pt
