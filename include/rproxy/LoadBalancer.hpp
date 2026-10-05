#pragma once

#include <atomic>
#include <cstddef>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "Config.hpp"

namespace net = boost::asio;

// @Refactor: Health monitoring belongs in a separate HealthMonitor class that
// this consults. Only the seam exists here so next() does not change shape in M10.
class LoadBalancer {
public:
    LoadBalancer(const Config& config, net::io_context& ioc)
        : m_config(config), m_resolver(ioc) {}

    // @Usage: Advance the round-robin cursor and return the next upstream, or
    // nullptr when the upstream list is empty. Called at accept time, before the
    // request is read, because round-robin needs no request data. That ordering is
    // what lets M5 pick a concrete session type from (client leg TLS, upstream TLS).
    const HostData* next() noexcept {
        const auto& upstreams = m_config.get_upstreams();
        if (upstreams.empty()) {
            return nullptr;
        }

        const auto index = m_counter.fetch_add(1, std::memory_order_relaxed);
        // @Usage: Skips unhealthy upstreams, bounded by the list length so an all
        // unhealthy list still returns nullptr rather than spinning. Consulted here, not
        // left to the caller, so adding health checks in M10 needs no other change.
        for (std::size_t i = 0; i < upstreams.size(); ++i) {
            const auto& candidate_entry = upstreams[(index + i) % upstreams.size()];
            if (is_healthy(candidate_entry)) {
                return &candidate_entry;
            }
        }

        return nullptr;
    }

    // @Usage: Exposed so sessions can call async_resolve. Never call the blocking
    // resolve() on it: a blocking lookup stalls the whole io_context thread on DNS.
    net::ip::tcp::resolver& resolver() noexcept { return m_resolver; }

    // @Usage: Always true until M10 wires a HealthMonitor in. next() already
    // consults this, so adding real checks later needs no change to next().
    bool is_healthy(const HostData&) const noexcept { return true; }

private:
    const Config& m_config;
    net::ip::tcp::resolver m_resolver;
    std::atomic<std::size_t> m_counter{0};
};
