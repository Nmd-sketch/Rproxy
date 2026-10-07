#pragma once
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct HostData {
    std::string name;
    std::string host;
    std::uint16_t port;
    bool uses_tls;

    // @Usage: Verify this upstream's certificate chain and hostname. Defaults true; turned
    // off only for an origin presenting a certificate this proxy has no CA for    
    bool verify_tls;

    // @Usage: Name to send as SNI and to match the certificate against, when it is not
    // the host we connect to. Needed when the upstream is reached by IP but the
    // certificate was issued for a name. Empty means "use host".
    std::string sni_name;

    HostData(std::string_view n, std::string_view h, std::uint16_t p, bool tls,
             bool verify = true, std::string_view sni = {})
        : name(n), host(h), port(p), uses_tls(tls), verify_tls(verify), sni_name(sni) {}

    // @Usage: The name this upstream is known by for TLS purposes, which is what SNI
    // carries and what the certificate is verified against.
    const std::string& tls_name() const noexcept {
        return sni_name.empty() ? host : sni_name;
    }
};

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

class Config {
    std::uint16_t port = 8080;
    std::size_t thread_num = 2;

    std::string certificate_path = "certificates/cert.pem";
    std::string key_path = "certificates/key.pem";

    // @Usage: Trust store for verifying upstream certificates, in PEM bundle form.
    // Empty means no CA is configured, which is only legal when no upstream both
    // uses TLS and asks to be verified: with nothing to trust, verify_peer would fail
    // every handshake. load_upstreams rejects that combination rather than letting the
    // proxy start and then reject traffic it could have diagnosed at startup.
    std::string ca_file;

    std::vector<HostData> upstreams_array;

    // @Usage: Defaults come from the design doc, section 5 step 2: 8 KiB of headers and
    // 8 MiB of body. header_bytes stays small because a request line plus headers is
    // normally well under that, whereas body_bytes has to accommodate ordinary uploads.
    // An 8 KiB body default rejects every non trivial POST with a 413, which reads as a
    // broken proxy rather than a configured limit.
    std::size_t body_bytes = 8 * 1024 * 1024;
    std::size_t header_bytes = 8 * 1024;

    std::chrono::milliseconds connect_ms{5000};
    std::chrono::milliseconds handshake_ms{8000};
    std::chrono::milliseconds read_ms{30000};

    static constexpr std::uint16_t kMinPort = 1;
    static constexpr std::uint16_t kMaxPort = 65535;

    std::string type_name(toml::node_type type) {
        std::ostringstream out;
        out << type;
        return out.str();
    }

    void require_table(const toml::table& tbl, std::string_view key, std::string_view path,
                              std::vector<std::string>& errors) {
        const auto node = tbl[key];
        if (static_cast<bool>(node) && !node.is_table()) {
            errors.push_back(std::string(path) + " must be a table, got " + type_name(node.type()));
        }
    }

    toml::table load_file(std::string_view config_path) {
        try {
            return toml::parse_file(config_path);
        } catch (const toml::parse_error& e) {
            throw ConfigError("Failed to load configuration file '" + std::string(config_path) +
                              "':\n" + std::string(e.description()) +
                              "\n  (line " + std::to_string(e.source().begin.line) +
                              ", column " + std::to_string(e.source().begin.column) + ")");
        }
    }

    std::int64_t read_int(const toml::table& tbl, std::string_view key,
                                 std::string_view ctx, std::int64_t lo, std::int64_t hi,
                                 std::int64_t fallback, std::vector<std::string>& errors) {
        const auto node = tbl[key];
        if (!static_cast<bool>(node)) {
            return fallback;
        }

        if (!node.is_integer()) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must be an integer, got " + type_name(node.type()));
            return fallback;
        }

        const auto value = node.value<std::int64_t>();
        if (!value) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must be an integer, got " + type_name(node.type()));
            return fallback;
        }

        if (*value < lo || *value > hi) {
            errors.push_back(std::string(ctx) + "." + std::string(key) + " is " +
                             std::to_string(*value) + ", outside allowed range [" +
                             std::to_string(lo) + ", " + std::to_string(hi) + "]");
            return fallback;
        }

        return *value;
    }

    std::string read_string(const toml::table& tbl, std::string_view key,
                                   std::string_view ctx, std::string fallback,
                                   std::vector<std::string>& errors) {
        const auto node = tbl[key];
        if (!static_cast<bool>(node)) {
            return fallback;
        }

        const auto value = node.value<std::string>();
        if (!value) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must be a string, got " + type_name(node.type()));
            return fallback;
        }

        if (value->find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must not be blank");
            return fallback;
        }

        return *value;
    }

    bool read_bool(const toml::table& tbl, std::string_view key,
                          std::string_view ctx, bool fallback, std::vector<std::string>& errors) {
        const auto node = tbl[key];
        if (!static_cast<bool>(node)) {
            return fallback;
        }

        if (!node.is_boolean()) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must be a boolean, got " + type_name(node.type()));
            return fallback;
        }

        const auto value = node.value<bool>();
        if (!value) {
            errors.push_back(std::string(ctx) + "." + std::string(key) +
                             " must be a boolean, got " + type_name(node.type()));
            return fallback;
        }

        return *value;
    }

    void load_upstreams(const toml::table& root, std::vector<HostData>& out,
                               std::vector<std::string>& errors) {
        const auto node = root["upstream"];
        if (!static_cast<bool>(node)) {
            return;
        }

        const auto* arr = node.as_array();
        if (!arr) {
            errors.push_back("upstream must be an array of tables, got " + type_name(node.type()));
            return;
        }

        int index = 0;
        for (const auto& elem : *arr) {
            const std::string ctx = "upstream[" + std::to_string(index) + "]";

            const auto* t = elem.as_table();
            if (!t) {
                errors.push_back(ctx + " must be a table, got " + type_name(elem.type()));
                ++index;
                continue;
            }

            const std::size_t before = errors.size();

            for (const std::string_view key : {"name", "host", "port"}) {
                if (!static_cast<bool>((*t)[key])) {
                    errors.push_back(ctx + "." + std::string(key) + " is required");
                }
            }

            const std::string name = read_string(*t, "name", ctx, "", errors);
            const std::string host = read_string(*t, "host", ctx, "", errors);
            const auto port_value = read_int(*t, "port", ctx, kMinPort, kMaxPort, 0, errors);

            // @Usage: tls = true routes this upstream to an ssl::stream leg, per
            // Server::accept. verify_tls turns certificate checking on, which is the
            // default; sni_name overrides the name presented in SNI and matched against
            // the certificate, for upstreams reached by IP.
            const bool upstream_tls = read_bool(*t, "tls", ctx, false, errors);
            const bool verify = read_bool(*t, "verify_tls", ctx, true, errors);
            const std::string sni = read_string(*t, "sni_name", ctx, "", errors);

            if (errors.size() != before) {
                ++index;
                continue;
            }

            const bool duplicate = std::any_of(out.begin(), out.end(),
                                               [&](const HostData& h) { return h.name == name; });
            if (duplicate) {
                errors.push_back("upstream[" + std::to_string(index) +
                                 "].name duplicates an earlier upstream: " + name);
                ++index;
                continue;
            }

            // @Correctness: An sni_name that is never sent is a silent misconfiguration:
            // the operator named a certificate, we connected by host, and the mismatch
            // surfaces as a handshake failure with no hint about the cause. Reject it
            // here, where the message can still say which key is wrong.
            if (!sni.empty() && !upstream_tls) {
                errors.push_back(ctx + ".sni_name has no effect when tls = false");
                ++index;
                continue;
            }

            out.emplace_back(name, host, static_cast<std::uint16_t>(port_value), upstream_tls,
                             verify, sni);
            ++index;
        }
    }

public:
    explicit Config(const toml::table& root) {
        std::vector<std::string> errors;

        require_table(root, "server", "server", errors);
        require_table(root, "limits", "limits", errors);
        require_table(root, "timeouts", "timeouts", errors);

        if (const auto* server = root["server"].as_table(); server) {
            constexpr std::string_view tls_ctx = "server.tls";
            require_table(*server, "tls", tls_ctx, errors);

            port = static_cast<std::uint16_t>(
                read_int(*server, "port", "server", kMinPort, kMaxPort, port, errors));
            thread_num = static_cast<std::size_t>(
                read_int(*server, "threads", "server", 1, 1024, static_cast<std::int64_t>(thread_num), errors));

            if (const auto* tls = (*server)["tls"].as_table(); tls) {
                certificate_path = read_string(*tls, "cert", tls_ctx, certificate_path, errors);
                key_path = read_string(*tls, "key", tls_ctx, key_path, errors);
                ca_file = read_string(*tls, "ca_file", tls_ctx, ca_file, errors);
            }
        }

        if (const auto* limits = root["limits"].as_table(); limits) {
            constexpr std::int64_t kMaxBody = 1LL << 30;
            constexpr std::int64_t kMaxHeader = 1LL << 24;

            body_bytes = static_cast<std::size_t>(read_int(
                *limits, "body_bytes", "limits", 1, kMaxBody,
                static_cast<std::int64_t>(body_bytes), errors));
            header_bytes = static_cast<std::size_t>(read_int(
                *limits, "header_bytes", "limits", 1, kMaxHeader,
                static_cast<std::int64_t>(header_bytes), errors));
        }

        if (const auto* timeouts = root["timeouts"].as_table(); timeouts) {
            connect_ms = std::chrono::milliseconds{
                read_int(*timeouts, "connect_ms", "timeouts", 1, 3600000,
                         static_cast<std::int64_t>(connect_ms.count()), errors)};
            handshake_ms = std::chrono::milliseconds{
                read_int(*timeouts, "handshake_ms", "timeouts", 1, 3600000,
                         static_cast<std::int64_t>(handshake_ms.count()), errors)};
            read_ms = std::chrono::milliseconds{
                read_int(*timeouts, "read_ms", "timeouts", 1, 3600000,
                         static_cast<std::int64_t>(read_ms.count()), errors)};
        }

        load_upstreams(root, upstreams_array, errors);

        // @Correctness: Checked here rather than left to the handshake. A TLS upstream
        // that asks to be verified but has no ca_file would otherwise start cleanly and
        // then fail every request to that origin with a 502 that says nothing about the
        // real cause. Startup is the only place this is still cheap to diagnose.
        if (ca_file.empty()) {
            for (const auto& up : upstreams_array) {
                if (up.uses_tls && up.verify_tls) {
                    errors.push_back("upstream '" + up.name +
                        "' uses TLS and asks to be verified, but server.tls.ca_file is not "
                        "set. Add a ca_file, or set verify_tls = false on that upstream to "
                        "accept an unverified origin certificate.");
                }
            }
        }

        if (!errors.empty()) {
            std::string message = "Invalid configuration:";
            for (const auto& e : errors) {
                message += "\n  - ";
                message += e;
            }
            throw ConfigError(message);
        }
    }

    explicit Config(std::string_view config_path) : Config(load_file(config_path)) {}

    std::uint16_t get_port() const noexcept { return port; }
    std::size_t get_thread_num() const noexcept { return thread_num; }

    const std::string& get_certificate_path() const noexcept { return certificate_path; }
    const std::string& get_key_path() const noexcept { return key_path; }
    const std::string& get_ca_file() const noexcept { return ca_file; }

    const std::vector<HostData>& get_upstreams() const noexcept { return upstreams_array; }

    std::size_t get_body_bytes() const noexcept { return body_bytes; }
    std::size_t get_header_bytes() const noexcept { return header_bytes; }

    std::chrono::milliseconds get_connect_ms() const noexcept { return connect_ms; }
    std::chrono::milliseconds get_handshake_ms() const noexcept { return handshake_ms; }
    std::chrono::milliseconds get_read_ms() const noexcept { return read_ms; }
};
