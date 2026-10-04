#include "../include/rproxy/ErrorResponse.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

std::size_t unescaped_quote_count(const std::string& text) {
    std::size_t quotes = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char c : text) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            ++quotes;
        }
    }

    return quotes;
}

}  // namespace

TEST(ErrorResponse, EveryFactorySetsTheDocumentedStatusCode)
{
    EXPECT_EQ(ErrorResponse::make_error_400(11).result_int(), 400);
    EXPECT_EQ(ErrorResponse::make_error_400_framing_conflict(11).result_int(), 400);
    EXPECT_EQ(ErrorResponse::make_error_413(1024, 11).result_int(), 413);
    EXPECT_EQ(ErrorResponse::make_error_431("X-Long", 11).result_int(), 431);
    EXPECT_EQ(ErrorResponse::make_error_500(11).result_int(), 500);
    EXPECT_EQ(ErrorResponse::make_error_502(11).result_int(), 502);
    EXPECT_EQ(ErrorResponse::make_error_503(11).result_int(), 503);
    EXPECT_EQ(ErrorResponse::make_error_504(11).result_int(), 504);
}

TEST(ErrorResponse, FramingConflictStillReportsBadRequest)
{
    const auto plain = ErrorResponse::make_error_400(11);
    const auto framing = ErrorResponse::make_error_400_framing_conflict(11);

    EXPECT_EQ(plain.result_int(), framing.result_int());
    EXPECT_NE(plain.body(), framing.body());
}

TEST(ErrorResponse, BodyIsWellFormedJsonForEveryFactory)
{
    const auto bodies = {
        ErrorResponse::make_error_400(11).body(),
        ErrorResponse::make_error_400_framing_conflict(11).body(),
        ErrorResponse::make_error_413(4096, 11).body(),
        ErrorResponse::make_error_431("X-Long", 11).body(),
        ErrorResponse::make_error_500(11).body(),
        ErrorResponse::make_error_502(11).body(),
        ErrorResponse::make_error_503(11).body(),
        ErrorResponse::make_error_504(11).body(),
    };

    for (const auto& body : bodies) {
        EXPECT_EQ(body.front(), '{') << body;
        EXPECT_EQ(body.back(), '}') << body;

        std::size_t braces = 0;
        bool in_string = false;
        bool escaped = false;

        for (const char c : body) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\' && in_string) {
                escaped = true;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
            } else if (!in_string && c == '{') {
                ++braces;
            } else if (!in_string && c == '}') {
                --braces;
            }
        }

        EXPECT_FALSE(in_string) << "unterminated string: " << body;
        EXPECT_EQ(braces, 0u) << "unbalanced braces: " << body;
    }
}

TEST(ErrorResponse, PreservesTheClientHttpVersion)
{
    EXPECT_EQ(ErrorResponse::make_error_400(10).version(), 10u);
    EXPECT_EQ(ErrorResponse::make_error_400(11).version(), 11u);
    EXPECT_EQ(ErrorResponse::make_error_502(10).version(), 10u);
}

TEST(ErrorResponse, HeadersAreWellFormed)
{
    const auto resp = ErrorResponse::make_error_502(11);

    EXPECT_EQ(resp.base().at("content-type"), "application/json");
    EXPECT_FALSE(resp.keep_alive());

    const std::string declared = resp.base()["Content-Length"];
    ASSERT_FALSE(declared.empty());
    EXPECT_EQ(declared, std::to_string(resp.body().size()));
}

TEST(ErrorResponse, ErrorSlugsAreDistinctPerCondition)
{
    EXPECT_NE(ErrorResponse::make_error_500(11).body().find("\"internal_server_error\""),
              std::string::npos);
    EXPECT_NE(ErrorResponse::make_error_502(11).body().find("\"bad_gateway\""), std::string::npos);
    EXPECT_NE(ErrorResponse::make_error_503(11).body().find("\"service_unavailable\""),
              std::string::npos);

    EXPECT_NE(ErrorResponse::make_error_500(11).body(), ErrorResponse::make_error_502(11).body());
    EXPECT_NE(ErrorResponse::make_error_502(11).body(), ErrorResponse::make_error_503(11).body());
    EXPECT_NE(ErrorResponse::make_error_503(11).body(), ErrorResponse::make_error_504(11).body());
}

TEST(ErrorResponse, HostileHeaderNameCannotBreakOutOfTheJsonString)
{
    const auto resp = ErrorResponse::make_error_431("X-Bad\"name", 11);

    EXPECT_NE(resp.body().find("X-Bad\\\""), std::string::npos) << resp.body();
    EXPECT_EQ(unescaped_quote_count(resp.body()), 8u)
        << "four quotes per key/value pair across two pairs: " << resp.body();
}

TEST(ErrorResponse, BackslashAndControlCharactersAreEscaped)
{
    const std::string control{"X\tTab", 5};

    EXPECT_NE(ErrorResponse::make_error_431("X\\Bad", 11).body().find("\\\\"),
              std::string::npos);
    EXPECT_NE(ErrorResponse::make_error_431(control, 11).body().find("\\t"), std::string::npos);
}