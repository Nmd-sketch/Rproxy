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

	void accept();

public:
	Server(std::string_view config_path) : m_io_ctx(), m_config(config_path)
    , m_load_balancer(m_config, m_io_ctx)
    , m_acceptor(m_io_ctx, net::ip::tcp::endpoint(net::ip::tcp::v4(), m_config.get_port()))
    , m_ssl_ctx(net::ssl::context::tlsv12_server) {

		m_ssl_ctx.use_certificate_chain_file(m_config.get_certificate_path());
		m_ssl_ctx.use_private_key_file(m_config.get_key_path(), net::ssl::context::pem);
		m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
	}

	void start();
	void debug_fn();

	LoadBalancer& load_balancer() noexcept { return m_load_balancer; }
	const Config& config() const noexcept { return m_config; }
	net::io_context& io_context() noexcept { return m_io_ctx; }
};
