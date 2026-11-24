// An ILA capture's CSV form (wave.hpp, IlaCsvReader), as Vivado 2026.2's
// write_hw_ila_data -csv_file writes it:
//
//   Sample in Buffer,Sample in Window,TRIGGER,count[7:0],wrap,phase[1:0],sum[11:0],sw_q[3:0]
//   Radix - UNSIGNED,UNSIGNED,UNSIGNED,HEX,HEX,HEX,UNSIGNED,BINARY
//   0,0,0,dc,0,3,219,0000
//
// (a probe's value in its radix: HEX, BINARY and OCTAL as digits, as many
// as its width needs or fewer; UNSIGNED and SIGNED in decimal, SIGNED with
// a minus where it is negative.) With probes in ASCII, and one given names
// for its values (tests/wave/ila_radix.txt):
//
//   Sample in Buffer,Sample in Window,TRIGGER,count[7:0],ch[7:0],pair[15:0],odd[11:0],state[2:0]
//   Radix - UNSIGNED,UNSIGNED,UNSIGNED,HEX,ASCII,ASCII,ASCII,HEX
//   7,7,0,e3,","," ",",","SHIFT"
//   33,33,0,fd,"","","",7
//
// (ASCII: the lowest byte as a character in quotes, "" where it is not
// printable, a quote or a comma in it as it is; a value with a name as
// that name in quotes, whatever the radix row says, and one with none in
// that radix.)
#include <algorithm>
#include <mutex>
#include <string>

#include "wave.hpp"

namespace blit {

namespace {

constexpr size_t kSlice = 1 << 20;  // the most read under one hold of the Wave's lock
constexpr u32 kMaxWidth = 1 << 16;  // bits of a probe (an ILA's are at most 4096)

constexpr std::pair<std::string_view, DumpRadix> kRadixes[] = {
    {"BINARY", DumpRadix::kBinary}, {"OCTAL", DumpRadix::kOctal},   {"HEX", DumpRadix::kHex},
    {"UNSIGNED", DumpRadix::kUnsigned}, {"SIGNED", DumpRadix::kSigned}, {"ASCII", DumpRadix::kAscii},
};

std::string_view radix_name(DumpRadix r) {
    const auto* k = std::find_if(std::begin(kRadixes), std::end(kRadixes), [&](const auto& e) { return e.second == r; });
    return k == std::end(kRadixes) ? std::string_view("NONE") : k->first;
}

bool parse_u64(std::string_view s, u64* out) {
    if (s.empty() || s.size() > 19) return false;
    u64 n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<u64>(c - '0');
    }
    *out = n;
    return true;
}

// "count[7:0]" as its name, msb and lsb; "wrap" as itself.
void split_range(std::string_view s, WaveVar* v) {
    v->name = std::string(s);
    const size_t open = s.rfind('[');
    if (open == std::string_view::npos || open == 0 || !s.ends_with(']')) return;
    const std::string_view in = s.substr(open + 1, s.size() - open - 2);
    const size_t colon = in.find(':');
    u64 msb = 0, lsb = 0;
    if (colon == std::string_view::npos ? !parse_u64(in, &msb) : !parse_u64(in.substr(0, colon), &msb) || !parse_u64(in.substr(colon + 1), &lsb)) return;
    if (colon == std::string_view::npos) lsb = msb;
    if (msb > kMaxWidth || lsb > kMaxWidth) return;
    v->name.resize(open);
    v->ranged = true;
    v->msb = static_cast<i32>(msb), v->lsb = static_cast<i32>(lsb);
    v->width = static_cast<u32>(msb > lsb ? msb - lsb : lsb - msb) + 1;
}

// A value in its radix into `width` bits (words of 64, the lowest first);
// false if it is not one, or does not fit. (Not ASCII: see sample().)
bool parse_value(std::string_view s, DumpRadix radix, u32 width, std::vector<u64>* out) {
    const size_t words = (width + 63) / 64;
    out->assign(words, 0);
    if (s.empty()) return false;
    auto fits = [&] {  // (nothing above the width)
        const u32 top = width % 64;
        return top == 0 || ((*out)[words - 1] >> top) == 0;
    };
    // Shifts the value up by `by` bits and adds d; false if bits fall off the top.
    auto push = [&](u32 by, u64 d) {
        if ((*out)[words - 1] >> (64 - by)) return false;
        for (size_t i = words; i-- > 0;) (*out)[i] = (*out)[i] << by | (i ? (*out)[i - 1] >> (64 - by) : 0);
        (*out)[0] |= d;
        return fits();
    };
    switch (radix) {
        case DumpRadix::kBinary: case DumpRadix::kOctal: case DumpRadix::kHex: {
            const u32 bits = radix == DumpRadix::kBinary ? 1 : radix == DumpRadix::kOctal ? 3 : 4;
            for (const char c : s) {
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                if (d >= (1 << bits) || !push(bits, static_cast<u64>(d))) return false;
            }
            return true;
        }
        case DumpRadix::kUnsigned: case DumpRadix::kSigned: {
            const bool minus = radix == DumpRadix::kSigned && s.front() == '-';
            if (minus) s.remove_prefix(1);
            if (s.empty()) return false;
            for (const char c : s) {
                if (c < '0' || c > '9') return false;
                // value * 10 + d, word by word.
                u64 carry = static_cast<u64>(c - '0');
                for (u64& w : *out) {
                    const unsigned __int128 x = static_cast<unsigned __int128>(w) * 10 + carry;
                    w = static_cast<u64>(x);
                    carry = static_cast<u64>(x >> 64);
                }
                if (carry || !fits()) return false;
            }
            if (radix == DumpRadix::kSigned) {
                // In two's complement: as large as 2^(width-1) - 1, or 2^(width-1) negated.
                const u64 sign = (*out)[(width - 1) / 64] >> ((width - 1) % 64) & 1;
                bool exactly = sign;  // (the value is 2^(width-1) itself)
                for (u32 b = 0; b + 1 < width && exactly; ++b) exactly = !((*out)[b / 64] >> (b % 64) & 1);
                if (sign && !(minus && exactly)) return false;
                if (minus) {  // negated: inverted, plus 1, within the width
                    u64 carry = 1;
                    for (u64& w : *out) {
                        w = ~w + carry;
                        carry = carry && w == 0;
                    }
                    if (width % 64) (*out)[words - 1] &= (1ull << (width % 64)) - 1;
                }
            }
            return true;
        }
        default: return false;
    }
}

// Change i of a probe's lane as Vivado writes it in `radix`: digits, as
// many as the width needs; decimal; an ASCII character ("" for x).
std::string vivado_text(const Lane& lane, u64 i, DumpRadix radix) {
    const u32 width = lane.width();
    if (radix == DumpRadix::kAscii) {
        const u64 c = lane.bits(i, 0, std::min<u32>(width, 8));
        return lane.xz(i, 0, std::min<u32>(width, 8)) ? std::string() : std::string(1, static_cast<char>(c));
    }
    if (radix == DumpRadix::kBinary || radix == DumpRadix::kOctal || radix == DumpRadix::kHex) {
        const u32 bits = radix == DumpRadix::kBinary ? 1 : radix == DumpRadix::kOctal ? 3 : 4;
        std::string out;
        for (u32 at = (width + bits - 1) / bits * bits; at > 0; at -= bits) {
            const u32 from = at - bits, n = std::min(bits, width - from);
            out += "0123456789abcdef"[lane.bits(i, from, n)];
        }
        return out;
    }
    // Decimal: the value divided by ten again and again (two's complement negated first, if signed and negative).
    std::vector<u64> v((width + 63) / 64);
    for (u32 b = 0; b < width; b += 64) v[b / 64] = lane.bits(i, b, std::min<u32>(64, width - b));
    const bool minus = radix == DumpRadix::kSigned && (v[(width - 1) / 64] >> ((width - 1) % 64) & 1);
    if (minus) {
        u64 carry = 1;
        for (u64& w : v) {
            w = ~w + carry;
            carry = carry && w == 0;
        }
        if (width % 64) v.back() &= (1ull << (width % 64)) - 1;
    }
    std::string out;
    do {
        u64 rem = 0;
        for (size_t k = v.size(); k-- > 0;) {
            const unsigned __int128 x = static_cast<unsigned __int128>(rem) << 64 | v[k];
            v[k] = static_cast<u64>(x / 10);
            rem = static_cast<u64>(x % 10);
        }
        out += static_cast<char>('0' + rem);
    } while (std::any_of(v.begin(), v.end(), [](u64 w) { return w != 0; }));
    if (minus) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

struct IlaCsvReader::Column {
    u32 lane = 0;
    u32 width = 1;
    DumpRadix radix = DumpRadix::kHex;
    bool names = false;  // its values as written (some by their names): a lane of text
    std::string name;  // as the first line has it
};

IlaCsvReader::IlaCsvReader(Wave* into, u64 max_bytes) : w_(into), max_(max_bytes) {}

IlaCsvReader::~IlaCsvReader() = default;

void IlaCsvReader::stop(std::string why) {
    if (stopped_) return;
    stopped_ = true;
    if (w_->error.empty()) w_->error = std::move(why);
}

bool IlaCsvReader::feed(std::string_view bytes) {
    while (!stopped_ && !bytes.empty()) {
        const size_t n = std::min(bytes.size(), kSlice);
        std::unique_lock lock(w_->mu);
        std::string_view in = bytes.substr(0, n);
        bytes.remove_prefix(n);
        while (!stopped_) {
            const size_t nl = in.find('\n');
            if (nl == std::string_view::npos) {
                carry_.append(in);
                if (carry_.size() > (1 << 20)) stop("line " + std::to_string(line_ + 1) + " is longer than an ILA capture's are");
                break;
            }
            if (carry_.empty()) {
                line(in.substr(0, nl));
            } else {
                carry_.append(in.substr(0, nl));
                const std::string l = std::move(carry_);
                carry_.clear();
                line(l);
            }
            in.remove_prefix(nl + 1);
        }
        w_->reached = w_->end;
    }
    return !stopped_;
}

void IlaCsvReader::finish() {
    std::unique_lock lock(w_->mu);
    if (!stopped_ && !carry_.empty()) line(carry_);
    carry_.clear();
    if (w_->error.empty() && (cols_.empty() || !have_)) w_->error = line_ == 0 ? "the capture is empty" : "the capture has no samples";
    w_->header = true;
    u64 changes = 0;
    for (Lane& l : w_->lanes) {
        l.shrink();
        changes += l.size();
    }
    w_->changes = changes;
    w_->reached = w_->end;
    w_->done = true;
    stopped_ = true;
}

void IlaCsvReader::line(std::string_view l) {
    ++line_;
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    // Its fields, at the commas: a quoted one (an ASCII character, a value's
    // name) to the first quote after its own that a comma or the line's end
    // follows, as Vivado escapes neither (`"""` is a quote, `","` a comma).
    fields_.clear();
    for (size_t at = 0;;) {
        size_t end = l.find(',', at);
        if (at < l.size() && l[at] == '"')
            for (size_t q = at + 1; q < l.size(); ++q)
                if (l[q] == '"' && (q + 1 == l.size() || l[q + 1] == ',')) {
                    end = q + 1 == l.size() ? std::string_view::npos : q + 1;
                    break;
                }
        fields_.push_back(l.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    if (line_ == 1) header(l);
    else if (line_ == 2) radixes(l);
    else if (!l.empty()) sample(l);
}

// The first line: the columns' names.
void IlaCsvReader::header(std::string_view l) {
    if (fields_[0] != "Sample in Buffer") return stop("not an ILA capture's CSV: it starts with " + std::string(l.substr(0, 40)));
    for (const std::string_view f : fields_)
        if (f.empty()) return stop("line 1: a column with no name");
    Wave& w = *w_;
    WaveScope dut;
    dut.name = "dut";  // (as its VCD form names it)
    w.scopes.push_back(std::move(dut));
    w.scopes[0].scopes.push_back(1);
    for (size_t i = 0; i < fields_.size(); ++i) {
        const std::string_view f = fields_[i];
        Column c;
        c.name = std::string(f);
        if (f == "Sample in Buffer" || f == "Sample in Window" || f == "TRIGGER") {
            (f == "Sample in Buffer" ? in_buffer_ : f == "Sample in Window" ? in_window_ : trigger_) = static_cast<i32>(i);
            c.lane = ~0u;
            cols_.push_back(std::move(c));
            continue;
        }
        WaveVar v;
        v.kind = VarKind::kReg;
        split_range(f, &v);
        v.code = std::to_string(i);
        v.scope = 1;
        v.lane = static_cast<u32>(w.lanes.size());
        c.lane = v.lane;
        c.width = v.width;
        w.lanes.emplace_back(LaneKind::kBits, v.width);
        mem_ += sizeof(Lane) + sizeof v + v.name.size() + v.code.size() + 8;
        w.scopes[1].vars.push_back(static_cast<u32>(w.vars.size()));
        w.vars.push_back(std::move(v));
        cols_.push_back(std::move(c));
    }
    if (max_ && mem_ > max_) stop("the capture's probes take more memory than timeline.max_mb allows");
}

// The second: each column's radix.
void IlaCsvReader::radixes(std::string_view l) {
    // (Its columns are no use without their radixes: none are kept.)
    auto none = [&](std::string why) {
        w_->scopes.resize(1);
        w_->scopes[0].scopes.clear();
        w_->vars.clear();
        w_->lanes.clear();
        cols_.clear();
        stop(std::move(why));
    };
    if (!l.starts_with("Radix - "))
        return none("no radix row (-legacy_csv_file): write it with -csv_file");
    fields_[0].remove_prefix(8);
    if (fields_.size() != cols_.size())
        return none("line 2: " + std::to_string(fields_.size()) + " radixes, where the first line names " + std::to_string(cols_.size()) + " columns");
    for (size_t i = 0; i < cols_.size(); ++i) {
        Column& c = cols_[i];
        const auto* r = std::find_if(std::begin(kRadixes), std::end(kRadixes), [&](const auto& k) { return k.first == fields_[i]; });
        if (r == std::end(kRadixes)) return none("line 2: " + c.name + " is in a radix this does not read: " + std::string(fields_[i].substr(0, 20)));
        c.radix = r->second;
        if (c.lane == ~0u) continue;
        WaveVar& v = w_->vars[c.lane];  // (a probe's lane is its variable's number)
        v.radix = c.radix;
        if (c.radix == DumpRadix::kAscii && c.width > 8) {
            // (ASCII writes no more than its lowest byte: the probe is read as that.)
            c.width = v.width = 8;
            v.msb = v.msb > v.lsb ? v.lsb + 7 : v.lsb - 7;
            w_->lanes[c.lane] = Lane(LaneKind::kBits, 8);
        }
    }
    w_->samples = true;
}

// A probe's values as written from now on, and those read before (in its
// radix, as Vivado wrote them): its first value given by its name has come.
void IlaCsvReader::to_names(Column& c) {
    Wave& w = *w_;
    WaveVar& v = w.vars[c.lane];
    split_range(c.name, &v);  // (all of it, if ASCII made it its lowest byte)
    v.kind = VarKind::kEnum;
    Lane text(LaneKind::kText, 0);
    const Lane& was = w.lanes[c.lane];
    for (u64 i = 0; i < was.size(); ++i) mem_ += text.add_text(was.time(i), vivado_text(was, i, c.radix));
    mem_ -= std::min<u64>(mem_, was.bytes());
    w.lanes[c.lane] = std::move(text);
    c.width = v.width;
    c.names = true;
}

// A sample: its number in the buffer is its time.
void IlaCsvReader::sample(std::string_view) {
    Wave& w = *w_;
    const std::string at = "line " + std::to_string(line_) + ": ";
    if (fields_.size() != cols_.size())
        return stop(at + std::to_string(fields_.size()) + " values, where the first line names " + std::to_string(cols_.size()) + " columns");
    u64 t = 0;
    if (!parse_u64(fields_[static_cast<size_t>(in_buffer_)], &t)) return stop(at + "not a sample's number: " + std::string(fields_[static_cast<size_t>(in_buffer_)].substr(0, 20)));
    if (have_ && t <= last_) return stop(at + "sample " + std::to_string(t) + " after sample " + std::to_string(last_));
    for (size_t i = 0; i < cols_.size(); ++i) {
        Column& c = cols_[i];
        if (c.lane == ~0u) continue;
        const std::string_view f = fields_[i];
        const bool quoted = f.size() >= 2 && f.front() == '"' && f.back() == '"';
        const std::string_view in = quoted ? f.substr(1, f.size() - 2) : f;
        auto wrong = [&] {
            stop(at + c.name + ": not a" + (c.radix == DumpRadix::kAscii ? "n " : " ") + std::string(radix_name(c.radix)) + " value of " + std::to_string(c.width) +
                 (c.width == 1 ? " bit: " : " bits: ") + std::string(f.substr(0, 40)));
        };
        // A name: quoted, and not an ASCII character.
        if (!c.names && quoted && (c.radix != DumpRadix::kAscii || in.size() > 1) && !in.empty()) to_names(c);
        if (c.names) {
            // (One with no name is still in its radix: read so, to know it is one.)
            if (!quoted && (c.radix == DumpRadix::kAscii || !parse_value(f, c.radix, c.width, &val_))) return wrong();
            mem_ += w.lanes[c.lane].add_text(t, in);
            continue;
        }
        if (c.radix == DumpRadix::kAscii) {
            // A printable character: its byte; none: not known (x), as Vivado leaves it out.
            const size_t words = (c.width + 63) / 64;
            val_.assign(words, 0);
            xz_.assign(words, 0);
            if (!quoted || (!in.empty() && (in[0] < 0x20 || in[0] > 0x7E || (c.width < 8 && static_cast<u8>(in[0]) >> c.width)))) return wrong();
            if (in.empty()) {
                val_[0] = xz_[0] = c.width >= 64 ? ~0ull : (1ull << c.width) - 1;
            } else {
                val_[0] = static_cast<u8>(in[0]);
            }
            mem_ += w.lanes[c.lane].add_bits(t, val_.data(), xz_.data());
            continue;
        }
        if (!parse_value(f, c.radix, c.width, &val_)) return wrong();
        mem_ += w.lanes[c.lane].add_bits(t, val_.data(), nullptr);
    }
    if (in_window_ >= 0 && have_ && fields_[static_cast<size_t>(in_window_)] == "0") w.windows.push_back(t);
    if (trigger_ >= 0 && fields_[static_cast<size_t>(trigger_)] == "1") w.triggers.push_back(t);
    if (!have_) w.start = t;
    have_ = true;
    last_ = w.end = t;
    if (max_ && mem_ > max_) stop("the capture needs more memory than timeline.max_mb allows: read up to sample " + std::to_string(t));
}

}  // namespace blit
