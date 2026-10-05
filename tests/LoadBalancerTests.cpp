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

TEST(LoadBalancer, ReturnsNullptrWhenNoUpstreams)
{
    net::io_context ioc;
    LoadBalancer lb(config_with(""), ioc);

    EXPECT_EQ(lb.next(), nullptr);
}

TEST(LoadBalancer, ReturnsTheSameUpstreamForASingleEntry)
{
    net::io_context ioc;
    LoadBalancer lb(config_with(R"(
[[upstream]]
name = "only"
host = "127.0.0.1"
port = 8081
)"), ioc);

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
    LoadBalancer lb(config_with(R"(
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
)"), ioc);

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
    LoadBalancer lb(config_with(""), ioc);

    EXPECT_NO_THROW(lb.resolver().cancel());
}

TEST(LoadBalancer, TreatsEveryUpstreamAsHealthy)
{
    net::io_context ioc;
    LoadBalancer lb(config_with(R"(
[[upstream]]
name = "a"
host = "127.0.0.1"
port = 8001
)"), ioc);

    const HostData* up = lb.next();
    ASSERT_NE(up, nullptr);
    EXPECT_TRUE(lb.is_healthy(*up));
}