add_executable(rproxy_tests
    smoke_test.cpp
    tls_test.cpp
    RequestToolsTests.cpp
    ../src/RequestTools.cpp
    ConfigTests.cpp
    ErrorResponseTests.cpp
    ../src/ErrorResponse.cpp
    LoadBalancerTests.cpp
    ../src/LoadBalancer.cpp
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

gtest_discover_tests(rproxy_tests)
