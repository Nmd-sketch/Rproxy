#pragma once
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include "Config.hpp"
#include "LoadBalancer.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

class Server {
    // @Usage: Declaration order here is load bearing, not cosmetic. Members destroy in
    // reverse, so putting the ssl::context members first makes them outlive m_io_ctx.
    //
    // @Correctness: io_context::stop() leaves pending handlers queued rather than
    // discarding them, and those handlers own sessions whose ssl::stream points into
    // these contexts. If the contexts died first, the streams would be freed after the
    // contexts they reference. m_io_ctx is declared after them so it, and the handlers
    // it holds, is torn down first. m_acceptor and m_load_balancer are declared after
    // m_io_ctx for the opposite reason: they hold executors into it, so they must die
    // first too. Neither pair of ssl::context constructors needs an io_context, which is
    // what makes this ordering legal.
    net::ssl::context m_ssl_ctx;
    net::ssl::context m_up_ssl_ctx;

	net::io_context m_io_ctx;
    Config m_config;
    LoadBalancer m_load_balancer;
	net::ip::tcp::acceptor m_acceptor;

    // @Usage: Set before the acceptor is closed, so the accept loop can tell "the server
    // is shutting down" from "this particular connection failed".
    //
    // @Correctness: Testing the error code alone is not enough. A closed acceptor reports
    // operation_aborted to the operation already in flight, but a fresh async_accept
    // issued against a closed acceptor fails immediately with bad_descriptor. Without
    // this flag that second case re-arms itself forever: every attempt fails instantly
    // and queues the next, spinning a core until the process dies.
    std::atomic<bool> m_stopping{false};

	void accept();

public:
	// @Usage: Makes start() return. Closes the acceptor so no further connections are
	// accepted, then stops the io_context so run() unwinds and any handlers still queued
	// are destroyed without being invoked.
	//
	// @Preconditions: May be called from another thread while start() is blocked in
	// run(), which is the normal case for a server being shut down.
	//
	// @Correctness: Joining the thread that called start() must happen before this
	// Server is destroyed. Sessions hold a raw Server* and every handler below captures
	// `this`, so destroying the Server while a handler is still queued would dangle.
	// io_context::stop() discards those handlers during run()'s unwind, which is why
	// stopping before destroying is sufficient and no drain is needed.
	void stop() noexcept {
		// @Correctness: Set before close(), not after. The accept handler reads this to
		// decide whether to re-arm, and close() immediately completes the in flight
		// async_accept on another thread, so the flag has to already be visible.
		m_stopping.store(true, std::memory_order_relaxed);

		sys::error_code ec;
		m_acceptor.close(ec);
		m_io_ctx.stop();
	}
	Server(std::string_view config_path) : m_ssl_ctx(net::ssl::context::tlsv12_server)
    , m_up_ssl_ctx(net::ssl::context::tlsv12_client)
    , m_io_ctx(), m_config(config_path)
    , m_load_balancer(m_config, m_io_ctx)
    , m_acceptor(m_io_ctx, net::ip::tcp::endpoint(net::ip::tcp::v4(), m_config.get_port()))
    {

		m_ssl_ctx.use_certificate_chain_file(m_config.get_certificate_path());
		m_ssl_ctx.use_private_key_file(m_config.get_key_path(), net::ssl::context::pem);
		m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));

        // @Usage: The CA bundle becomes this context's trust store, so it is loaded once
        // here rather than per handshake. Config guarantees ca_file is non empty whenever
        // some upstream both uses TLS and asks to be verified, but this has to cope with
        // the case where no upstream does: loading a bundle then would be a startup
        // failure caused by a file no traffic needs.
        //
        // @Correctness: verify_peer is set unconditionally and the session downgrades it
        // to verify_none for an upstream with verify_tls = false. Setting the permissive
        // mode here instead would let a misread flag silently disable checking on a leg
        // that should have had it.
        if (!m_config.get_ca_file().empty()) {
            m_up_ssl_ctx.load_verify_file(m_config.get_ca_file());
        }
		m_up_ssl_ctx.set_verify_mode(net::ssl::verify_peer);
	}

	void start();
	void debug_fn();

	LoadBalancer& load_balancer() noexcept { return m_load_balancer; }
	const Config& config() const noexcept { return m_config; }
	net::io_context& io_context() noexcept { return m_io_ctx; }

	// @Usage: Read by SessionBase::make_upstream to build an ssl::stream upstream leg.
	// Exposed as a reference because the context is stateful and expensive to copy.
	net::ssl::context& upstream_ssl_ctx() noexcept { return m_up_ssl_ctx; }
};
