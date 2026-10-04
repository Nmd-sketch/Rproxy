#include "../include/rproxy/ErrorResponse.hpp"

#include <cstdio>
#include <string>

namespace {

    std::string json_escape(std::string_view in) {
        std::string out;
        out.reserve(in.size());

        for (const char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }

        return out;
    }

    http::response<http::string_body> make_json(http::status status, std::string_view slug,
                                                std::string_view message, unsigned version) {
        http::response<http::string_body> resp;

        resp.result(status);
        resp.version(version);
        resp.set(http::field::content_type, "application/json");
        resp.keep_alive(false);
        resp.body() = std::string("{\"error\": \"") + json_escape(slug) +
                      "\", \"message\": \"" + json_escape(message) + "\"}";
        resp.prepare_payload();

        return resp;
    }

}  // namespace

http::response<http::string_body> ErrorResponse::make_error_400(unsigned version) {
    return make_json(http::status::bad_request, "bad_request",
                     "The request could not be understood by the proxy.", version);
}

http::response<http::string_body> ErrorResponse::make_error_400_framing_conflict(
    unsigned version) {
    return make_json(http::status::bad_request, "bad_request",
                     "The request declared both Content-Length and Transfer-Encoding, so its "
                     "body framing is ambiguous and the request was rejected.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_413(
    std::size_t max_payload_size_bytes, unsigned version) {
    return make_json(http::status::payload_too_large, "payload_too_large",
                     "Maximum upload size is " + std::to_string(max_payload_size_bytes) +
                         " bytes.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_431(
    std::string_view header_name, unsigned version) {
    return make_json(http::status::request_header_fields_too_large,
                     "request_header_fields_too_large",
                     "The request header field '" + std::string(header_name) +
                         "' exceeded the maximum allowed size.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_500(unsigned version) {
    return make_json(http::status::internal_server_error, "internal_server_error",
                     "The proxy was unable to complete your request. Please try again later.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_502(unsigned version) {
    return make_json(http::status::bad_gateway, "bad_gateway",
                     "The proxy received an invalid response from the upstream server.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_503(unsigned version) {
    return make_json(http::status::service_unavailable, "service_unavailable",
                     "The proxy is not ready to handle the request. Please try again later.",
                     version);
}

http::response<http::string_body> ErrorResponse::make_error_504(unsigned version) {
    return make_json(http::status::gateway_timeout, "gateway_timeout",
                     "The upstream server did not respond in time. Please try again later.",
                     version);
}
