#include "../include/rproxy/PlainSession.hpp"
#include <iostream>

void PlainSession::start() {
    self()->read_request();
}

void PlainSession::read_request() {
    auto shared_self = shared_from_this();

    http::async_read(self()->m_socket, m_buf, self()->m_req,
            [this, shared_self] (const beast::error_code ec, std::size_t bytes_read) {
            
                if (ec) {
                    std::cerr << "Error parsing the http request, dropping the connection...\n";
                    return;
                }
                std::cout << "The read handler has been called!\n";

                shared_self->send_response();
            }
    );
}

void PlainSession::send_response() {
    auto shared_self = shared_from_this();


	http::response<http::string_body> resp;
	resp.body() = "Request received successfully!";
	resp.set(http::field::content_type, "text/plain");
	resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
	resp.prepare_payload();

    http::async_write(self()->m_socket, resp, 
            [shared_self] (const beast::error_code ec, std::size_t bytes) {
                
                if (ec) {
                    std::cerr << "Error sending the http request to the client\n";
                    return;
                }
            }
    );

}


