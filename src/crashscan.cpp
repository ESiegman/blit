// Crash dumps decoded in place (crashscan.hpp). The dumps' text, as their
// sources print it:
//
// ESP-IDF v6.1.1 (components/esp_system):
//   panic.c:361-366         "Guru Meditation Error: Core " panic_print_dec(core)
//                           (" 0": two digits, a space for the tens)
//                           " panic'ed (" reason "). " description "\r\n"
//   port/arch/xtensa/panic_arch.c:41-58
//                           "Core  0 register dump:", then "PC      : 0x%08x  PS ..."
//                           four to a line
//   port/arch/xtensa/debug_helpers.c:38-46, 71, 88-93
//                           "\r\n\r\nBacktrace:" then " 0x%08x:0x%08x" a frame
//                           (PC:SP, the PC already made a call's address,
//                           esp_cpu_process_stack_pc), then " |<-CORRUPTED" or
//                           " |<-CONTINUES" when it stops short, "\r\n\r\n"
//   port/arch/riscv/panic_arch.c:153-184
//                           "Core  0 register dump:" then "\r\nMEPC    : 0x%08x  RA      : 0x%08x  SP ..."
//   fp_unwind.c:108, eh_frame_parser.c:908
//                           RISC-V's backtrace, where one is built in: "Backtrace:" and
//                           the same " 0x%08x:0x%08x" frames
//   panic.c:428             "\r\nELF file SHA256: " and the hash; panic.c:460, 486 "Rebooting..."
// components/esp_libc/src/abort.c:16-17
//                           "abort() was called at PC 0x" (itoa: lower case, no
//                           leading zeros) " on core " core
#include "crashscan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "match.hpp"

namespace blit {

namespace {

constexpr std::string_view kDim = "\x1b[2m", kPlain = "\x1b[0m";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "0x" and hexadecimal digits at `at` (at most 16 of them): the number,
// and where it ends; false if there is none there.
bool hex_at(std::string_view s, size_t at, u64* v, size_t* end) {
    if (at + 2 >= s.size() || s[at] != '0' || (s[at + 1] != 'x' && s[at + 1] != 'X') || hex_digit(s[at + 2]) < 0) return false;
    u64 n = 0;
    size_t i = at + 2, digits = 0;
    for (; i < s.size() && hex_digit(s[i]) >= 0; ++i, ++digits) n = n << 4 | static_cast<u64>(hex_digit(s[i]));
    if (digits > 16) return false;
    *v = n;
    *end = i;
    return true;
}

// The number after `key` in a line ("MEPC    : 0x42005d6a"): the first
// "0x" after it, with only spaces and a colon between.
bool hex_after(std::string_view s, std::string_view key, u64* v) {
    const size_t k = s.find(key);
    if (k == std::string_view::npos) return false;
    size_t at = k + key.size();
    while (at < s.size() && (s[at] == ' ' || s[at] == ':')) ++at;
    size_t end;
    return hex_at(s, at, v, &end);
}

// Every "0x..." in a piece of text that is not part of a longer word.
std::vector<u64> hexes(std::string_view s) {
    std::vector<u64> out;
    for (size_t at = s.find("0x"); at != std::string_view::npos && out.size() < CrashScan::kMaxAddresses; at = s.find("0x", at + 1)) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) continue;
        u64 v;
        size_t end;
        if (hex_at(s, at, &v, &end)) {
            out.push_back(v);
            at = end - 1;
        }
    }
    return out;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string base_name(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

}  // namespace

CrashScan::CrashScan(CrashOptions o) : o_(std::move(o)) {
    while (o_.root.size() > 1 && o_.root.ends_with('/')) o_.root.pop_back();
}

bool CrashScan::pattern_ok(std::string_view pattern, std::string* why) {
    if (pattern.empty()) return *why = "an empty pattern", false;
    return lua_pattern_check(pattern, why) >= 0;
}

bool CrashScan::decodes_itself(std::string_view command) {
    const bool idf = command.find("idf.py") != std::string_view::npos && command.find("monitor") != std::string_view::npos;
    return idf || command.find("esp_idf_monitor") != std::string_view::npos || command.find("esp-idf-monitor") != std::string_view::npos;
}

const char* CrashScan::gather(const char* p, const char* const e) {
    while (p < e) {
        if (esc_ == kText) {
            const char* q = p;
            while (q < e && static_cast<u8>(*q) >= 0x20 && *q != 0x7F) ++q;
            if (q > p) {
                if (cr_) line_.clear(), cr_ = false;  // (a line written over)
                if (line_.size() < kMaxLine) line_.append(p, std::min<size_t>(static_cast<size_t>(q - p), kMaxLine - line_.size()));
                p = q;
                continue;
            }
            const char c = *p;
            if (c == '\n') return p;
            ++p;
            if (c == '\r') {
                cr_ = true;
            } else if (c == '\x1b') {
                esc_ = kEsc;
            } else if (c == '\t') {
                if (cr_) line_.clear(), cr_ = false;
                if (line_.size() < kMaxLine) line_ += ' ';
            }
            continue;
        }
        const u8 c = static_cast<u8>(*p++);
        switch (esc_) {
            case kEsc: esc_ = c == '[' ? kCsi : (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') ? kString : (c >= 0x20 && c <= 0x2F) ? kEscMore : kText; break;
            case kEscMore: if (c >= 0x30) esc_ = kText; break;
            case kCsi: if (c >= 0x40 && c <= 0x7E) esc_ = kText; break;
            case kString:
                if (c == 0x07) esc_ = kText;
                else if (c == 0x1B) esc_ = kStringEsc;
                break;
            case kStringEsc: esc_ = c == '\\' ? kText : kString; break;
            default: esc_ = kText; break;
        }
    }
    return e;
}

void CrashScan::feed(std::string_view bytes, std::vector<Insert>* out) {
    const char* const begin = bytes.data();
    const char* const e = begin + bytes.size();
    for (const char* p = begin; (p = gather(p, e)) < e;) {
        ++p;  // (the LF)
        cr_ = false;
        adding_.clear();
        line(line_, &adding_);
        line_.clear();
        if (adding_.empty()) continue;
        // Each dim, from the line's start whatever the program left the cursor at.
        std::string text = "\r";
        for (const std::string& a : adding_) text.append(kDim).append(a).append(kPlain).append("\r\n");
        out->push_back({static_cast<size_t>(p - begin), std::move(text)});
    }
}

void CrashScan::take_line(std::string_view raw, std::vector<std::string>* add) {
    const char* const e = raw.data() + raw.size();
    for (const char* p = raw.data(); (p = gather(p, e)) < e;) ++p;  // (an LF in it is none of the line's)
    cr_ = false;
    line(line_, add);
    line_.clear();
}

void CrashScan::finish() {
    if (!line_.empty()) {
        std::vector<std::string> none;  // (nothing more is written: what it would add has nowhere to go)
        const bool quiet = o_.quiet;
        o_.quiet = true;
        line(line_, &none);
        o_.quiet = quiet;
        line_.clear();
    }
    end();
}

const AddrSource* CrashScan::source() {
    if (!asked_) {
        asked_ = true;
        if (o_.elf) src_ = o_.elf(&why_);
        if (!src_ && why_.empty()) why_ = "no ELF to decode it with";
    }
    return src_.get();
}

bool CrashScan::own(const std::string& path) const {
    std::string_view root = o_.root;
    if (root.starts_with("ssh://")) {  // (its path there)
        const size_t slash = root.find('/', 6);
        root = slash == std::string_view::npos ? std::string_view() : root.substr(slash);
    }
    if (root.empty() || path.size() <= root.size() || !path.starts_with(root) || path[root.size()] != '/') return false;
    const std::string_view rest = std::string_view(path).substr(root.size());
    for (const std::string_view not_own : {"/build/", "/managed_components/", "/.pio/", "/_deps/"})
        if (rest.find(not_own) != std::string_view::npos) return false;
    return true;
}

void CrashScan::begin(u8 kind, std::string reason) {
    end();
    dump_ = kind;
    reason_ = std::move(reason);
    ++dumps_;
}

// A dump done: its problem, at the innermost frame of the program's own code
// (else the innermost with a place at all).
void CrashScan::end() {
    if (dump_ == kNone) return;
    const u8 kind = dump_;
    dump_ = kNone;
    backtrace_ = false;
    dump_lines_ = 0;
    std::vector<SourceFrame> frames = std::move(frames_);
    frames_.clear();
    const std::string reason = std::move(reason_);
    reason_.clear();
    const SourceFrame* at = nullptr;
    for (const SourceFrame& f : frames)
        if (f.line && own(f.path)) {
            at = &f;
            break;
        }
    if (!at)
        for (const SourceFrame& f : frames)
            if (f.line && !f.path.empty()) {
                at = &f;
                break;
            }
    if (!at) return;
    std::string prefix;  // ("ssh://host" for a project there: its paths are paths there)
    if (o_.root.starts_with("ssh://")) prefix = o_.root.substr(0, o_.root.find('/', 6));
    auto full = [&](const std::string& path) { return !prefix.empty() && path.starts_with('/') ? prefix + path : path; };
    BuildProblem p;
    p.path = full(at->path);
    p.line = at->line;
    p.col = at->col;
    p.severity = 1;
    p.code = !reason.empty() ? reason : kind == kOwn ? "panic" : "";
    p.message = "Crash at " + base_name(at->path) + ":" + std::to_string(at->line) + (at->function.empty() ? std::string() : " in " + at->function) +
                (reason.empty() ? std::string() : " (" + reason + ")");
    for (const SourceFrame& f : frames) {
        if (p.notes.size() >= BuildScan::kMaxNotes) break;
        BuildNote n;
        n.path = f.line ? full(f.path) : std::string();
        n.line = f.line;
        n.col = f.col;
        n.message = (f.function.empty() ? std::string("??") : f.function) + (f.inlined ? " (inlined)" : "");
        if (!f.line && !f.path.empty()) n.message += " in " + f.path;
        p.notes.push_back(std::move(n));
    }
    const std::string key = p.path + '\n' + std::to_string(p.line) + '\n' + p.message + '\n' + p.code;
    p.count = ++seen_[key];
    if (found_.size() < BuildScan::kMaxProblems) found_.push_back(std::move(p));
}

void CrashScan::decode(const std::vector<u64>& addrs, std::string_view label, std::vector<std::string>* add, bool keep) {
    if (addrs.empty()) return;
    const AddrSource* src = source();
    if (!src) {
        if (!said_ && !o_.quiet) {
            said_ = true;
            add->push_back("Not decoded: " + why_);
        }
        return;
    }
    const u32 width = src->elf().address_size() * 2;
    for (u64 a : addrs) {
        std::vector<SourceFrame> frames = src->lookup(a);
        if (frames.empty()) continue;  // (not in the program: the ROM's, a stack address)
        if (!o_.quiet) {
            char head[40];
            std::snprintf(head, sizeof head, "0x%0*llx: ", static_cast<int>(width), static_cast<unsigned long long>(a));
            for (size_t i = 0; i < frames.size(); ++i) {
                std::string text;
                if (i == 0) {
                    if (!label.empty()) text.append(label).append(" ");
                    text += head;
                }
                text += frame_words(frames[i], i > 0);
                add->push_back(std::move(text));
            }
        }
        if (keep && frames_.size() < BuildScan::kMaxNotes) frames_.insert(frames_.end(), frames.begin(), frames.end());
    }
}

void CrashScan::line(std::string_view text, std::vector<std::string>* add) {
    if (dump_ != kNone && ++dump_lines_ > kMaxDumpLines) end();
    const bool hex = text.find("0x") != std::string_view::npos;

    // The program's own format (lines one after another are one dump).
    bool taken = false;
    for (const std::string& pat : o_.patterns) {
        std::string_view caps[kMaxLuaCaptures];
        const int n = lua_find(text, pat, caps);
        if (n < 0) continue;
        taken = true;
        if (dump_ != kOwn) begin(kOwn, {});
        decode(hexes(n > 0 ? caps[0] : text), {}, add, true);
        break;
    }
    if (taken) return;
    if (dump_ == kOwn) end();

    // A program that decodes its own dumps (idf.py monitor, PlatformIO's
    // esp32_exception_decoder): its lines after a dump's mean the dumps
    // after this one are not decoded again here.
    if (dump_ != kNone && hex && !o_.quiet) {
        std::string_view t = trimmed(text);
        if (t.starts_with("--- ")) t.remove_prefix(4);
        if (t.starts_with('#')) {  // ("#0  0x400d1234:0x3ffb1230 in f at path:line")
            const size_t sp = t.find(' ');
            t = sp == std::string_view::npos ? std::string_view() : trimmed(t.substr(sp));
        }
        u64 v;
        size_t e;
        if (hex_at(t, 0, &v, &e) && e < t.size() && (t[e] == ':') && t.find(" at ", e) != std::string_view::npos) {
            o_.quiet = true;
            return;
        }
    }

    // ESP-IDF's.
    if (const size_t g = text.find("Guru Meditation Error"); g != std::string_view::npos) {
        std::string reason;
        if (const size_t open = text.find("panic'ed (", g); open != std::string_view::npos) {
            const size_t close = text.find(')', open + 10);
            if (close != std::string_view::npos) reason = std::string(text.substr(open + 10, close - open - 10));
        }
        begin(kEsp, std::move(reason));
        return;
    }
    if (!hex) {
        if (dump_ == kEsp && (text.find("ELF file SHA256") != std::string_view::npos || text.find("Rebooting") != std::string_view::npos ||
                              text.find("Stack memory:") != std::string_view::npos || (backtrace_ && trimmed(text).empty())))
            end();
        return;
    }
    if (const size_t a = text.find("abort() was called at PC "); a != std::string_view::npos) {
        u64 v;
        size_t e;
        if (dump_ == kNone) begin(kEsp, "abort()");
        if (hex_at(text, a + 25, &v, &e)) decode({v}, {}, add, true);
        return;
    }
    if (const size_t b = text.find("Backtrace:"); b != std::string_view::npos) {
        // Each "PC:SP" pair's PC; " |<-CORRUPTED" ends what can be trusted.
        std::string_view rest = text.substr(b + 10);
        if (const size_t bad = rest.find("|<-"); bad != std::string_view::npos) rest = rest.substr(0, bad);
        std::vector<u64> pcs;
        for (size_t at = 0; at < rest.size() && pcs.size() < kMaxAddresses;) {
            while (at < rest.size() && rest[at] == ' ') ++at;
            u64 pc, sp;
            size_t e, e2;
            if (!hex_at(rest, at, &pc, &e)) break;
            pcs.push_back(pc);
            at = e;
            if (at < rest.size() && rest[at] == ':' && hex_at(rest, at + 1, &sp, &e2)) at = e2;
        }
        const bool in_dump = dump_ == kEsp;
        if (in_dump) frames_.clear();  // (the backtrace says more than the registers did)
        decode(pcs, {}, add, in_dump);
        backtrace_ = in_dump;
        return;
    }
    if (dump_ == kEsp && text.find("MEPC") != std::string_view::npos) {  // RISC-V's register dump: where it stopped, and where it was called from
        u64 mepc, ra;
        if (hex_after(text, "MEPC", &mepc)) decode({mepc}, "MEPC", add, !backtrace_);
        if (hex_after(text, "RA ", &ra) || hex_after(text, "RA:", &ra)) decode({ra}, "RA", add, !backtrace_);
    }
}

}  // namespace blit
