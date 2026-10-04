#include "../include/rproxy/RequestTools.hpp"
#include <unordered_set>
#include <string>
#include <vector>

//Lookup table for erasing the standard hop by hop headers
std::unordered_set<http::field> headers_lookup_table{
    http::field::connection,
    http::field::keep_alive,
    http::field::proxy_authenticate,
    http::field::proxy_authorization,
    http::field::te,
    http::field::trailer,
    http::field::transfer_encoding,
    http::field::upgrade,
    http::field::alt_svc,
    http::field::strict_transport_security,
    http::field::x_forwarded_for
};

http::request<http::string_body> RequestTools::make_clean_request(http::request<http::string_body> req) {
    
    // First check what the client wants us to erase, specified in the connection field
    auto conn_it = req.find(http::field::connection);
    if (conn_it != req.end()) {
        std::vector<std::string> temp;
        for (auto token : http::token_list(conn_it->value())) {
            temp.push_back(token);
        }

        for (const auto& name : temp) {
            req.erase(name);
        }
    }
    
    // Now we erase the rest of the hop by hop standard http fields
    auto it = req.begin();
    while (it != req.end()) {
        if (headers_lookup_table.count(it->name())) {
            it = req.erase(it);
        } else {
            ++it;
        }
    }

    return req;
}

// @Implementation: Same headers are removed
http::response<http::string_body> RequestTools::make_clean_response(http::response<http::string_body> resp) {

    auto conn_it = resp.find(http::field::connection);
    if (conn_it != resp.end()) {
        std::vector<std::string> temp;
        for (auto token : http::token_list(conn_it->value())) {
            temp.push_back(token);
        }
        
        for (const auto& name : temp) {
            resp.erase(name);
        }
    }
    
    auto it = resp.begin();
    while (it != resp.end()) {
        if (headers_lookup_table.count(it->name())) {
            it = resp.erase(it);
        } else {
            ++it;
        }
    }

    return resp;
}

void RequestTools::append_x_headers(http::request<http::string_body>& req, std::string_view client_ip, std::string_view conn_type) {
    // @Correctness: We overwrite rather than append because we don't trust client-supplied values
    req.set(http::field::x_forwarded_for, client_ip);

    // @Correctness: "http" or "https" based on the type of the connection
    req.set(http::field::x_forwarded_proto, conn_type);
    
    // @Correctness: Preserve the original host from the client, added to X-Forwarded-Host
    auto host_it = req.find(http::field::host);
    if (host_it != req.end()) req.set(http::field::x_forwarded_host, host_it->value());
}


