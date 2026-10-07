#include "../include/rproxy/Config.hpp"
#include "../include/rproxy/LoadBalancer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

Config config_with(const std::string& body) {
    return Config(toml::parse(body));
}

}  // namespace

// @Usage: Every case below binds the Config to a named local before constructing the
// LoadBalancer. LoadBalancer holds `const Config&`, and config_with returns by value, so
// writing `LoadBalancer lb(config_with(...), ioc)` leaves m_config dangling the instant
// the statement ends and next() then reads freed memory. It surfaced as an intermittent
// access violation in ReturnsTheSameUpstreamForASingleEntry, which touches no sockets:
// the crash rate tracked nothing but heap reuse.
//
// This is the same hazard as the Beast serializer issue on the session side, one level up:
// a reference whose referent does not outlive the user of that reference.

TEST(LoadBalancer, ReturnsNullptrWhenNoUpstreams)
{
    net::io_context ioc;
    const Config config = config_with("");
    LoadBalancer lb(config, ioc);

    EXPECT_EQ(lb.next(), nullptr);
}

TEST(LoadBalancer, ReturnsTheSameUpstreamForASingleEntry)
{
    net::io_context ioc;
    const Config config = config_with(R"(
[[upstream]]
name = "only"
host = "127.0.0.1"
port = 8081
)");
    LoadBalancer lb(config, ioc);

    for (int i = 0; i < 5; ++i) {
        const HostData* up = lb.next();
        ASSERT_NE(up, nullptr);
        EXPECT_EQ(up->name, "only");
        EXPECT_EQ(up->host, "127.0.0.1");
        EXPECT_EQ(up->port, 8081);
        EXPECT_FALSE(up->uses_tls);
    }
}

TEST(LoadBalancer, CyclesThroughEveryUpstreamInOrder)
{
    net::io_context ioc;
    const Config config = config_with(R"(
[[upstream]]
name = "a"
host = "127.0.0.1"
port = 8001

[[upstream]]
name = "b"
host = "127.0.0.1"
port = 8002

[[upstream]]
name = "c"
host = "127.0.0.1"
port = 8003
)");
    LoadBalancer lb(config, ioc);

    std::vector<std::string> seen;
    for (int i = 0; i < 7; ++i) {
        const HostData* up = lb.next();
        ASSERT_NE(up, nullptr);
        seen.push_back(up->name);
    }

    EXPECT_EQ(seen, (std::vector<std::string>{"a", "b", "c", "a", "b", "c", "a"}));
}

TEST(LoadBalancer, ResolverIsUsable)
{
    net::io_context ioc;
    const Config config = config_with("");
    LoadBalancer lb(config, ioc);

    EXPECT_NO_THROW(lb.resolver().cancel());
}

TEST(LoadBalancer, TreatsEveryUpstreamAsHealthy)
{
    net::io_context ioc;
    const Config config = config_with(R"(
[[upstream]]
name = "a"
host = "127.0.0.1"
port = 8001
)");
    LoadBalancer lb(config, ioc);

    const HostData* up = lb.next();
    ASSERT_NE(up, nullptr);
    EXPECT_TRUE(lb.is_healthy(*up));
}