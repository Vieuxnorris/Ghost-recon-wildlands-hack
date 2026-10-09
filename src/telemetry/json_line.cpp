#include "telemetry/json_line.h"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace grw {

namespace {
bool PutRaw(char*& p, char* limit, const char* s, size_t n) {
    if (p + n > limit) return false;
    std::memcpy(p, s, n);
    p += n;
    return true;
}
bool PutStr(char*& p, char* limit, const char* s) { return PutRaw(p, limit, s, std::strlen(s)); }

bool PutEscaped(char*& p, char* limit, const char* s) {
    if (!PutRaw(p, limit, "\"", 1)) return false;
    for (; *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        char esc[7];
        size_t n = 0;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = char(c); n = 2; }
        else if (c < 0x20) { n = size_t(std::snprintf(esc, sizeof esc, "\\u%04x", c)); }
        else { esc[0] = char(c); n = 1; }
        if (!PutRaw(p, limit, esc, n)) return false;
    }
    return PutRaw(p, limit, "\"", 1);
}

bool PutNum(char*& p, char* limit, double v) {
    if (!std::isfinite(v)) return PutStr(p, limit, "null");
    const auto r = std::to_chars(p, limit, v, std::chars_format::general, 7);
    if (r.ec != std::errc{}) return false;
    p = r.ptr;
    return true;
}

bool PutInt(char*& p, char* limit, int64_t v) {
    const auto r = std::to_chars(p, limit, v);
    if (r.ec != std::errc{}) return false;
    p = r.ptr;
    return true;
}
}  // namespace

JsonLine::JsonLine(Line& out, const char* type, double t_ms, uint64_t frame)
    : out_(out), cur_(out.text), limit_(out.text + sizeof(out.text) - kReserve) {
    char* p = cur_;
    const bool ok = PutStr(p, limit_, "{\"type\":") && PutEscaped(p, limit_, type) &&
                    PutStr(p, limit_, ",\"t\":") && PutNum(p, limit_, t_ms) &&
                    PutStr(p, limit_, ",\"frame\":") && PutInt(p, limit_, int64_t(frame));
    Commit(ok ? p : cur_);
    truncated_ = !ok;
}

bool JsonLine::Key(const char* key, char*& p) {
    p = cur_;
    return PutStr(p, limit_, ",") && PutEscaped(p, limit_, key) && PutStr(p, limit_, ":");
}

JsonLine& JsonLine::Num(const char* key, double v) {
    char* p;
    if (Key(key, p) && PutNum(p, limit_, v)) Commit(p);
    else truncated_ = true;
    return *this;
}

JsonLine& JsonLine::Int(const char* key, int64_t v) {
    char* p;
    if (Key(key, p) && PutInt(p, limit_, v)) Commit(p);
    else truncated_ = true;
    return *this;
}

JsonLine& JsonLine::Bool(const char* key, bool v) {
    char* p;
    if (Key(key, p) && PutStr(p, limit_, v ? "true" : "false")) Commit(p);
    else truncated_ = true;
    return *this;
}

JsonLine& JsonLine::Str(const char* key, const char* v) {
    char* p;
    if (Key(key, p) && PutEscaped(p, limit_, v)) Commit(p);
    else truncated_ = true;
    return *this;
}

void JsonLine::Finish() {
    char* end = out_.text + sizeof(out_.text);
    char* p = cur_;
    if (truncated_) PutStr(p, end, ",\"truncated\":true");
    PutStr(p, end, "}\n");
    out_.len = uint16_t(p - out_.text);
}

}  // namespace grw
