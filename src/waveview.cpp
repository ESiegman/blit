// The wave tab's rows, values in a radix, and the view kept beside the
// project (waveview.hpp).
#include "waveview.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "json.hpp"

namespace blit {

namespace {

constexpr std::string_view kRadixNames[kWaveRadixCount] = {"hex", "unsigned", "signed", "binary", "octal", "ascii", "enum", "fixed", "float"};

// A value of the lane, a word of 64 bits at a time (bit 0 of the value in
// bit 0 of the first), and its x/z plane alongside.
struct Bits {
    std::vector<u64> val, xz;
    u32 n = 0;
    bool any_xz = false;
    bool bit(u32 b) const { return (val[b / 64] >> (b % 64)) & 1; }
    bool q(u32 b) const { return (xz[b / 64] >> (b % 64)) & 1; }
    // Bit b as a character: 0 1 z x (VPI's planes: x and z have the second bit, x the first too).
    char ch(u32 b) const { return q(b) ? (bit(b) ? 'x' : 'z') : (bit(b) ? '1' : '0'); }
};

void read_bits(const Lane& lane, u64 i, u32 from, u32 n, Bits* out) {
    out->n = n;
    const u32 words = (n + 63) / 64;
    out->val.assign(words, 0);
    out->xz.assign(words, 0);
    out->any_xz = false;
    for (u32 k = 0; k < words; ++k) {
        const u32 take = std::min<u32>(64, n - k * 64);
        out->val[k] = lane.bits(i, from + k * 64, take);
        out->xz[k] = lane.xz(i, from + k * 64, take);
        out->any_xz |= out->xz[k] != 0;
    }
}

// Digits of `per` bits each (4 for hex, 3 for octal, 1 for binary), the
// highest first; a digit with x or z in it as x or z (wholly so) or X or Z.
void digits(const Bits& b, u32 per, std::string* out) {
    static constexpr char kDigits[] = "0123456789abcdef";
    const u32 count = (b.n + per - 1) / per;
    out->resize(count);
    for (u32 d = 0; d < count; ++d) {
        const u32 lo = d * per, hi = std::min(b.n, lo + per);
        u32 v = 0, xs = 0, zs = 0;
        for (u32 k = lo; k < hi; ++k) {
            if (b.q(k)) (b.bit(k) ? xs : zs) += 1;
            else v |= static_cast<u32>(b.bit(k)) << (k - lo);
        }
        char c = kDigits[v];
        if (xs + zs == hi - lo) c = xs == 0 ? 'z' : zs == 0 ? 'x' : 'X';
        else if (xs) c = 'X';
        else if (zs) c = 'Z';
        (*out)[count - 1 - d] = c;
    }
}

// An unsigned number of any width in decimal: limbs of 32 bits, divided by
// a billion until nothing is left.
std::string decimal(const std::vector<u64>& words, u32 n) {
    std::vector<u32> limbs;
    for (u32 k = 0; k * 32 < n; ++k) {
        u32 l = static_cast<u32>(words[k / 2] >> (k % 2 * 32));
        if (n - k * 32 < 32) l &= (1u << (n - k * 32)) - 1;
        limbs.push_back(l);
    }
    while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    if (limbs.empty()) return "0";
    std::vector<u32> parts;  // groups of nine digits, the lowest first
    while (!limbs.empty()) {
        u64 rem = 0;
        for (size_t k = limbs.size(); k-- > 0;) {
            const u64 cur = rem << 32 | limbs[k];
            limbs[k] = static_cast<u32>(cur / 1'000'000'000u);
            rem = cur % 1'000'000'000u;
        }
        parts.push_back(static_cast<u32>(rem));
        while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    }
    std::string out = std::to_string(parts.back());
    char buf[16];
    for (size_t k = parts.size() - 1; k-- > 0;) {
        std::snprintf(buf, sizeof buf, "%09u", parts[k]);
        out += buf;
    }
    return out;
}

// The words made the two's complement negation of an n-bit number.
void negate(std::vector<u64>& words, u32 n) {
    u64 carry = 1;
    for (u64& w : words) {
        w = ~w;
        const u64 s = w + carry;
        carry = carry && s == 0;
        w = s;
    }
    if (n % 64) words.back() &= (u64{1} << (n % 64)) - 1;
}

template <typename T>
std::string shortest(T v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

float half_to_float(u32 h) {
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// A fixed-point number: v (the n-bit value, sign-extended if it is signed)
// over 2^frac, in as many decimals as it has (at most 12), exactly but for
// those it would need past them.
std::string fixed_text(u64 raw, u32 n, u32 frac, bool sign) {
    bool neg = false;
    u64 mag = raw;
    if (sign && n > 0 && ((raw >> (n - 1)) & 1)) {
        neg = true;
        mag = (n == 64 ? u64{0} : u64{1} << n) - raw;  // (2^n - raw, in n bits)
    }
    const u64 whole = frac >= 64 ? 0 : mag >> frac;
    u64 part = frac == 0 ? 0 : frac >= 64 ? mag : mag & ((u64{1} << frac) - 1);
    std::string out = (neg ? "-" : "") + std::to_string(whole);
    if (part) {
        out += '.';
        for (int d = 0; d < 12 && part; ++d) {
            const unsigned __int128 t = static_cast<unsigned __int128>(part) * 10;
            out += static_cast<char>('0' + static_cast<int>(t >> frac));
            part = static_cast<u64>(t & ((static_cast<unsigned __int128>(1) << frac) - 1));
        }
    }
    return out;
}

bool parse_u64(std::string_view s, u64* out) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2), base = 16;
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) s.remove_prefix(2), base = 2;
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out, base);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

}  // namespace

std::string_view wave_radix_name(WaveRadix r) { return kRadixNames[std::min<u32>(static_cast<u32>(r), kWaveRadixCount - 1)]; }

bool wave_radix_parse(std::string_view s, WaveRadix* out) {
    for (u32 k = 0; k < kWaveRadixCount; ++k)
        if (kRadixNames[k] == s) return *out = static_cast<WaveRadix>(k), true;
    return false;
}

const std::string* WaveEnum::find(u64 v) const {
    const auto it = std::ranges::lower_bound(names, v, {}, &std::pair<u64, std::string>::first);
    return it != names.end() && it->first == v ? &it->second : nullptr;
}

bool WaveEnum::parse(std::string_view s, WaveEnum* out) {
    out->names.clear();
    std::vector<std::string_view> words;
    for (size_t at = 0; at < s.size();) {
        while (at < s.size() && (s[at] == ' ' || s[at] == ',' || s[at] == '\t')) ++at;
        size_t end = at;
        while (end < s.size() && s[end] != ' ' && s[end] != ',' && s[end] != '\t') ++end;
        if (end > at) words.push_back(s.substr(at, end - at));
        at = end;
    }
    if (words.empty()) return false;
    u64 next = 0;
    for (const std::string_view w : words) {
        const size_t eq = w.find('=');
        u64 v = next;
        std::string_view name = w;
        if (eq != std::string_view::npos) {
            if (!parse_u64(w.substr(0, eq), &v)) return false;
            name = w.substr(eq + 1);
        }
        if (name.empty()) return false;
        std::erase_if(out->names, [&](const auto& p) { return p.first == v; });
        out->names.emplace_back(v, std::string(name));
        next = v + 1;
    }
    std::ranges::sort(out->names, {}, &std::pair<u64, std::string>::first);
    return true;
}

std::string WaveEnum::text() const {
    bool from_zero = true;
    for (size_t k = 0; k < names.size(); ++k) from_zero &= names[k].first == k;
    std::string out;
    for (const auto& [v, name] : names) {
        if (!out.empty()) out += ' ';
        if (!from_zero) out += std::to_string(v) + "=";
        out += name;
    }
    return out;
}

bool WaveFormat::parse_q(std::string_view s, u32 width, WaveFormat* f) {
    bool sign = true;
    if (!s.empty() && (s[0] == 'U' || s[0] == 'u')) sign = false, s.remove_prefix(1);
    if (s.empty() || (s[0] != 'Q' && s[0] != 'q')) return false;
    s.remove_prefix(1);
    const size_t dot = s.find('.');
    if (dot == std::string_view::npos) return false;
    u64 m = 0, n = 0;
    if (!parse_u64(s.substr(0, dot), &m) || !parse_u64(s.substr(dot + 1), &n)) return false;
    if (m + n != width || n > 63 || width > 64) return false;
    f->frac = static_cast<u8>(n);
    f->sign = sign;
    return true;
}

std::string WaveFormat::q_text(u32 width) const {
    return std::string(sign ? "" : "U") + "Q" + std::to_string(width >= frac ? width - frac : 0) + "." + std::to_string(frac);
}

WaveRadix wave_radix_default(const WaveVar& v) {
    if (v.width <= 1) return WaveRadix::kBinary;
    // (A Verilog integer, an int and their kin are signed.)
    if (v.kind == VarKind::kInteger || v.kind == VarKind::kInt || v.kind == VarKind::kShortInt || v.kind == VarKind::kLongInt || v.kind == VarKind::kByte) return WaveRadix::kSigned;
    return WaveRadix::kHex;
}

void wave_value(const Lane& lane, u64 i, const WaveFormat& f, std::string* out) {
    out->clear();
    if (i >= lane.size()) return;
    if (lane.kind() != LaneKind::kBits) {
        lane.format(i, out);
        return;
    }
    const u32 width = lane.width();
    if (f.bit >= 0 || width == 1) {
        const u32 b = f.bit >= 0 ? std::min<u32>(static_cast<u32>(f.bit), width - 1) : 0;
        out->assign(1, "01zx"[static_cast<u32>(lane.bit(i, b))]);
        return;
    }
    Bits b;
    read_bits(lane, i, 0, width, &b);
    if (b.any_xz) {
        switch (f.radix) {
            case WaveRadix::kBinary: digits(b, 1, out); return;
            case WaveRadix::kOctal: digits(b, 3, out); return;
            case WaveRadix::kHex: digits(b, 4, out); return;
            default: {
                bool all_z = true;
                for (u32 k = 0; k < width && all_z; ++k) all_z = b.q(k) && !b.bit(k);
                out->assign(all_z ? "z" : "x");
                return;
            }
        }
    }
    const u64 low = b.val[0];
    switch (f.radix) {
        case WaveRadix::kBinary: digits(b, 1, out); return;
        case WaveRadix::kOctal: digits(b, 3, out); return;
        case WaveRadix::kUnsigned: *out = width <= 64 ? std::to_string(low) : decimal(b.val, width); return;
        case WaveRadix::kSigned:
            if (!b.bit(width - 1)) {
                *out = width <= 64 ? std::to_string(low) : decimal(b.val, width);
            } else if (width <= 64) {
                const i64 v = width == 64 ? static_cast<i64>(low) : static_cast<i64>(low | (~u64{0} << width));
                *out = std::to_string(v);
            } else {
                negate(b.val, width);
                *out = "-" + decimal(b.val, width);
            }
            return;
        case WaveRadix::kAscii:
            for (u32 hi = (width + 7) / 8; hi-- > 0;) {
                u32 c = 0;
                for (u32 k = 0; k < 8 && hi * 8 + k < width; ++k) c |= static_cast<u32>(b.bit(hi * 8 + k)) << k;
                out->push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
            }
            return;
        case WaveRadix::kEnum:
            if (width <= 64 && f.names)
                if (const std::string* name = f.names->find(low)) {
                    *out = *name;
                    return;
                }
            break;
        case WaveRadix::kFixed:
            if (width <= 64) {
                *out = fixed_text(low, width, std::min<u32>(f.frac, width), f.sign);
                return;
            }
            break;
        case WaveRadix::kFloat:
            if (width == 64) return void(*out = shortest(std::bit_cast<double>(low)));
            if (width == 32) return void(*out = shortest(std::bit_cast<float>(static_cast<u32>(low))));
            if (width == 16) return void(*out = shortest(half_to_float(static_cast<u32>(low))));
            break;
        default: break;
    }
    digits(b, 4, out);  // (hex, and what has no way of its own for this value)
}

std::vector<WaveRow> wave_rows(const std::vector<WaveItem>& items, const Wave& w) {
    std::vector<WaveRow> out;
    auto signal = [&](const WaveSignal& s, u32 item, u32 member, u8 depth) {
        out.push_back({WaveRow::kSignal, item, member, 0, depth});
        if (!s.open || s.var >= w.vars.size()) return;
        const WaveVar& v = w.vars[s.var];
        if (v.lane >= w.lanes.size() || w.lanes[v.lane].kind() != LaneKind::kBits || w.lanes[v.lane].width() < 2) return;
        for (u32 b = w.lanes[v.lane].width(); b-- > 0;) out.push_back({WaveRow::kBit, item, member, b, static_cast<u8>(depth + 1)});
    };
    for (u32 i = 0; i < items.size(); ++i) {
        const WaveItem& it = items[i];
        if (!it.group) {
            signal(it.sig, i, ~0u, 0);
            continue;
        }
        out.push_back({WaveRow::kGroup, i, ~0u, 0, 0});
        if (it.open)
            for (u32 m = 0; m < it.members.size(); ++m) signal(it.members[m], i, m, 1);
    }
    return out;
}

const WaveSignal* wave_row_signal(const std::vector<WaveItem>& items, const WaveRow& r) {
    if (r.item >= items.size() || r.kind == WaveRow::kGroup) return nullptr;
    const WaveItem& it = items[r.item];
    if (!it.group) return &it.sig;
    return r.member < it.members.size() ? &it.members[r.member] : nullptr;
}

WaveSignal* wave_row_signal(std::vector<WaveItem>& items, const WaveRow& r) {
    return const_cast<WaveSignal*>(wave_row_signal(static_cast<const std::vector<WaveItem>&>(items), r));
}

WaveFormat wave_row_format(const std::vector<WaveItem>& items, const WaveRow& r) {
    const WaveSignal* s = wave_row_signal(items, r);
    if (!s) return {};
    if (r.kind != WaveRow::kBit) return s->fmt;
    WaveFormat f;
    f.radix = WaveRadix::kBinary;
    f.bit = static_cast<i32>(r.bit);
    return f;
}

std::string wave_row_name(const std::vector<WaveItem>& items, const WaveRow& r, const Wave& w) {
    if (r.kind == WaveRow::kGroup) return r.item < items.size() ? items[r.item].name : std::string();
    const WaveSignal* s = wave_row_signal(items, r);
    if (!s || s->var >= w.vars.size()) return {};
    const WaveVar& v = w.vars[s->var];
    if (r.kind == WaveRow::kBit) {
        const i64 at = !v.ranged ? r.bit : v.msb >= v.lsb ? v.lsb + static_cast<i64>(r.bit) : v.lsb - static_cast<i64>(r.bit);
        return v.name + "[" + std::to_string(at) + "]";
    }
    return wave_var_name(w, s->var);
}

std::string wave_var_name(const Wave& w, u32 var) {
    const WaveVar& v = w.vars[var];
    std::string out = w.path(var);
    if (v.ranged) out += v.msb == v.lsb ? "[" + std::to_string(v.msb) + "]" : "[" + std::to_string(v.msb) + ":" + std::to_string(v.lsb) + "]";
    return out;
}

std::vector<WaveItem> wave_default_items(const Wave& w, u32 most) {
    std::vector<WaveItem> groups;
    std::vector<u8> seen(w.lanes.size(), 0);
    u32 count = 0;
    // Depth first, as the dump declares them.
    std::vector<u32> stack(w.scopes[0].scopes.rbegin(), w.scopes[0].scopes.rend());
    auto take = [&](u32 scope, WaveItem* into) {
        for (const u32 v : w.scopes[scope].vars) {
            if (count >= most) return;
            const WaveVar& var = w.vars[v];
            if (var.lane < seen.size()) {
                if (seen[var.lane]) continue;
                seen[var.lane] = 1;
            }
            WaveSignal s;
            s.var = v;
            s.fmt.radix = wave_radix_default(var);
            into->members.push_back(s);
            ++count;
        }
    };
    WaveItem top;  // (variables of the root itself, which a dump rarely has)
    take(0, &top);
    if (!top.members.empty()) {
        top.group = true;
        groups.push_back(std::move(top));
    }
    while (!stack.empty() && count < most) {
        const u32 s = stack.back();
        stack.pop_back();
        WaveItem g;
        g.group = true;
        if (!w.scopes[s].vars.empty()) g.name = [&] {
            std::string p = w.scopes[s].name;
            for (u32 up = w.scopes[s].parent, depth = 0; up != 0 && depth < w.scopes.size(); up = w.scopes[up].parent, ++depth) p = w.scopes[up].name + "." + p;
            return p;
        }();
        take(s, &g);
        if (!g.members.empty()) groups.push_back(std::move(g));
        for (auto it = w.scopes[s].scopes.rbegin(); it != w.scopes[s].scopes.rend(); ++it) stack.push_back(*it);
    }
    if (groups.size() != 1) return groups;
    std::vector<WaveItem> plain;  // (one scope: its signals, no heading)
    for (WaveSignal& s : groups[0].members) {
        WaveItem it;
        it.sig = s;
        plain.push_back(std::move(it));
    }
    return plain;
}

namespace {

void write_signal(json::Writer& j, const Wave& w, const WaveSignal& s) {
    j.begin_object();
    j.member("path", w.path(s.var));
    if (w.vars[s.var].width > 1) j.member("radix", wave_radix_name(s.fmt.radix));
    if (s.fmt.frac > 0 || !s.fmt.sign) j.member("q", s.fmt.q_text(w.vars[s.var].width));
    if (s.open) j.member("open", true);
    j.end_object();
}

bool read_signal(const Wave& w, json::Value v, WaveSignal* out) {
    const i64 var = w.find(v["path"].str());
    if (var < 0) return false;
    out->var = static_cast<u32>(var);
    const WaveVar& wv = w.vars[out->var];
    out->fmt.radix = wave_radix_default(wv);
    WaveRadix r;
    if (wave_radix_parse(v["radix"].str(), &r)) out->fmt.radix = r;
    if (v["q"].is_string()) WaveFormat::parse_q(v["q"].str(), wv.width, &out->fmt);
    out->open = v["open"].boolean();
    return true;
}

}  // namespace

std::string wave_view_json(const Wave& w, const WaveSaved& s) {
    json::Writer j;
    j.begin_object();
    j.member("version", 1);
    j.key("rows").begin_array();
    for (const WaveItem& it : s.items) {
        if (!it.group) {
            write_signal(j, w, it.sig);
            continue;
        }
        j.begin_object();
        j.member("group", it.name);
        j.member("open", it.open);
        j.key("rows").begin_array();
        for (const WaveSignal& m : it.members) write_signal(j, w, m);
        j.end_array();
        j.end_object();
    }
    j.end_array();
    if (!s.enums.empty()) {
        j.key("enums").begin_object();
        for (const auto& [path, e] : s.enums) j.member(path, e->text());
        j.end_object();
    }
    if (s.marked) {
        j.key("markers").begin_object();
        for (u32 k = 0; k < 26; ++k)
            if (s.marked & (1u << k)) j.member(std::string(1, static_cast<char>('a' + k)), s.marks[k]);
        j.end_object();
    }
    if (s.has_cursor) j.member("cursor", s.cursor);
    if (s.to > s.from) {
        j.member("from", s.from);
        j.member("to", s.to);
    }
    j.end_object();
    j.out += '\n';
    return std::move(j.out);
}

bool wave_view_parse(const Wave& w, std::string_view text, WaveSaved* out, u32* dropped) {
    *out = WaveSaved{};
    *dropped = 0;
    json::Doc doc;
    if (!doc.parse(std::string(text)) || !doc.root().is_object()) return false;
    const json::Value root = doc.root();
    for (const json::Value e : root["enums"]) {
        auto names = std::make_shared<WaveEnum>();
        if (WaveEnum::parse(e.str(), names.get())) out->enums[std::string(e.key())] = std::move(names);
    }
    auto attach = [&](WaveSignal& s) {
        if (const auto it = out->enums.find(w.path(s.var)); it != out->enums.end()) s.fmt.names = it->second;
    };
    for (const json::Value r : root["rows"]) {
        WaveItem it;
        if (r["group"].is_string()) {
            it.group = true;
            it.name = r["group"].str();
            it.open = r["open"].boolean(true);
            for (const json::Value m : r["rows"]) {
                WaveSignal s;
                if (read_signal(w, m, &s)) attach(s), it.members.push_back(std::move(s));
                else ++*dropped;
            }
        } else if (read_signal(w, r, &it.sig)) {
            attach(it.sig);
        } else {
            ++*dropped;
            continue;
        }
        out->items.push_back(std::move(it));
    }
    for (const json::Value m : root["markers"]) {
        const std::string_view k = m.key();
        if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z' && m.is_number()) {
            out->marks[k[0] - 'a'] = static_cast<u64>(std::max<i64>(0, m.integer()));
            out->marked |= 1u << (k[0] - 'a');
        }
    }
    if (root["cursor"].is_number()) out->cursor = static_cast<u64>(std::max<i64>(0, root["cursor"].integer())), out->has_cursor = true;
    if (root["from"].is_number() && root["to"].is_number()) out->from = root["from"].number(), out->to = root["to"].number();
    return true;
}

std::string wave_view_path(const std::string& root, const std::string& dump) {
    const size_t slash = dump.find_last_of('/');
    std::string stem = slash == std::string::npos ? dump : dump.substr(slash + 1);
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    std::string dir = root;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir + "/.blit/waves/" + stem + ".json";
}

}  // namespace blit
