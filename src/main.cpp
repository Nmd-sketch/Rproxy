#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include "../include/rproxy/ClientSession.hpp"
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

#define PORT "8080"
#define ORIGIN_PORT "8081"


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
	ssl_ctx.use_certificate_chain_file("certificates/cert.pem");
	ssl_ctx.use_private_key_file("certificates/key.pem", net::ssl::context::pem);

	net::ip::tcp::acceptor acceptor{io_ctx, net::ip::tcp::endpoint(net::ip::tcp::v4(), std::atoi(PORT))};
	std::cout << "Server running on port 8080\n";

	my_accept(acceptor, ssl_ctx);

	auto guard = net::make_work_guard(io_ctx);
	io_ctx.run();

}
