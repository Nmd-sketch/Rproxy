# reverse_proxy

An HTTP reverse proxy in C++20, built on Boost.Beast and Boost.Asio, with TLS on both
legs and upstream certificate verification.

At this stage it is a **complete, tested HTTP/1.1 forward proxy for a single request per
connection**, not a production-ready proxy. The known gaps are listed below and are
tracked deliberately rather than hidden.

---

## Status

| Milestone | State | Notes |
|---|---|---|
| M1 Config (toml++) | Done | Fails fast, cross-field validation |
| M2 Upstream leg + forwarding | Done | Bytes flow to a real origin |
| M3 Header correctness | Done | Hop-by-hop stripping, `X-Forwarded-*` |
| M4 Framing guard + limits | Done | CL+TE closed, 400 / 413 / 431 |
| M5 Upstream TLS + SNI | Done | 2×2 client/upstream matrix, verify-by-default |
| M6 Timeouts, shutdown, errors | Partial | Per-leg deadlines and JSON errors are in; shutdown aborts in-flight requests rather than draining them |
| M7 Keep-alive + pooling | Not started | One request per connection, see limitations |
| M8 Thread pool, logging, metrics | Not started | See limitations |
| M10 Health checks | Not started | `is_healthy()` currently always true |
| M11 Response cache | Not started | Intentionally last; highest risk |

## What works

- HTTP/1.1 forwarding to one of several configured upstreams, selected round-robin
- TLS **client** leg and TLS **upstream** leg, independently, covering all four combinations
- Upstream certificate verification on by default, using one global CA bundle, with
  per-upstream opt-out and hostname assertion via SNI
- Request **and** response size limits, with the correct status for each side
- Request smuggling defence (Content-Length + Transfer-Encoding conflicts are rejected)
- Per-leg deadlines: connect, TLS handshake, and read
- A connection that is accepted but never says anything is timed out and closed
- JSON error responses with the right status for each failure mode

## Known limitations

These are real and current. None of them are accidents; they are unfinished milestones.

- **`server.threads` is parsed and validated, then ignored.** The server runs a single
  `io_context::run()`. Setting `threads = 4` changes nothing. This is a known defect,
  not a feature.
- **One request per connection.** Every response sets `Connection: close`. Clients pay a
  fresh TCP and TLS handshake per request.
- **No upstream failover.** A resolve or connect failure returns 502 immediately instead
  of retrying the next upstream.
- **Shutdown is abortive.** `stop()` stops accepting and tears down in-flight requests
  rather than letting them finish.
- **No health checking.** Upstream selection is blind round-robin; a dead upstream takes
  down the share of traffic that lands on it.
- **Windows / MSYS2 MinGW-w64 only.** See [Requirements](#requirements). The CMake and
  test code is partially platform-guarded but has not been built on Linux.

---

## Requirements

Verified against this toolchain:

- MSYS2 with the MinGW-w64 (UCRT64 or MINGW64) environment
- GCC 14+ with C++20
- CMake 3.24+ and Ninja
- Boost 1.90+ (Beast and Asio)
- OpenSSL 3.0+ (library **and** the `openssl` command line tool, used at build time to
  generate test certificates)
- toml++ v3 (header-only)

With MSYS2's `pacman`:

```sh
pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,boost,openssl,toml++} openssl
```

`RPROXY_MSYS2_PREFIX` is auto-detected from `MSYSTEM_PREFIX` or `C:/msys64/mingw64`;
override it if your install lives elsewhere.

---

## Build and test

```sh
cmake --preset dev                  # configure (Release)
cmake --build --preset dev -j 2     # build
ctest --preset dev --output-on-failure
```

`cmake --preset debug` / `cmake --build --preset debug` builds a Debug variant with
symbols. Test certificates are generated into `build/dev/bin/certificates` as part of
the build, so a fresh clone works without running `openssl` by hand.

Running the test binary directly is useful when a single case misbehaves:

```sh
# all Proxy integration cases in one process, which widens the timing window
./build/dev/bin/rproxy_tests.exe --gtest_filter='Proxy.*'

# one case, with the harness watchdog raised from its 15s default
RPROXY_TEST_TIMEOUT=60 ./build/dev/bin/rproxy_tests.exe --gtest_filter='Proxy.RoutesOverTls'
```

---

## Running it

`main()` reads `config.toml` from the current working directory.

For a TLS listener you need your own certificate:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout key.pem -out cert.pem -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

Then a minimal `config.toml`:

```toml
[server]
port = 8080
threads = 1          # parsed, validated, and ignored — see limitations

[server.tls]
cert = "cert.pem"
key  = "key.pem"

[[upstream]]
name = "api"
host = "127.0.0.1"
port = 8081
tls  = false

[limits]
header_bytes = 8192
body_bytes   = 8388608

[timeouts]
connect_ms   = 5000
handshake_ms = 5000
read_ms      = 30000
```

### Configuration reference

| Key | Type | Default | Meaning |
|---|---|---|---|
| `[server] port` | int 1–65535 | `8080` | Listen port |
| `[server] threads` | int 1–1024 | `1` | **Currently ignored** |
| `[server.tls] cert` | string | — | Server certificate chain |
| `[server.tls] key` | string | — | Private key |
| `[server.tls] ca_file` | string | — | Trust store for **upstream** verification |
| `[[upstream]] name` | string | — | Identifier, must be unique |
| `[[upstream]] host` | string | — | Hostname or IP literal |
| `[[upstream]] port` | int 1–65535 | — | Origin port |
| `[[upstream]] tls` | bool | `false` | Use TLS to this origin |
| `[[upstream]] verify_tls` | bool | `true` | Verify the origin's certificate |
| `[[upstream]] sni_name` | string | host | Name for SNI and hostname assertion |
| `[limits] header_bytes` | int | `8192` | Max header size, both directions |
| `[limits] body_bytes` | int | `8388608` | Max body size, both directions |
| `[timeouts] connect_ms` | int | `5000` | Upstream TCP connect |
| `[timeouts] handshake_ms` | int | `5000` | TLS handshake, both directions, and the pre-detection window |
| `[timeouts] read_ms` | int | `30000` | Request and response read |

Configuration is validated at startup and a bad config exits with a message rather than
failing later. Cross-field rules include: a TLS upstream with `verify_tls = true` and no
`ca_file` is rejected, and `sni_name` on a non-TLS upstream is rejected.

SNI is skipped automatically when the host is an IP literal, since RFC 6066 forbids an
IP in the `server_name` extension. Configure `sni_name` to give such an upstream a name
to verify against.

### Error responses

All errors are JSON.

| Status | Cause |
|---|---|
| `400` | Malformed request, or a Content-Length + Transfer-Encoding conflict |
| `413` | Request body exceeded `limits.body_bytes` |
| `431` | Request headers exceeded `limits.header_bytes` |
| `502` | Origin connect, handshake, or response failed, including origin-side limit breaches |
| `503` | No upstream configured |
| `504` | A deadline expired |

---

## Architecture

```
Config (toml++) ──► Server ──► LoadBalancer ──► SessionBase ──► RequestTools
   validates          accepts    round-robin       the flow      hop-by-hop rules
   & fails fast                    + health        (CRTP)        X-Forwarded-*
                                       seam                        ErrorResponse
```

`SessionBase` is a single CRTP state machine holding the whole request flow:

```
read_request → connect_upstream → handshake_upstream → write_upstream → relay_response
```

The four concrete sessions are pure type aliases over it, differing only in the two leg
types:

| | Upstream plain | Upstream TLS |
|---|---|---|
| **Client plain** | `PlainSession` | `PlainToTlsSession` |
| **Client TLS** | `TlsToPlainSession` | `TlsToTlsSession` |

Each handler captures `shared_from_this()`, so a session stays alive exactly as long as
it has an operation in flight.

### Layout

```
include/rproxy/
  Config.hpp         toml++ parsing, validation, defaults
  Server.hpp/.cpp    acceptor, accept loop, shutdown
  LoadBalancer.hpp   round-robin selection, health seam
  SessionBase.hpp    the request flow, as a CRTP base
  PlainSession.hpp   the two plain-client leaves
  TlsSession.hpp     the two TLS-client leaves
  RequestTools.hpp   hop-by-hop rules, X-Forwarded-*, payload framing
  ErrorResponse.hpp  JSON error factories
src/                 implementations of the above, plus main()
tests/               GoogleTest suites, one file per component
```

---

## Tests

115 tests, roughly 2,000 lines of test code against 1,000 lines of production code.

| Suite | Count | Covers |
|---|---|---|
| `Config` | 47 | Parsing, defaults, ranges, cross-field rejection |
| `Proxy` | 32 | End-to-end against stub origins, including TLS both ways |
| `ErrorResponse` | 9 | Statuses, JSON escaping, framing |
| `MakeCleanRequest` | 7 | Hop-by-hop stripping |
| `AppendXHeaders` | 5 | `X-Forwarded-*` |
| `LoadBalancer` | 5 | Round-robin, empty list |
| `MakeCleanResponse` | 4 | Hop-by-hop stripping |
| `Tls` | 4 | Stream layering |
| `Scaffold` | 2 | Sanity |

`ProxyTests.cpp` is mostly harness: stub origins (plain and TLS), pinned-port allocation,
a raw client socket for stall cases, a per-test watchdog, and a per-connection
close-probe used to assert that *both* legs are released on timeout.

Each `Proxy` case runs in its own process via `gtest_discover_tests`, so the suite is
serial and deterministic. The watchdog fails a case that exceeds 15s rather than letting
it hang, which is what makes a spin attributable instead of mysterious.

---

## Roadmap

1. **Failover retry** — try the next upstream when resolve or connect fails
2. **Client keep-alive** — loop the session, and stop closing the connection on upstream
   errors while still closing it on request-framing errors
3. **Upstream pooling** — share origin connections across sessions
4. **Honour `server.threads`**, then add structured logging and metrics
5. **Health checks**, completing the load balancer seam
6. **Graceful drain** on shutdown

Response caching and WebSocket relay are deliberately out of scope for now: the first is
the easiest place in this design to introduce a security bug, and the second needs its
own buffer discipline rather than the request/response path.

---

## License

MIT. See [LICENSE](LICENSE).