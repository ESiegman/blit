#include "json.hpp"

#include <charconv>  // for whole numbers only: the floating-point versions add 150 KB of tables
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "utf8.hpp"

namespace blit::json {

// ---------------------------------------------------------------- reading

struct Doc::Parser {
    Doc& d;
    const char* p;
    const char* end;
    int depth = 0;

    static constexpr int kMaxDepth = 200;

    void space() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
    }

    u32 add(Type t) {
        d.nodes_.push_back({});
        d.nodes_.back().type = t;
        return static_cast<u32>(d.nodes_.size() - 1);
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(u32* out) {
        if (end - p < 4) return false;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex(p[i]);
            if (h < 0) return false;
            v = v * 16 + static_cast<u32>(h);
        }
        p += 4;
        *out = v;
        return true;
    }

    void put_utf8(u32 cp) {
        std::string& a = d.arena_;
        if (cp < 0x80) {
            a += static_cast<char>(cp);
        } else if (cp < 0x800) {
            a += static_cast<char>(0xC0 | (cp >> 6));
            a += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            a += static_cast<char>(0xE0 | (cp >> 12));
            a += static_cast<char>(0x80 | ((cp >> 6) & 63));
            a += static_cast<char>(0x80 | (cp & 63));
        } else {
            a += static_cast<char>(0xF0 | (cp >> 18));
            a += static_cast<char>(0x80 | ((cp >> 12) & 63));
            a += static_cast<char>(0x80 | ((cp >> 6) & 63));
            a += static_cast<char>(0x80 | (cp & 63));
        }
    }

    // At an opening quote. Most strings have no escapes and are used where
    // they stand; the others are unescaped into the arena.
    bool string(u32* at, u32* len, bool* owned) {
        ++p;
        const char* start = p;
        while (p < end && *p != '"' && *p != '\\') ++p;
        if (p >= end) return false;
        if (*p == '"') {
            *at = static_cast<u32>(start - d.text_.data());
            *len = static_cast<u32>(p - start);
            *owned = false;
            ++p;
            return true;
        }
        const size_t begin = d.arena_.size();
        d.arena_.append(start, static_cast<size_t>(p - start));
        while (p < end && *p != '"') {
            if (*p != '\\') {
                const char* run = p;
                while (p < end && *p != '"' && *p != '\\') ++p;
                d.arena_.append(run, static_cast<size_t>(p - run));
                continue;
            }
            if (++p >= end) return false;
            const char e = *p++;
            switch (e) {
                case 'n': d.arena_ += '\n'; break;
                case 't': d.arena_ += '\t'; break;
                case 'r': d.arena_ += '\r'; break;
                case 'b': d.arena_ += '\b'; break;
                case 'f': d.arena_ += '\f'; break;
                case 'u': {
                    u32 cp;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char* save = p;
                        p += 2;
                        u32 lo;
                        if (hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else p = save;
                    }
                    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // half of a pair on its own
                    put_utf8(cp);
                    break;
                }
                default: d.arena_ += e; break;  // \" \\ \/
            }
        }
        if (p >= end) return false;
        ++p;
        *at = static_cast<u32>(begin);
        *len = static_cast<u32>(d.arena_.size() - begin);
        *owned = true;
        return true;
    }

    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, word, n) != 0) return false;
        p += n;
        return true;
    }

    // Parses one value into a new node; its index goes to *out.
    bool value(u32* out) {
        space();
        if (p >= end) return false;
        switch (*p) {
            case '{': {
                if (++depth > kMaxDepth) return false;
                const u32 me = add(Type::Object);
                ++p;
                u32 last = 0, count = 0;
                space();
                if (p < end && *p == '}') {
                    ++p;
                } else {
                    for (;;) {
                        space();
                        if (p >= end || *p != '"') return false;
                        u32 kat, klen;
                        bool kowned;
                        if (!string(&kat, &klen, &kowned)) return false;
                        space();
                        if (p >= end || *p != ':') return false;
                        ++p;
                        u32 child;
                        if (!value(&child)) return false;
                        Node& c = d.nodes_[child];
                        c.key_at = kat;
                        c.key_len = klen;
                        c.key_owned = kowned;
                        if (last) d.nodes_[last].next = child;
                        else d.nodes_[me].child = child;
                        last = child;
                        ++count;
                        space();
                        if (p < end && *p == ',') {
                            ++p;
                            continue;
                        }
                        if (p < end && *p == '}') {
                            ++p;
                            break;
                        }
                        return false;
                    }
                }
                d.nodes_[me].count = count;
                --depth;
                *out = me;
                return true;
            }
            case '[': {
                if (++depth > kMaxDepth) return false;
                const u32 me = add(Type::Array);
                ++p;
                u32 last = 0, count = 0;
                space();
                if (p < end && *p == ']') {
                    ++p;
                } else {
                    for (;;) {
                        u32 child;
                        if (!value(&child)) return false;
                        if (last) d.nodes_[last].next = child;
                        else d.nodes_[me].child = child;
                        last = child;
                        ++count;
                        space();
                        if (p < end && *p == ',') {
                            ++p;
                            continue;
                        }
                        if (p < end && *p == ']') {
                            ++p;
                            break;
                        }
                        return false;
                    }
                }
                d.nodes_[me].count = count;
                --depth;
                *out = me;
                return true;
            }
            case '"': {
                const u32 me = add(Type::String);
                u32 at, len;
                bool owned;
                if (!string(&at, &len, &owned)) return false;
                Node& n = d.nodes_[me];
                n.str_at = at;
                n.str_len = len;
                n.str_owned = owned;
                *out = me;
                return true;
            }
            case 't':
            case 'f': {
                const bool v = *p == 't';
                if (!literal(v ? "true" : "false")) return false;
                const u32 me = add(Type::Bool);
                d.nodes_[me].num = v ? 1 : 0;
                *out = me;
                return true;
            }
            case 'n':
                if (!literal("null")) return false;
                *out = add(Type::Null);
                return true;
            default: {
                // Nearly every number in a message is a small whole one
                // (a line, a column, an id): those are read by hand. The
                // rest go to strtod; the text is NUL-terminated, being a
                // std::string, so it cannot run off the end.
                const char* q = p;
                const bool neg = q < end && *q == '-';
                if (neg) ++q;
                u64 whole = 0;
                int digits = 0;
                while (q < end && *q >= '0' && *q <= '9' && digits < 18) whole = whole * 10 + static_cast<u64>(*q++ - '0'), ++digits;
                double v;
                if (digits > 0 && (q == end || (*q != '.' && *q != 'e' && *q != 'E' && !(*q >= '0' && *q <= '9')))) {
                    v = neg ? -static_cast<double>(whole) : static_cast<double>(whole);
                    p = q;
                } else {
                    if (p >= end || !((*p >= '0' && *p <= '9') || *p == '-')) return false;
                    char* stop = nullptr;
                    v = std::strtod(p, &stop);
                    if (stop == p || stop > end) return false;
                    p = stop;
                }
                const u32 me = add(Type::Number);
                d.nodes_[me].num = v;
                *out = me;
                return true;
            }
        }
    }
};

bool Doc::parse(std::string text) {
    text_ = std::move(text);
    arena_.clear();
    nodes_.clear();
    nodes_.reserve(text_.size() / 12 + 8);
    nodes_.push_back({});
    Parser ps{*this, text_.data(), text_.data() + text_.size()};
    u32 root;
    bool ok = ps.value(&root);
    if (ok) {
        ps.space();
        ok = ps.p == ps.end && root == 1;
    }
    if (!ok) nodes_.clear();
    return ok;
}

Type Value::type() const { return doc_ ? doc_->nodes_[node_].type : Type::None; }

bool Value::boolean(bool otherwise) const {
    return type() == Type::Bool ? doc_->nodes_[node_].num != 0 : otherwise;
}

double Value::number(double otherwise) const {
    return type() == Type::Number ? doc_->nodes_[node_].num : otherwise;
}

i64 Value::integer(i64 otherwise) const {
    if (type() != Type::Number) return otherwise;
    const double v = doc_->nodes_[node_].num;
    if (!(v > -9.2e18 && v < 9.2e18)) return otherwise;
    return static_cast<i64>(v);
}

std::string_view Value::str() const {
    return type() == Type::String ? doc_->str_of(doc_->nodes_[node_]) : std::string_view();
}

std::string_view Value::key() const { return doc_ ? doc_->key_of(doc_->nodes_[node_]) : std::string_view(); }

Value Value::operator[](std::string_view k) const {
    if (type() != Type::Object) return Value();
    for (u32 c = doc_->nodes_[node_].child; c; c = doc_->nodes_[c].next)
        if (doc_->key_of(doc_->nodes_[c]) == k) return Value(doc_, c);
    return Value();
}

size_t Value::size() const {
    const Type t = type();
    return t == Type::Array || t == Type::Object ? doc_->nodes_[node_].count : 0;
}

Value Value::at(size_t i) const {
    if (type() != Type::Array) return Value();
    u32 c = doc_->nodes_[node_].child;
    while (c && i--) c = doc_->nodes_[c].next;
    return c ? Value(doc_, c) : Value();
}

Value::Iter Value::begin() const {
    Iter it;
    const Type t = type();
    if (t == Type::Array || t == Type::Object) {
        it.doc_ = doc_;
        it.at_ = doc_->nodes_[node_].child;
    }
    return it;
}

Value Value::Iter::operator*() const { return Value(doc_, at_); }

Value::Iter& Value::Iter::operator++() {
    at_ = doc_->nodes_[at_].next;
    return *this;
}

// ---------------------------------------------------------------- writing

void quote(std::string& out, std::string_view s) {
    out += '"';
    const u8* p = reinterpret_cast<const u8*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        // Plain ASCII goes out in runs.
        size_t run = i;
        while (run < n && p[run] >= 0x20 && p[run] < 0x80 && p[run] != '"' && p[run] != '\\') ++run;
        if (run > i) {
            out.append(s.data() + i, run - i);
            i = run;
            if (i >= n) break;
        }
        const u8 c = p[i];
        if (c >= 0x80) {
            const Decoded d = decode_utf8(p + i, n - i);
            if (d.cp == 0xFFFD && d.len == 1) out += "\xEF\xBF\xBD";
            else out.append(s.data() + i, d.len);
            i += d.len;
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
        }
        ++i;
    }
    out += '"';
}

void dump(std::string& out, Value v) {
    switch (v.type()) {
        case Type::None:
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean() ? "true" : "false"; break;
        case Type::Number: {
            const double n = v.number();
            char buf[32];
            if (n == std::floor(n) && std::fabs(n) < 9e15) {
                const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<i64>(n));
                out.append(buf, static_cast<size_t>(r.ptr - buf));
            } else if (std::isfinite(n)) {
                out.append(buf, static_cast<size_t>(std::snprintf(buf, sizeof buf, "%.17g", n)));
            } else {
                out += "null";
            }
            break;
        }
        case Type::String: quote(out, v.str()); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (Value e : v) {
                if (!first) out += ',';
                first = false;
                dump(out, e);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (Value m : v) {
                if (!first) out += ',';
                first = false;
                quote(out, m.key());
                out += ':';
                dump(out, m);
            }
            out += '}';
            break;
        }
    }
}

Writer& Writer::value(i64 n) {
    comma();
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, n);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
    return *this;
}

Writer& Writer::value(double n) {
    comma();
    if (!std::isfinite(n)) {
        out += "null";
        return *this;
    }
    char buf[32];
    out.append(buf, static_cast<size_t>(std::snprintf(buf, sizeof buf, "%.17g", n)));
    return *this;
}

}  // namespace blit::json
