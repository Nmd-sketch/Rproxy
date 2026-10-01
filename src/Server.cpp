#include "../include/rproxy/Server.hpp"
#include "../include/rproxy/ClientSession.hpp"
#include <iostream>

void Server::accept() {
	m_acceptor.async_accept([this](const sys::error_code& ec, net::ip::tcp::socket client) {

			if (!ec) {
				std::make_shared<ClientSession>(std::move(client), this->m_ssl_ctx, this)->start();
			} else {
				std::cerr << "Error accepting client: " << ec.what() << '\n';
			}

			this->accept();
		}
	);
}


void Server::start() {
	// @Refactor: Temporally running in port 8080, later change the port on which the
	// server is running
	std::cout << "Server running in port 8080" << '\n';
	this->accept();
	auto guard = net::make_work_guard(this->m_io_ctx);
	this->m_io_ctx.run();
}

void Server::debug_fn() {
	std::cout << "ClientConnection communicated successfully with the Server!\n";
}
