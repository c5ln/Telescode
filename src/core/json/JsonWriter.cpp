// src/core/json/JsonWriter.cpp

#include "core/json/JsonWriter.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace TS {

std::string JsonWriter::EscapeString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\"";  break;
            case '\\': out += "\\\\";  break;
            case '\b': out += "\\b";   break;
            case '\f': out += "\\f";   break;
            case '\n': out += "\\n";   break;
            case '\r': out += "\\r";   break;
            case '\t': out += "\\t";   break;
            default:
                if (c < 0x20) {
                    // The remaining characters JSON requires be escaped, and the
                    // only ones that need the numeric form.
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    // Bytes >= 0x80 are passed through: source text is UTF-8 and
                    // JSON is defined over UTF-8, so re-encoding would only
                    // corrupt identifiers that already read correctly.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string JsonWriter::NumberToString(double v)
{
    // NaN and infinity have no JSON spelling. Emitting them anyway is what turns
    // a data file into a parse error at the consumer, so they become null and the
    // caller sees a missing number instead of a broken document.
    if (!std::isfinite(v)) return "null";

    // %.17g always round-trips a double; trying shorter forms first keeps the
    // common case readable instead of 0.10000000000000001.
    char buf[40];
    for (int prec : {15, 16, 17}) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

void JsonWriter::newlineIndent()
{
    if (!pretty_) return;
    // Nothing written yet means this is the opening brace of the document, which
    // must not be preceded by a blank first line.
    if (!out_.empty()) out_.push_back('\n');
    out_.append(static_cast<size_t>(depth_) * 2, ' ');
}

void JsonWriter::prefixValue()
{
    // A value that follows a key sits on the same line, right after the colon.
    if (after_key_) { after_key_ = false; return; }
    if (need_comma_) out_.push_back(',');
    newlineIndent();
}

void JsonWriter::beginObject()
{
    prefixValue();
    out_.push_back('{');
    ++depth_;
    need_comma_ = false;
}

void JsonWriter::endObject()
{
    --depth_;
    if (need_comma_) newlineIndent();   // an empty object stays as {}
    out_.push_back('}');
    need_comma_ = true;
}

void JsonWriter::beginArray()
{
    prefixValue();
    out_.push_back('[');
    ++depth_;
    need_comma_ = false;
}

void JsonWriter::endArray()
{
    --depth_;
    if (need_comma_) newlineIndent();   // an empty array stays as []
    out_.push_back(']');
    need_comma_ = true;
}

void JsonWriter::key(const std::string& name)
{
    if (need_comma_) out_.push_back(',');
    newlineIndent();
    out_ += EscapeString(name);
    out_.push_back(':');
    if (pretty_) out_.push_back(' ');
    need_comma_ = false;
    after_key_  = true;
}

void JsonWriter::value(const std::string& v)
{
    prefixValue();
    out_ += EscapeString(v);
    need_comma_ = true;
}

void JsonWriter::value(const char* v)
{
    if (!v) { valueNull(); return; }
    value(std::string(v));
}

void JsonWriter::value(double v)
{
    prefixValue();
    out_ += NumberToString(v);
    need_comma_ = true;
}

void JsonWriter::value(int v)
{
    prefixValue();
    out_ += std::to_string(v);
    need_comma_ = true;
}

void JsonWriter::value(bool v)
{
    prefixValue();
    out_ += v ? "true" : "false";
    need_comma_ = true;
}

void JsonWriter::valueNull()
{
    prefixValue();
    out_ += "null";
    need_comma_ = true;
}

} // namespace TS
