#pragma once
#include "SessionBase.hpp"

class PlainSession : public SessionBase<PlainSession, net::ip::tcp::socket> {
    using Base = SessionBase<PlainSession, net::ip::tcp::socket>;

public:
    using Base::Base;
};