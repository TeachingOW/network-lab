#include "json.h"

#include <cstring>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

namespace {
// 1 for the bytes JSON strings must escape: control characters, '"' and '\'
struct EscTable {
    bool t[256] = {};
    constexpr EscTable() { for (int i = 0; i < 0x20; i++) t[i] = true; t['"'] = t['\\'] = true; }
};
constexpr EscTable kEsc;
constexpr char kHex[] = "0123456789abcdef";

// Index of the first byte in p[0..n) that needs escaping, or n.
inline size_t scan(const char* p, size_t n) {
    size_t i = 0;
#ifdef __SSE2__
    const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1F);
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, quote), _mm_cmpeq_epi8(x, bslash)),
                                 _mm_cmpeq_epi8(_mm_max_epu8(x, ctl), ctl));  // x <= 0x1F, unsigned
        if (unsigned bits = (unsigned)_mm_movemask_epi8(m)) return i + __builtin_ctz(bits);
    }
#endif
    while (i < n && !kEsc.t[(unsigned char)p[i]]) i++;
    return i;
}
}  // namespace

// Copies runs that need no escaping in one append each; finds them 16 bytes at a time.
void json_escape(std::string& o, std::string_view s) {
    const char* p = s.data();
    size_t n = s.size();
    for (;;) {
        size_t k = scan(p, n);
        o.append(p, k);
        if (k == n) return;
        unsigned char ch = (unsigned char)p[k];
        switch (ch) {
            case '"': o.append("\\\"", 2); break;
            case '\\': o.append("\\\\", 2); break;
            case '\n': o.append("\\n", 2); break;
            case '\r': o.append("\\r", 2); break;
            case '\t': o.append("\\t", 2); break;
            default: {
                char b[6] = {'\\', 'u', '0', '0', kHex[ch >> 4], kHex[ch & 15]};
                o.append(b, 6);
            }
        }
        p += k + 1;
        n -= k + 1;
    }
}

void json_unescape(std::string& o, std::string_view s) {
    const char* p = s.data();
    const char* e = p + s.size();
    while (p < e) {
        const char* b = (const char*)memchr(p, '\\', e - p);
        if (!b || b + 1 >= e) { o.append(p, e - p); return; }
        o.append(p, b - p);
        switch (b[1]) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            default: o += b[1];  // \" \\ \/ (other escapes pass through)
        }
        p = b + 2;
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
