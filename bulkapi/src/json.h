// Minimal JSON / NDJSON helpers for the item payloads. Not a full JSON parser.
#pragma once

#include <charconv>
#include <string>
#include <string_view>

#include "types.h"

inline void append_u64(std::string& o, u64 v) {
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v);
    o.append(b, r.ptr - b);
}

inline bool parse_u64(std::string_view s, u64& out) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

void json_escape(std::string& o, std::string_view s);
void json_unescape(std::string& o, std::string_view s);

// {"id":123,"v":"text"}  (no nested objects; v is a JSON string).
// `esc` is set when v contains backslash escapes and needs json_unescape.
bool parse_item(std::string_view line, u64& id, std::string_view& val, bool& esc);

// Calls f(line) for every non-empty line (LF or CRLF terminated).
template <class F>
void for_each_line(std::string_view body, F f) {
    while (!body.empty()) {
        size_t nl = body.find('\n');
        std::string_view line = body.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) f(line);
        if (nl == std::string_view::npos) break;
        body.remove_prefix(nl + 1);
    }
}

// Calls f(id) for every run of digits; ids are separated by any non-digit characters.
template <class F>
void for_each_id(std::string_view s, F f) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] < '0' || s[i] > '9')) i++;
        size_t b = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        u64 id;
        if (i > b && parse_u64(s.substr(b, i - b), id)) f(id);
    }
}
