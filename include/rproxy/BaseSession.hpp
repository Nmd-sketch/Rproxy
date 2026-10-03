#pragma once
#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include "Server.hpp"
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

//@Refactor: Make this class a CRTP class
template <typename Derived>
class BaseSession : public std::enable_shared_from_this<Derived> {
protected:
	http::request<http::string_body> m_req;
	http::response<http::string_body> m_resp;
	Server* m_serv_ref;

public:
	BaseSession(Server* s_ref) : m_serv_ref(s_ref) {}
    Derived* self() { return static_cast<Derived*>(this); };
};



