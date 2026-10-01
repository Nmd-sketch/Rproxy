add_executable(rproxy_tests
    smoke_test.cpp
    tls_test.cpp)

target_link_libraries(rproxy_tests PRIVATE
    rproxy_deps
    GTest::gtest
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rproxy_tests)
