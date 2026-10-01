#include "../include/rproxy/ClientSession.hpp"
#include <iostream>

void ClientSession::start() {
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

void ClientSession::start_tls_handshake() {
	auto self = shared_from_this();
	sys::error_code ec;

	// Start the async handshake
	m_ssl_stream.async_handshake(net::ssl::stream_base::server, self->m_buf.data(),
		[self] (const sys::error_code& ec, std::size_t bytes) {

			if (ec) {
				std::cerr << "Error during async handshake\n";
				return;
			}

			// Maybe a TLS connection class is innecessary for this, i have
			// an ssl stream arleady, just start reading from there and
			// get the HTTP request
			self->read_tls_request();
		}
	);
}

void ClientSession::read_tls_request() {
	auto self = shared_from_this();

	http::async_read(self->m_ssl_stream, self->m_buf, m_req,
		[self] (beast::error_code ec, std::size_t bytes_read) {

			if (ec) {
				std::cerr << "Error parsing the http request, dropping the connection...\n";
				return;
			}

			// Gonna leave it here, return the request once it gets received. Temporal test
			// stub
			self->send_tls_response();
		}
	);
}

void ClientSession::send_tls_response() {
	auto self = shared_from_this();

	http::response<http::string_body> resp;
	resp.body() = "Ssl request received successfully!";
	resp.set(http::field::content_type, "text/plain");
	resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
	resp.prepare_payload();

	http::async_write(self->m_ssl_stream, resp,
		[self] (beast::error_code ec, std::size_t bytes) {

			if (ec) {
				std::cerr << "Error sending the http request to the client!\n";
				return;
			}
		}
	);
}

void ClientSession::read_request() {
	auto self = shared_from_this();

	http::async_read(self->m_socket, self->m_buf, self->m_req,
		[self] (beast::error_code ec, std::size_t bytes_read) {

			if (ec) {
				std::cerr << "Error parsing the http request, dropping the connection...\n";
				return;
			}

			// Gonna leave it here, return the request once it gets received. Temporal test
			// stub
			self->send_response();
		}
	);
}

void ClientSession::send_response() {
	auto self = shared_from_this();

	http::response<http::string_body> resp;
	resp.body() = "Request received successfully!";
	resp.set(http::field::content_type, "text/plain");
	resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
	resp.prepare_payload();

	http::async_write(self->m_socket, resp,
		[self] (beast::error_code ec, std::size_t bytes) {

			if (ec) {
				std::cerr << "Error sending the http request to the client!\n";
				return;
			}
		}
	);
}
