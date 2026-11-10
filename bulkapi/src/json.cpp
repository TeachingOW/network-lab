#include "json.h"

#include <cstdio>

void json_escape(std::string& o, std::string_view s) {
    size_t run = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 0x20 && ch != '"' && ch != '\\') continue;
        o.append(s.data() + run, i - run);
        run = i + 1;
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", ch);
                o += b;
            }
        }
    }
    o.append(s.data() + run, s.size() - run);
}

void json_unescape(std::string& o, std::string_view s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        switch (n) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            default: o += n;  // \" \\ \/ (other escapes pass through)
        }
    }
}

bool parse_item(std::string_view line, u64& id, std::string_view& val, bool& esc) {
    size_t p = line.find("\"id\"");
    if (p == std::string_view::npos) return false;
    p += 4;
    while (p < line.size() && (line[p] == ' ' || line[p] == ':')) p++;
    size_t e = p;
    while (e < line.size() && line[e] >= '0' && line[e] <= '9') e++;
    if (!parse_u64(line.substr(p, e - p), id)) return false;
    size_t q = line.find("\"v\"", e);
    if (q == std::string_view::npos) q = line.find("\"v\"");
    if (q == std::string_view::npos) return false;
    q += 3;
    while (q < line.size() && (line[q] == ' ' || line[q] == ':')) q++;
    if (q >= line.size() || line[q] != '"') return false;
    size_t b = ++q;
    esc = false;
    while (q < line.size() && line[q] != '"') {
        if (line[q] == '\\') { esc = true; q++; }
        q++;
    }
    if (q >= line.size()) return false;
    val = line.substr(b, q - b);
    return true;
}
