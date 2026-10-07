#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "../include/rproxy/Server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

namespace {

// @Usage: A test that runs longer than this is spinning rather than slow. Single process
// mode is how this bug reproduces reliably, so the ceiling has to live in the harness
// rather than in ctest, which only sees whole process runs. Overridable so a hung run can
// be left alive long enough to attach a debugger to it.
inline int per_test_timeout_seconds() {
    const char* override = std::getenv("RPROXY_TEST_TIMEOUT");
    if (override != nullptr) {
        const int value = std::atoi(override);
        if (value > 0) {
            return value;
        }
    }
    return 15;
}

// @Correctness: gtest keeps a test's name in static storage for the life of the program, so a
// bare pointer stays valid to read from the watchdog thread.
std::atomic<const char*> g_current_test_name{nullptr};
std::atomic<bool> g_test_running{false};
std::atomic<long long> g_test_start_ms{0};

long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// @Usage: The watchdog. A spin inside io_context::run() never returns, so the only way to
// tell a spin from a slow test is to bound it and say which case it was. _Exit is used so
// a wedged io_context or a half torn down static cannot stall the report.
struct Watchdog {
    Watchdog() {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                if (!g_test_running.load(std::memory_order_relaxed)) {
                    continue;
                }

                const auto elapsed = steady_ms() - g_test_start_ms.load(std::memory_order_relaxed);
                if (elapsed > per_test_timeout_seconds() * 1000) {
                    const char* name = g_current_test_name.load(std::memory_order_relaxed);
                    std::cerr << "\n*** TEST SPUN: " << (name ? name : "?") << " ran "
                              << (elapsed / 1000) << "s, limit " << per_test_timeout_seconds()
                              << "s ***" << std::endl;
                    std::cerr.flush();
                    std::_Exit(3);
                }
            }
        }).detach();
    }
};

const Watchdog g_watchdog;

// @Usage: Feeds the watchdog. gtest gives no per-test timeout of its own, so without this
// a spin inside io_context::run() is indistinguishable from a slow test.
class WatchdogListener : public ::testing::EmptyTestEventListener {
    void OnTestStart(const ::testing::TestInfo& info) override {
        g_current_test_name.store(info.name(), std::memory_order_relaxed);
        g_test_start_ms.store(steady_ms(), std::memory_order_relaxed);
        g_test_running.store(true, std::memory_order_relaxed);
    }

    void OnTestEnd(const ::testing::TestInfo& info) override {
        g_test_running.store(false, std::memory_order_relaxed);
    }
};

const int g_watchdog_listener_registered =
    (::testing::UnitTest::GetInstance()->listeners().Append(new WatchdogListener), 0);

// @Usage: Pinned test ports, so a case keeps the same port from run to run.
//
// Binding port 0 and releasing it leaves a window in which the number can be taken by a
// later allocation in the same process or by a leftover connection from a previous one.
// These ranges sit below the Windows dynamic range (49152-65535), so nothing the proxy
// opens outbound can claim one of them.
//
// The ranges are disjoint on purpose. Returns502WhenTheOriginRefusesTheConnection needs a
// port that is guaranteed dead while RunningProxy needs one that is guaranteed live, and
// both derive from the same test name, so a shared range would hand the same number to
// both and the proxy would end up listening on its own "dead" port.
//
// Worth keeping straight: this was adopted while hunting the intermittent failures and it
// did NOT fix them. The cause was two dangling references, one in this file and one in
// SessionBase. Pinned ports are determinism, not a bug fix.
constexpr std::uint16_t kProxyPortLo = 8000;
constexpr std::uint16_t kProxyPortHi = 8049;
constexpr std::uint16_t kOriginPortLo = 8050;
constexpr std::uint16_t kOriginPortHi = 8099;
constexpr std::uint16_t kDeadPortLo = 8100;
constexpr std::uint16_t kDeadPortHi = 8109;

// @Usage: gtest_discover_tests runs each Proxy case in its own process
// (--gtest_filter=Proxy.Xyz), so a static counter cannot hand out distinct ports the
// way it would if the suite shared an address space. Hashing the test name does, and
// keeps a given case on the same port from run to run.
std::uint32_t test_name_hash() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();

    std::uint32_t h = 2166136261u;
    const std::string name = info ? info->name() : std::string{};
    for (const unsigned char c : name) {
        h = (h ^ c) * 16777619u;
    }
    return h;
}

// Walk forward from the hashed start until a port accepts a bind. Probing covers a
// port still held by a previous process's TIME_WAIT sockets, and also absorbs the
// birthday collisions expected when 25 tests share a range.
std::uint16_t pinned_port(std::uint16_t lo, std::uint16_t hi) {
    const auto span = static_cast<std::uint32_t>(hi - lo) + 1;

    for (std::uint32_t i = 0; i < span; ++i) {
        const auto candidate =
            static_cast<std::uint16_t>(lo + (test_name_hash() + i) % span);

        net::io_context ioc;
        net::ip::tcp::acceptor probe(ioc);
        sys::error_code ec;
        probe.open(net::ip::tcp::v4(), ec);
        if (ec) {
            break;
        }
        probe.bind(net::ip::tcp::endpoint(net::ip::tcp::v4(), candidate), ec);
        if (!ec) {
            return candidate;
        }
    }

    throw std::runtime_error("no pinned port available in range");
}

// A socket already listening on a pinned port.
//
// @Correctness: The acceptor is bound once and held for the listener's whole life,
// so unlike the old port 0 approach there is no release gap for anything to slip
// into. Origin ports are pinned because the proxy dials them, and a port stolen
// between allocation and dial would send the request to whatever grabbed it.
struct TCPListener {
    net::io_context ctx;
    net::ip::tcp::acceptor acceptor;

    TCPListener() : acceptor(ctx) {
        sys::error_code ec;
        acceptor.open(net::ip::tcp::v4(), ec);
        if (ec) {
            throw std::runtime_error("TCPListener open failed");
        }

        // @Correctness: Deliberately NOT setting reuse_address. On Windows SO_REUSEADDR
        // lets a second socket bind a port that already has a live listener, so with it
        // set a collision is invisible: both sockets report a successful bind and the
        // kernel then hands the connection to whichever it prefers. Without it a
        // duplicate bind fails and the probing below is actually able to find a port
        // nobody else is listening on.
        const auto span = static_cast<std::uint32_t>(kOriginPortHi - kOriginPortLo) + 1;
        for (std::uint32_t i = 0; i < span; ++i) {
            const auto candidate = static_cast<std::uint16_t>(
                kOriginPortLo + (test_name_hash() + i) % span);
            acceptor.bind(net::ip::tcp::endpoint(net::ip::tcp::v4(), candidate), ec);
            if (!ec) {
                break;
            }
        }
        if (ec) {
            throw std::runtime_error("no pinned origin port available");
        }

        acceptor.listen();
    }

    void close() {
        sys::error_code ec;
        acceptor.close(ec);
    }

    std::uint16_t port() const {
        return acceptor.local_endpoint().port();
    }
};

// @Usage: Replaces the four hand rolled listener threads that used to be detached, so no
// origin outlives its test.
//
// @Correctness: A detached origin thread never exits. It loops in accept() until the
// listener is closed, and detaching means nothing in the test ever closes it, so the
// socket stays bound for the rest of the process.
//
// That is a genuine leak, but it was NOT the cause of the intermittent segfaults and
// hangs; those came from the dangling references fixed in SessionBase and
// LoadBalancerTests. It is still worth closing: because SO_REUSEADDR lets a second bind
// succeed against a live listener on Windows, a leaked listener makes the pinned-port
// probe blind, so two sockets could end up listening on one port and a request would be
// delivered to an origin belonging to a finished test. TCPListener therefore no longer
// sets SO_REUSEADDR, which turns that collision back into a visible bind failure that
// the probe walks past.
//
// Closing the listener unblocks accept() with operation_aborted and the loop returns on
// ec, so the join is prompt. stopping() covers bodies that sleep: they poll it so
// teardown does not have to wait out a long sleep.
class JoinedOrigin {
public:
    JoinedOrigin() : m_listener(std::make_shared<TCPListener>()) {}

    // Body is invoked once per accepted connection and owns whatever it does with the
    // socket. It must not block for longer than the test tolerates; poll stopping() if it
    // sleeps.
    template <typename Body>
    void serve_loop(Body body) {
        m_thread = std::thread([listener = m_listener, body = std::move(body), this] {
            for (;;) {
                net::ip::tcp::socket sock{listener->ctx};
                sys::error_code ec;
                listener->acceptor.accept(sock, ec);
                if (ec || stopping()) {
                    return;
                }
                body(sock);
            }
        });
    }

    bool stopping() const { return m_stopping.load(std::memory_order_relaxed); }

    ~JoinedOrigin() {
        m_stopping.store(true, std::memory_order_relaxed);
        m_listener->close();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    JoinedOrigin(const JoinedOrigin&) = delete;
    JoinedOrigin& operator=(const JoinedOrigin&) = delete;

    std::uint16_t port() const { return m_listener->port(); }

private:
    std::shared_ptr<TCPListener> m_listener;
    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
};

// @Usage: Upper bound on any blocking socket operation the origin serving thread
// performs. This thread is joined by ~StubOriginT, so an unbounded blocking read or
// handshake anywhere in serve() turns into a test that never finishes. The proxy under
// test is not expected to stall a connection it has opened, but the harness must not
// hang forever if it does.
constexpr int kOriginIoTimeoutSeconds = 5;

// winsock2.h, for SO_RCVTIMEO and SO_SNDTIMEO. ws2tcpip.h is not portable, and
// <winsock2.h> has to precede <windows.h>, which asio/ssl.hpp may already have pulled in.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

// @Usage: Server side TLS context for the TLS origin. Reuses the proxy's own cert
// pair, which is sound only because the leaf is signed by the same CA the proxy is
// given as ca_file, so verification actually succeeds rather than being skipped.
net::ssl::context make_origin_tls_context(const char* cert = nullptr,
                                          const char* key = nullptr) {
    const std::filesystem::path certs = RPROXY_TLS_CERT_DIR;

    // @Usage: .string() rather than .c_str() because Asio takes a const char* and
    // path::value_type is wchar_t under MinGW, so a raw pointer would not narrow.
    net::ssl::context ctx{net::ssl::context::tlsv12_server};
    ctx.use_certificate_chain_file(
        cert != nullptr ? std::string(cert)
                        : std::filesystem::path(certs / "cert.pem").string());
    ctx.use_private_key_file(
        key != nullptr ? std::string(key)
                       : std::filesystem::path(certs / "key.pem").string(),
        net::ssl::context::pem);
    return ctx;
}

// @Usage: The two ways an origin can wrap an accepted socket. Templating on the leg
// rather than the stream type keeps the accept / record / reply loop in one place,
// so PlainOrigin and TlsOrigin cannot drift apart.
// @Usage: Bounds a blocking socket operation. Set through the native handle because
// Asio exposes no socket_base wrapper for SO_RCVTIMEO / SO_SNDTIMEO on this platform:
// they are plain int millisecond options in winsock2.h, and socket_base only wraps the
// boolean and named-integer options.
//
// @Correctness: This exists so ~StubOriginT's join can always complete. Every blocking
// read, write, and handshake on the serving thread goes through here; without a bound,
// a peer that stalls mid-exchange parks the thread forever and the test never finishes.
void set_io_timeout(net::ip::tcp::socket& sock, int seconds) {
    const int millis = seconds * 1000;

    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&millis), sizeof(millis));
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&millis), sizeof(millis));
}

struct PlainLeg {
    using stream_type = net::ip::tcp::socket;

    // @Usage: A tcp::socket is its own lowest layer. Present on both legs so serve()
    // can bound its blocking operations without branching on the type.
    static net::ip::tcp::socket& socket(stream_type& stream) noexcept { return stream; }

    static stream_type wrap(net::ip::tcp::socket sock, net::ssl::context&) {
        return sock;
    }

    // Nothing to set up: a plain socket is ready for HTTP immediately. serve() bounds
    // the read itself, so no timeout is needed here.
    static bool prepare(stream_type&, std::chrono::seconds) noexcept { return true; }
};

struct TlsLeg {
    using stream_type = net::ssl::stream<net::ip::tcp::socket>;

    static net::ip::tcp::socket& socket(stream_type& stream) noexcept {
        return stream.next_layer();
    }

    static stream_type wrap(net::ip::tcp::socket sock, net::ssl::context& ctx) {
        return stream_type(std::move(sock), ctx);
    }

    // @Correctness: The handshake is driven explicitly here. Asynchronous ssl::stream
    // operations start it themselves, which is why SessionBase can rely on
    // async_handshake, but the synchronous read below is just a read as far as the engine
    // is concerned. Skipping this fails the first read with SSL_R_UNINITIALIZED and the
    // peer sees a connection reset.
    static bool prepare(stream_type& stream, std::chrono::seconds timeout) noexcept {
        // @Usage: The timeout has to be installed before the handshake, not after,
        // because the handshake is the blocking call that would otherwise park. Set
        // through socket() so this stays symmetric with PlainLeg.
        set_io_timeout(socket(stream), static_cast<int>(timeout.count()));

        sys::error_code ec;
        stream.handshake(net::ssl::stream_base::server, ec);
        return !ec;
    }
};

// A looping origin that replies with a fixed body and records the last request it saw.
//
// The listener is owned by a shared_ptr and closed in the destructor, which makes the
// serving thread's accept() fail and return. Detaching it instead would leave the
// thread writing to m_observed and touching the acceptor after those were destroyed.
template <typename Leg>
class StubOriginT {
public:
    using stream_type = typename Leg::stream_type;

    explicit StubOriginT(std::string body, std::string cert = {}, std::string key = {})
        : m_body(std::move(body)),
          m_ctx(make_origin_tls_context(cert.empty() ? nullptr : cert.c_str(),
                                       key.empty() ? nullptr : key.c_str())),
          m_listener(std::make_shared<TCPListener>()) {

m_port = m_listener->port();
        m_thread = std::thread([this, listener = m_listener] { serve(*listener); });
    }

~StubOriginT() {
        m_listener->close();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    StubOriginT(const StubOriginT&) = delete;
    StubOriginT& operator=(const StubOriginT&) = delete;

    std::uint16_t port() const { return m_port; }

    // @Usage: Returns a copy. The serving thread writes m_observed, and reading the
    // reference directly from the test thread would be an unsynchronized data race.
    std::string observed_request() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_observed;
    }

private:
    void serve(TCPListener& listener) {
        for (;;) {
            net::ip::tcp::socket sock{listener.ctx};
            sys::error_code ec;
            listener.acceptor.accept(sock, ec);
            if (ec) {
                return;
            }

            stream_type stream = Leg::wrap(std::move(sock), m_ctx);

            // @Usage: The handshake has to be bounded. This is a blocking call on a thread
            // the destructor joins, so a peer that connects and then says nothing would
            // park it forever and the join would never return. The proxy under test
            // always either handshakes or drops the connection, but the harness cannot
            // assume that holds for every code path or every future test.
            if (!Leg::prepare(stream, std::chrono::seconds(kOriginIoTimeoutSeconds))) {
                // A TLS origin handed plaintext by a proxy that believes this leg is TLS
                // fails here. Loop back to accept rather than throwing out of the serving
                // thread and taking the process down.
                continue;
            }

            // @Usage: Bounded for the same reason as the handshake above. Without a timeout
            // this read parks a thread the destructor joins whenever a peer connects,
            // sends a partial request, and stalls. send_timeout covers the write below
            // for the same reason.
            set_io_timeout(Leg::socket(stream), kOriginIoTimeoutSeconds);

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(stream, buffer, req, ec);
            if (ec) {
                continue;
            }

            std::string wire;
            wire += std::string(req.method_string()) + " " + std::string(req.target()) + "\r\n";
            for (auto it = req.begin(); it != req.end(); ++it) {
                wire += std::string(it->name_string()) + ": " + std::string(it->value()) + "\r\n";
            }
            wire += "\r\n";
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_observed = wire;
            }

            http::response<http::string_body> resp;
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "text/plain");
            resp.body() = m_body;
            resp.prepare_payload();
            http::write(stream, resp, ec);
        }
    }

    std::shared_ptr<TCPListener> m_listener;
    std::uint16_t m_port{0};
    std::string m_body;
    net::ssl::context m_ctx;
    mutable std::mutex m_mutex;
    std::string m_observed;
    std::thread m_thread;
};

using StubOrigin = StubOriginT<PlainLeg>;
using TlsOrigin = StubOriginT<TlsLeg>;

std::filesystem::path make_workspace(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("rproxy_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// @Usage: RPROXY_TLS_CERT_DIR comes from CMake and is absolute. Resolving the
// certificates relative to the working directory would depend on where ctest
// launched the process from, which differs between a direct run and a ctest run.
// @Usage: Absolute path to the generated CA, so a TLS upstream can actually be
// verified. CMake generates the authority and the leaf it signed into the same
// directory as cert.pem; see tests/CMakeLists.txt.
//
// @Correctness: generic_string, not string. The path operator joins with the native
// separator, which is a backslash on Windows, and TOML basic strings treat a
// backslash as the start of an escape. A mixed "certificates\ca.pem" fails to parse
// with "unknown escape sequence '\c'". Slashes are valid unescaped in a basic string.
const std::string kTestCaFile =
    std::filesystem::path(RPROXY_TLS_CERT_DIR).append("ca.pem").generic_string();

// @Usage: A self signed certificate from an authority the proxy is never told to trust.
// Lets the negative tests prove verification is enforced rather than skipped.
const std::string kRogueCert =
    std::filesystem::path(RPROXY_TLS_CERT_DIR).append("rogue.pem").generic_string();
const std::string kRogueKey =
    std::filesystem::path(RPROXY_TLS_CERT_DIR).append("rogue-key.pem").generic_string();

void copy_certs(const std::filesystem::path& dir) {
    const std::filesystem::path certs = RPROXY_TLS_CERT_DIR;
    ASSERT_TRUE(std::filesystem::exists(certs / "cert.pem"))
        << certs / "cert.pem missing";
    ASSERT_TRUE(std::filesystem::exists(certs / "key.pem"))
        << certs / "key.pem missing";

    // @Correctness: The CA has to land in the workspace too. Server loads ca_file with
    // load_verify_file, which resolves relative to the process working directory, and
    // RunningProxy points that at the workspace for the life of the proxy.
    ASSERT_TRUE(std::filesystem::exists(certs / "ca.pem"))
        << certs / "ca.pem missing";

    for (const char* file : {"cert.pem", "key.pem", "ca.pem"}) {
        std::filesystem::copy(certs / file, dir / file);
    }
}

void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::ofstream out(p, std::ios::out | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "could not write " << p;
    out << contents;
}

std::string make_config(std::uint16_t port, const std::string& upstream_block,
                        int read_ms = 30000, std::size_t body_bytes = 8388608,
                        std::size_t header_bytes = 8192,
                        std::string_view ca_file = kTestCaFile) {
    std::string tls_block =
        "[server.tls]\n"
        "cert = \"cert.pem\"\n"
        "key  = \"key.pem\"\n";

    // @Usage: The key is omitted rather than written empty, because an empty ca_file is
    // what Config treats as "no trust store configured", and a test that wants to prove
    // that rejection needs the key genuinely absent.
    if (!ca_file.empty()) {
        tls_block += "ca_file = \"" + std::string(ca_file) + "\"\n";
    }

    return "[server]\n"
           "port = " + std::to_string(port) + "\n"
           "threads = 1\n\n" +
           tls_block + "\n" +
           upstream_block +
           "[limits]\n"
           "header_bytes = " + std::to_string(header_bytes) + "\n"
           "body_bytes   = " + std::to_string(body_bytes) + "\n\n"
           "[timeouts]\n"
           "connect_ms = 5000\n"
           "handshake_ms = 5000\n"
           "read_ms    = " + std::to_string(read_ms) + "\n";
}

// @Usage: Hand out a pinned port from the given range instead of binding port 0 and
// releasing it. See the kProxyPortLo comment for why the ranges are disjoint and why
// the numbers sit below the Windows dynamic range.
std::uint16_t free_port(std::uint16_t lo, std::uint16_t hi) {
    return pinned_port(lo, hi);
}

void serve_forever(Server* server) {
    server->start();
}

// @Usage: The tls flag here is what selects the upstream leg type at accept time, so it
// is what routes a request to PlainSession versus PlainToTlsSession. verify_tls and
// sni_name default to the secure choice: verification on, and the connected host used as
// the TLS name. A test that needs otherwise passes them explicitly.
std::string upstream_block_for(std::uint16_t origin_port, bool tls = false,
                               std::string_view sni_name = {}) {
    std::string block = "[[upstream]]\nname = \"stub\"\nhost = \"127.0.0.1\"\nport = " +
                        std::to_string(origin_port) + "\ntls = " + (tls ? "true" : "false") +
                        "\n";

    if (!sni_name.empty()) {
        block += "sni_name = \"" + std::string(sni_name) + "\"\n";
    }

    return block + "\n";
}

// A running proxy, stopped and joined on destruction.
//
// The Server is owned rather than leaked, and the serving thread is joined rather than
// detached, so nothing survives into process teardown. See the destructor for why that
// mattered.
//
// Server resolves cert/key relative to the process working directory, so the
// proxy only works while the process sits inside its workspace. The previous
// working directory is restored on scope exit; without that, running several
// Proxy cases in one process leaves the last workspace in place for the next one.
class RunningProxy {
public:
    RunningProxy(std::string workspace, std::string upstream_block, int read_ms = 30000,
                 std::size_t body_bytes = 8388608, std::size_t header_bytes = 8192) {
        const auto dir = make_workspace(workspace);
        copy_certs(dir);

        const auto proxy_port = free_port(kProxyPortLo, kProxyPortHi);
        write_file(dir / "config.toml",
                   make_config(proxy_port, upstream_block, read_ms, body_bytes, header_bytes));

        m_previous_cwd = std::filesystem::current_path();
        std::filesystem::current_path(dir);

        m_server = std::make_unique<Server>("config.toml");
        m_thread = std::thread(serve_forever, m_server.get());

        m_port = proxy_port;
    }

    // @Correctness: Order matters here. stop() first so run() unwinds, then join so the
    // serving thread is definitely gone, then destroy the Server, and only then restore
    // the working directory.
    //
    // This fixture used to leak the Server and detach the serving thread, on the
    // reasoning that each gtest case is its own process. That left a thread running an
    // unbounded accept loop while the process was being torn down, which faulted at
    // random: whichever Proxy case happened to overlap teardown died, so the failures
    // moved between tests run to run and could not be reproduced on demand.
    //
    // @Correctness: The Server must outlive the join. Sessions hold a raw Server* and
    // accept() handlers capture `this`, so destroying it while a handler is still queued
    // would dangle. io_context::stop() discards those handlers as run() unwinds, which is
    // why joining before the unique_ptr resets is sufficient.
    ~RunningProxy() {
        m_server->stop();

        if (m_thread.joinable()) {
            m_thread.join();
        }

        m_server.reset();

        std::error_code ec;
        std::filesystem::current_path(m_previous_cwd, ec);
    }

    RunningProxy(const RunningProxy&) = delete;
    RunningProxy& operator=(const RunningProxy&) = delete;

    std::uint16_t port() const { return m_port; }

private:
    std::filesystem::path m_previous_cwd;
    std::uint16_t m_port{0};
    std::unique_ptr<Server> m_server;
    std::thread m_thread;
};

// Sends a request through the proxy and returns the parsed response.
http::response<http::string_body> send_through_proxy(std::uint16_t proxy_port,
                                                     const http::request<http::string_body>& req) {
    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));

    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    beast::flat_buffer buffer;
    http::write(sock, req);

    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);
    return resp;
}

http::response<http::string_body> get_through_proxy(std::uint16_t proxy_port) {
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    return send_through_proxy(proxy_port, req);
}

// @Usage: Client leg variant, so a test can drive TlsToPlainSession or TlsToTlsSession.
// @Correctness: verify_none rather than a trust store, because the proxy presents the
// throwaway test certificate. This asserts that a TLS client leg is classified as TLS,
// not that the certificate is trusted.
http::response<http::string_body> get_through_proxy_tls(std::uint16_t proxy_port) {
    net::io_context ioc;
    net::ssl::context client_ctx{net::ssl::context::tlsv12_client};
    client_ctx.set_verify_mode(net::ssl::verify_none);

    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy_port));

    net::ssl::stream<net::ip::tcp::socket> sock{ioc, client_ctx};
    sock.next_layer().connect(*endpoints.begin());
    sock.handshake(net::ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");

    beast::flat_buffer buffer;
    http::write(sock, req);

    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);
    return resp;
}

// Reads a header value from a captured raw request, matching the field name exactly.
// A substring search would report X-Keep-Alive: as Keep-Alive:, so the line has to be
// matched at the start of the field name.
std::optional<std::string> header_in(const std::string& wire, std::string_view name) {
    std::size_t pos = wire.find("\r\n");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += 2;

    while (pos < wire.size()) {
        const std::size_t end = wire.find("\r\n", pos);
        if (end == std::string::npos || end == pos) {
            return std::nullopt;
        }
        const std::string line = wire.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string field = line.substr(0, colon);
            if (beast::iequals(field, name)) {
                return line.substr(colon + 1);
            }
        }
        pos = end + 2;
    }
    return std::nullopt;
}

}  // namespace

TEST(Proxy, RelaysTheOriginBody)
{
    StubOrigin origin("ORIGIN-BODY-X");
    const RunningProxy proxy("relays", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
}

TEST(Proxy, StripsHopByHopHeaders)
{
    StubOrigin origin("BODY");
    const RunningProxy proxy("hopbyhop", upstream_block_for(origin.port()));

    // @Usage: The client must actually send the headers for this to assert anything.
    // A default GET carries neither Connection nor Keep-Alive, so the previous version
    // of this test asserted their absence from a request that never had them.
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    req.set(http::field::connection, "keep-alive, X-Internal-Token");
    req.set(http::field::keep_alive, "timeout=5");
    req.set(http::field::te, "trailers");
    req.set("X-Internal-Token", "secret");
    req.set("X-Keep-Me", "yes");

    const auto resp = send_through_proxy(proxy.port(), req);
    ASSERT_EQ(resp.result_int(), 200);

    const std::string seen = origin.observed_request();
    EXPECT_FALSE(header_in(seen, "Connection").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "Keep-Alive").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "TE").has_value()) << seen;
    EXPECT_FALSE(header_in(seen, "X-Internal-Token").has_value())
        << "named by Connection, so it is hop by hop too: " << seen;

    // End to end headers must survive, and X-Keep-Alive must not be mistaken for
    // Keep-Alive by a substring match.
    EXPECT_TRUE(header_in(seen, "X-Keep-Me").has_value()) << seen;
    EXPECT_TRUE(header_in(seen, "Host").has_value()) << seen;
}

TEST(Proxy, AddsForwardedHeadersAndKeepsHost)
{
    StubOrigin origin("BODY");
    const RunningProxy proxy("forwarded", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy(proxy.port());
    ASSERT_EQ(resp.result_int(), 200);

    const std::string seen = origin.observed_request();

    const auto xff = header_in(seen, "X-Forwarded-For");
    ASSERT_TRUE(xff.has_value()) << seen;
    EXPECT_EQ(*xff, " 127.0.0.1");

    const auto proto = header_in(seen, "X-Forwarded-Proto");
    ASSERT_TRUE(proto.has_value()) << seen;
    EXPECT_EQ(*proto, " http");

    const auto fwd_host = header_in(seen, "X-Forwarded-Host");
    ASSERT_TRUE(fwd_host.has_value()) << seen;
    EXPECT_EQ(*fwd_host, " example.com");

    const auto host = header_in(seen, "Host");
    ASSERT_TRUE(host.has_value()) << seen;
    EXPECT_EQ(*host, " example.com");
}

TEST(Proxy, RelaysAChunkedResponseWithACorrectContentLength)
{
    // The origin replies chunked. The parser dechunks without writing a
    // Content-Length back, so without prepare_payload() the client would receive
    // a body with no length header at all.
    //
    // @Correctness: JoinedOrigin owns the listener and joins its thread on scope exit, so
    // no socket outlives the test. See the fixture for why the detached version made
    // later tests steal each other's connections.
    JoinedOrigin origin;
    const auto origin_port = origin.port();

    origin.serve_loop([](net::ip::tcp::socket& sock) {
        sys::error_code ec;

        // @Usage: Drain the request before replying, even though this stub ignores it.
        // Closing a socket that still holds unread received data makes Windows send an RST
        // rather than a FIN, and the proxy's pending read then fails with WSAECONNABORTED
        // before it ever sees the reply. Which of the two wins is a timing race, so this
        // surfaced as an intermittent 502 on a test that never touches the request.
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        set_io_timeout(sock, kOriginIoTimeoutSeconds);
        http::read(sock, buffer, req, ec);

        static const std::string reply =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "d\r\nORIGIN-BODY-X\r\n"
            "0\r\n\r\n";
        net::write(sock, net::buffer(reply), ec);
    });

    const RunningProxy proxy("chunked", upstream_block_for(origin_port));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
    ASSERT_TRUE(resp.has_content_length());
    EXPECT_EQ(resp.base().at("Content-Length"), std::to_string(resp.body().size()));
}

TEST(Proxy, Returns503WhenNoUpstreamIsConfigured)
{
    const RunningProxy proxy("noupstream", "");

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 503);
}

TEST(Proxy, Returns502WhenTheOriginRefusesTheConnection)
{
    // A port with nothing behind it. Drawn from its own range so it can never collide
// with the live proxy port this same test is about to stand up.
const auto dead_port = free_port(kDeadPortLo, kDeadPortHi);
    const RunningProxy proxy("refused", upstream_block_for(dead_port));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);
}

TEST(Proxy, RelaysAHeadResponseWithoutWaitingForABody)
{
    // A HEAD response carries Content-Length but no body, so a parser that does not
    // skip the body waits forever for bytes the origin will never send.
    JoinedOrigin origin;
    const auto origin_port = origin.port();

    origin.serve_loop([](net::ip::tcp::socket& sock) {
        sys::error_code ec;

        // Drained for the same reason as the chunked stub: closing with the request still
        // unread turns the close into an RST and costs the proxy its response.
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        set_io_timeout(sock, kOriginIoTimeoutSeconds);
        http::read(sock, buffer, req, ec);

        static const std::string reply =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 42\r\n"
            "\r\n";
        net::write(sock, net::buffer(reply), ec);

        // Hold the connection open, which is what made the proxy hang before.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });

    const RunningProxy proxy("head", upstream_block_for(origin_port));

    http::request<http::string_body> req{http::verb::head, "/", 11};
    req.set(http::field::host, "example.com");

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_TRUE(resp.body().empty());
}

TEST(Proxy, RefusesATraceRequestCarryingABodyWithoutTerminating)
{
    // Beast's prepare_payload throws std::invalid_argument for TRACE with a body. If
    // that escapes the completion handler it unwinds out of io_context::run() and
    // terminates the process, so one request would kill the proxy for everyone.
    StubOrigin origin("BODY");
    const RunningProxy proxy("trace", upstream_block_for(origin.port()));

    http::request<http::string_body> req{http::verb::trace, "/x", 11};
    req.set(http::field::host, "example.com");
    req.body() = "hello";
    // @Usage: Content-Length is set by hand rather than through prepare_payload(),
    // which is the call under test on the proxy side and would throw here too.
    req.set(http::field::content_length, "5");

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 400);

    // The proxy must still be serving: a second, ordinary request has to succeed.
    const auto follow_up = get_through_proxy(proxy.port());
    EXPECT_EQ(follow_up.result_int(), 200);
    EXPECT_EQ(follow_up.body(), "BODY");
}

TEST(Proxy, TimesOutWhenTheOriginNeverReplies)
{
    JoinedOrigin origin;
    const auto origin_port = origin.port();

    origin.serve_loop([&origin](net::ip::tcp::socket& sock) {
        sys::error_code ec;

        // Accept, read the request, then say nothing at all.
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(sock, buffer, req, ec);

        // @Usage: Polled in slices rather than one long sleep. The silence has to outlast
        // the proxy's 1500ms read timeout to make the test meaningful, but the thread is
        // joined on scope exit, so a fixed 30s sleep would hold teardown for 30s.
        for (int i = 0; i < 300 && !origin.stopping(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sock.close(ec);
    });

    const RunningProxy proxy("timeout", upstream_block_for(origin_port), 1500);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 504);
}

TEST(Proxy, TimesOutWhenAClientStallsBeforeSendingARequest)
{
    const RunningProxy proxy("slowloris", "", 1500);

    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy.port()));
    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    // Connect, then send nothing. The proxy must give up rather than hold the session.
    static const std::string partial = "GET / HTTP/1.1\r\nHost: example.com\r\n";
    net::write(sock, net::buffer(partial));

    beast::flat_buffer buffer;
    http::response<http::string_body> resp;
    beast::error_code ec;
    http::read(sock, buffer, resp, ec);

    ASSERT_FALSE(ec) << "expected a 504 response, got: " << ec.message();
    EXPECT_EQ(resp.result_int(), 504);
}

TEST(Proxy, Returns400OnAMalformedRequestLine)
{
    const RunningProxy proxy("malformed", "");

    net::io_context ioc;
    net::ip::tcp::resolver res(ioc);
    auto endpoints = res.resolve("127.0.0.1", std::to_string(proxy.port()));
    net::ip::tcp::socket sock(ioc);
    sock.connect(*endpoints.begin());

    static const std::string junk = "NOT-A-VALID-REQUEST-LINE\r\n\r\n";
    net::write(sock, net::buffer(junk));

    beast::flat_buffer buffer;
    http::response<http::string_body> resp;
    http::read(sock, buffer, resp);

    EXPECT_EQ(resp.result_int(), 400);
}

TEST(Proxy, RoutesOverTls)
{
    StubOrigin origin("ORIGIN-TLS");
    const RunningProxy proxy("tls", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy_tls(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-TLS");
}

// Covers PlainToTlsSession: a plain client leg reaching a TLS origin.
//
// This is the case the 2x2 dispatch has to get right on the upstream side, and it is
// what proves tls = true actually changes the upstream leg type rather than being
// parsed and ignored.
TEST(Proxy, ForwardsToATlsUpstreamFromAPlainClient)
{
    TlsOrigin origin("ORIGIN-TLS-UPSTREAM");
    const RunningProxy proxy("uptls", upstream_block_for(origin.port(), true));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-TLS-UPSTREAM");

    // @Usage: The proto header is derived from the client leg, so it stays http here
    // even though the origin leg was TLS. Worth pinning so a future change does not
    // report the upstream leg's scheme by mistake.
    const auto proto = header_in(origin.observed_request(), "X-Forwarded-Proto");
    ASSERT_TRUE(proto.has_value()) << origin.observed_request();
    EXPECT_EQ(*proto, " http");
}

// Covers TlsToTlsSession: TLS on both legs, the only cell with two handshakes in the
// request path.
TEST(Proxy, ForwardsToATlsUpstreamFromATlsClient)
{
    TlsOrigin origin("ORIGIN-TLS-BOTH");
    const RunningProxy proxy("uptlsboth", upstream_block_for(origin.port(), true));

    const auto resp = get_through_proxy_tls(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-TLS-BOTH");

    // Opposite of the plain client case: now the client leg was TLS.
    const auto proto = header_in(origin.observed_request(), "X-Forwarded-Proto");
    ASSERT_TRUE(proto.has_value()) << origin.observed_request();
    EXPECT_EQ(*proto, " https");
}

// The mirror of the above: a TLS client to a plain origin, which is what
// TlsToPlainSession exists for.
TEST(Proxy, ForwardsToAPlainUpstreamFromATlsClient)
{
    StubOrigin origin("ORIGIN-PLAIN-UPSTREAM");
    const RunningProxy proxy("tlsplain", upstream_block_for(origin.port()));

    const auto resp = get_through_proxy_tls(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-PLAIN-UPSTREAM");

    const auto proto = header_in(origin.observed_request(), "X-Forwarded-Proto");
    ASSERT_TRUE(proto.has_value()) << origin.observed_request();
    EXPECT_EQ(*proto, " https");
}

// @Usage: A tls = true upstream that turns out not to speak TLS has to surface as 502,
// not as a hung connection or a process kill. This is the negative case for the
// upstream handshake: the proxy completes a TCP connect, sends a ClientHello, and the
// origin replies with plaintext HTTP instead.
TEST(Proxy, Returns502WhenATlsUpstreamSpeaksPlainHttp)
{
    StubOrigin origin("SHOULD-NEVER-BE-RELAYED");
    const RunningProxy proxy("tlsplainfail", upstream_block_for(origin.port(), true));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);

    // @Correctness: The origin must not have been reached with a readable request. If the
    // proxy forwarded the request before handshaking, this body would come back as a 200.
    EXPECT_TRUE(origin.observed_request().empty()) << origin.observed_request();
}

// The response side needs its own cap. limits.body_bytes alone protected the request
// leg, but a hostile or broken origin can stream an unbounded body into m_up_buf just
// as easily, and nothing stopped that.
TEST(Proxy, Returns502WhenTheOriginResponseExceedsTheBodyLimit)
{
    StubOrigin origin("THIS-RESPONSE-BODY-IS-WAY-TOO-LONG");
    const RunningProxy proxy("respbig", upstream_block_for(origin.port()),
                             30000, /*body_bytes=*/16);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);

    // @Correctness: Nothing may reach the client. A partial body already streamed
    // before the limit tripped would leave the client with a truncated response wearing
    // a 502's status, which is worse than an honest failure.
    EXPECT_TRUE(resp.body().find("THIS-RESPONSE-BODY") == std::string::npos) << resp.body();
}

// Counterweight to the above. Without a test on the passing side, a limit wired one byte
// too tight would look identical to the limit working.
TEST(Proxy, RelaysAResponseThatFitsUnderTheBodyLimit)
{
    StubOrigin origin("ORIGIN-BODY-X");
    const RunningProxy proxy("respok", upstream_block_for(origin.port()),
                             30000, /*body_bytes=*/64);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-BODY-X");
}

// Covers the request side of the same limit. The wiring existed before any test did,
// so nothing pinned it: a body_limit that was never applied, or applied to the wrong
// leg, would have passed the whole suite.
TEST(Proxy, Returns413WhenTheRequestBodyExceedsTheBodyLimit)
{
    StubOrigin origin("SHOULD-NEVER-BE-RELAYED");
    const RunningProxy proxy("reqbig", upstream_block_for(origin.port()),
                             30000, /*body_bytes=*/16);

    http::request<http::string_body> req{http::verb::post, "/upload", 11};
    req.set(http::field::host, "example.com");
    req.body() = "THIS-REQUEST-BODY-IS-WAY-TOO-LONG";
    req.prepare_payload();

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 413);

    // @Correctness: The limit has to stop the request before it is forwarded. Reaching
    // the origin first would mean the body was already uploaded over the wire, which is
    // exactly the cost the limit exists to avoid.
    EXPECT_TRUE(origin.observed_request().empty()) << origin.observed_request();
}

// Covers the request side header cap, which like body_bytes had no test pinning it.
TEST(Proxy, Returns431WhenTheRequestHeadersExceedTheHeaderLimit)
{
    StubOrigin origin("SHOULD-NEVER-BE-RELAYED");
    const RunningProxy proxy("reqhdr", upstream_block_for(origin.port()),
                             30000, /*body_bytes=*/8388608, /*header_bytes=*/256);

    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "example.com");
    // One field large enough to push the header block past the configured 256 bytes.
    req.set("X-Filler", std::string(400, 'a'));

    const auto resp = send_through_proxy(proxy.port(), req);

    EXPECT_EQ(resp.result_int(), 431);
    EXPECT_TRUE(origin.observed_request().empty()) << origin.observed_request();
}

// The response header block needs a cap too. An origin can send an unbounded header
// section for exactly the same reason it can send an unbounded body, and the request
// side limit does not cover it.
TEST(Proxy, Returns502WhenTheOriginResponseHeadersExceedTheHeaderLimit)
{
    // @Usage: The origin replies with a header far wider than the proxy's own header
    // limit allows, which is the realistic version of this: a proxy that accepts small
    // headers from a client must not accept unlimited ones from an origin.
    JoinedOrigin origin;
    const auto origin_port = origin.port();

    origin.serve_loop([](net::ip::tcp::socket& sock) {
        sys::error_code ec;

        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(sock, buffer, req, ec);
        if (ec) {
            return;
        }

        std::string reply =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "X-Wide: " + std::string(4000, 'b') + "\r\n\r\nBODY";
        net::write(sock, net::buffer(reply), ec);
    });

    const RunningProxy proxy("resphdr", upstream_block_for(origin_port),
                             30000, /*body_bytes=*/8388608, /*header_bytes=*/256);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);
    EXPECT_TRUE(resp.body().find("BODY") == std::string::npos) << resp.body();
}

// Without this, every other TLS upstream test would pass just as well whether
// verification is enforced or silently skipped: a CA signed leaf is trusted either way.
// The origin here presents a self signed certificate from an authority the proxy is
// never given, so a relayed response means the chain check is not running.
TEST(Proxy, Returns502WhenTheUpstreamCertificateIsNotSignedByTheConfiguredCa)
{
TlsOrigin origin("SHOULD-NEVER-BE-RELAYED", kRogueCert, kRogueKey);
    const RunningProxy proxy("untrusted", upstream_block_for(origin.port(), true));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);
    EXPECT_TRUE(origin.observed_request().empty()) << origin.observed_request();
}

// The chain is fine here: it is signed by the configured CA. What fails is the name, so
// this separates hostname verification from chain verification rather than repeating the
// case above. The name asserted is absent from the leaf SAN, which carries only
// localhost and 127.0.0.1.
TEST(Proxy, Returns502WhenTheUpstreamCertificateDoesNotMatchTheSniName)
{
    TlsOrigin origin("SHOULD-NEVER-BE-RELAYED");
    const RunningProxy proxy("namemismatch",
                             upstream_block_for(origin.port(), true, "not-in-the-san.example"));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 502);
    EXPECT_TRUE(origin.observed_request().empty()) << origin.observed_request();
}

// The documented escape hatch. Same untrusted origin as the chain test above, but with
// verification turned off, so it has to be relayed. Without this the opt out would be
// untested and therefore untrustworthy.
TEST(Proxy, RelaysAnUntrustedUpstreamWhenVerificationIsDisabled)
{
TlsOrigin origin("ORIGIN-UNVERIFIED", kRogueCert, kRogueKey);

    std::string block = "[[upstream]]\nname = \"stub\"\nhost = \"127.0.0.1\"\nport = " +
                        std::to_string(origin.port()) + "\ntls = true\nverify_tls = false\n\n";

    const RunningProxy proxy("noverify", block);

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-UNVERIFIED");
}

// sni_name is the documented way to reach a TLS upstream by IP while still presenting and
// checking a name. The host is 127.0.0.1, which is an IP literal and so sends no SNI on
// its own, yet the certificate is issued for localhost. This only passes if sni_name is
// what drives both the SNI extension and the hostname check.
TEST(Proxy, UsesSniNameForVerificationWhenTheHostIsAnIpLiteral)
{
    TlsOrigin origin("ORIGIN-VIA-SNI");
    const RunningProxy proxy("sni", upstream_block_for(origin.port(), true, "localhost"));

    const auto resp = get_through_proxy(proxy.port());

    EXPECT_EQ(resp.result_int(), 200);
    EXPECT_EQ(resp.body(), "ORIGIN-VIA-SNI");
}
