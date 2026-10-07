add_executable(rproxy_tests
    smoke_test.cpp
    tls_test.cpp
    RequestToolsTests.cpp
    ../src/RequestTools.cpp
    ConfigTests.cpp
    ErrorResponseTests.cpp
    ../src/ErrorResponse.cpp
    LoadBalancerTests.cpp
    ProxyTests.cpp
    ../src/Server.cpp
)

target_link_libraries(rproxy_tests PRIVATE
    rproxy_deps
    GTest::gtest
    GTest::gtest_main)

include(GoogleTest)

# ProxyTests needs the TLS material, which lives beside the binaries. It is
# passed as an absolute path rather than resolved relative to the working
# directory, because gtest_discover_tests regenerates its per-test ctest
# entries at build time and overwrites any WORKING_DIRECTORY set here.
target_compile_definitions(rproxy_tests PRIVATE
    RPROXY_TLS_CERT_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/certificates")

# Test certificate authority, generated rather than committed.
#
# Upstream verification is on by default, so a test origin cannot use an ad hoc
# self signed certificate: the proxy has to be given something it can actually
# trust. That means a CA, a leaf signed by it, and the CA handed to the proxy as
# ca_file. Generating at build time keeps a fresh clone working without anyone
# running openssl by hand first.
#
# The leaf carries SANs for both localhost and 127.0.0.1 so a test can exercise
# name based verification and the IP literal path separately.
find_program(RPROXY_OPENSSL_EXECUTABLE openssl)

if(NOT RPROXY_OPENSSL_EXECUTABLE)
    message(FATAL_ERROR
        "openssl not found on PATH. It is required to generate the test "
        "certificate authority used by the upstream TLS verification tests.")
endif()

set(RPROXY_CERT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/certificates")
set(RPROXY_CA_FILE "${RPROXY_CERT_DIR}/ca.pem")
set(RPROXY_CA_KEY "${RPROXY_CERT_DIR}/ca-key.pem")
set(RPROXY_LEAF_CERT "${RPROXY_CERT_DIR}/cert.pem")
set(RPROXY_LEAF_KEY "${RPROXY_CERT_DIR}/key.pem")
set(RPROXY_LEAF_SAN "${RPROXY_CERT_DIR}/leaf.ext")

# A self signed leaf from an authority the proxy is never told to trust. Lets a test
# prove that verification is actually enforced rather than silently skipped: if the
# origin presents this and the proxy still relays the request, checking is off.
set(RPROXY_ROGUE_CERT "${RPROXY_CERT_DIR}/rogue.pem")
set(RPROXY_ROGUE_KEY "${RPROXY_CERT_DIR}/rogue-key.pem")

add_custom_command(
    OUTPUT "${RPROXY_LEAF_CERT}" "${RPROXY_ROGUE_CERT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RPROXY_CERT_DIR}"
    COMMAND ${RPROXY_OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048
            -keyout "${RPROXY_CA_KEY}" -out "${RPROXY_CA_FILE}"
            -days 3650 -nodes -subj "/CN=rproxy-test-ca"
    COMMAND ${CMAKE_COMMAND} -E echo "subjectAltName=DNS:localhost,IP:127.0.0.1"
            > "${RPROXY_LEAF_SAN}"
    COMMAND ${RPROXY_OPENSSL_EXECUTABLE} req -newkey rsa:2048
            -keyout "${RPROXY_LEAF_KEY}" -out "${RPROXY_CERT_DIR}/leaf.csr"
            -nodes -subj "/CN=localhost"
    COMMAND ${RPROXY_OPENSSL_EXECUTABLE} x509 -req
            -in "${RPROXY_CERT_DIR}/leaf.csr"
            -CA "${RPROXY_CA_FILE}" -CAkey "${RPROXY_CA_KEY}" -CAcreateserial
            -out "${RPROXY_LEAF_CERT}" -days 3650
            -extfile "${RPROXY_LEAF_SAN}"
    COMMAND ${RPROXY_OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048
            -keyout "${RPROXY_ROGUE_KEY}" -out "${RPROXY_ROGUE_CERT}"
            -days 3650 -nodes -subj "/CN=localhost"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${RPROXY_CERT_DIR}/leaf.csr"
            "${RPROXY_CERT_DIR}/leaf.ext" "${RPROXY_CERT_DIR}/ca.srl"
    DEPENDS "${RPROXY_CA_FILE}"
    COMMENT "Generating the rproxy test certificate authority and leaf certificates"
    VERBATIM)

add_custom_target(rproxy_test_certs DEPENDS "${RPROXY_LEAF_CERT}" "${RPROXY_ROGUE_CERT}")
add_dependencies(rproxy_tests rproxy_test_certs)

# @Usage: The 5s default discovery timeout is too tight here. Discovery runs the freshly
# linked binary, which has to load the Boost DLLs while ninja is still saturating the
# machine, and it was being killed as "Process terminated due to timeout" even though the
# same command succeeds standalone. 120s costs nothing when discovery is fast.
gtest_discover_tests(rproxy_tests DISCOVERY_TIMEOUT 120)
