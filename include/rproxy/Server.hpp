#pragma once
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
	net::io_context m_io_ctx;
    Config m_config;
    LoadBalancer m_load_balancer;
	net::ip::tcp::acceptor m_acceptor;
    net::ssl::context m_ssl_ctx;

    // @Usage: Client side context for the upstream leg. Distinct from m_ssl_ctx because
    // the two legs differ in kind: the downstream one is a server context holding our
    // certificate, this one is a client context used to verify the origin's.
    net::ssl::context m_up_ssl_ctx;

	void accept();

public:
	Server(std::string_view config_path) : m_io_ctx(), m_config(config_path)
    , m_load_balancer(m_config, m_io_ctx)
    , m_acceptor(m_io_ctx, net::ip::tcp::endpoint(net::ip::tcp::v4(), m_config.get_port()))
    , m_ssl_ctx(net::ssl::context::tlsv12_server)
    , m_up_ssl_ctx(net::ssl::context::tlsv12_client) {

		m_ssl_ctx.use_certificate_chain_file(m_config.get_certificate_path());
		m_ssl_ctx.use_private_key_file(m_config.get_key_path(), net::ssl::context::pem);
		m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));

        // @Refactor: Verification is disabled because Config carries no CA path yet, and
        // verify_peer with no trust store loaded fails every handshake. That makes a TLS
        // upstream unverified until a ca_file / verify option is added, at which point
        // this becomes verify_peer with the opt-out a separate flag.
		m_up_ssl_ctx.set_verify_mode(net::ssl::verify_none);
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
