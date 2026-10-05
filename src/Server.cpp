#include "../include/rproxy/Server.hpp"
#include "../include/rproxy/TlsSession.hpp"
#include "../include/rproxy/PlainSession.hpp"
#include <memory>
#include <iostream>

void Server::accept() {
	m_acceptor.async_accept([this](const sys::error_code& ec, net::ip::tcp::socket client) {
            
            auto buf = std::make_shared<beast::flat_buffer>();
            auto socket = std::make_shared<net::ip::tcp::socket>(std::move(client));

			if (!ec) {
                // @Usage: Captured here because this is the last point where the raw
                // socket exists. Once wrapped in an ssl_stream the IP would need
                // next_layer() on one type and the socket itself on the other.
                std::string client_ip;
                {
                    sys::error_code ep_ec;
                    const auto ep = socket->remote_endpoint(ep_ec);
                    if (!ep_ec) {
                        client_ip = ep.address().to_string();
                    }
                }

                beast::async_detect_ssl(*socket, *buf,
                        [this, socket, buf, client_ip] (const beast::error_code ec, bool result) mutable {

                            if (ec) {
                                std::cerr << "SSL detection error\n";
                                return;
                            }

                            const HostData* upstream = this->load_balancer().next();

                            if (result) {
                                std::make_shared<TlsSession>(net::ssl::stream<net::ip::tcp::socket>(std::move(*socket),
                                            this->m_ssl_ctx), this, std::move(*buf),
                                            std::move(client_ip), upstream)->start();
                            } else {
                                std::make_shared<PlainSession>(std::move(*socket), this,
                                            std::move(*buf), std::move(client_ip),
                                            upstream)->start();
                            }

                        }
                );

			} else {
				std::cerr << "Error accepting client: " << ec.what() << '\n';
			}

			this->accept();
		}
	);
}


void Server::start() {
	std::cout << "Server running in port 8080" << '\n';
	this->accept();
	auto guard = net::make_work_guard(this->m_io_ctx);
	this->m_io_ctx.run();
}

void Server::debug_fn() {
	std::cout << "ClientConnection communicated successfully with the Server!\n";
}
