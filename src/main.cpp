#include <memory>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include "../include/rproxy/Server.hpp"
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

int main()
{
	Server s(8080);
	s.start();
}
