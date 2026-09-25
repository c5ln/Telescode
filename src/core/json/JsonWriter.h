// src/core/json/JsonWriter.h
// Minimal streaming JSON writer.
//
// Exists so the core can emit JSON without taking on a dependency, and so the
// escaping and number formatting rules live in exactly one place. It tracks
// whether a separating comma is due and nothing else -- the caller is
// responsible for balancing begin/end pairs.

#pragma once

#include <string>

namespace TS {

class JsonWriter {
public:
    // `pretty` indents two spaces per level and puts one member per line.
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    // Emits a member name; must be followed by exactly one value or container.
    void key(const std::string& name);

    void value(const std::string& v);
    void value(const char* v);
    void value(double v);
    void value(int v);
    void value(bool v);
    void valueNull();

    // key + value in one call, for the common flat-member case.
    void member(const std::string& k, const std::string& v) { key(k); value(v); }
    void member(const std::string& k, const char* v)        { key(k); value(v); }
    void member(const std::string& k, double v)             { key(k); value(v); }
    void member(const std::string& k, int v)                { key(k); value(v); }
    void member(const std::string& k, bool v)               { key(k); value(v); }

    const std::string& str() const { return out_; }

    // Escapes one string into a JSON string literal, quotes included.
    static std::string EscapeString(const std::string& s);

    // Shortest representation that round-trips a double exactly. Non-finite
    // input has no JSON spelling, so it becomes null.
    static std::string NumberToString(double v);

private:
    void prefixValue();      // comma + newline/indent before a value or key
    void newlineIndent();

    std::string out_;
    bool        pretty_     = false;
    int         depth_      = 0;
    bool        need_comma_ = false;
    bool        after_key_  = false;
};

} // namespace TS
