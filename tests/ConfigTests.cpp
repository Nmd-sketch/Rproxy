#include "../include/rproxy/Config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

toml::table parse_ok(const std::string& text) {
    return toml::parse(text);
}

class TempFile {
public:
    TempFile(std::string path, std::string contents) : path_(std::move(path)) {
        std::ofstream file(path_, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << contents;
        }
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

std::string error_text(const toml::table& root) {
    try {
        Config config(root);
        (void)config;
        return {};
    } catch (const ConfigError& e) {
        return e.what();
    }
}

}  // namespace

TEST(Config, ParsesEveryField)
{
    const toml::table root = parse_ok(R"(
[server]
port = 1234
threads = 4

[server.tls]
cert = "folder/cert.pem"
key = "folder/key.pem"

[[upstream]]
name = "backend"
host = "1.2.3.4"
port = 8080
tls = true

[limits]
header_bytes = 8192
body_bytes = 7654321

[timeouts]
connect_ms = 5000
handshake_ms = 6000
read_ms = 7000
)");

    const Config config(root);

    EXPECT_EQ(config.get_port(), 1234);
    EXPECT_EQ(config.get_thread_num(), 4u);
    EXPECT_EQ(config.get_certificate_path(), "folder/cert.pem");
    EXPECT_EQ(config.get_key_path(), "folder/key.pem");

    ASSERT_EQ(config.get_upstreams().size(), 1u);
    EXPECT_EQ(config.get_upstreams()[0].name, "backend");
    EXPECT_EQ(config.get_upstreams()[0].host, "1.2.3.4");
    EXPECT_EQ(config.get_upstreams()[0].port, 8080u);
    EXPECT_TRUE(config.get_upstreams()[0].uses_tls);

    EXPECT_EQ(config.get_header_bytes(), 8192u);
    EXPECT_EQ(config.get_body_bytes(), 7654321u);
    EXPECT_EQ(config.get_connect_ms(), std::chrono::milliseconds(5000));
    EXPECT_EQ(config.get_handshake_ms(), std::chrono::milliseconds(6000));
    EXPECT_EQ(config.get_read_ms(), std::chrono::milliseconds(7000));
}

// @Usage: These are the values an operator gets by writing no [limits] section at all,
// so they are part of the contract, not incidental. 8 KiB of headers and 8 MiB of body
// come from the design doc, section 5 step 2. The body default in particular has to be
// generous: an 8 KiB cap rejects every non trivial POST with a 413, and the operator who
// hits that has no [limits] block to go looking at.
TEST(Config, EmptyDocumentYieldsDocumentedDefaults)
{
    const Config config(parse_ok(""));

    EXPECT_EQ(config.get_port(), 8080);
    EXPECT_EQ(config.get_thread_num(), 2u);
    EXPECT_EQ(config.get_certificate_path(), "certificates/cert.pem");
    EXPECT_EQ(config.get_key_path(), "certificates/key.pem");
    EXPECT_TRUE(config.get_upstreams().empty());
    EXPECT_EQ(config.get_header_bytes(), 8u * 1024);
    EXPECT_EQ(config.get_body_bytes(), 8u * 1024 * 1024);
    EXPECT_EQ(config.get_connect_ms(), std::chrono::milliseconds(5000));
    EXPECT_EQ(config.get_handshake_ms(), std::chrono::milliseconds(8000));
    EXPECT_EQ(config.get_read_ms(), std::chrono::milliseconds(30000));
}

TEST(Config, PartialDocumentKeepsDefaultsForOmittedKeys)
{
    const Config config(parse_ok(R"(
[server]
threads = 8

[timeouts]
read_ms = 1000
)"));

    EXPECT_EQ(config.get_port(), 8080);
    EXPECT_EQ(config.get_thread_num(), 8u);
    EXPECT_EQ(config.get_read_ms(), std::chrono::milliseconds(1000));
    EXPECT_EQ(config.get_connect_ms(), std::chrono::milliseconds(5000));
    EXPECT_EQ(config.get_handshake_ms(), std::chrono::milliseconds(8000));
    EXPECT_EQ(config.get_certificate_path(), "certificates/cert.pem");
}

TEST(Config, MultipleUpstreamsPreserveOrder)
{
    const Config config(parse_ok(R"(
[[upstream]]
name = "first"
host = "10.0.0.1"
port = 80

[[upstream]]
name = "second"
host = "10.0.0.2"
port = 443
tls = true

[[upstream]]
name = "third"
host = "10.0.0.3"
port = 8080
)"));

    ASSERT_EQ(config.get_upstreams().size(), 3u);
    EXPECT_EQ(config.get_upstreams()[0].name, "first");
    EXPECT_EQ(config.get_upstreams()[1].name, "second");
    EXPECT_TRUE(config.get_upstreams()[1].uses_tls);
    EXPECT_EQ(config.get_upstreams()[2].host, "10.0.0.3");
    EXPECT_FALSE(config.get_upstreams()[2].uses_tls);
}

TEST(Config, UpstreamTlsDefaultsToFalseWhenOmitted)
{
    const Config config(parse_ok(R"(
[[upstream]]
name = "plain"
host = "127.0.0.1"
port = 9000
)"));

    ASSERT_EQ(config.get_upstreams().size(), 1u);
    EXPECT_FALSE(config.get_upstreams()[0].uses_tls);
}

TEST(Config, BoundaryPortsAreAccepted)
{
    const Config config(parse_ok(R"(
[server]
port = 65535

[[upstream]]
name = "low"
host = "127.0.0.1"
port = 1
)"));

    EXPECT_EQ(config.get_port(), 65535);
    ASSERT_EQ(config.get_upstreams().size(), 1u);
    EXPECT_EQ(config.get_upstreams()[0].port, 1u);
}

TEST(Config, SingleThreadIsAccepted)
{
    const Config config(parse_ok("[server]\nthreads = 1\n"));
    EXPECT_EQ(config.get_thread_num(), 1u);
}

TEST(Config, RejectsOutOfRangeServerPort)
{
    const std::string error = error_text(parse_ok("[server]\nport = 0\n"));
    EXPECT_NE(error.find("server.port"), std::string::npos) << error;
    EXPECT_NE(error.find("outside allowed range"), std::string::npos) << error;
}

TEST(Config, RejectsServerPortAboveMax)
{
    const std::string error = error_text(parse_ok("[server]\nport = 70000\n"));
    EXPECT_NE(error.find("server.port"), std::string::npos) << error;
}

TEST(Config, RejectsZeroThreads)
{
    const std::string error = error_text(parse_ok("[server]\nthreads = 0\n"));
    EXPECT_NE(error.find("server.threads"), std::string::npos) << error;
}

TEST(Config, RejectsNonIntegerPort)
{
    const std::string error = error_text(parse_ok("[server]\nport = \"eighty\"\n"));
    EXPECT_NE(error.find("must be an integer"), std::string::npos) << error;
}

TEST(Config, RejectsNonIntegerTimeout)
{
    const std::string error = error_text(parse_ok("[timeouts]\nread_ms = \"soon\"\n"));
    EXPECT_NE(error.find("timeouts.read_ms"), std::string::npos) << error;
    EXPECT_NE(error.find("must be an integer"), std::string::npos) << error;
}

TEST(Config, RejectsZeroAndNegativeTimeouts)
{
    const std::string zero = error_text(parse_ok("[timeouts]\nconnect_ms = 0\n"));
    EXPECT_NE(zero.find("timeouts.connect_ms"), std::string::npos) << zero;

    const std::string negative = error_text(parse_ok("[timeouts]\nread_ms = -1\n"));
    EXPECT_NE(negative.find("timeouts.read_ms"), std::string::npos) << negative;
}

TEST(Config, RejectsZeroLimit)
{
    const std::string error = error_text(parse_ok("[limits]\nheader_bytes = 0\n"));
    EXPECT_NE(error.find("limits.header_bytes"), std::string::npos) << error;
}

TEST(Config, RejectsTableWhereScalarExpected)
{
    const std::string error = error_text(parse_ok("[server]\nport = { a = 1 }\n"));
    EXPECT_NE(error.find("server.port"), std::string::npos) << error;
    EXPECT_NE(error.find("must be an integer"), std::string::npos) << error;
}

TEST(Config, RejectsScalarWhereTableExpected)
{
    const std::string error = error_text(parse_ok("server = 5\n"));
    EXPECT_NE(error.find("server must be a table"), std::string::npos) << error;
}

TEST(Config, RejectsScalarTlsBlock)
{
    const std::string error = error_text(parse_ok("[server]\ntls = \"yes\"\n"));
    EXPECT_NE(error.find("server.tls must be a table"), std::string::npos) << error;
}

TEST(Config, RejectsEmptyCertificatePath)
{
    const std::string error = error_text(parse_ok("[server.tls]\ncert = \"\"\n"));
    EXPECT_NE(error.find("server.tls.cert"), std::string::npos) << error;
    EXPECT_NE(error.find("must not be blank"), std::string::npos) << error;
}

TEST(Config, RejectsUpstreamMissingName)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
host = "127.0.0.1"
port = 80
)"));
    EXPECT_NE(error.find("upstream[0].name"), std::string::npos) << error;
}

TEST(Config, RejectsUpstreamMissingHost)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "nameless-host"
port = 80
)"));
    EXPECT_NE(error.find("upstream[0].host"), std::string::npos) << error;
}

TEST(Config, RejectsUpstreamMissingPort)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "portless"
host = "127.0.0.1"
)"));
    EXPECT_NE(error.find("upstream[0].port"), std::string::npos) << error;
}

TEST(Config, RejectsUpstreamPortOutOfRange)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "bad"
host = "127.0.0.1"
port = 70000
)"));
    EXPECT_NE(error.find("upstream[0].port"), std::string::npos) << error;
}

TEST(Config, RejectsNonBooleanUpstreamTls)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "bad-tls"
host = "127.0.0.1"
port = 80
tls = "yes"
)"));
    EXPECT_NE(error.find("must be a boolean"), std::string::npos) << error;
}

TEST(Config, RejectsDuplicateUpstreamNames)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "same"
host = "10.0.0.1"
port = 80

[[upstream]]
name = "same"
host = "10.0.0.2"
port = 80
)"));
    EXPECT_NE(error.find("duplicates an earlier upstream"), std::string::npos) << error;
    EXPECT_NE(error.find("upstream[1]"), std::string::npos) << error;
}

TEST(Config, RejectsUpstreamThatIsNotATable)
{
    const std::string error = error_text(parse_ok("upstream = [1, 2, 3]\n"));
    EXPECT_NE(error.find("upstream[0] must be a table"), std::string::npos) << error;
    EXPECT_NE(error.find("upstream[2] must be a table"), std::string::npos) << error;
}

TEST(Config, RejectsScalarElementInsideUpstreamArray)
{
    const std::string error = error_text(parse_ok(R"(
upstream = ["not-a-table"]
)"));
    EXPECT_NE(error.find("upstream[0] must be a table"), std::string::npos) << error;
}

TEST(Config, ReportsEveryProblemAtOnce)
{
    const std::string error = error_text(parse_ok(R"(
[server]
port = 0
threads = 0

[timeouts]
read_ms = -5

[[upstream]]
name = "dup"
host = "127.0.0.1"
port = 80

[[upstream]]
name = "dup"
host = "127.0.0.2"
port = 80
)"));

    EXPECT_NE(error.find("server.port"), std::string::npos) << error;
    EXPECT_NE(error.find("server.threads"), std::string::npos) << error;
    EXPECT_NE(error.find("timeouts.read_ms"), std::string::npos) << error;
    EXPECT_NE(error.find("duplicates an earlier upstream"), std::string::npos) << error;
}

TEST(Config, InvalidUpstreamIsNotAddedToTheList)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "good"
host = "10.0.0.1"
port = 80

[[upstream]]
name = "bad"
host = "10.0.0.2"
port = 99999
)"));

    EXPECT_NE(error.find("upstream[1].port"), std::string::npos) << error;
}

TEST(Config, ThrowsConfigErrorOnValidationFailure)
{
    EXPECT_THROW(Config(parse_ok("[server]\nport = 0\n")), ConfigError);
}

TEST(Config, MissingFileThrowsRatherThanExiting)
{
    try {
        Config config(std::string("definitely_not_a_real_config_file.toml"));
        FAIL() << "expected ConfigError for a missing file";
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find("definitely_not_a_real_config_file.toml"),
                  std::string::npos)
            << e.what();
    }
}

TEST(Config, MalformedFileThrowsWithParseDescription)
{
    const TempFile file("malformed_config_fixture.toml", "[server\nport = = 8080\n");

    try {
        Config config(file.path());
        FAIL() << "expected ConfigError for malformed TOML";
    } catch (const ConfigError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("malformed_config_fixture.toml"), std::string::npos) << message;
        EXPECT_NE(message.find("line"), std::string::npos) << message;
        EXPECT_NE(message.find("column"), std::string::npos) << message;
    } catch (const std::exception& e) {
        FAIL() << "wrong exception type: " << e.what();
    }
}

TEST(Config, ValidFileOnDiskRoundTrips)
{
    const TempFile file("valid_config_fixture.toml", R"(
[server]
port = 9443

[[upstream]]
name = "origin"
host = "127.0.0.1"
port = 8081
)");

    const Config config(file.path());

    EXPECT_EQ(config.get_port(), 9443);
    ASSERT_EQ(config.get_upstreams().size(), 1u);
    EXPECT_EQ(config.get_upstreams()[0].port, 8081);
}

TEST(Config, RejectsBooleanWhereIntegerExpected)
{
    EXPECT_NE(error_text(parse_ok("[server]\nport = true\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[limits]\nbody_bytes = true\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[timeouts]\nread_ms = true\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[[upstream]]\nname=\"a\"\nhost=\"h\"\nport = true\n"))
                  .find("must be an integer"),
              std::string::npos);
}

TEST(Config, RejectsWholeValuedFloatWhereIntegerExpected)
{
    EXPECT_NE(error_text(parse_ok("[server]\nport = 8e3\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[server]\nport = 8000.0\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[server]\nthreads = 4.0\n")).find("must be an integer"),
              std::string::npos);
}

TEST(Config, RejectsFractionalNumber)
{
    EXPECT_NE(error_text(parse_ok("[server]\nport = 80.5\n")).find("must be an integer"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[server]\nthreads = 3.9\n")).find("must be an integer"),
              std::string::npos);
}

TEST(Config, RejectsIntegerWhereBooleanExpected)
{
    EXPECT_NE(error_text(parse_ok("[[upstream]]\nname=\"a\"\nhost=\"h\"\nport=80\ntls = 1\n"))
                  .find("must be a boolean"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[[upstream]]\nname=\"a\"\nhost=\"h\"\nport=80\ntls = \"true\"\n"))
                  .find("must be a boolean"),
              std::string::npos);
}

TEST(Config, RejectsBlankCertificatePath)
{
    const std::string error = error_text(parse_ok("[server.tls]\ncert = \"   \"\n"));
    EXPECT_NE(error.find("server.tls.cert"), std::string::npos) << error;
    EXPECT_NE(error.find("must not be blank"), std::string::npos) << error;
}

TEST(Config, RejectsBlankUpstreamHost)
{
    const std::string error = error_text(parse_ok("[[upstream]]\nname=\"a\"\nhost=\"  \"\nport=80\n"));
    EXPECT_NE(error.find("upstream[0].host"), std::string::npos) << error;
    EXPECT_NE(error.find("must not be blank"), std::string::npos) << error;
}

TEST(Config, RejectsScalarLimitsTable)
{
    EXPECT_NE(error_text(parse_ok("limits = 5\n")).find("limits must be a table"), std::string::npos);
}

TEST(Config, RejectsScalarTimeoutsTable)
{
    EXPECT_NE(error_text(parse_ok("timeouts = 5\n")).find("timeouts must be a table"), std::string::npos);
}

TEST(Config, RejectsThreadsAboveCeiling)
{
    EXPECT_NE(error_text(parse_ok("[server]\nthreads = 1025\n")).find("server.threads"), std::string::npos);
}

TEST(Config, RejectsBodyBytesAboveCeiling)
{
    EXPECT_NE(error_text(parse_ok("[limits]\nbody_bytes = 2147483649\n")).find("limits.body_bytes"),
              std::string::npos);
}

TEST(Config, RejectsHeaderBytesAboveCeiling)
{
    EXPECT_NE(error_text(parse_ok("[limits]\nheader_bytes = 16777217\n")).find("limits.header_bytes"),
              std::string::npos);
}

TEST(Config, RejectsTimeoutAboveCeiling)
{
    EXPECT_NE(error_text(parse_ok("[timeouts]\nconnect_ms = 3600001\n")).find("timeouts.connect_ms"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[timeouts]\nhandshake_ms = 3600001\n")).find("timeouts.handshake_ms"),
              std::string::npos);
    EXPECT_NE(error_text(parse_ok("[timeouts]\nread_ms = 3600001\n")).find("timeouts.read_ms"),
              std::string::npos);
}

TEST(Config, AcceptsValuesExactlyAtCeiling)
{
    const Config config(parse_ok(R"(
[server]
threads = 1024

[limits]
body_bytes = 1073741824
header_bytes = 16777216

[timeouts]
connect_ms = 3600000
)"));

    EXPECT_EQ(config.get_thread_num(), 1024u);
    EXPECT_EQ(config.get_body_bytes(), 1073741824u);
    EXPECT_EQ(config.get_header_bytes(), 16777216u);
    EXPECT_EQ(config.get_connect_ms(), std::chrono::milliseconds(3600000));
}

TEST(Config, RejectedUpstreamDoesNotCascadeExtraErrors)
{
    const std::string error = error_text(parse_ok(R"(
[[upstream]]
name = "dup"
host = "10.0.0.1"
port = 80

[[upstream]]
host = "10.0.0.2"
port = 80
)"));

    EXPECT_NE(error.find("upstream[1].name is required"), std::string::npos) << error;
    EXPECT_EQ(error.find("duplicates an earlier upstream"), std::string::npos) << error;
}

TEST(Config, EmptyUpstreamArrayIsAccepted)
{
    const Config config(parse_ok("upstream = []\n"));
    EXPECT_TRUE(config.get_upstreams().empty());
}