#include "wave.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "qos.hpp"

namespace blit {

namespace {

constexpr std::string_view kScopeKinds[] = {"module", "task", "function", "begin", "fork", "generate", "struct",
                                            "union", "class", "interface", "package", "program", "other"};
static_assert(std::size(kScopeKinds) == static_cast<size_t>(ScopeKind::kOther) + 1);

constexpr std::string_view kVarKinds[] = {"wire", "reg", "logic", "bit", "integer", "int", "shortint", "longint", "byte", "time",
                                          "parameter", "real", "realtime", "shortreal", "string", "event", "enum", "port",
                                          "supply0", "supply1", "tri", "triand", "trior", "trireg", "tri0", "tri1", "wand", "wor",
                                          "uwire", "other"};
static_assert(std::size(kVarKinds) == static_cast<size_t>(VarKind::kOther) + 1);

template <class E, size_t N>
E kind_of(const std::string_view (&names)[N], std::string_view s) {
    for (size_t i = 0; i + 1 < N; ++i)
        if (names[i] == s) return static_cast<E>(i);
    return static_cast<E>(N - 1);
}

constexpr size_t kSlice = 1 << 20;  // the most read under one hold of the Wave's lock
constexpr u32 kMaxWidth = 1 << 20;  // bits of a variable (a wider one is taken as this wide)
constexpr size_t kMaxNotes = 50;

// The characters VCD separates its tokens with.
struct Spaces {
    bool is[256] = {};
    constexpr Spaces() { is[' '] = is['\t'] = is['\n'] = is['\r'] = is['\v'] = is['\f'] = true; }
};
constexpr Spaces kSpace;

inline bool space(char c) { return kSpace.is[static_cast<u8>(c)]; }

// A digit of a vector's value, as VPI's (value, x/z) bits: 0 1 z x, and
// what other writers use for them (GHDL's std_logic: U W - as x, L H as 0 1).
struct Digits {
    u8 v[256] = {};  // bit 0: value, bit 1: x/z, bit 2: not a digit at all
    constexpr Digits() {
        for (u8& d : v) d = 4 | 3;
        v['0'] = v['l'] = v['L'] = 0;
        v['1'] = v['h'] = v['H'] = 1;
        v['z'] = v['Z'] = 2;
        v['x'] = v['X'] = v['u'] = v['U'] = v['w'] = v['W'] = v['-'] = 3;
    }
};
constexpr Digits kDigit;

bool parse_u64(std::string_view s, u64* out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_i32(std::string_view s, i32* out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// "[7:0]" or "[3]".
bool parse_range(std::string_view s, i32* msb, i32* lsb) {
    if (s.size() < 3 || s.front() != '[' || s.back() != ']') return false;
    s = s.substr(1, s.size() - 2);
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos) {
        if (!parse_i32(s, msb)) return false;
        *lsb = *msb;
        return true;
    }
    return parse_i32(s.substr(0, colon), msb) && parse_i32(s.substr(colon + 1), lsb);
}

}  // namespace

std::string_view scope_kind_name(ScopeKind k) { return kScopeKinds[std::min<size_t>(static_cast<size_t>(k), std::size(kScopeKinds) - 1)]; }
std::string_view var_kind_name(VarKind k) { return kVarKinds[std::min<size_t>(static_cast<size_t>(k), std::size(kVarKinds) - 1)]; }

std::string Wave::path(u32 var) const {
    const WaveVar& v = vars[var];
    size_t n = v.name.size();
    u32 depth = 0;
    for (u32 s = v.scope; s != 0 && depth < scopes.size(); s = scopes[s].parent, ++depth) n += scopes[s].name.size() + 1;
    std::string out(n, '.');
    size_t at = n - v.name.size();
    std::memcpy(out.data() + at, v.name.data(), v.name.size());
    depth = 0;
    for (u32 s = v.scope; s != 0 && depth < scopes.size(); s = scopes[s].parent, ++depth) {
        at -= scopes[s].name.size() + 1;
        std::memcpy(out.data() + at, scopes[s].name.data(), scopes[s].name.size());
    }
    return out;
}

i64 Wave::find(std::string_view p) const {
    u32 s = 0;
    for (;;) {
        // The longest name of a scope inside s that the path goes on from
        // (a name may have dots of its own, as escaped identifiers can).
        u32 next = 0;
        size_t used = 0;
        for (const u32 c : scopes[s].scopes) {
            const std::string& n = scopes[c].name;
            if (n.size() > used && p.size() > n.size() && p.starts_with(n) && p[n.size()] == '.') next = c, used = n.size();
        }
        for (const u32 v : scopes[s].vars)
            if (vars[v].name == p) return v;
        if (!next) return -1;
        p.remove_prefix(used + 1);
        s = next;
    }
}

std::string Wave::time_text(u64 t) const {
    if (samples) return std::to_string(t);
    // t * mult * 10^exp seconds, in the largest of fs ps ns us ms s that
    // leaves at least 1 of it.
    static constexpr std::string_view kUnits[] = {"fs", "ps", "ns", "us", "ms", "s"};
    if (t == 0) return "0 " + std::string(kUnits[std::clamp((unit_exp + 15) / 3, 0, 5)]);
    long double v = static_cast<long double>(t) * unit_mult;
    i32 e = unit_exp;
    while (e > 0) v *= 10, --e;
    while (e > -15 && e % 3 != 0) v *= 10, --e;
    i32 u = (e + 15) / 3;
    while (u < 5 && v >= 1000) v /= 1000, ++u;
    char buf[48];
    const int n = std::snprintf(buf, sizeof buf, "%.3Lf", v);
    std::string s(buf, static_cast<size_t>(std::max(n, 0)));
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s + " " + std::string(kUnits[u]);
}

bool Wave::time_of(u64 digits, u32 frac, bool unit, u32 mult, i32 exp, u64* out) const {
    if (unit && samples) return false;
    // digits * mult * 10^(exp - frac) seconds over unit_mult * 10^unit_exp:
    // digits * mult / unit_mult * 10^e, rounded to the nearest.
    using u128 = unsigned __int128;
    const i32 e = unit ? exp - static_cast<i32>(frac) - unit_exp : -static_cast<i32>(frac);
    u128 num = static_cast<u128>(digits) * (unit ? mult : 1), den = unit ? unit_mult : 1;
    if (e < -30 || e > 30) return false;
    for (i32 k = 0; k < e; ++k) {
        num *= 10;
        if (num >> 100) return false;
    }
    for (i32 k = 0; k > e; --k) den *= 10;
    const u128 t = (num + den / 2) / den;
    if (t >> 64) return false;
    *out = static_cast<u64>(t);
    return true;
}

size_t Wave::bytes() const {
    size_t n = sizeof(Wave);
    for (const Lane& l : lanes) n += l.bytes();
    for (const WaveVar& v : vars) n += sizeof v + v.name.capacity() + v.code.capacity();
    for (const WaveScope& s : scopes) n += sizeof s + s.name.capacity() + (s.scopes.capacity() + s.vars.capacity()) * sizeof(u32);
    return n;
}

// Identifier codes to lanes. Codes of one and two characters of VCD's own
// alphabet (! to ~), which Verilator and Icarus use for the first 8,930
// variables, are looked up in tables; others of up to seven bytes in a
// table by hash; longer ones in a map.
struct VcdReader::Ids {
    static constexpr u32 kNone = ~0u;
    std::vector<u32> one = std::vector<u32>(94, kNone), two = std::vector<u32>(94 * 94, kNone);
    std::vector<u64> keys;
    std::vector<u32> lanes;
    size_t used = 0;
    std::unordered_map<std::string, u32> longer;

    static bool small(char c) { return c >= '!' && c <= '~'; }
    static u64 key(std::string_view c) {
        u64 k = static_cast<u64>(c.size()) << 56;
        std::memcpy(&k, c.data(), c.size());  // (little or big end: either is a key)
        return k | static_cast<u64>(c.size()) << 56;
    }
    size_t slot(u64 k) const { return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> 40) & (keys.size() - 1); }

    i64 find(std::string_view c) const {
        if (c.size() == 1 && small(c[0])) {
            const u32 l = one[static_cast<u8>(c[0]) - '!'];
            return l == kNone ? -1 : static_cast<i64>(l);
        }
        if (c.size() == 2 && small(c[0]) && small(c[1])) {
            const u32 l = two[(static_cast<u8>(c[0]) - '!') * 94 + static_cast<u8>(c[1]) - '!'];
            return l == kNone ? -1 : static_cast<i64>(l);
        }
        if (c.size() <= 7 && !c.empty()) {
            if (keys.empty()) return -1;
            const u64 k = key(c);
            for (size_t s = slot(k);; s = (s + 1) & (keys.size() - 1)) {
                if (keys[s] == k) return lanes[s];
                if (keys[s] == 0) return -1;
            }
        }
        const auto it = longer.find(std::string(c));
        return it == longer.end() ? -1 : static_cast<i64>(it->second);
    }

    void add(std::string_view c, u32 lane) {
        if (c.size() == 1 && small(c[0])) {
            one[static_cast<u8>(c[0]) - '!'] = lane;
        } else if (c.size() == 2 && small(c[0]) && small(c[1])) {
            two[(static_cast<u8>(c[0]) - '!') * 94 + static_cast<u8>(c[1]) - '!'] = lane;
        } else if (c.size() <= 7 && !c.empty()) {
            if ((used + 1) * 2 > keys.size()) {
                std::vector<u64> ok = std::move(keys);
                std::vector<u32> ol = std::move(lanes);
                keys.assign(std::max<size_t>(64, ok.size() * 2), 0);
                lanes.assign(keys.size(), 0);
                for (size_t i = 0; i < ok.size(); ++i)
                    if (ok[i]) put(ok[i], ol[i]);
            }
            put(key(c), lane);
            ++used;
        } else {
            longer.emplace(std::string(c), lane);
        }
    }
    void put(u64 k, u32 lane) {
        size_t s = slot(k);
        while (keys[s] != 0 && keys[s] != k) s = (s + 1) & (keys.size() - 1);
        keys[s] = k, lanes[s] = lane;
    }
    size_t bytes() const { return (one.size() + two.size() + lanes.size()) * 4 + keys.size() * 8 + longer.size() * 64; }
};

VcdReader::VcdReader(Wave* into, u64 max_bytes) : w_(into), max_(max_bytes), ids_(std::make_unique<Ids>()) {
    mem_ = ids_->bytes();
}

VcdReader::~VcdReader() = default;

void VcdReader::note(std::string s) {
    if (w_->notes.size() >= kMaxNotes) {
        ++notes_dropped_;
        return;
    }
    w_->notes.push_back("line " + std::to_string(line_) + ": " + std::move(s));
}

void VcdReader::stop(std::string why) {
    if (stopped_) return;
    stopped_ = true;
    if (w_->error.empty()) w_->error = std::move(why);
}

i64 VcdReader::lane_of(std::string_view code) const { return ids_->find(code); }

bool VcdReader::feed(std::string_view bytes) {
    while (!stopped_ && !bytes.empty()) {
        const size_t n = std::min(bytes.size(), kSlice);
        std::unique_lock lock(w_->mu);
        run(bytes.substr(0, n), false);
        w_->reached = now_;
        bytes.remove_prefix(n);
    }
    return !stopped_;
}

static void ila_marks(Wave& w);

void VcdReader::finish() {
    std::unique_lock lock(w_->mu);
    if (!stopped_) run({}, true);
    if (want_ && !stopped_) note("a value with no identifier code after it, at the end");
    if (!w_->header) {
        if (w_->vars.empty() && w_->error.empty()) w_->error = "not a VCD dump: no $var and no $enddefinitions";
        else if (!w_->vars.empty()) note("no $enddefinitions");
        w_->header = true;
    }
    if (notes_dropped_) w_->notes.push_back("and " + std::to_string(notes_dropped_) + " more");
    notes_dropped_ = 0;
    u64 changes = 0;
    for (Lane& l : w_->lanes) {
        l.shrink();
        changes += l.size();
    }
    w_->changes = changes;
    if (w_->samples) ila_marks(*w_);
    w_->reached = w_->end;
    w_->done = true;
    stopped_ = true;
}

// An ILA capture's triggers and windows, from its VCD form's _TRIGGER and
// _WINDOW: each flips at a trigger, or at a window's first sample (Vivado
// 2026.2 writes them so: a capture of four windows has _TRIGGER 0 to 1 at
// the first trigger, back to 0 at the second, and so on); one that starts
// at 1 triggered at its first sample.
static void ila_marks(Wave& w) {
    w.triggers.clear();
    w.windows.clear();
    for (const WaveVar& v : w.vars) {
        if ((v.name != "_TRIGGER" && v.name != "_WINDOW") || v.lane >= w.lanes.size()) continue;
        const Lane& l = w.lanes[v.lane];
        if (l.kind() != LaneKind::kBits || l.width() != 1) continue;
        std::vector<u64>& to = v.name == "_TRIGGER" ? w.triggers : w.windows;
        if (!to.empty()) continue;  // (the first of that name)
        for (u64 i = 0; i < l.size(); ++i)
            if (i > 0 || (v.name == "_TRIGGER" && l.bit(0, 0) == Bit::k1)) to.push_back(l.time(i));
    }
}

// Splits a piece into tokens; one cut by its end waits in carry_ for the
// next piece (or for the end of the dump).
void VcdReader::run(std::string_view in, bool last) {
    const char* p = in.data();
    const char* const e = p + in.size();
    if (!carry_.empty()) {
        const char* q = p;
        while (q < e && !space(*q)) ++q;
        carry_.append(p, q);
        if (q == e && !last) return;
        token(carry_);
        keep_pending();
        carry_.clear();
        p = q;
    }
    while (!stopped_) {
        while (p < e && space(*p)) line_ += *p++ == '\n';
        if (p == e) break;
        const char* q = p + 1;
        while (q < e && !space(*q)) ++q;
        if (q == e && !last) {
            carry_.assign(p, q);
            break;
        }
        token(std::string_view(p, static_cast<size_t>(q - p)));
        p = q;
    }
    keep_pending();
}

// A value whose code is still to come keeps a copy of itself, for when the
// bytes it was read from are gone.
void VcdReader::keep_pending() {
    if (!want_ || owned_) return;
    pending_.assign(pend_);
    pend_ = pending_;
    owned_ = true;
}

void VcdReader::token(std::string_view t) {
    if (body_) body_token(t);
    else header_token(t);
}

void VcdReader::header_token(std::string_view t) {
    if (kw_.empty()) {
        if (t == "$end") {
            note("an $end with nothing to end");
            return;
        }
        if (t == "$dumpvars" || t == "$dumpall" || t == "$dumpon" || t == "$dumpoff" || (t[0] != '$' && !w_->vars.empty())) {
            note("no $enddefinitions before the values");
            body_ = true;
            w_->header = true;
            body_token(t);
            return;
        }
        if (t[0] != '$') {
            stop("not a VCD dump: it starts with " + std::string(t.substr(0, 40)));
            return;
        }
        kw_ = t;  // (one of VCD's, or of the writer's own: read to its $end)
        args_.clear();
        return;
    }
    if (t != "$end") {
        if (t[0] == '$' && kw_ != "$comment" && kw_ != "$date" && kw_ != "$version" && (t == "$var" || t == "$scope" || t == "$upscope" || t == "$enddefinitions")) {
            note(kw_ + " with no $end");
            declaration();
            header_token(t);
            return;
        }
        if (args_.size() < 64) args_.emplace_back(t);
        return;
    }
    declaration();
}

void VcdReader::declaration() {
    const std::string kw = std::move(kw_);
    kw_.clear();
    Wave& w = *w_;
    auto joined = [&] {
        std::string s;
        for (const std::string& a : args_) s += (s.empty() ? "" : " ") + a;
        return s;
    };
    if (kw == "$enddefinitions") {
        body_ = true;
        // (Vivado's ILA capture: its times are samples, and its own _TRIGGER,
        // _WINDOW and _GAP beside the probes say where it triggered.)
        if (w.version.starts_with("Vivado"))
            for (const WaveVar& v : w.vars)
                if (v.name == "_TRIGGER" && v.width == 1) w.samples = true;
        w.header = true;
    } else if (kw == "$date") {
        w.date = joined();
    } else if (kw == "$version") {
        w.version = joined();
    } else if (kw == "$timescale") {
        // "1ps", "1 ps", "10 ns": 1, 10 or 100 of s, ms, us, ns, ps or fs.
        const std::string s = joined();
        size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        u64 mult = 0;
        std::string unit(s.substr(i));
        unit.erase(std::remove(unit.begin(), unit.end(), ' '), unit.end());
        static constexpr std::pair<std::string_view, i32> kUnits[] = {{"s", 0}, {"ms", -3}, {"us", -6}, {"ns", -9}, {"ps", -12}, {"fs", -15}};
        const auto* u = std::find_if(std::begin(kUnits), std::end(kUnits), [&](const auto& k) { return k.first == unit; });
        if (!parse_u64(s.substr(0, i), &mult) || (mult != 1 && mult != 10 && mult != 100) || u == std::end(kUnits)) {
            note("a $timescale that is not 1, 10 or 100 of s, ms, us, ns, ps or fs: " + s);
        } else {
            w.unit_mult = static_cast<u32>(mult);
            w.unit_exp = u->second;
            w.unit_given = true;
        }
    } else if (kw == "$scope") {
        WaveScope s;
        s.kind = args_.empty() ? ScopeKind::kOther : kind_of<ScopeKind>(kScopeKinds, args_[0]);
        s.name = args_.size() > 1 ? args_[1] : std::string();
        for (size_t i = 2; i < args_.size(); ++i) s.name += " " + args_[i];
        if (args_.size() < 2) note("a $scope with no name");
        s.parent = cur_scope_;
        const u32 id = static_cast<u32>(w.scopes.size());
        mem_ += sizeof s + s.name.size();
        w.scopes.push_back(std::move(s));
        w.scopes[cur_scope_].scopes.push_back(id);
        cur_scope_ = id;
    } else if (kw == "$upscope") {
        if (cur_scope_ == 0) note("an $upscope with no $scope open");
        cur_scope_ = w.scopes[cur_scope_].parent;
    } else if (kw == "$var") {
        // $var kind width code name [range] $end
        if (args_.size() < 4) {
            note("a $var with too few words: " + joined());
            return;
        }
        WaveVar v;
        v.kind = kind_of<VarKind>(kVarKinds, args_[0]);
        u64 width = 0;
        if (!parse_u64(args_[1], &width) || width == 0) {
            note("a $var's width that is not a number: " + args_[1]);
            width = 1;
        }
        if (width > kMaxWidth) {
            note("a $var wider than " + std::to_string(kMaxWidth) + " bits, taken as that wide: " + args_[3]);
            width = kMaxWidth;
        }
        v.width = static_cast<u32>(width);
        v.code = args_[2];
        v.name = args_[3];
        for (size_t i = 4; i < args_.size(); ++i) {
            if (args_[i].starts_with('[') && !v.ranged && parse_range(args_[i], &v.msb, &v.lsb)) v.ranged = true;
            else v.name += " " + args_[i];
        }
        // "count[7:0]", the range not apart from the name.
        if (!v.ranged && v.name.ends_with(']')) {
            const size_t open = v.name.rfind('[');
            if (open != std::string::npos && open > 0 && v.name.find(':', open) != std::string::npos &&
                parse_range(std::string_view(v.name).substr(open), &v.msb, &v.lsb)) {
                v.ranged = true;
                v.name.resize(open);
            }
        }
        const LaneKind lk = v.kind == VarKind::kReal || v.kind == VarKind::kRealTime || v.kind == VarKind::kShortReal ? LaneKind::kReal
                            : v.kind == VarKind::kString                                                         ? LaneKind::kText
                            : v.kind == VarKind::kEvent                                                          ? LaneKind::kEvent
                                                                                                                  : LaneKind::kBits;
        const i64 have = ids_->find(v.code);
        if (have >= 0) {
            v.lane = static_cast<u32>(have);
            const Lane& l = w.lanes[v.lane];
            if (l.kind() != lk || (lk == LaneKind::kBits && l.width() != v.width))
                note("an alias of another kind or width than the first with its code: " + v.name);
        } else {
            v.lane = static_cast<u32>(w.lanes.size());
            w.lanes.emplace_back(lk, v.width);
            ids_->add(v.code, v.lane);
            mem_ += sizeof(Lane);
            if (lk == LaneKind::kBits && val_.size() < (v.width + 63) / 64) {
                val_.resize((v.width + 63) / 64);
                xz_.resize(val_.size());
            }
        }
        v.scope = cur_scope_;
        mem_ += sizeof v + v.name.size() + v.code.size() + 8;
        w.scopes[cur_scope_].vars.push_back(static_cast<u32>(w.vars.size()));
        w.vars.push_back(std::move(v));
        if (max_ && mem_ > max_) stop("the dump's definitions take more memory than timeline.max_mb allows");
    }
    // ($comment and keywords of their own: nothing.)
}

void VcdReader::body_token(std::string_view t) {
    if (skip_) {
        if (t == "$end") skip_ = false;
        return;
    }
    if (want_) {
        const char c = want_;
        want_ = 0;
        value(c, t);
        return;
    }
    const char c = t[0];
    switch (c) {
        case '#': {
            u64 at = 0;
            if (!parse_u64(t.substr(1), &at)) {
                note("not a time: " + std::string(t.substr(0, 40)));
                return;
            }
            if (!timed_) {
                timed_ = true;
                w_->start = at;
                now_ = at;
            } else if (at < now_) {
                note("a time before the one before it: " + std::string(t) + " after #" + std::to_string(now_));
            } else {
                now_ = at;
            }
            w_->end = std::max(w_->end, now_);
            return;
        }
        case 'b': case 'B': case 'r': case 'R': case 's': case 'S':
            want_ = c;
            pend_ = t.substr(1);
            owned_ = false;
            return;
        case '$':
            if (t == "$end" || t == "$dumpvars" || t == "$dumpall") return;
            if (t == "$dumpoff" || t == "$dumpon") return dumpoff(t == "$dumpon");
            skip_ = true;  // $comment, and anything else, to its $end
            return;
        default:
            if (!(kDigit.v[static_cast<u8>(c)] & 4)) {
                if (t.size() < 2) {
                    note("a value with no identifier code: " + std::string(t));
                    return;
                }
                const i64 lane = lane_of(t.substr(1));
                if (lane < 0) {
                    note("a value for a code not declared: " + std::string(t.substr(1, 40)));
                    return;
                }
                if (!timed_) timed_ = true, w_->start = now_;
                scalar(c, static_cast<u32>(lane));
                if (max_ && mem_ > max_) over();
                return;
            }
            note("not a value: " + std::string(t.substr(0, 40)));
            return;
    }
}

void VcdReader::over() { stop("the dump needs more memory than timeline.max_mb allows: read up to " + w_->time_text(now_)); }

void VcdReader::value(char kind, std::string_view code) {
    const i64 lane = lane_of(code);
    if (lane < 0) {
        note("a value for a code not declared: " + std::string(code.substr(0, 40)));
        return;
    }
    if (!timed_) timed_ = true, w_->start = now_;
    const std::string_view v = pend_;
    switch (kind) {
        case 'b': case 'B': vector(v, static_cast<u32>(lane)); break;
        case 'r': case 'R': real(v, static_cast<u32>(lane)); break;
        default: {
            Lane& l = w_->lanes[static_cast<size_t>(lane)];
            if (l.kind() == LaneKind::kText) mem_ += l.add_text(now_, v);
            else if (l.kind() == LaneKind::kEvent) mem_ += l.add_event(now_);
            else note("a string value for a variable that is not a string");
        }
    }
    if (max_ && mem_ > max_) over();
}

void VcdReader::scalar(char c, u32 lane) {
    Lane& l = w_->lanes[lane];
    switch (l.kind()) {
        case LaneKind::kBits:
            if (l.width() == 1) {
                const u8 d = kDigit.v[static_cast<u8>(c)];
                const u64 v = d & 1, m = (d >> 1) & 1;
                mem_ += l.add_bits(now_, &v, m ? &m : nullptr);
            } else {
                vector(std::string_view(&c, 1), lane);
            }
            break;
        case LaneKind::kReal: mem_ += l.add_real(now_, c == '0' ? 0.0 : c == '1' ? 1.0 : std::nan("")); break;
        case LaneKind::kText: mem_ += l.add_text(now_, std::string_view(&c, 1)); break;
        case LaneKind::kEvent: mem_ += l.add_event(now_); break;
    }
}

// "b1x0z": the first digit the highest bit. One shorter than the variable
// is widened as IEEE 1364-2005 18.2.2 says: with 0 if it starts with 0 or
// 1, else with x or z; one longer keeps its lowest bits.
void VcdReader::vector(std::string_view v, u32 lane) {
    Lane& l = w_->lanes[lane];
    if (l.kind() == LaneKind::kText) {
        mem_ += l.add_text(now_, v);
        return;
    }
    if (l.kind() == LaneKind::kEvent) {
        mem_ += l.add_event(now_);
        return;
    }
    const u32 width = l.kind() == LaneKind::kReal ? 64 : l.width();
    const u32 words = (width + 63) / 64;
    if (v.empty()) {
        note("an empty vector value");
        return;
    }
    for (const char ch : v)
        if (kDigit.v[static_cast<u8>(ch)] & 4) {
            note("not a vector value: b" + std::string(v.substr(0, 40)));
            return;
        }
    if (width <= 64) {
        u64 val = 0, xz = 0;
        const size_t from = v.size() > width ? v.size() - width : 0;
        for (size_t i = from; i < v.size(); ++i) {
            const u8 d = kDigit.v[static_cast<u8>(v[i])];
            val = val << 1 | (d & 1);
            xz = xz << 1 | (d >> 1 & 1);
        }
        const size_t n = v.size() - from;
        if (n < width) {
            const u8 d = kDigit.v[static_cast<u8>(v[0])];
            if (d & 2) {
                const u64 high = (width == 64 ? ~0ull : (1ull << width) - 1) & ~((1ull << n) - 1);
                xz |= high;
                if (d & 1) val |= high;
            }
        }
        if (l.kind() == LaneKind::kReal) {
            mem_ += l.add_real(now_, xz ? std::nan("") : static_cast<double>(val));
            return;
        }
        mem_ += l.add_bits(now_, &val, xz ? &xz : nullptr);
        return;
    }
    std::fill(val_.begin(), val_.begin() + words, 0);
    std::fill(xz_.begin(), xz_.begin() + words, 0);
    const size_t n = std::min<size_t>(v.size(), width);
    bool any = false;
    for (size_t b = 0; b < n; ++b) {
        const u8 d = kDigit.v[static_cast<u8>(v[v.size() - 1 - b])];
        val_[b / 64] |= static_cast<u64>(d & 1) << (b % 64);
        xz_[b / 64] |= static_cast<u64>(d >> 1 & 1) << (b % 64);
        any |= (d & 2) != 0;
    }
    if (n < width && (kDigit.v[static_cast<u8>(v[0])] & 2)) {
        const bool one = kDigit.v[static_cast<u8>(v[0])] & 1;
        for (size_t b = n; b < width; ++b) {
            xz_[b / 64] |= 1ull << (b % 64);
            if (one) val_[b / 64] |= 1ull << (b % 64);
        }
        any = true;
    }
    mem_ += l.add_bits(now_, val_.data(), any ? xz_.data() : nullptr);
}

void VcdReader::real(std::string_view v, u32 lane) {
    Lane& l = w_->lanes[lane];
    double d = 0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), d);
    if (r.ec != std::errc() || r.ptr != v.data() + v.size()) {
        if (l.kind() == LaneKind::kText) {
            mem_ += l.add_text(now_, v);
            return;
        }
        note("not a real value: r" + std::string(v.substr(0, 40)));
        return;
    }
    switch (l.kind()) {
        case LaneKind::kReal: mem_ += l.add_real(now_, d); break;
        case LaneKind::kText: mem_ += l.add_text(now_, v); break;
        case LaneKind::kEvent: mem_ += l.add_event(now_); break;
        case LaneKind::kBits: {
            // (A real for a variable of bits: its whole part, as an integer.)
            const u64 bits = std::isfinite(d) ? static_cast<u64>(static_cast<i64>(std::clamp(d, -9.2e18, 9.2e18))) : 0;
            u64 x = std::isfinite(d) ? 0 : ~0ull;
            if (l.width() <= 64) {
                mem_ += l.add_bits(now_, &bits, x ? &x : nullptr);
            } else {
                std::fill(val_.begin(), val_.end(), 0);
                std::fill(xz_.begin(), xz_.end(), x);
                val_[0] = bits;
                mem_ += l.add_bits(now_, val_.data(), x ? xz_.data() : nullptr);
            }
            break;
        }
    }
}

// $dumpoff and $dumpon: the stretch between them kept. The values inside
// them are read as any others: a $dumpoff lists the variables as x (IEEE
// 1364-2005 18.1.3), and they are x until a $dumpon lists them again. (Not
// every variable is listed: Icarus 13 leaves out parameters, which keep
// their values, and so they are not made x here.)
void VcdReader::dumpoff(bool on) {
    std::vector<std::pair<u64, u64>>& off = w_->off;
    if (!on && (off.empty() || off.back().second != ~0ull)) off.push_back({now_, ~0ull});
    else if (on && !off.empty() && off.back().second == ~0ull) off.back().second = now_;
}

WaveLoad::WaveLoad(std::shared_ptr<Wave> into, ReadAt read_at, u64 size, u64 max_bytes, std::function<void()> notify)
    : wave_(std::move(into)), size_(size), notify_(std::move(notify)) {
    thread_ = std::jthread([this, read_at = std::move(read_at), max_bytes](std::stop_token stop) { run(stop, read_at, max_bytes); });
}

WaveLoad::~WaveLoad() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void WaveLoad::wait() {
    if (thread_.joinable()) thread_.join();
}

bool WaveLoad::local_file(const std::string& path, ReadAt* out, u64* size, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0) {
        *error = std::strerror(errno);
        if (fd >= 0) ::close(fd);
        return false;
    }
    *size = static_cast<u64>(st.st_size);
    const auto file = std::shared_ptr<int>(new int(fd), [](int* f) {
        ::close(*f);
        delete f;
    });
    *out = [file](u64 offset, char* buf, size_t len) -> i64 {
        for (;;) {
            const ssize_t n = ::pread(*file, buf, len, static_cast<off_t>(offset));
            if (n >= 0 || errno != EINTR) return n;
        }
    };
    return true;
}

void WaveLoad::run(std::stop_token stop, ReadAt read_at, u64 max_bytes) {
    thread_soon(Soon::kWaitedFor);
    // (Which reader: VCD's, or an ILA capture's CSV, as the first bytes say.)
    std::unique_ptr<VcdReader> vcd;
    std::unique_ptr<IlaCsvReader> csv;
    struct {
        decltype(vcd)& v;
        decltype(csv)& c;
        bool feed(std::string_view s) { return c ? c->feed(s) : v->feed(s); }
        void finish() { c ? c->finish() : v->finish(); }
    } reader{vcd, csv};
    std::vector<char> buf(4 << 20);
    u64 off = 0, told = 0;
    bool header_told = false;
    while (!stop.stop_requested()) {
        const i64 n = read_at(off, buf.data(), buf.size());
        if (n < 0) {
            const int e = errno;
            std::unique_lock lock(wave_->mu);
            if (wave_->error.empty()) wave_->error = std::string("could not read it: ") + std::strerror(e);
            break;
        }
        if (n == 0) break;
        if (!vcd && !csv) {
            if (dump_kind(std::string_view(buf.data(), static_cast<size_t>(n))) == DumpKind::kIlaCsv) csv = std::make_unique<IlaCsvReader>(wave_.get(), max_bytes);
            else vcd = std::make_unique<VcdReader>(wave_.get(), max_bytes);
        }
        off += static_cast<u64>(n);
        read_.store(off, std::memory_order_relaxed);
        const bool more = reader.feed(std::string_view(buf.data(), static_cast<size_t>(n)));
        if (notify_) {
            const u64 now = now_ns();
            if ((!header_told && wave_->header) || now - told >= 100'000'000) {
                header_told = wave_->header;
                told = now;
                notify_();
            }
        }
        if (!more) break;
    }
    if (stop.stop_requested()) {
        std::unique_lock lock(wave_->mu);
        if (wave_->error.empty()) wave_->error = "stopped";
    }
    if (!vcd && !csv) vcd = std::make_unique<VcdReader>(wave_.get(), max_bytes);  // (nothing read: VCD's reader says what that is)
    reader.finish();
    finished_.store(true, std::memory_order_release);
    if (notify_) notify_();
}

}  // namespace blit
