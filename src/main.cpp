#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include <thread>
#include <iostream>
#include <array>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

#define PORT "8080"
#define ORIGIN_PORT "8081"

// Now time to create the basic module in charge of redirecting
// first we need to make a session.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
	beast::flat_buffer m_buf;
	std::array<char, 64> m_aux_buf;
	http::request<http::string_body> m_req;
	http::response<http::string_body> m_resp;
	net::ip::tcp::socket m_socket;
	net::ssl::context& m_ssl_ctx;

	void read_request() {

	}

	void send_response() {

	}

	// Starts the TLS handshake sequence if the first byte received was 0x16
	void start_tls_handshake() {

	}

public:
	ClientSession(net::ip::tcp::socket socket, net::ssl::context& ssl_ctx) : m_socket(std::move(socket)), m_ssl_ctx(ssl_ctx){}

	void start() {
		auto self = shared_from_this();
		m_socket.async_receive(net::buffer(m_aux_buf), net::ip::tcp::socket::message_peek,
		 [self](const sys::error_code& ec, std::size_t bytes) {
				if (ec) {
					std::cerr << "Error receiving request: " << ec.message() << '\n';
					return;
				}

				// 0x16 indicates TLS handshake
				if (bytes > 0 && self->m_aux_buf[0] == 0x16) {
					self->start_tls_handshake();
				}

				// Normal HTTP request
				else {
					self->read_request();
				}
			}
		);
	}

};

// Assumes that the acceptor outlives the function calls, rearms only when a client connects
void my_accept(net::ip::tcp::acceptor& acceptor, net::ssl::context& ssl_ctx) {
	acceptor.async_accept([&acceptor, &ssl_ctx](const sys::error_code& e, net::ip::tcp::socket client) {
			if (e) {
				std::cerr << "Error accepting client: " << e.message() << '\n';
				my_accept(acceptor, ssl_ctx);
			}

			std::make_shared<ClientSession>(std::move(client), ssl_ctx)->start();
			my_accept(acceptor, ssl_ctx);
		}
	);
}


int main()
{
	// Create the I/O context and the SSL protocol context
	// It's necessary to load the keys at startup
	net::io_context io_ctx;
	net::ssl::context ssl_ctx(net::ssl::context::tlsv12_server);
	ssl_ctx.use_certificate_chain_file("certificates/server.crt");
	ssl_ctx.use_private_key_file("certificates/server.key", net::ssl::context::pem);

	net::ip::tcp::acceptor acceptor{io_ctx, net::ip::tcp::endpoint(net::ip::tcp::v4(), std::atoi(PORT))};
	std::cout << "Server running on port 8080\n";

	my_accept(acceptor, ssl_ctx);

}
