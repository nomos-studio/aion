// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// aion_control_thread — rt_control_thread subclass that handles msg_route_set.
//
// All other message types are handled by the base class (session, graph, params,
// notes, modulators, Link transport, scheduler bundles).  This class only adds
// routing matrix updates.

#include <nomos/rt/ipc.hpp>
#include <nomos/rt/ipc_channel.hpp>
#include <nomos/rt/rt_control_thread.hpp>

#include "routing_matrix.hpp"

#include <optional>
#include <string>

namespace aion {

class aion_control_thread final : public nomos::rt::rt_control_thread {
  public:
    aion_control_thread(nomos::rt::rt_control_thread::config cfg,
                        nomos::rt::param_queue&              queue,
                        nomos::rt::input_event_queue&        in_queue,
                        RoutingMatrix&                       matrix)
        : nomos::rt::rt_control_thread{std::move(cfg), queue, in_queue}
        , matrix_{matrix}
    {}

  protected:
    void dispatch_extension(int /*conn_fd*/,
                            const nomos::rt::ipc::message& msg,
                            std::optional<nomos::rt::session>& /*sess*/) override {
        if (msg.type() != nomos::rt::ipc::msg_route_set) return;
        const std::string_view payload{
            reinterpret_cast<const char*>(msg.payload.data()),
            msg.payload.size()};
        matrix_.apply_edn(payload);
    }

  private:
    RoutingMatrix& matrix_;
};

} // namespace aion
