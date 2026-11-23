// An ILA capture's CSV form (wave.hpp, IlaCsvReader), as Vivado 2026.2's
// write_hw_ila_data -csv_file writes it:
//
//   Sample in Buffer,Sample in Window,TRIGGER,count[7:0],wrap,phase[1:0],sum[11:0],sw_q[3:0]
//   Radix - UNSIGNED,UNSIGNED,UNSIGNED,HEX,HEX,HEX,UNSIGNED,BINARY
//   0,0,0,dc,0,3,219,0000
//
// (a probe's value in its radix: HEX, BINARY and OCTAL as digits, as many
// as its width needs or fewer; UNSIGNED and SIGNED in decimal, SIGNED with
// a minus where it is negative.)
#include <algorithm>
#include <mutex>
#include <string>

#include "wave.hpp"

namespace blit {

namespace {

constexpr size_t kSlice = 1 << 20;  // the most read under one hold of the Wave's lock
constexpr u32 kMaxWidth = 1 << 16;  // bits of a probe (an ILA's are at most 4096)

constexpr std::pair<std::string_view, DumpRadix> kRadixes[] = {
    {"BINARY", DumpRadix::kBinary}, {"OCTAL", DumpRadix::kOctal}, {"HEX", DumpRadix::kHex}, {"UNSIGNED", DumpRadix::kUnsigned}, {"SIGNED", DumpRadix::kSigned},
};

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
// false if it is not one, or does not fit.
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

}  // namespace

struct IlaCsvReader::Column {
    u32 lane = 0;
    u32 width = 1;
    DumpRadix radix = DumpRadix::kHex;
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
    if (w_->error.empty() && (!w_->header || !have_)) w_->error = line_ == 0 ? "the capture is empty" : "the capture has no samples";
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
    fields_.clear();
    for (size_t at = 0;;) {
        const size_t comma = l.find(',', at);
        fields_.push_back(l.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        if (comma == std::string_view::npos) break;
        at = comma + 1;
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
        stop(std::move(why));
    };
    if (!l.starts_with("Radix - "))
        return none("an ILA capture's CSV with no radix row (written with -legacy_csv_file): its values cannot be read for sure. Write it with -csv_file, or as VCD");
    fields_[0].remove_prefix(8);
    if (fields_.size() != cols_.size())
        return none("line 2: " + std::to_string(fields_.size()) + " radixes, where the first line names " + std::to_string(cols_.size()) + " columns");
    for (size_t i = 0; i < cols_.size(); ++i) {
        Column& c = cols_[i];
        const auto* r = std::find_if(std::begin(kRadixes), std::end(kRadixes), [&](const auto& k) { return k.first == fields_[i]; });
        if (r == std::end(kRadixes)) return none("line 2: " + c.name + " is in a radix this does not read: " + std::string(fields_[i].substr(0, 20)));
        c.radix = r->second;
        if (c.lane != ~0u) w_->vars[c.lane].radix = c.radix;  // (a probe's lane is its variable's number)
    }
    w_->samples = true;
    w_->header = true;
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
        const Column& c = cols_[i];
        if (c.lane == ~0u) continue;
        if (!parse_value(fields_[i], c.radix, c.width, &val_)) {
            const auto* r = std::find_if(std::begin(kRadixes), std::end(kRadixes), [&](const auto& k) { return k.second == c.radix; });
            return stop(at + c.name + ": not a " + std::string(r->first) + " value of " + std::to_string(c.width) + (c.width == 1 ? " bit: " : " bits: ") +
                        std::string(fields_[i].substr(0, 40)));
        }
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
