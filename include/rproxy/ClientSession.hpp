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

	// We construct the stream as a thin wrapper around a reference to the socket,
	// if the connection isn't actually TLS we simply don't use it
	net::ssl::stream<net::ip::tcp::socket&> m_ssl_stream;
	net::ssl::context& m_ssl_ctx;

	void read_request();

	void send_response();

	// Starts the TLS handshake sequence if the first byte received was 0x16
	void start_tls_handshake();

	// Uses the ssl stream to read the request instead of treating it
	// like a normal request
	void read_tls_request();

	// Uses the ssl stream to send the response instead of treating it
	// like a normal request
	void send_tls_response();

public:
	ClientSession(net::ip::tcp::socket socket, net::ssl::context& ssl_ctx) : m_socket(std::move(socket)), m_ssl_ctx(ssl_ctx),
	m_ssl_stream(m_socket, ssl_ctx){}

	void start();

};
