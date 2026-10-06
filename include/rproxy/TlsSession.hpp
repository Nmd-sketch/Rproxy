#pragma once
#include "SessionBase.hpp"

// @Usage: Client leg is TLS, upstream leg is plain. Both classes must name
// themselves as the CRTP Derived parameter, not a shared third name: Derived is
// what enable_shared_from_this and self() cast to, so aliasing Base to a different
// type would make self() return the wrong pointer.

class TlsToPlainSession
    : public SessionBase<TlsToPlainSession, net::ssl::stream<net::ip::tcp::socket>> {
    using Base = SessionBase<TlsToPlainSession, net::ssl::stream<net::ip::tcp::socket>>;

public:
    using Base::Base;
};

class TlsToTlsSession
    : public SessionBase<TlsToTlsSession, net::ssl::stream<net::ip::tcp::socket>,
                         net::ssl::stream<net::ip::tcp::socket>> {
    using Base = SessionBase<TlsToTlsSession, net::ssl::stream<net::ip::tcp::socket>,
                             net::ssl::stream<net::ip::tcp::socket>>;

public:
    using Base::Base;
};