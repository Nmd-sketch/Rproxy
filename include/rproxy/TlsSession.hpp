#pragma once
#include "SessionBase.hpp"

class TlsSession : public SessionBase<TlsSession, net::ssl::stream<net::ip::tcp::socket>> {
    using Base = SessionBase<TlsSession, net::ssl::stream<net::ip::tcp::socket>>;

public:
    using Base::Base;
};