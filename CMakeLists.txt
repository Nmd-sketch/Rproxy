cmake_minimum_required(VERSION 3.24)

project(reverse_proxy
    VERSION 0.1.0
    DESCRIPTION "HTTP reverse proxy built on Boost.Beast and Boost.Asio"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(RPROXY_MSYS2_PREFIX "" CACHE PATH
    "MSYS2 MINGW64 prefix to search when CMake runs outside an MSYS2 shell")

if(NOT RPROXY_MSYS2_PREFIX)
    if(DEFINED ENV{MSYSTEM_PREFIX})
        set(RPROXY_MSYS2_PREFIX "$ENV{MSYSTEM_PREFIX}")
    elseif(EXISTS "C:/msys64/mingw64")
        set(RPROXY_MSYS2_PREFIX "C:/msys64/mingw64")
    endif()
endif()

if(RPROXY_MSYS2_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${RPROXY_MSYS2_PREFIX}")
    message(STATUS "MSYS2 prefix: ${RPROXY_MSYS2_PREFIX}")
endif()

add_library(rproxy_deps INTERFACE)
target_include_directories(rproxy_deps INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(WIN32)
    target_compile_definitions(rproxy_deps INTERFACE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00)
endif()

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost REQUIRED COMPONENTS beast asio)
find_package(OpenSSL 3.0 REQUIRED)
message(STATUS "OpenSSL: ${OPENSSL_VERSION} (${OPENSSL_INCLUDE_DIR})")

target_link_libraries(rproxy_deps INTERFACE
    Boost::beast
    Boost::asio
    OpenSSL::SSL
    OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(rproxy_deps INTERFACE ws2_32 mswsock)
endif()

if(MINGW)
    target_link_options(rproxy_deps INTERFACE -static)
endif()

add_executable(rproxy src/main.cpp src/Server.cpp src/RequestTools.cpp
                src/ErrorResponse.cpp src/LoadBalancer.cpp)
target_link_libraries(rproxy PRIVATE rproxy_deps)

include(FetchContent)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
