// tests/core/test_json_writer.cpp
// JsonWriter: escaping, number formatting, and structural output.

#include "core/json/JsonWriter.h"

#include <gtest/gtest.h>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
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

// ── Locale independence ──────────────────────────────────────────────────────
// JSON fixes '.' as the decimal separator. snprintf/strtod follow LC_NUMERIC, so
// an embedding caller that selects a comma locale used to make the writer emit
// "0,1" -- and the old round-trip guard could not catch it, because strtod read
// the comma back just as happily. These tests stand in for that hostile caller.
//
// Switching the process locale is what the *test* does to reproduce the
// condition; the fix itself never calls setlocale.

class CommaNumericLocale : public ::testing::Test {
protected:
    void SetUp() override {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        saved_ = current ? current : "C";
        for (const char* name : {"de-DE", "German", "French_France.1252",
                                 "de_DE.UTF-8", "fr_FR.UTF-8"}) {
            if (std::setlocale(LC_NUMERIC, name)) { active_ = true; break; }
        }
        if (!active_)
            GTEST_SKIP() << "no comma-decimal locale on this machine";
        // Guard against a locale that exists but is not actually comma-decimal.
        if (std::string(std::localeconv()->decimal_point) != ",") {
            std::setlocale(LC_NUMERIC, saved_.c_str());
            active_ = false;
            GTEST_SKIP() << "selected locale is not comma-decimal";
        }
    }
    void TearDown() override {
        if (active_) std::setlocale(LC_NUMERIC, saved_.c_str());
    }
private:
    std::string saved_;
    bool        active_ = false;
};

TEST_F(CommaNumericLocale, NumbersStillUseADot) {
    // Sanity check that the locale really is in force for this process.
    ASSERT_EQ(std::string(std::localeconv()->decimal_point), ",");

    EXPECT_EQ(JsonWriter::NumberToString(0.1), "0.1");
    EXPECT_EQ(JsonWriter::NumberToString(2.5), "2.5");
    EXPECT_EQ(JsonWriter::NumberToString(1.0 / 3.0), "0.3333333333333333");
    EXPECT_EQ(JsonWriter::NumberToString(0.30000000000000004), "0.30000000000000004");
    EXPECT_EQ(JsonWriter::NumberToString(-0.5), "-0.5");
    EXPECT_EQ(JsonWriter::NumberToString(1e-9), "1e-09");
}

TEST_F(CommaNumericLocale, NoCommaAppearsInsideANumber) {
    for (double v : {0.1, 2.5, -0.5, 1.0 / 3.0, 3.14159265358979,
                     0.19120571239616843}) {
        const std::string s = JsonWriter::NumberToString(v);
        EXPECT_EQ(s.find(','), std::string::npos) << "comma leaked into " << s;
        EXPECT_NE(s.find('.'), std::string::npos) << "no decimal point in " << s;
    }
}

TEST_F(CommaNumericLocale, WholeDocumentStaysValidJson) {
    // A stray comma would not merely look wrong -- it would split one number into
    // two array elements and change the document's shape.
    JsonWriter w;
    w.beginObject();
    w.member("pagerank", 0.19120571239616843);
    w.member("combined", 0.6);
    w.key("scores");
    w.beginArray();
    w.value(0.1); w.value(2.5); w.value(-0.5);
    w.endArray();
    w.endObject();
    EXPECT_EQ(w.str(),
              "{\"pagerank\":0.19120571239616843,\"combined\":0.6,"
              "\"scores\":[0.1,2.5,-0.5]}");
}

TEST_F(CommaNumericLocale, RoundTripStillExactUnderACommaLocale) {
    // Parsed back with the classic locale, the way a JSON consumer would.
    for (double v : {0.1, 1.0 / 3.0, 1e-9, 0.30000000000000004,
                     1.7976931348623157e308, 2.2250738585072014e-308}) {
        std::istringstream in(JsonWriter::NumberToString(v));
        in.imbue(std::locale::classic());
        double parsed = 0.0;
        in >> parsed;
        EXPECT_FALSE(in.fail());
        EXPECT_DOUBLE_EQ(parsed, v);
    }
}

// ── Exact representations ────────────────────────────────────────────────────
// Pins the chosen representation for a spread of awkward doubles. These are the
// values the pre-fix implementation produced, so this is what proves the locale
// fix changed no number's spelling.

TEST(JsonNumber, ExactRepresentationsAreStable) {
    struct Case { double v; const char* want; };
    const Case cases[] = {
        { 0.0,                      "0"                       },
        { 1.0,                      "1"                       },
        { -42.0,                    "-42"                     },
        { 0.1,                      "0.1"                     },
        { 1.0 / 3.0,                "0.3333333333333333"      },
        { 1e-9,                     "1e-09"                   },
        { 1e9,                      "1000000000"              },
        { 1e21,                     "1e+21"                   },
        { 1e-21,                    "1e-21"                   },
        { 1.7976931348623157e308,   "1.7976931348623157e+308" },
        { 2.2250738585072014e-308,  "2.2250738585072014e-308" },
        { 5e-324,                   "4.94065645841247e-324"   },
        { 0.30000000000000004,      "0.30000000000000004"     },
        { 123456789012345.0,        "123456789012345"         },
        { 1234567890123456.0,       "1234567890123456"        },
        { -0.5,                     "-0.5"                    },
        { 0.6,                      "0.6"                     },
        { 0.19120571239616843,      "0.19120571239616843"     },
        { 3.14159265358979,         "3.14159265358979"        },
    };
    for (const Case& c : cases)
        EXPECT_EQ(JsonWriter::NumberToString(c.v), c.want)
            << "for %.17g = " << c.v;
}

// ── Subnormals ───────────────────────────────────────────────────────────────
// num_get signals underflow with failbit, and some implementations (libc++) do
// that for a subnormal result even though the conversion was exact. If the
// round-trip check treated that as a failure, every subnormal would fall through
// to the 17-digit fallback on those toolchains and not on others -- the same
// double would serialize differently per platform.

TEST(JsonNumber, SmallestSubnormalKeepsItsShortForm) {
    // 15 significant digits already round-trip this value, so the ladder must
    // stop there rather than reaching the 17-digit spelling.
    EXPECT_EQ(JsonWriter::NumberToString(5e-324), "4.94065645841247e-324");
    EXPECT_EQ(JsonWriter::NumberToString(std::numeric_limits<double>::denorm_min()),
              "4.94065645841247e-324");
}

TEST(JsonNumber, SubnormalsRoundTripExactly) {
    const double vals[] = {
        5e-324,                                            // denorm_min
        std::numeric_limits<double>::denorm_min() * 3.0,
        2.2250738585072009e-308,                           // largest subnormal
        1e-320,
    };
    for (double v : vals) {
        ASSERT_EQ(std::fpclassify(v), FP_SUBNORMAL) << "fixture " << v << " is not subnormal";
        const std::string s = JsonWriter::NumberToString(v);
        EXPECT_NE(s, "null") << "subnormal serialized as null";
        std::istringstream in(s);
        in.imbue(std::locale::classic());
        double parsed = 0.0;
        in >> parsed;
        EXPECT_EQ(parsed, v) << "for " << s;
    }
}

TEST(JsonNumber, SubnormalsAreStillFiniteNotNull) {
    // fpclassify-based acceptance must not be mistaken for a non-finite check.
    EXPECT_NE(JsonWriter::NumberToString(std::numeric_limits<double>::denorm_min()), "null");
    EXPECT_EQ(JsonWriter::NumberToString(std::numeric_limits<double>::quiet_NaN()), "null");
}
