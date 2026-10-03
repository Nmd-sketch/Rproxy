#pragma once
#include "BaseSession.hpp"

class PlainSession : public BaseSession<PlainSession> {
    net::ip::tcp::socket m_socket;
    beast::flat_buffer m_buf;

    void read_request();
    void send_response();

public:
    PlainSession(net::ip::tcp::socket&& socket, Server* s_ref, beast::flat_buffer buf = {}) : BaseSession<PlainSession>(s_ref), m_socket(std::move(socket)), m_buf(std::move(buf)) 
    {}

    void start();
    
};
