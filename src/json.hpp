#pragma once
// A small JSON reader and writer, for talking to language servers.
//
// Reading: Doc::parse() turns a message into a flat array of nodes. A Value
// is a cheap handle into that array; asking a Value for something it is not
// (a member of a number, the text of an object) gives an empty answer
// instead of failing, so code that picks a message apart reads straight
// through without checking every step.
//
// Writing: Writer appends to a string and puts the commas in.
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit::json {

enum class Type : u8 { None, Null, Bool, Number, String, Array, Object };

class Doc;

class Value {
public:
    Value() = default;

    Type type() const;
    bool exists() const { return doc_ != nullptr; }
    explicit operator bool() const { return exists() && type() != Type::Null; }
    bool is_object() const { return type() == Type::Object; }
    bool is_array() const { return type() == Type::Array; }
    bool is_string() const { return type() == Type::String; }
    bool is_number() const { return type() == Type::Number; }

    bool boolean(bool otherwise = false) const;
    double number(double otherwise = 0) const;
    i64 integer(i64 otherwise = 0) const;
    std::string_view str() const;  // "" unless a string

    // Member of an object; a Value that does not exist if there is none.
    Value operator[](std::string_view key) const;
    // Number of elements or members.
    size_t size() const;
    // Element of an array.
    Value at(size_t i) const;

    // for (Value v : array) and for (Value m : object), with m.key().
    std::string_view key() const;
    class Iter {
    public:
        Value operator*() const;
        Iter& operator++();
        bool operator!=(const Iter& o) const { return at_ != o.at_; }

    private:
        friend class Value;
        const Doc* doc_ = nullptr;
        u32 at_ = 0;
    };
    Iter begin() const;
    Iter end() const { return Iter(); }

private:
    friend class Doc;
    Value(const Doc* d, u32 n) : doc_(d), node_(n) {}
    const Doc* doc_ = nullptr;
    u32 node_ = 0;
};

class Doc {
public:
    // False if the text is not JSON; the document is then empty.
    bool parse(std::string text);
    Value root() const { return nodes_.empty() ? Value() : Value(this, 1); }
    size_t node_count() const { return nodes_.empty() ? 0 : nodes_.size() - 1; }

private:
    friend class Value;
    struct Node {
        Type type = Type::None;
        bool key_owned = false, str_owned = false;  // text lives in arena_, not text_
        u32 next = 0;                               // next sibling, 0 = last
        u32 child = 0, count = 0;                   // first child and how many
        u32 key_at = 0, key_len = 0;
        u32 str_at = 0, str_len = 0;
        double num = 0;
    };
    struct Parser;
    std::string_view key_of(const Node& n) const {
        return std::string_view(n.key_owned ? arena_ : text_).substr(n.key_at, n.key_len);
    }
    std::string_view str_of(const Node& n) const {
        return std::string_view(n.str_owned ? arena_ : text_).substr(n.str_at, n.str_len);
    }

    std::string text_;        // the message; strings without escapes point into it
    std::string arena_;       // strings that had escapes, unescaped
    std::vector<Node> nodes_;  // [0] is unused so that index 0 can mean "none"
};

// Appends s as a JSON string, quotes included. Bytes that are not valid
// UTF-8 become U+FFFD.
void quote(std::string& out, std::string_view s);
// Appends a parsed value as JSON text ("null" for one that does not exist).
void dump(std::string& out, Value v);

class Writer {
public:
    std::string out;

    Writer& begin_object() { return open('{'); }
    Writer& end_object() { return close('}'); }
    Writer& begin_array() { return open('['); }
    Writer& end_array() { return close(']'); }
    Writer& key(std::string_view k) {
        comma();
        quote(out, k);
        out += ':';
        fresh_ = true;
        return *this;
    }
    Writer& value(std::string_view s) {
        comma();
        quote(out, s);
        return *this;
    }
    Writer& value(const char* s) { return value(std::string_view(s)); }
    Writer& value(const std::string& s) { return value(std::string_view(s)); }
    Writer& value(i64 n);
    Writer& value(int n) { return value(static_cast<i64>(n)); }
    Writer& value(u32 n) { return value(static_cast<i64>(n)); }
    Writer& value(u64 n) { return value(static_cast<i64>(n)); }
    Writer& value(double n);
    Writer& value(bool b) { return raw(b ? "true" : "false"); }
    Writer& null() { return raw("null"); }
    // Text that already is JSON.
    Writer& raw(std::string_view json) {
        comma();
        out += json;
        return *this;
    }
    // key + value in one go.
    template <class T>
    Writer& member(std::string_view k, const T& v) {
        key(k);
        return value(v);
    }

private:
    void comma() {
        if (!fresh_) out += ',';
        fresh_ = false;
    }
    Writer& open(char c) {
        comma();
        out += c;
        fresh_ = true;
        return *this;
    }
    Writer& close(char c) {
        out += c;
        fresh_ = false;
        return *this;
    }
    bool fresh_ = true;  // nothing written yet at this level: no comma needed
};

}  // namespace blit::json
