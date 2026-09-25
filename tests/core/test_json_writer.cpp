// tests/core/test_json_writer.cpp
// JsonWriter: escaping, number formatting, and structural output.

#include "core/json/JsonWriter.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <string>

using TS::JsonWriter;

// ── EscapeString ─────────────────────────────────────────────────────────────

TEST(JsonEscape, WrapsPlainTextInQuotes) {
    EXPECT_EQ(JsonWriter::EscapeString("abc"), "\"abc\"");
}

TEST(JsonEscape, EmptyStringIsTwoQuotes) {
    EXPECT_EQ(JsonWriter::EscapeString(""), "\"\"");
}

TEST(JsonEscape, EscapesQuoteAsBackslashQuote) {
    // Input:  a"b        Output: "a\"b"
    const std::string out = JsonWriter::EscapeString("a\"b");
    EXPECT_EQ(out, std::string("\"a") + "\\\"" + "b\"");
    EXPECT_EQ(out.size(), 6u);   // " a \ " b "
}

TEST(JsonEscape, EscapesBackslashAsTwoBackslashes) {
    // Input: one backslash -> output must be two.
    const std::string out = JsonWriter::EscapeString("\\");
    EXPECT_EQ(out, "\"\\\\\"");
    EXPECT_EQ(out.size(), 4u);
}

TEST(JsonEscape, ControlCharactersBecomeTwoCharEscapes) {
    // The escape must be the two characters backslash + 'n', never a real newline.
    const std::string out = JsonWriter::EscapeString("a\nb");
    EXPECT_EQ(out, "\"a\\nb\"");
    EXPECT_EQ(out.find('\n'), std::string::npos);

    EXPECT_EQ(JsonWriter::EscapeString("\t"), "\"\\t\"");
    EXPECT_EQ(JsonWriter::EscapeString("\r"), "\"\\r\"");
    EXPECT_EQ(JsonWriter::EscapeString("\b"), "\"\\b\"");
    EXPECT_EQ(JsonWriter::EscapeString("\f"), "\"\\f\"");
}

TEST(JsonEscape, OtherControlCharactersUseNumericForm) {
    EXPECT_EQ(JsonWriter::EscapeString(std::string(1, '\x01')), "\"\\u0001\"");
    EXPECT_EQ(JsonWriter::EscapeString(std::string(1, '\x1f')), "\"\\u001f\"");
}

TEST(JsonEscape, Utf8PassesThroughUnchanged) {
    // Korean identifiers occur in this codebase's own comments and in scanned
    // repos; re-encoding them would corrupt text that is already valid UTF-8.
    const std::string in = "\xED\x95\x9C";   // U+D55C
    EXPECT_EQ(JsonWriter::EscapeString(in), "\"" + in + "\"");
}

TEST(JsonEscape, DoesNotEscapeForwardSlash) {
    // Legal either way in JSON; file_ids are full of them, so leaving them bare
    // keeps the output readable.
    EXPECT_EQ(JsonWriter::EscapeString("src/ui/ts_app.cpp"), "\"src/ui/ts_app.cpp\"");
}

// ── NumberToString ───────────────────────────────────────────────────────────

TEST(JsonNumber, IntegralDoublesStayShort) {
    EXPECT_EQ(JsonWriter::NumberToString(0.0), "0");
    EXPECT_EQ(JsonWriter::NumberToString(1.0), "1");
    EXPECT_EQ(JsonWriter::NumberToString(-42.0), "-42");
}

TEST(JsonNumber, RoundTripsExactly) {
    const double vals[] = { 0.1, 1.0 / 3.0, 1e-9, 1.7976931348623157e308,
                            0.30000000000000004, 2.2250738585072014e-308 };
    for (double v : vals) {
        const std::string s = JsonWriter::NumberToString(v);
        EXPECT_DOUBLE_EQ(std::strtod(s.c_str(), nullptr), v) << "for " << s;
    }
}

TEST(JsonNumber, PrefersTheShortestFormThatRoundTrips) {
    EXPECT_EQ(JsonWriter::NumberToString(0.1), "0.1");
}

TEST(JsonNumber, NonFiniteBecomesNull) {
    // NaN/inf have no JSON spelling; emitting them would break the consumer's parse.
    EXPECT_EQ(JsonWriter::NumberToString(std::numeric_limits<double>::quiet_NaN()), "null");
    EXPECT_EQ(JsonWriter::NumberToString(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(JsonWriter::NumberToString(-std::numeric_limits<double>::infinity()), "null");
}

// ── Structure ────────────────────────────────────────────────────────────────

TEST(JsonWriterStructure, EmptyContainers) {
    { JsonWriter w; w.beginObject(); w.endObject(); EXPECT_EQ(w.str(), "{}"); }
    { JsonWriter w; w.beginArray();  w.endArray();  EXPECT_EQ(w.str(), "[]"); }
}

TEST(JsonWriterStructure, FlatObjectCommasAndTypes) {
    JsonWriter w;
    w.beginObject();
    w.member("s", std::string("v"));
    w.member("i", 7);
    w.member("d", 0.5);
    w.member("b", true);
    w.key("n"); w.valueNull();
    w.endObject();
    EXPECT_EQ(w.str(), "{\"s\":\"v\",\"i\":7,\"d\":0.5,\"b\":true,\"n\":null}");
}

TEST(JsonWriterStructure, NestedContainersSeparateCorrectly) {
    JsonWriter w;
    w.beginObject();
    w.key("items");
    w.beginArray();
    w.beginObject(); w.member("a", 1); w.endObject();
    w.beginObject(); w.member("a", 2); w.endObject();
    w.endArray();
    w.member("after", 3);
    w.endObject();
    EXPECT_EQ(w.str(), "{\"items\":[{\"a\":1},{\"a\":2}],\"after\":3}");
}

TEST(JsonWriterStructure, ArrayOfScalars) {
    JsonWriter w;
    w.beginArray();
    w.value(1); w.value(2); w.value(3);
    w.endArray();
    EXPECT_EQ(w.str(), "[1,2,3]");
}

TEST(JsonWriterStructure, NullCharPointerBecomesNull) {
    JsonWriter w;
    w.beginObject();
    w.key("k"); w.value(static_cast<const char*>(nullptr));
    w.endObject();
    EXPECT_EQ(w.str(), "{\"k\":null}");
}

TEST(JsonWriterStructure, KeysAreEscapedToo) {
    JsonWriter w;
    w.beginObject();
    w.member(std::string("a\"b"), 1);
    w.endObject();
    EXPECT_EQ(w.str(), "{\"a\\\"b\":1}");
}

TEST(JsonWriterStructure, PrettyOutputIndentsAndStaysParseable) {
    JsonWriter w(true);
    w.beginObject();
    w.member("a", 1);
    w.key("b");
    w.beginArray();
    w.value(2);
    w.endArray();
    w.endObject();
    EXPECT_EQ(w.str(),
              "{\n"
              "  \"a\": 1,\n"
              "  \"b\": [\n"
              "    2\n"
              "  ]\n"
              "}");
}
