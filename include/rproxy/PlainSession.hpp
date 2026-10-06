#pragma once
#include "SessionBase.hpp"

// @Usage: Client leg is plain. PlainSession talks to a plain upstream,
// PlainToTlsSession to a TLS one, so the two differ only in the Upstream
// parameter and share the whole flow through SessionBase.

class PlainSession : public SessionBase<PlainSession, net::ip::tcp::socket> {
    using Base = SessionBase<PlainSession, net::ip::tcp::socket>;

public:
    using Base::Base;
};

class PlainToTlsSession
    : public SessionBase<PlainToTlsSession, net::ip::tcp::socket,
                         net::ssl::stream<net::ip::tcp::socket>> {
    using Base = SessionBase<PlainToTlsSession, net::ip::tcp::socket,
                             net::ssl::stream<net::ip::tcp::socket>>;

public:
    using Base::Base;
};