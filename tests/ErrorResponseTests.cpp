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
    EXPECT_EQ(ErrorResponse::make_error_431(11).result_int(), 431);
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
        ErrorResponse::make_error_431(11).body(),
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

TEST(ErrorResponse, QuotesAndBackslashesAreEscapedSoTextCannotBreakOutOfTheString)
{
    EXPECT_EQ(ErrorResponse::detail::json_escape("X-Bad\"name"), "X-Bad\\\"name");
    EXPECT_EQ(ErrorResponse::detail::json_escape("X\\Bad"), "X\\\\Bad");

    // A backslash immediately before a quote is the case that breaks naive escapers:
    // the backslash must be doubled first or the quote escapes it and terminates the
    // string.
    EXPECT_EQ(ErrorResponse::detail::json_escape("a\\\"b"), "a\\\\\\\"b");
}

TEST(ErrorResponse, ControlCharactersAreEscapedRatherThanPassedThrough)
{
    EXPECT_EQ(ErrorResponse::detail::json_escape("X\tTab"), "X\\tTab");
    EXPECT_EQ(ErrorResponse::detail::json_escape("X\nNew"), "X\\nNew");
    EXPECT_EQ(ErrorResponse::detail::json_escape("X\rReturn"), "X\\rReturn");

    // Remaining C0 controls have no short escape, so they become \uXXXX.
    EXPECT_EQ(ErrorResponse::detail::json_escape(std::string_view("X\x01", 2)), "X\\u0001");

    // Printable ASCII and UTF-8 pass through untouched, so escaping does not mangle
    // ordinary text.
    EXPECT_EQ(ErrorResponse::detail::json_escape("plain-text_123"), "plain-text_123");
}

TEST(ErrorResponse, EveryFactoryBodyIsSingleLineSoItStaysValidJson)
{
    for (const std::string& body : {
             ErrorResponse::make_error_400(11).body(),
             ErrorResponse::make_error_400_framing_conflict(11).body(),
             ErrorResponse::make_error_413(4096, 11).body(),
             ErrorResponse::make_error_431(11).body(),
             ErrorResponse::make_error_500(11).body(),
             ErrorResponse::make_error_502(11).body(),
             ErrorResponse::make_error_503(11).body(),
             ErrorResponse::make_error_504(11).body(),
         }) {
        EXPECT_EQ(body.find('\n'), std::string::npos) << body;
        EXPECT_EQ(body.find('\r'), std::string::npos) << body;
        EXPECT_EQ(unescaped_quote_count(body), 8u)
            << "four quotes per key/value pair across two pairs: " << body;
    }
}
