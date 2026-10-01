#pragma once
#include "Connection.hpp"
#include <boost/asio/ssl.hpp>
#include <boost/asio.hpp>
namespace net = boost::asio;
namespace sys = boost::system;

// In what differs a Tls connection from a regular connection?
// Maybe i do need to decrypt the TLS encoding before stripping
// the hop to hop headers and resending back to the origin server
class TlsConnection : public Connection {
	net::ssl::stream<net::ip::tcp::socket> m_ssl_stream;

public:
	TlsConnection(net::ip::tcp::socket&& socket, net::ssl::context& ssl_ctx) : m_ssl_stream(socket, ssl_ctx) {}
	void process_client() override;
};
