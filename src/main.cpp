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
#include "../include/rproxy/Config.hpp"

int main()
{
	try {
		Server s("config.toml");
		s.start();
	} catch (const ConfigError& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
