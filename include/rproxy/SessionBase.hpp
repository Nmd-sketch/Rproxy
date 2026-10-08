#pragma once
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <iostream>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/system_error.hpp>
#include <boost/version.hpp>
#include <boost/asio/ssl.hpp>
#include "ErrorResponse.hpp"
#include "RequestTools.hpp"
#include "Server.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace sys = boost::system;

template <typename T>
struct is_ssl_stream : std::false_type {};

template <typename NextLayer>
struct is_ssl_stream<net::ssl::stream<NextLayer>> : std::true_type {};

template <typename T>
inline constexpr bool is_ssl_stream_v = is_ssl_stream<T>::value;

// @Usage: async_connect has to be handed the layer that actually owns the socket.
// For a plain upstream that is the stream itself; for a TLS upstream it is the
// tcp::socket wrapped by the ssl::stream. Partial ordering picks the second
// overload whenever the argument is an ssl::stream.
template <typename Stream>
Stream& lowest_layer(Stream& stream) noexcept {
    return stream;
}

template <typename NextLayer>
NextLayer& lowest_layer(net::ssl::stream<NextLayer>& stream) noexcept {
    return stream.next_layer();
}

// @Usage: Builds the second leg at construction time. Neither leg type is
// default constructible: a tcp::socket needs an io_context, and an ssl::stream
// needs the context on top of that (stream.hpp, the stream(Arg&&, context&)
// constructor). Constructing here keeps the member a value rather than forcing
// every call site to assemble and move one in.
template <typename Upstream>
Upstream make_upstream(Server* s_ref) {
    if constexpr (is_ssl_stream_v<Upstream>) {
        return Upstream(net::ip::tcp::socket(s_ref->io_context()),
                        s_ref->upstream_ssl_ctx());
    } else {
        return Upstream(s_ref->io_context());
    }
}

template <typename Derived, typename DownStream, typename Upstream = net::ip::tcp::socket>
class SessionBase : public std::enable_shared_from_this<Derived> {
protected:
    DownStream m_down;
    beast::flat_buffer m_buf;
    http::request<http::string_body> m_req;
    http::response<http::string_body> m_resp;

    // @Correctness: Error responses are owned HERE rather than built as a local and
    // handed to async_write. Beast's serializer keeps a reference to the message for the
    // whole operation, so a local or a by-value parameter is already destroyed by the time
    // the write runs. The read then serializes freed memory: a garbage Content-Length or
    // chunk framing makes Beast's serializer spin forever inside consume(), which wedges
    // the io_context thread so Server::start never returns and the harness join hangs, or
    // faults outright when the garbage happens to look like a pointer.
    //
    // Storing it in the session makes it outlive the write, because every write here
    // captures self in its handler and the session cannot be destroyed before its own
    // handler runs. One member is enough: a session only ever sends one error response.
    http::response<http::string_body> m_err;
    Server* m_serv_ref;
    std::string m_client_ip;
    const HostData* m_upstream;

    // @Note: Second leg. Built closed and opened by async_connect later, because a
    // tcp::socket has no default constructor and the upstream is not known until the
    // request has been read and routed.
    Upstream m_up;
    beast::flat_buffer m_up_buf;

    // @Note: Held as a member because async_connect iterates it after the completion
    // handler returns. A local in the handler would dangle.
    net::ip::tcp::resolver::results_type m_endpoints;

    // @Correctness: Beast's steady_timer, used as a deadline. A step times out rather than
    // hanging forever, because an unbounded wait is a denial of service that any peer
    // can trigger: a slowloris client on the client leg, a silent origin on the
    // upstream leg.
    net::steady_timer m_deadline;

    // @Usage: Drain budget for an error response. Long enough that the 504 lands in any
    // real socket buffer, short enough that a client which never reads cannot hold the
    // session. Deliberately not configurable: it bounds a flush we control, not a
    // network condition the operator is describing.
    static constexpr std::chrono::milliseconds kErrorDrainGrace{1000};

    void arm_deadline(std::chrono::milliseconds budget) {
        m_deadline.expires_after(budget);
        m_deadline.async_wait(
            [self = this->shared_from_this()] (const beast::error_code ec) {


                if (ec == net::error::operation_aborted) {
                    return;
                }

                if (ec) {
                    std::cerr << "Deadline timer error: " << ec.message() << '\n';
                    return;
                }

                self->m_deadline.cancel();

                std::cerr << "Upstream step timed out\n";
                self->send_error(ErrorResponse::make_error_504(self->m_req.version()));
            }
        );
    }

    void close_upstream_and_downstream() {
        //@Note: Even if close returns an error, the underlying socket descriptor
        //gets closed by the OS. Which is what we want anyways,
        sys::error_code close_ec;
        beast::get_lowest_layer(m_down).close(close_ec);
        beast::get_lowest_layer(m_up).close(close_ec);
    }

    void disarm_deadline() {
        m_deadline.cancel();
        m_deadline.expires_at(std::chrono::steady_clock::now());
    }

    void read_request() {
        auto self = this->shared_from_this();


        // @Note: Bounds a client that connects and then stalls without sending a
        // request, or trickles headers forever.
        self->arm_deadline(self->m_serv_ref->config().get_read_ms());
        
        // @Correctness: The size of the incoming request must have an upper bound
        // or we might receive huge payloads from malicious requests. We guard against
        // that by setting up body and header size limits
        auto req_parser = std::make_shared<http::request_parser<http::string_body>>();
        req_parser->body_limit(self->m_serv_ref->config().get_body_bytes());
        req_parser->header_limit(self->m_serv_ref->config().get_header_bytes());

        http::async_read(m_down, m_buf, *req_parser,
            [self, req_parser] (const beast::error_code ec, std::size_t) {


                if (ec) {
                    // @Correctness: A Content-Length plus Transfer-Encoding conflict is a
                    // smuggling attempt, so it is reported distinctly from an ordinary
                    // malformed request. Beast rejects both orders before we get here.
                    const bool framing_conflict =
                        ec == http::error::bad_transfer_encoding ||
                        ec == http::error::bad_content_length ||
                        ec == http::error::multiple_content_length;

                    const bool body_size_exceeded = ec == http::error::body_limit; 
                    const bool header_size_exceeded = ec == http::error::header_limit;

                    self->disarm_deadline();
                    std::cerr << "Error parsing the http request (" << ec.message()
                              << "), dropping the connection...\n";

                    // @Correctness: The request may not have parsed far enough to know the client's
                    // version, so 10 is the safest floor to reply with.
                    //
                    // Written to m_err, a session member, because async_write's
                    // serializer holds a reference to the message for the whole operation.
                    // See m_err for exactly what handing it a local costs.
                    self->m_err = framing_conflict
                        ? ErrorResponse::make_error_400_framing_conflict(10)
                        : (body_size_exceeded) ? 
                        ErrorResponse::make_error_413(self->m_serv_ref->config().get_body_bytes(), 10) : 
                        (header_size_exceeded) ? 
                        ErrorResponse::make_error_431(req_parser->release().version()) : 
                        ErrorResponse::make_error_400(10);

                    //@Note: If we get an error sending the error response to the client there is nothing
                    //else we can do, just drop the connection entirely

                    http::async_write(self->m_down, self->m_err,
                            [self] (const beast::error_code ec, std::size_t) {

                            if (ec) {
                                std::cerr << "Error sending response\n";
                            }
                        }
                    );
                    return;
                }

                self->m_req = req_parser->release();
                self->connect_upstream();
            }
        );
    }

    // @Usage: Runs after the upstream TCP connect succeeds. On a TLS leg there is one
    // more step before the socket can carry HTTP: the TLS handshake, budgeted by
    // handshake_ms. On a plain leg there is nothing to do, so the whole body is discarded
    // at compile time and no handshake code is instantiated.
    void handshake_upstream() {
        auto self = this->shared_from_this();

        if constexpr (is_ssl_stream_v<Upstream>) {
            const std::string& tls_name = self->m_upstream->tls_name();

            // @Usage: SNI so a name based origin can pick the right certificate.
            //
            // @Correctness: Skipped for an IP literal. RFC 6066 section 3 forbids an
            // IP address in the server_name extension, so sending one is a protocol
            // violation; OpenSSL origins ignore a name they do not recognise, but stricter
            // ones abort the handshake. An IP configured as the host simply gets no SNI,
            // and sni_name is the way to give such an upstream a name to use.
            sys::error_code ip_ec;
            const net::ip::address parsed = net::ip::make_address(tls_name, ip_ec);
            const bool is_ip_literal = !ip_ec;

            if (!tls_name.empty() && !is_ip_literal) {
                SSL_set_tlsext_host_name(self->m_up.native_handle(), tls_name.c_str());
            }

            // @Usage: Verification is per stream, not per context. The upstream context is
            // shared by every session, so setting the verify callback there would let one
            // upstream's hostname assertion leak into another's handshake and make the
            // result depend on which connection happened to go first.
            //
            // @Correctness: The context is left at verify_peer by Server. Downgrading to
            // verify_none here is the explicit opt out for an upstream whose certificate
            // this proxy has no CA for. The inverse is never done: verify_tls = true does
            // not restore checking here, it only declines to remove it, which keeps a
            // misread flag from quietly enabling verification on an origin the operator
            // meant to skip.
            if (self->m_upstream->verify_tls) {
                self->m_up.set_verify_mode(net::ssl::verify_peer);
                self->m_up.set_verify_callback(net::ssl::host_name_verification{tls_name});
            } else {
                self->m_up.set_verify_mode(net::ssl::verify_none);
            }

            self->arm_deadline(self->m_serv_ref->config().get_handshake_ms());

            self->m_up.async_handshake(net::ssl::stream_base::client,
                [self] (const sys::error_code& ec) {

                    if (ec) {
                        std::cerr << "Upstream TLS handshake failed: " << ec.message() << '\n';
                        self->disarm_deadline();
                        self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                        return;
                    }

                    // @Usage: The handshake budget is spent, so the read budget takes over
                    // for the forward and relay steps, same handoff as after connect.
                    self->disarm_deadline();
                    self->arm_deadline(self->m_serv_ref->config().get_read_ms());

                    self->write_upstream();
                }
            );
        } else {
            self->write_upstream();
        }
    }

    void connect_upstream() {
        auto self = this->shared_from_this();
        
        //@Correctness: If there are no upstreams we cannot serve the request right now
        //so we send error 503 back to the client. But this only should happen if there
        //are absolutely no upstreams, if there is at least 1 upstream available we should
        //route to that one
        if (self->m_upstream == nullptr) {
            std::cerr << "No upstream available\n";
            self->send_error(ErrorResponse::make_error_503(self->m_req.version()));
            return;
        }

        self->arm_deadline(self->m_serv_ref->config().get_connect_ms());

        self->m_serv_ref->load_balancer().resolver().async_resolve(
            self->m_upstream->host,
            std::to_string(self->m_upstream->port),
            [self] (const sys::error_code& ec, net::ip::tcp::resolver::results_type results) {
               
                //@Fix: A resolve or connect failure handles error 502 without retrying with
                //another possibly healthy upstream. This feature will be implemented in another
                //iteration, for now we just stick with this.
                if (ec) {
                    std::cerr << "Upstream resolve failed: " << ec.message() << '\n';
                    self->disarm_deadline();
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->m_endpoints = std::move(results);

                // @Note: net::async_connect (the free function), not the socket member, because
                // the range overload lives in <boost/asio/connect.hpp>. It takes
                // results_type by const reference and owns the iteration itself, so
                // m_endpoints must outlive this call, which is why it is a member.
                //
                // @Correctness: The target is lowest_layer(m_up), not m_up. An ssl::stream is
                // not a socket, so connecting to it directly does not compile on a TLS leg.
                // lowest_layer resolves to the socket under the ssl::stream, and to m_up
                // itself on a plain leg, so one call covers both.
                net::async_connect(lowest_layer(self->m_up), self->m_endpoints,
                    [self] (const sys::error_code& ec, const net::ip::tcp::endpoint&) {

                        if (ec) {
                            std::cerr << "Upstream connect failed: " << ec.message() << '\n';
                            self->disarm_deadline();
                            self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                            return;
                        }

                        // @Usage: The connect budget is spent; the read budget takes over
                        // for the forward and relay steps.
                        self->disarm_deadline();
                        self->arm_deadline(self->m_serv_ref->config().get_read_ms());

                        self->handshake_upstream();
                    }
                );
            }
        );
    }

    void write_upstream() {
        auto self = this->shared_from_this();

        // @Usage: append_x_headers must run AFTER make_clean_request. The hop by hop
        // table contains x_forwarded_for, so the cleaner erases it and running these
        // the other way round silently drops X-Forwarded-For.
        self->m_req = RequestTools::make_clean_request(self->m_req);

        const char* proto = "http";
        if constexpr (is_ssl_stream_v<DownStream>) {
            proto = "https";
        }

        RequestTools::append_x_headers(self->m_req, self->m_client_ip, proto);

        // @Usage: Bounds a client that stalls mid-body. Without it a declared
        // Content-Length that never arrives pins the session indefinitely.
        self->arm_deadline(self->m_serv_ref->config().get_read_ms());

        // @Usage: Required because make_clean_request strips Transfer-Encoding. The
        // parser dechunks without writing a Content-Length back, so a chunked request
        // would otherwise reach the origin with a body and no length header.
        //
        // @Correctness: prepare_payload throws std::invalid_argument for a TRACE
        // carrying a body, which any client can send. Without this catch the throw
        // would leave the completion handler, unwind out of io_context::run() and
        // terminate the whole process, so a single request is a remote kill switch.
        try {
            self->m_req.prepare_payload();
        } catch (const std::invalid_argument& e) {
            std::cerr << "Refusing to forward a request Beast rejects: " << e.what() << '\n';
            self->send_error(ErrorResponse::make_error_400(self->m_req.version()));
            return;
        }

        http::async_write(self->m_up, self->m_req,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    self->disarm_deadline();
                    std::cerr << "Error forwarding the request upstream: " << ec.message() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->relay_response();
            }
        );
    }

    void relay_response() {
        auto self = this->shared_from_this();

        // @Correctness: A HEAD response legitimately has no body, so the parser must be told
        // to stop at the headers. Without skip(true) it waits for body bytes the origin
        // will never send, and the session hangs until the connection times out. We set up
        // the parser to be a shared pointer so we don't get a dangling pointer inside the handler
        // due to how asio works.
        auto parser = std::make_shared<http::response_parser<http::string_body>>();

        // @Correctness: We set up body and header limits to keep the server response bounded
        // to a reasonable size.
        parser->body_limit(self->m_serv_ref->config().get_body_bytes());
        parser->header_limit(self->m_serv_ref->config().get_header_bytes());

        // @Note: No body, so let's make the parser skip it instead
        if (self->m_req.method() == http::verb::head) {
            parser->skip(true);
        }

        http::async_read(self->m_up, self->m_up_buf, *parser,
            [self, parser] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    self->disarm_deadline();

                    // @Usage: 502 rather than 413 or 431. The client did nothing wrong
                    // here; the origin produced a response larger than this proxy will
                    // relay, so blaming the client would send it off to fix the wrong thing.
                    if (ec == http::error::body_limit) {
                        std::cerr << "Upstream response body exceeded limits.body_bytes\n";
                    } else if (ec == http::error::header_limit) {
                        std::cerr << "Upstream response headers exceeded limits.header_bytes\n";
                    } else {
                        std::cerr << "Error reading the upstream response: " << ec.message() << '\n';
                    }

                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                self->disarm_deadline();
                self->m_resp = parser->get();

                const unsigned result = self->m_resp.result_int();

                // @Usage: A 1xx/204/304 must not carry a Content-Length. RFC 9110 15.3.5
                // forbids it on a 204 and 304, and the parser leaves whatever the
                // origin sent in place, so it has to be stripped before relaying.
                if (result / 100 == 1 || result == 204 || result == 304) {
                    self->m_resp.base().erase(http::field::content_length);
                }

                self->m_resp = RequestTools::make_clean_response(self->m_resp);

                // @Usage: Required because make_clean_response strips
                // Transfer-Encoding. payload_size() is known for string_body, so this
                // re-derives a correct Content-Length.
                //
                // @Correctness: Throws std::invalid_argument if the origin sent a
                // 1xx/204/304 with a body. Catching it here keeps a broken origin from
                // terminating the process by throwing out of io_context::run().
                try {
                    self->m_resp.prepare_payload();
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Upstream response rejected by Beast: " << e.what() << '\n';
                    self->send_error(ErrorResponse::make_error_502(self->m_req.version()));
                    return;
                }

                // @Correctness: prepare_payload re-adds Content-Length: 0 on a bodiless
                // status, which RFC 9110 forbids. Strip it again, this time after the
                // re-derivation rather than before it.
                if (result / 100 == 1 || result == 204 || result == 304) {
                    self->m_resp.base().erase(http::field::content_length);
                }

                // @Note: The session handles one request and then closes, so the
                // response has to say so. Without this a keep-alive client waits for
                // bytes that never come.
                self->m_resp.keep_alive(false);

                http::async_write(self->m_down, self->m_resp,
                    [self] (const beast::error_code ec, std::size_t) {

                        if (ec) {
                            // @Correctness: A failed write means the client is already gone or the
                            // connection is broken, so there is no channel left to report this
                            // on. Log it and let the session die rather than attempting another
                            // write to the same dead stream, which cannot succeed either.
                            std::cerr << "Error sending the http response to the client\n";
                            return;
                        }

                        self->m_serv_ref->debug_fn();
                    }
                );
            }
        );
    }

    // @Note: We move the response to the class member m_err and let async_write to send 
    // m_err to the downstream. Since async_write takes the http response by reference,
    // passing a reference would make the response a dangling pointer and cause a SEGFAULT 
    void send_error(http::response<http::string_body> resp) {
        auto self = this->shared_from_this();

        self->disarm_deadline();
        self->m_err = std::move(resp);

//@Correctness: How long the client gets to accept the error response before both
        //legs are forced shut, so a client that stalls mid response cannot pin the
        //session forever. The expiry must be stated here: disarm_deadline() deliberately
        //leaves the timer already expired, so a bare async_wait would inherit that and
        //close both legs immediately instead of draining. That happened to still deliver
        //the response because the write was queued first and won the race, which is a
        //scheduling accident rather than a property to rely on.
        m_deadline.expires_after(kErrorDrainGrace);
        m_deadline.async_wait(
            [self] (const sys::error_code ec) {

                // @Correctness: A cancelled wait still runs its handler, with
                //operation_aborted. The write completing calls disarm_deadline(), so
                // without this guard every successful error response logged a spurious
                // "error sending the error response" on its way out.
                if (ec == net::error::operation_aborted) {
                    return;
                }

                if (ec) {
                    std::cerr << "Error: " << ec.what() << '\n';
                }

                self->close_upstream_and_downstream();
            }
        );

        http::async_write(self->m_down, self->m_err,
            [self] (const beast::error_code ec, std::size_t) {

                if (ec) {
                    std::cerr << "Error sending the error response to the client\n";
                }

                self->disarm_deadline();
                self->close_upstream_and_downstream();
                
            }
        );
    }

public:
    // @Usage: @Preconditions @a s_ref outlives this session. The io_context and the
    // upstream ssl context are read to build the second leg, so they cannot be passed
    // in as loose arguments: every call site would have to thread them through.
    //
    // @Correctness: The upstream leg is built in the member initialiser list rather than
    // assigned later. An ssl::stream is not assignable from a tcp::socket, and it has no
    // default constructor, so a post-construction assignment cannot express "the upstream
    // is a TLS stream". Choosing the type here is what lets Upstream stay a plain value.
    SessionBase(DownStream&& down, Server* s_ref, beast::flat_buffer buf = {},
                std::string client_ip = {}, const HostData* upstream = nullptr)
        : m_down(std::move(down)), m_buf(std::move(buf)), m_serv_ref(s_ref),
          m_client_ip(std::move(client_ip)), m_upstream(upstream),
          m_up(make_upstream<Upstream>(s_ref)), m_deadline(s_ref->io_context()) {}

    void start() {
        if constexpr (is_ssl_stream_v<DownStream>) {
            auto self = this->shared_from_this();
            
            //@Correctness: Set up a deadline to prevent the client from never completing
            //the async handshake and pinning the session forever.
            //
            //@Correctness: handshake_ms, not read_ms. This budget covers the TLS
            //handshake and nothing else; read_ms guards the request that comes after it.
            //Borrowing read_ms happens to agree whenever both are configured the same,
            //and then quietly kills a legitimate slow handshake on a client that was
            //configured with a tight read budget.
            self->arm_deadline(this->m_serv_ref->config().get_handshake_ms());
            m_down.async_handshake(net::ssl::stream_base::server, m_buf.data(),
                [self] (const sys::error_code& ec, std::size_t bytes) {

                    if (ec) {
                        std::cerr << "TLS handshake error: " << ec.message() << '\n';
                        self->disarm_deadline();    
                        return;
                    }

                    self->disarm_deadline();    
                    self->m_buf.consume(self->m_buf.size());
                    std::cout << "TLS handshake done, reading request\n";

                    self->read_request();
                }
            );
        } else {
            this->read_request();
        }
    }

    Derived* self() noexcept { return static_cast<Derived*>(this); };
};
