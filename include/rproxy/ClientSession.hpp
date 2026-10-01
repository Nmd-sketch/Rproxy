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
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
	beast::flat_buffer m_buf;
	std::array<char, 64> m_aux_buf;
	http::request<http::string_body> m_req;
	http::response<http::string_body> m_resp;
	net::ip::tcp::socket m_socket;
	net::ssl::context& m_ssl_ctx;

	void read_request();

	void send_response();

	// Starts the TLS handshake sequence if the first byte received was 0x16
	void start_tls_handshake();

public:
	ClientSession(net::ip::tcp::socket socket, net::ssl::context& ssl_ctx) : m_socket(std::move(socket)), m_ssl_ctx(ssl_ctx){}

	void start();

};
