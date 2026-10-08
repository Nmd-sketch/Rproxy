#include "../include/rproxy/Server.hpp"
#include "../include/rproxy/TlsSession.hpp"
#include "../include/rproxy/PlainSession.hpp"
#include <memory>
#include <iostream>

void Server::accept() {
	// @Correctness: Refuse to issue a new async_accept once shutdown has begun. Issuing
	// one against a closed acceptor fails immediately with bad_descriptor rather than
	// cancelling, so it would re-arm itself forever and spin a core during teardown.
	if (m_stopping.load(std::memory_order_relaxed)) {
		return;
	}


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

                // @Usage: A connection that is accepted but never classifies gets killed here.
                //
                // @Correctness: This is the only deadline covering the window between
                // accept and async_detect_ssl completing, because detect_ssl cannot
                // classify a leg until the client has sent enough bytes and nothing arms
                // a timer until a session exists. handshake_ms is the matching budget:
                // it guards the same handshake this window delays. The 3 second literal
                // this replaced ignored the configuration entirely.
                auto conn_timer = std::make_shared<net::steady_timer>(m_io_ctx.get_executor());
                conn_timer->expires_after(m_config.get_handshake_ms());
                conn_timer->async_wait(
                    [this, socket] (const sys::error_code ec) {

                        // @Correctness: Cancelling the timer is the SUCCESS path here, not a
                        // failure. Every connection that speaks has its timer cancelled by
                        // the detect_ssl handler, and a cancelled wait still runs its
                        // handler, so logging here printed one operation_aborted line per
                        // successful request. Only genuine timer errors are worth reporting.
                        if (ec == net::error::operation_aborted) {
                            return;
                        }

                        if (ec) {
                            std::cerr << "Error: " << ec.what() << '\n';
                            return;
                        }
                        
                        // @Correctness: We send error 504 to the client socket before killing the socket, close when
                        // the handle runs.
                        auto resp = std::make_shared<http::response<http::string_body>>(ErrorResponse::make_error_504(10));
                        http::async_write(*socket, *resp, 
                                [this, socket, resp] (const beast::error_code ec, std::size_t bytes) {
                                   
                                    if (ec) {
                                        std::cerr << "Error sending err response to client: " << ec.what() << '\n';
                                    }
                                    

                                    sys::error_code close_ec;
                                    socket->close(close_ec);

                                }
                        );

                    } 
                );

                beast::async_detect_ssl(*socket, *buf,
                        [this, socket, buf, client_ip, conn_timer] (const beast::error_code ec, bool result) mutable {
                            
                            // @Note: The server sent data, we can disarm the timer and proceed
                            conn_timer->cancel();

                            if (ec) {
                                std::cerr << "SSL detection error\n";
                                return;
                            }

                            const HostData* upstream = this->load_balancer().next();

                            // @Correctness: next() returns nullptr when no upstream is
                            // configured. The session has to exist anyway so it can answer
                            // 503, so the TLS flag is read defensively rather than
                            // dereferencing a null upstream and crashing the accept loop.
                            const bool up_tls = upstream != nullptr && upstream->uses_tls;

                            // @Usage: The 2x2 matrix of (client leg, upstream leg). Plain is
                            // the fallback when there is no upstream, because that leg is
                            // never connected to and the session replies 503 instead.
                            if (result && up_tls) {
                                std::make_shared<TlsToTlsSession>(
                                        net::ssl::stream<net::ip::tcp::socket>(std::move(*socket),
                                                this->m_ssl_ctx), this, std::move(*buf),
                                                std::move(client_ip), upstream)->start();
                            } else if (result) {
                                std::make_shared<TlsToPlainSession>(
                                        net::ssl::stream<net::ip::tcp::socket>(std::move(*socket),
                                                this->m_ssl_ctx), this, std::move(*buf),
                                                std::move(client_ip), upstream)->start();
                            } else if (up_tls) {
                                std::make_shared<PlainToTlsSession>(std::move(*socket), this,
                                                std::move(*buf), std::move(client_ip),
                                                upstream)->start();
                            } else {
                                std::make_shared<PlainSession>(std::move(*socket), this,
                                                std::move(*buf), std::move(client_ip),
                                                upstream)->start();
                            }

                        }
                );

			} else if (ec != net::error::operation_aborted) {
				// @Usage: operation_aborted is the expected outcome of stop(), which closes
				// the acceptor to unwind the pending accept. Logging it turned every normal
				// shutdown into a stream of errors that look like faults worth chasing.
				std::cerr << "Error accepting client: " << ec.what() << '\n';
			}

			// @Correctness: Re-arm only while the server is still running. The flag is
			// checked rather than the error code, because a closed acceptor reports
			// operation_aborted to the pending operation but bad_descriptor to a new one,
			// and only the flag rules out both.
			if (!m_stopping.load(std::memory_order_relaxed)) {
				this->accept();
			} 
			
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
