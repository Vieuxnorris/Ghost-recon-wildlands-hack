#pragma once
#include <cstdint>

namespace grw {

struct Line {
    uint16_t len = 0;
    char text[1022];  // aim_tick reaches ~600 bytes
};

// Builds one JSON object + '\n' in a fixed buffer, without allocating. A field that does not fit is
// skipped and the line ends with "truncated":true, so it always stays valid JSON.
class JsonLine {
public:
    JsonLine(Line& out, const char* type, double t_ms, uint64_t frame);
    JsonLine& Num(const char* key, double v);
    JsonLine& Int(const char* key, int64_t v);
    JsonLine& Bool(const char* key, bool v);
    JsonLine& Str(const char* key, const char* v);
    void Finish();

private:
    bool Key(const char* key, char*& p);
    void Commit(char* p) { cur_ = p; }

    static constexpr int kReserve = 20;  // room for ,"truncated":true}\n
    Line& out_;
    char* cur_;
    char* limit_;
    bool truncated_ = false;
};

}  // namespace grw
