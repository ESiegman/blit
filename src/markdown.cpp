#include "markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

#include "utf8.hpp"

namespace blit {

namespace {

// ---------------------------------------------------------------- text

u32 cols_of(std::string_view s) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += cp_width(d.cp);
        i += d.len ? d.len : 1;
    }
    return w;
}

// The bytes of the first character of s (at least 1 if there is any).
size_t first_char(std::string_view s) {
    if (s.empty()) return 0;
    const size_t len = decode_utf8(reinterpret_cast<const u8*>(s.data()), s.size()).len;
    return std::clamp<size_t>(len, 1, s.size());
}

// The bytes of s that fit in n columns.
size_t fit(std::string_view s, u32 n) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    size_t i = 0;
    while (i < s.size()) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        const u32 cw = cp_width(d.cp);
        if (w + cw > n) break;
        w += cw;
        i += d.len ? d.len : 1;
    }
    return i;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool blank(std::string_view s) { return trim(s).empty(); }

// Columns of white space in front (a tab to the next multiple of 4).
int indent_of(std::string_view s) {
    int n = 0;
    for (const char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n = (n / 4 + 1) * 4;
        else break;
    }
    return n;
}

// The line without up to n columns of white space in front.
std::string strip_cols(std::string_view s, int n) {
    int at = 0;
    size_t i = 0;
    while (i < s.size() && at < n && (s[i] == ' ' || s[i] == '\t')) {
        const int next = s[i] == '\t' ? (at / 4 + 1) * 4 : at + 1;
        if (next > n) {  // (a tab only partly taken: the rest of it as spaces)
            std::string out(static_cast<size_t>(next - n), ' ');
            return out + std::string(s.substr(i + 1));
        }
        at = next;
        ++i;
    }
    return std::string(s.substr(i));
}

// The line from column n on, whatever is before it (a list item's marker).
std::string from_col(std::string_view s, int n) {
    int at = 0;
    size_t i = 0;
    while (i < s.size() && at < n) {
        if (s[i] == '\t') {
            const int next = (at / 4 + 1) * 4;
            if (next > n) return std::string(static_cast<size_t>(next - n), ' ') + std::string(s.substr(i + 1));
            at = next;
        } else {
            ++at;
        }
        ++i;
    }
    return std::string(s.substr(i));
}

// Tabs as spaces. With `inks` (one for each byte of s), *out_inks gets one
// for each byte of what comes back (a tab's for its spaces).
std::string expand_tabs(std::string_view s, const u8* inks = nullptr, std::vector<u8>* out_inks = nullptr) {
    std::string out;
    u32 col = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\t') {
            const u32 to = (col / 4 + 1) * 4;
            out.append(to - col, ' ');
            if (inks) out_inks->insert(out_inks->end(), to - col, inks[i]);
            col = to;
        } else {
            out += c;
            if (inks) out_inks->push_back(inks[i]);
            if (!is_cont(static_cast<u8>(c))) ++col;
        }
    }
    return out;
}

void encode_utf8(u32 cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------- inline

struct Span {
    std::string text;
    u8 style = 0, ink = kMdText, tone = kMdNoTone;
    std::string link;
    bool brk = false;  // a line break, not text
    std::string image;          // a picture's file (the text is what stands for it, if it cannot be drawn)
    u32 image_w = 0, image_h = 0;  // the size it asks for (<img width=...>; 0: none)
};

// The value of an HTML tag's attribute (`name="v"`, `name='v'`, `name=v`), or "".
std::string attribute(std::string_view tag, std::string_view name) {
    for (size_t at = 0; (at = tag.find(name, at)) != std::string_view::npos; at += name.size()) {
        const bool starts = at > 0 && (tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n');
        size_t k = at + name.size();
        while (k < tag.size() && tag[k] == ' ') ++k;
        if (!starts || k >= tag.size() || tag[k] != '=') continue;
        ++k;
        while (k < tag.size() && tag[k] == ' ') ++k;
        if (k >= tag.size()) return {};
        if (tag[k] == '"' || tag[k] == '\'') {
            const size_t end = tag.find(tag[k], k + 1);
            return std::string(tag.substr(k + 1, end == std::string_view::npos ? std::string_view::npos : end - k - 1));
        }
        const size_t end = tag.find_first_of(" \t\n/>", k);
        return std::string(tag.substr(k, end == std::string_view::npos ? std::string_view::npos : end - k));
    }
    return {};
}

// A size an attribute gives in pixels ("200", "200px"); 0 for none or a percentage.
u32 attribute_px(std::string_view tag, std::string_view name) {
    const std::string v = attribute(tag, name);
    u32 n = 0;
    size_t k = 0;
    for (; k < v.size() && std::isdigit(static_cast<unsigned char>(v[k])) && n < 100000; ++k) n = n * 10 + static_cast<u32>(v[k] - '0');
    return k < v.size() && v[k] == '%' ? 0 : n;
}

struct Refs {
    std::map<std::string, std::string> to;  // a link's label (lower case) -> where it goes
    size_t longest = 0;                     // the longest label (text longer is none of them)
    void set(std::string label, std::string dest) {
        longest = std::max(longest, label.size());
        to[std::move(label)] = std::move(dest);
    }
};

constexpr int kMostDepth = 32;  // quotes, lists, emphasis and links nested deeper than this are taken as text

std::string entity(std::string_view s, size_t* len) {
    static const std::pair<const char*, const char*> kNamed[] = {{"&amp;", "&"},  {"&lt;", "<"},       {"&gt;", ">"},       {"&quot;", "\""},
                                                                 {"&#39;", "'"},  {"&apos;", "'"},     {"&nbsp;", "\xC2\xA0"}, {"&copy;", "\xC2\xA9"},
                                                                 {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"}, {"&hellip;", "\xE2\x80\xA6"}, {"&rarr;", "\xE2\x86\x92"}};
    for (const auto& [name, text] : kNamed)
        if (s.starts_with(name)) {
            *len = std::string_view(name).size();
            return text;
        }
    if (s.starts_with("&#")) {
        const size_t semi = s.substr(0, 12).find(';');  // (not past where it could be: the rest of a long line, for each &#)
        if (semi != std::string_view::npos && semi < 12) {
            const bool hex = s.size() > 2 && (s[2] == 'x' || s[2] == 'X');
            const std::string num(s.substr(hex ? 3 : 2, semi - (hex ? 3 : 2)));
            char* end = nullptr;
            const unsigned long cp = std::strtoul(num.c_str(), &end, hex ? 16 : 10);
            if (end && *end == 0 && cp > 0 && cp < 0x110000) {
                *len = semi + 1;
                std::string out;
                encode_utf8(static_cast<u32>(cp), out);
                return out;
            }
        }
    }
    *len = 0;
    return {};
}

// What parse_inline looks for in a piece of text again and again, each
// found once (on first use): the marks that close others, brackets'
// partners, runs of backticks, the next of a few characters. (A long line
// of marks that close nothing, thousands of `*`, `[` or `<`, once took time
// with the square of its length: each mark looked to its end.)
class Marks {
  public:
    explicit Marks(std::string_view s) : s_(s) {}

    // Where the run of c (exactly n long) that closes one opened before `from`
    // is, or npos. (Most are a few words on. Once a scan goes far, what a scan
    // finds from each place it passes is kept, and the next to get there stops.)
    size_t closer(size_t from, char c, size_t n) {
        std::vector<size_t>& found_from = closers_[c == '*' ? n - 1 : c == '_' ? n + 2 : 6];
        path_.clear();
        size_t steps = 0;
        size_t found = std::string_view::npos;
        for (size_t j = from; j < s_.size();) {
            if (j > from) {
                if (found_from.empty()) {
                    if (++steps > kNear) {
                        found_from.assign(s_.size(), kUnknown);
                        return closer(from, c, n);
                    }
                } else if (found_from[j] != kUnknown) {
                    found = found_from[j];
                    break;
                } else {
                    path_.push_back(j);
                }
            }
            if (s_[j] == '\\') {
                j += 2;
                continue;
            }
            if (s_[j] == '`') {  // (not inside code)
                size_t k = j;
                while (k < s_.size() && s_[k] == '`') ++k;
                const size_t end = ticks(k, k - j);
                j = end != std::string_view::npos ? end + (k - j) : k;
                continue;
            }
            if (s_[j] != c) {
                ++j;
                continue;
            }
            size_t k = j;
            while (k < s_.size() && s_[k] == c) ++k;
            const size_t run = k - j;
            const bool after_space = j > 0 && (s_[j - 1] == ' ' || s_[j - 1] == '\t');
            const bool word_after = c == '_' && k < s_.size() && std::isalnum(static_cast<unsigned char>(s_[k]));
            if (run >= n && !after_space && !word_after && j > from) {
                found = j + (run - n);
                break;
            }
            j = k;
        }
        for (const size_t j : path_) found_from[j] = found;
        return found;
    }

    // The ] or ) that closes the [ or ( at i (brackets inside may nest), or
    // npos. (Looked for, while it is near; past that, every one is paired at once.)
    size_t bracket_end(size_t i, char open) {
        std::vector<size_t>& ends = open == '[' ? square_ : round_;
        const char close = open == '[' ? ']' : ')';
        if (ends.empty()) {
            const size_t near = std::min(s_.size(), i + kNear);
            int depth = 0;
            for (size_t j = i; j < near; ++j) {
                if (s_[j] == '\\') {
                    ++j;
                    continue;
                }
                if (s_[j] == open) ++depth;
                else if (s_[j] == close && --depth == 0) return j;
            }
            if (near == s_.size()) return std::string_view::npos;
            ends.assign(s_.size(), std::string_view::npos);
            std::vector<size_t> opened;
            for (size_t j = 0; j < s_.size(); ++j) {
                if (s_[j] == '\\') {
                    ++j;
                    continue;
                }
                if (s_[j] == open) {
                    opened.push_back(j);
                } else if (s_[j] == close && !opened.empty()) {
                    ends[opened.back()] = j;
                    opened.pop_back();
                }
            }
        }
        return ends[i];
    }

    // Where the first run of backticks at least n long starts, at k (the end
    // of a run) or after, or npos: s.find(std::string(n, '`'), k).
    size_t ticks(size_t k, size_t n) {
        if (!ticks_found_) {  // (looked for, while near; past that, every run is found at once)
            const size_t near = std::min(s_.size(), k + kNear);
            for (size_t j = s_.find('`', k); j < near;) {
                size_t e = j;
                while (e < s_.size() && s_[e] == '`') ++e;
                if (e - j >= n) return j;
                j = s_.find('`', e);
            }
            if (near == s_.size()) return std::string_view::npos;
            ticks_found_ = true;
            for (size_t j = s_.find('`'); j != std::string_view::npos; j = s_.find('`', j)) {
                size_t e = j;
                while (e < s_.size() && s_[e] == '`') ++e;
                tick_at_.push_back(j);
                tick_len_.push_back(e - j);
                j = e;
            }
            // (and for each run, the next one longer: the runs between it and that are no answer where it is not)
            tick_longer_.resize(tick_at_.size());
            std::vector<size_t> later;
            for (size_t r = tick_at_.size(); r-- > 0;) {
                while (!later.empty() && tick_len_[later.back()] <= tick_len_[r]) later.pop_back();
                tick_longer_[r] = later.empty() ? tick_at_.size() : later.back();
                later.push_back(r);
            }
        }
        size_t r = static_cast<size_t>(std::lower_bound(tick_at_.begin(), tick_at_.end(), k) - tick_at_.begin());
        while (r < tick_at_.size() && tick_len_[r] < n) r = tick_longer_[r];
        return r < tick_at_.size() ? tick_at_[r] : std::string_view::npos;
    }

    // The first ch (one of kNext) at `from` or after, asked with `from` never going back.
    size_t next(char ch, size_t from) {
        size_t& at = next_[std::strchr(kNext, ch) - kNext];
        if (at == kUnknown || (at != std::string_view::npos && at < from)) at = s_.find(ch, from);
        return at;
    }

    // Where what is before the > at gt ends, the spaces, tabs and returns in front of it left out.
    size_t trimmed_end(size_t gt) {
        if (trimmed_for_ != gt) {
            trimmed_for_ = gt;
            trimmed_ = gt;
            while (trimmed_ > 0 && (s_[trimmed_ - 1] == ' ' || s_[trimmed_ - 1] == '\t' || s_[trimmed_ - 1] == '\r')) --trimmed_;
        }
        return trimmed_;
    }

  private:
    static constexpr size_t kUnknown = std::string_view::npos - 1;
    static constexpr size_t kNear = 256;  // how far a scan looks before what it finds is kept for the next
    static constexpr const char* kNext = "<>:@ ";
    std::string_view s_;
    std::vector<size_t> closers_[7];  // for each mark (*, **, ***, _, __, ___, ~~), what a scan from each place finds
    std::vector<size_t> path_;
    std::vector<size_t> square_, round_;
    bool ticks_found_ = false;
    std::vector<size_t> tick_at_, tick_len_, tick_longer_;
    size_t next_[5] = {kUnknown, kUnknown, kUnknown, kUnknown, kUnknown};
    size_t trimmed_for_ = std::string_view::npos, trimmed_ = 0;
};

void parse_inline(std::string_view s, const Span& as, const Refs& refs, std::vector<Span>& out) {
    static thread_local int depth = 0;
    if (depth >= kMostDepth) {  // (nested deeper than anyone writes: as it is)
        Span sp = as;
        sp.text = std::string(s);
        out.push_back(std::move(sp));
        return;
    }
    struct Deeper {
        int& d;
        explicit Deeper(int& x) : d(++x) {}
        ~Deeper() { --d; }
    } deeper(depth);
    std::string buf;
    auto flush = [&] {
        if (buf.empty()) return;
        Span sp = as;
        sp.text = std::move(buf);
        out.push_back(std::move(sp));
        buf.clear();
    };
    auto link_span = [&](std::string_view text, const std::string& url) {
        flush();
        Span inner = as;
        inner.ink = kMdLink;
        inner.link = url;
        parse_inline(text, inner, refs, out);
    };
    Marks marks(s);
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            buf += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '\n') {
            flush();
            Span br = as;
            br.brk = true;
            out.push_back(br);
            ++i;
            continue;
        }
        if (c == '`') {
            size_t k = i;
            while (k < s.size() && s[k] == '`') ++k;
            const std::string_view run = s.substr(i, k - i);
            const size_t end = marks.ticks(k, run.size());
            if (end == std::string_view::npos || (end + run.size() < s.size() && s[end + run.size()] == '`')) {
                buf += run;
                i = k;
                continue;
            }
            std::string code(s.substr(k, end - k));
            for (char& ch : code)
                if (ch == '\n') ch = ' ';
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos) code = code.substr(1, code.size() - 2);
            flush();
            Span sp = as;
            sp.text = code;
            sp.ink = as.link.empty() ? kMdCode : kMdLink;
            sp.tone = kMdCodeTone;
            sp.style &= static_cast<u8>(~kMdItalic);
            out.push_back(std::move(sp));
            i = end + run.size();
            continue;
        }
        if (c == '<') {
            const size_t end = marks.next('>', i);
            if (end != std::string_view::npos) {
                const std::string_view inner = s.substr(i + 1, end - i - 1);
                // (what is between, trimmed, of which no more is copied than tells a <br> or an <img>: each of a
                // thousand < before one > once copied all there was up to it)
                size_t from = i + 1;
                while (from < end && (s[from] == ' ' || s[from] == '\t')) ++from;
                const size_t to = std::max(from, marks.trimmed_end(end));
                const std::string tag = lower(s.substr(from, std::min<size_t>(to - from, 5)));
                if (to - from <= 4 && (tag == "br" || tag == "br/" || tag == "br /")) {
                    flush();
                    Span br = as;
                    br.brk = true;
                    out.push_back(br);
                    i = end + 1;
                    continue;
                }
                const auto in_it = [&](char ch) { return marks.next(ch, i + 1) < end; };
                const size_t colon = in_it(':') ? marks.next(':', i + 1) - i - 1 : std::string_view::npos;
                const bool url = colon != std::string_view::npos && colon > 1 && !in_it(' ') &&
                                 std::all_of(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(colon), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '.' || ch == '-'; });
                const bool mail = !url && in_it('@') && !in_it(' ') && !inner.empty();
                if (url || mail) {
                    link_span(inner, url ? std::string(inner) : "mailto:" + std::string(inner));
                    i = end + 1;
                    continue;
                }
                // A picture: <img src="..." alt="..." width="...">.
                if (tag.starts_with("img") && (tag.size() == 3 || tag[3] == ' ' || tag[3] == '\t' || tag[3] == '\n' || tag[3] == '/')) {
                    const std::string src = attribute(inner, "src"), alt = attribute(inner, "alt");
                    if (!src.empty()) {
                        flush();
                        Span pic = as;
                        pic.text = "\xE2\x96\xA3 " + (alt.empty() ? std::string("image") : alt);
                        pic.ink = kMdLink;
                        pic.link = as.link.empty() ? src : as.link;
                        pic.image = src;
                        pic.image_w = attribute_px(inner, "width");
                        pic.image_h = attribute_px(inner, "height");
                        out.push_back(std::move(pic));
                    }
                    i = end + 1;
                    continue;
                }
                // Inline HTML (<kbd>, <sup>, </b> ...): its tags go, its text stays.
                if (!inner.empty() && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '/') && !in_it('<')) {
                    i = end + 1;
                    continue;
                }
            }
        }
        if ((c == '!' && i + 1 < s.size() && s[i + 1] == '[') || c == '[') {
            const bool image = c == '!';
            const size_t open = image ? i + 1 : i;
            const size_t close = marks.bracket_end(open, '[');
            if (close != std::string_view::npos) {
                const std::string_view text = s.substr(open + 1, close - open - 1);
                std::string url;
                size_t next = std::string_view::npos;
                if (close + 1 < s.size() && s[close + 1] == '(') {
                    const size_t end = marks.bracket_end(close + 1, '(');
                    if (end != std::string_view::npos) {
                        std::string_view dest = trim(s.substr(close + 2, end - close - 2));
                        if (dest.starts_with('<')) dest = dest.substr(1, dest.find('>') == std::string_view::npos ? dest.size() - 1 : dest.find('>') - 1);
                        else if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);  // (and a title after it)
                        url = std::string(dest);
                        next = end + 1;
                    }
                } else {
                    std::string_view label = text;
                    size_t end = close + 1;
                    if (close + 1 < s.size() && s[close + 1] == '[') {
                        const size_t e2 = s.find(']', close + 2);
                        if (e2 != std::string_view::npos) {
                            if (e2 > close + 2) label = s.substr(close + 2, e2 - close - 2);
                            end = e2 + 1;
                        }
                    }
                    // (text longer than any label is not looked up: brackets nested thousands deep each copied what they hold)
                    if (const auto it = label.size() <= refs.longest ? refs.to.find(lower(label)) : refs.to.end(); it != refs.to.end()) {
                        url = it->second;
                        next = end;
                    } else if (text.starts_with('^')) {  // a footnote's mark
                        flush();
                        Span note = as;
                        note.text = "[" + std::string(text.substr(1)) + "]";
                        note.ink = kMdMuted;
                        out.push_back(std::move(note));
                        i = close + 1;
                        continue;
                    }
                }
                if (next != std::string_view::npos) {
                    if (image) {  // (what stands for it where it is not drawn: what it shows; a click opens it, or follows the link it is in)
                        flush();
                        Span pic = as;
                        pic.text = "\xE2\x96\xA3 " + (text.empty() ? std::string("image") : std::string(text));
                        pic.ink = kMdLink;
                        pic.link = as.link.empty() ? url : as.link;
                        pic.image = url;
                        out.push_back(std::move(pic));
                    } else {
                        link_span(text, url);
                    }
                    i = next;
                    continue;
                }
            }
        }
        if (c == '*' || c == '_' || (c == '~' && i + 1 < s.size() && s[i + 1] == '~')) {
            size_t k = i;
            while (k < s.size() && s[k] == c) ++k;
            const size_t run = std::min<size_t>(k - i, c == '~' ? 2 : 3);
            const bool space_after = k >= s.size() || s[k] == ' ' || s[k] == '\t' || s[k] == '\n';
            const bool intraword = c == '_' && i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]));
            if (!space_after && !intraword && (c != '~' || k - i == 2)) {
                const size_t end = marks.closer(i + run, c, run);
                if (end != std::string_view::npos) {
                    flush();
                    Span inner = as;
                    if (c == '~') inner.ink = kMdMuted, inner.style |= kMdStrike;  // (struck out: a line through, and quieter)
                    else inner.style |= static_cast<u8>(run == 1 ? kMdItalic : run == 2 ? kMdBold : kMdBold | kMdItalic);
                    parse_inline(s.substr(i + run, end - i - run), inner, refs, out);
                    i = end + run;
                    continue;
                }
            }
            buf.append(s.substr(i, k - i));
            i = k;
            continue;
        }
        if (c == '&') {
            size_t len = 0;
            const std::string e = entity(s.substr(i), &len);
            if (len) {
                buf += e;
                i += len;
                continue;
            }
        }
        if ((c == 'h' || c == 'w') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '(') && as.link.empty() &&
            (s.substr(i).starts_with("https://") || s.substr(i).starts_with("http://") || s.substr(i).starts_with("www."))) {
            size_t end = i;
            while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '\n' && s[end] != '<') ++end;
            while (end > i && std::strchr(".,:;!?)'\"", s[end - 1])) --end;  // (the sentence's, not the address's)
            const std::string url(s.substr(i, end - i));
            link_span(url, url.starts_with("www.") ? "https://" + url : url);
            i = end;
            continue;
        }
        buf += c;
        ++i;
    }
    flush();
}

// ---------------------------------------------------------------- blocks

struct Line {
    std::string_view text;
    u32 no = 0;
};

struct Block {
    enum Kind { kPara, kHeading, kCode, kQuote, kList, kItem, kTable, kRule, kHtml } kind = kPara;
    u32 line = 0;
    int level = 0;
    std::vector<std::string> text;
    std::vector<u32> nos;
    std::string info;
    std::vector<Block> kids;
    bool ordered = false, loose = false;
    int start = 1, task = -1;
    std::vector<std::vector<std::string>> table;  // the header first
    std::vector<int> align;                       // -1 left, 0 centre, 1 right
};

bool is_rule(std::string_view s) {
    s = trim(s);
    if (s.size() < 3 || (s[0] != '-' && s[0] != '*' && s[0] != '_')) return false;
    int n = 0;
    for (const char c : s) {
        if (c == s[0]) ++n;
        else if (c != ' ' && c != '\t') return false;
    }
    return n >= 3;
}

// An alert's kind (GitHub's "[!NOTE]" and its like, alone on a quote's first
// line, in any case): 1 note, 2 tip, 3 important, 4 warning, 5 caution; 0 for
// anything else (a quote that starts with a bracket).
int alert_kind(std::string_view s) {
    s = trim(s);
    if (s.size() < 6 || s.size() > 12 || !s.starts_with("[!") || s.back() != ']') return 0;
    static const char* const kKinds[] = {"note", "tip", "important", "warning", "caution"};
    const std::string name = lower(s.substr(2, s.size() - 3));
    for (int k = 0; k < 5; ++k)
        if (name == kKinds[k]) return k + 1;
    return 0;
}

bool is_fence(std::string_view s, char* fc, size_t* len) {
    s = trim(s);
    if (s.size() < 3 || (s[0] != '`' && s[0] != '~')) return false;
    size_t k = 0;
    while (k < s.size() && s[k] == s[0]) ++k;
    if (k < 3) return false;
    if (s[0] == '`' && s.substr(k).find('`') != std::string_view::npos) return false;
    *fc = s[0];
    *len = k;
    return true;
}

int atx_level(std::string_view s) {
    int n = 0;
    while (n < static_cast<int>(s.size()) && s[static_cast<size_t>(n)] == '#') ++n;
    if (n < 1 || n > 6) return 0;
    if (static_cast<size_t>(n) < s.size() && s[static_cast<size_t>(n)] != ' ' && s[static_cast<size_t>(n)] != '\t') return 0;
    return n;
}

// A list item's marker: whether it is ordered, its number, its kind
// ('-', '*', '+', '.', ')'), and the column its text starts at.
struct Marker {
    bool ordered = false;
    int number = 0;
    char kind = 0;
    int content = 0;
};
bool list_marker(std::string_view line, Marker* m) {
    const int ind = indent_of(line);
    if (ind > 3) return false;
    const std::string s = strip_cols(line, ind);
    size_t k = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '*' || s[0] == '+')) {
        m->ordered = false;
        m->kind = s[0];
        k = 1;
    } else {
        while (k < s.size() && k < 9 && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        if (k == 0 || k >= s.size() || (s[k] != '.' && s[k] != ')')) return false;
        m->ordered = true;
        m->number = std::atoi(s.substr(0, k).c_str());
        m->kind = s[k];
        ++k;
    }
    if (k < s.size() && s[k] != ' ' && s[k] != '\t') return false;
    int spaces = 0;
    size_t j = k;
    while (j < s.size() && s[j] == ' ') ++j, ++spaces;
    if (j >= s.size()) spaces = 1;          // (an empty item)
    else if (spaces > 4) spaces = 1;        // (code inside the item)
    m->content = ind + static_cast<int>(k) + spaces;
    return true;
}

std::vector<std::string> table_cells(std::string_view s) {
    s = trim(s);
    if (s.starts_with('|')) s.remove_prefix(1);
    if (s.ends_with('|') && !s.ends_with("\\|")) s.remove_suffix(1);
    std::vector<std::string> out;
    std::string cell;
    bool code = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (s[i] == '`') {
            code = !code;
            cell += '`';
        } else if (s[i] == '|' && !code) {
            out.emplace_back(trim(cell));
            cell.clear();
        } else {
            cell += s[i];
        }
    }
    out.emplace_back(trim(cell));
    return out;
}

bool table_delims(std::string_view s, std::vector<int>* align) {
    if (s.find('-') == std::string_view::npos || (s.find('|') == std::string_view::npos && trim(s).find(' ') != std::string_view::npos)) return false;
    align->clear();
    for (const std::string& c : table_cells(s)) {
        if (c.empty()) return false;
        const bool left = c.front() == ':', right = c.back() == ':';
        const std::string_view mid = std::string_view(c).substr(left ? 1 : 0, c.size() - (left ? 1 : 0) - (right ? 1 : 0));
        if (mid.empty() || mid.find_first_not_of('-') != std::string_view::npos) return false;
        align->push_back(left && right ? 0 : right ? 1 : -1);
    }
    return !align->empty();
}

// Whether a line starts a block of its own (and so ends a paragraph).
bool starts_block(std::string_view line) {
    if (blank(line) || indent_of(line) > 3) return false;
    const std::string s = strip_cols(line, indent_of(line));
    char fc = 0;
    size_t fl = 0;
    Marker m;
    if (is_fence(s, &fc, &fl) || atx_level(s) || is_rule(s) || s.starts_with('>')) return true;
    if (list_marker(line, &m)) return (!m.ordered || m.number == 1) && !blank(std::string_view(s).substr(std::min(s.size(), static_cast<size_t>(m.content - indent_of(line)))));
    return false;
}

std::vector<Block> parse_blocks(const std::vector<Line>& lines, Refs& refs);

void parse_into(const std::vector<Line>& lines, Refs& refs, std::vector<Block>& out) {
    const size_t n = lines.size();
    size_t i = 0;
    while (i < n) {
        const std::string_view t = lines[i].text;
        const u32 no = lines[i].no;
        if (blank(t)) {
            ++i;
            continue;
        }
        const int ind = indent_of(t);
        if (ind >= 4) {  // indented code
            Block b;
            b.kind = Block::kCode;
            b.line = no;
            while (i < n && (blank(lines[i].text) || indent_of(lines[i].text) >= 4)) {
                b.text.push_back(blank(lines[i].text) ? std::string() : strip_cols(lines[i].text, 4));
                b.nos.push_back(lines[i].no);
                ++i;
            }
            while (!b.text.empty() && b.text.back().empty()) b.text.pop_back(), b.nos.pop_back();
            out.push_back(std::move(b));
            continue;
        }
        const std::string s = strip_cols(t, ind);
        char fc = 0;
        size_t flen = 0;
        if (is_fence(s, &fc, &flen)) {
            Block b;
            b.kind = Block::kCode;
            b.line = no;
            b.info = std::string(trim(std::string_view(s).substr(flen)));
            ++i;
            while (i < n) {
                char fc2 = 0;
                size_t flen2 = 0;
                const std::string_view l = lines[i].text;
                if (indent_of(l) <= 3 && is_fence(l, &fc2, &flen2) && fc2 == fc && flen2 >= flen && trim(trim(l).substr(flen2)).empty()) {
                    ++i;
                    break;
                }
                b.text.push_back(strip_cols(l, ind));
                b.nos.push_back(lines[i].no);
                ++i;
            }
            out.push_back(std::move(b));
            continue;
        }
        if (const int level = atx_level(s)) {
            Block b;
            b.kind = Block::kHeading;
            b.line = no;
            b.level = level;
            std::string_view h = trim(std::string_view(s).substr(static_cast<size_t>(level)));
            size_t end = h.size();
            while (end > 0 && h[end - 1] == '#') --end;
            if (end == 0 || h[end - 1] == ' ') h = trim(h.substr(0, end));
            b.text.emplace_back(h);
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (is_rule(s)) {
            Block b;
            b.kind = Block::kRule;
            b.line = no;
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (s.starts_with('>')) {
            Block b;
            b.kind = Block::kQuote;
            b.line = no;
            std::vector<Line> sub;
            std::vector<std::string> keep;  // (the lines' text, kept alive for the views into it)
            keep.reserve(n - i);
            bool para = false;
            while (i < n) {
                const std::string_view l = lines[i].text;
                const int li = indent_of(l);
                const std::string ls = li <= 3 ? strip_cols(l, li) : std::string();
                if (li <= 3 && ls.starts_with('>')) {
                    std::string rest = ls.substr(1);
                    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                    para = !blank(rest);
                    keep.push_back(std::move(rest));
                } else if (para && !blank(l) && !starts_block(l)) {  // (a paragraph's lazy line)
                    keep.emplace_back(l);
                } else {
                    break;
                }
                sub.push_back({{}, lines[i].no});
                ++i;
            }
            // An alert: its tag says what kind, and is not text of it.
            if (!keep.empty() && (b.level = alert_kind(keep[0])) != 0) keep[0].clear();
            for (size_t k = 0; k < sub.size(); ++k) sub[k].text = keep[k];
            b.kids = parse_blocks(sub, refs);
            out.push_back(std::move(b));
            continue;
        }
        Marker m;
        if (list_marker(t, &m)) {
            Block list;
            list.kind = Block::kList;
            list.line = no;
            list.ordered = m.ordered;
            list.start = m.number;
            const char kind = m.kind;
            bool gap = false;
            while (i < n) {
                Marker mi;
                if (!list_marker(lines[i].text, &mi) || mi.kind != kind || mi.ordered != list.ordered) break;
                if (gap) list.loose = true;
                Block item;
                item.kind = Block::kItem;
                item.line = lines[i].no;
                std::vector<std::string> keep;
                std::vector<u32> nos;
                const std::string first = from_col(lines[i].text, mi.content);
                keep.push_back(first);
                nos.push_back(lines[i].no);
                ++i;
                bool last_blank = blank(first), inner_gap = false;
                while (i < n) {
                    const std::string_view l = lines[i].text;
                    if (blank(l)) {
                        keep.emplace_back();
                        nos.push_back(lines[i].no);
                        last_blank = true;
                        ++i;
                        continue;
                    }
                    Marker next;
                    if (indent_of(l) < mi.content && list_marker(l, &next)) break;  // (the next item, or another list)
                    if (indent_of(l) >= mi.content) {
                        if (last_blank && keep.size() > 1) inner_gap = true;
                        keep.push_back(strip_cols(l, mi.content));
                    } else if (!last_blank && !starts_block(l)) {
                        keep.emplace_back(trim(l));  // (lazy continuation)
                    } else {
                        break;
                    }
                    nos.push_back(lines[i].no);
                    last_blank = false;
                    ++i;
                }
                gap = false;
                while (!keep.empty() && blank(keep.back())) keep.pop_back(), nos.pop_back(), gap = true;
                if (inner_gap) list.loose = true;
                // A task: [ ] or [x] in front.
                if (!keep.empty() && keep[0].size() >= 3 && keep[0][0] == '[' && keep[0][2] == ']' && (keep[0][1] == ' ' || keep[0][1] == 'x' || keep[0][1] == 'X') &&
                    (keep[0].size() == 3 || keep[0][3] == ' ')) {
                    item.task = keep[0][1] == ' ' ? 0 : 1;
                    keep[0] = keep[0].size() > 4 ? keep[0].substr(4) : std::string();
                }
                std::vector<Line> sub;
                for (size_t k = 0; k < keep.size(); ++k) sub.push_back({keep[k], nos[k]});
                item.kids = parse_blocks(sub, refs);
                list.kids.push_back(std::move(item));
                if (i < n && blank(lines[i].text)) gap = true;
            }
            out.push_back(std::move(list));
            continue;
        }
        if (i + 1 < n && s.find('|') != std::string::npos) {
            std::vector<int> align;
            if (table_delims(lines[i + 1].text, &align)) {
                std::vector<std::string> head = table_cells(s);
                if (head.size() == align.size()) {
                    Block b;
                    b.kind = Block::kTable;
                    b.line = no;
                    b.align = align;
                    b.table.push_back(std::move(head));
                    b.nos.push_back(no);
                    i += 2;
                    while (i < n && !blank(lines[i].text) && (lines[i].text.find('|') != std::string_view::npos) && !starts_block(lines[i].text)) {
                        std::vector<std::string> row = table_cells(lines[i].text);
                        row.resize(align.size());
                        b.table.push_back(std::move(row));
                        b.nos.push_back(lines[i].no);
                        ++i;
                    }
                    out.push_back(std::move(b));
                    continue;
                }
            }
        }
        if (s.starts_with('<') && s.size() > 1 && (std::isalpha(static_cast<unsigned char>(s[1])) || s[1] == '/' || s[1] == '!')) {
            Block b;
            b.kind = Block::kHtml;
            b.line = no;
            // A comment, and <script>, <style> and <pre>, run to their end,
            // blank lines and all; anything else to a blank line.
            const std::string head = lower(s.substr(0, 8));
            std::string end;
            if (head.starts_with("<!--")) end = "-->";
            else if (head.starts_with("<script")) end = "</script>";
            else if (head.starts_with("<style")) end = "</style>";
            else if (head.starts_with("<pre")) end = "</pre>";
            while (i < n && (!end.empty() || !blank(lines[i].text))) {
                b.text.emplace_back(trim(lines[i].text));
                b.nos.push_back(lines[i].no);
                const bool closed = !end.empty() && lower(lines[i].text).find(end) != std::string::npos;
                ++i;
                if (closed) break;
            }
            out.push_back(std::move(b));
            continue;
        }
        if (s.starts_with('[')) {  // a link's definition: [label]: where
            const size_t close = s.find("]:");
            if (close != std::string::npos && close > 1) {
                std::string_view dest = trim(std::string_view(s).substr(close + 2));
                if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);
                if (dest.starts_with('<') && dest.ends_with('>')) dest = dest.substr(1, dest.size() - 2);
                if (!dest.empty()) {
                    refs.set(lower(s.substr(1, close - 1)), std::string(dest));
                    ++i;
                    continue;
                }
            }
        }
        // A paragraph (or a heading underlined with === or ---).
        Block b;
        b.kind = Block::kPara;
        b.line = no;
        while (i < n) {
            const std::string_view l = lines[i].text;
            if (blank(l)) break;
            if (!b.text.empty()) {
                const std::string_view tl = trim(l);
                if (indent_of(l) <= 3 && !tl.empty() && (tl.find_first_not_of('=') == std::string_view::npos || tl.find_first_not_of('-') == std::string_view::npos)) {
                    b.kind = Block::kHeading;
                    b.level = tl[0] == '=' ? 1 : 2;
                    ++i;
                    break;
                }
                if (starts_block(l)) break;
                std::vector<int> align;  // (a table's head: the table starts here)
                if (l.find('|') != std::string_view::npos && i + 1 < n && table_delims(lines[i + 1].text, &align) && table_cells(l).size() == align.size()) break;
            }
            b.text.emplace_back(l);
            b.nos.push_back(lines[i].no);
            ++i;
        }
        out.push_back(std::move(b));
    }
}

std::vector<Block> parse_blocks(const std::vector<Line>& lines, Refs& refs) {
    // Nested deeper than anyone writes (a line of a thousand >): the rest is
    // a paragraph, so that the stack and the time stay small.
    static thread_local int depth = 0;
    std::vector<Block> out;
    if (depth >= kMostDepth) {
        Block b;
        b.line = lines.empty() ? 0 : lines[0].no;
        for (const Line& l : lines)
            if (!blank(l.text)) b.text.emplace_back(trim(l.text)), b.nos.push_back(l.no);
        if (!b.text.empty()) out.push_back(std::move(b));
        return out;
    }
    ++depth;
    parse_into(lines, refs, out);
    --depth;
    return out;
}

// A paragraph's lines as one text: a line that ends in two spaces or a
// backslash breaks there; the others run on.
std::string joined(const std::vector<std::string>& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        std::string_view l = text[i];
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        bool hard = false;
        if (l.ends_with("  ")) hard = true;
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
        if (l.ends_with('\\') && !l.ends_with("\\\\")) {
            hard = true;
            l.remove_suffix(1);
        }
        out += l;
        if (i + 1 < text.size()) out += hard ? '\n' : ' ';
    }
    return out;
}

// ---------------------------------------------------------------- layout

// The spans as lines at most `width` wide: words kept whole where they fit.
std::vector<std::vector<MdRun>> wrap(const std::vector<Span>& spans, u32 width) {
    std::vector<std::vector<MdRun>> lines(1);
    u32 at = 0;
    auto add = [&](std::string_view text, const Span& sp) {
        std::vector<MdRun>& l = lines.back();
        if (!l.empty() && l.back().ink == sp.ink && l.back().style == sp.style && l.back().tone == sp.tone && l.back().link == sp.link) l.back().text.append(text);
        else l.push_back({std::string(text), sp.ink, sp.style, sp.tone, sp.link});
        at += cols_of(text);
    };
    auto newline = [&] {
        while (!lines.back().empty() && lines.back().back().tone == kMdNoTone && lines.back().back().text.ends_with(' ')) {
            std::string& t = lines.back().back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back(), --at;
            if (t.empty()) lines.back().pop_back();
        }
        lines.emplace_back();
        at = 0;
    };
    for (const Span& sp : spans) {
        if (sp.brk) {
            newline();
            continue;
        }
        std::string_view t = sp.text;
        while (!t.empty()) {
            // The next word (with what follows it up to the next one), or the spaces before it.
            if (t[0] == ' ') {
                size_t k = 0;
                while (k < t.size() && t[k] == ' ') ++k;
                if (at > 0 && at + k <= width) add(t.substr(0, k), sp);
                else if (at > 0) newline();
                t.remove_prefix(k);
                continue;
            }
            size_t k = t.find(' ');
            if (k == std::string_view::npos) k = t.size();
            const std::string_view word = t.substr(0, k);
            const u32 w = cols_of(word);
            if (at + w > width && at > 0) newline();
            if (w > width) {  // longer than a line: cut
                std::string_view rest = word;
                while (!rest.empty()) {
                    size_t cut = fit(rest, width - at);
                    if (cut == 0) cut = first_char(rest);  // (a wide character in one column: over the edge, whole)
                    add(rest.substr(0, cut), sp);
                    rest.remove_prefix(cut);
                    if (!rest.empty()) newline();
                }
            } else {
                add(word, sp);
            }
            t.remove_prefix(k);
        }
    }
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

struct Level {
    std::vector<MdRun> first, rest;
    bool used = false;
    bool list = false;  // a list item's (how deep lists are, for their bullets)
};

// Whether these spans are pictures and nothing else (but space).
bool pictures_only(const std::vector<Span>& spans) {
    bool any = false;
    for (const Span& sp : spans) {
        if (!sp.image.empty()) any = true;
        else if (!sp.brk && sp.text.find_first_not_of(" \t") != std::string::npos) return false;
    }
    return any;
}

struct Layout {
    std::vector<MdRow>& rows;
    u32 width;
    const Refs& refs;
    std::vector<Level> levels;
    const MdPictureFn* picture = nullptr;
    const MdCodeFn* colour = nullptr;
    const MdDiagramFn* diagram = nullptr;
    bool named = true;  // a code block's language named over it

    u32 indent() const {
        u32 n = 0;
        for (const Level& l : levels) n += cols_of_runs(l.rest);
        return n;
    }
    static u32 cols_of_runs(const std::vector<MdRun>& runs) {
        u32 n = 0;
        for (const MdRun& r : runs) n += cols_of(r.text);
        return n;
    }
    u32 room() const { return width > indent() + 1 ? width - indent() : 1; }
    // A row: the levels' marks in front (each level's first mark on its first row), then these.
    MdRow& row(std::vector<MdRun> runs, u32 line) {
        MdRow r;
        r.line = line;
        for (Level& l : levels) {
            const std::vector<MdRun>& mark = l.used ? l.rest : l.first;
            r.runs.insert(r.runs.end(), mark.begin(), mark.end());
            l.used = true;
        }
        for (MdRun& run : runs)
            if (!run.text.empty()) r.runs.push_back(std::move(run));
        rows.push_back(std::move(r));
        return rows.back();
    }
    void gap(u32 line) {  // a blank row (a quote's bar goes on through it)
        if (rows.empty()) return;
        MdRow r;
        r.line = line;
        for (const Level& l : levels) {
            std::vector<MdRun> mark = l.rest;
            for (MdRun& m : mark)
                if (m.text.find_first_not_of(' ') == std::string::npos) m.text.clear();
            r.runs.insert(r.runs.end(), mark.begin(), mark.end());
        }
        rows.push_back(std::move(r));
    }

    void blocks(const std::vector<Block>& bs, bool tight, u8 ink) {
        for (size_t i = 0; i < bs.size(); ++i) {
            // (A heading of the top two levels is underlined: what follows
            // starts right under its line, so that it belongs to the heading,
            // with the blank row above the heading setting it off.)
            const bool under_rule = i > 0 && bs[i - 1].kind == Block::kHeading && bs[i - 1].level <= 2;
            if (i > 0 && !tight && !under_rule) gap(bs[i].line);
            block(bs[i], tight, ink);
        }
    }

    // Pictures, side by side as far as they fit (in the middle, if
    // `centre`); one that cannot be drawn is its text, on a row of its own.
    void pictures(const std::vector<Span>& spans, u32 line, bool centre) {
        std::vector<MdPic> strip;
        u32 used = 0;
        auto flush = [&] {
            if (strip.empty()) return;
            u32 tall = 0;
            for (const MdPic& p : strip) tall = std::max(tall, p.rows);
            const u32 shift = indent() + (centre && room() > used ? (room() - used) / 2 : 0);
            for (u32 r = 0; r < tall; ++r) {
                MdRow& at = row({}, line);
                for (const MdPic& p : strip)
                    if (r < p.rows) {
                        at.pics.push_back(p);
                        at.pics.back().col += shift;
                        at.pics.back().part = r;
                    }
            }
            strip.clear();
            used = 0;
        };
        for (const Span& sp : spans) {
            if (sp.image.empty()) continue;
            MdPic p;
            if (picture && *picture && (*picture)(sp.image, room(), sp.image_w, sp.image_h, &p) && p.cols && p.rows) {
                p.src = sp.image;
                p.link = sp.link;
                if (!strip.empty() && used + 1 + p.cols > room()) flush();
                p.col = strip.empty() ? 0 : used + 1;
                used = p.col + p.cols;
                strip.push_back(std::move(p));
            } else {
                flush();
                std::vector<MdRun> runs;
                const u32 w = cols_of(sp.text);
                if (centre && room() > w) runs.push_back({std::string((room() - w) / 2, ' ')});
                runs.push_back({sp.text, sp.ink, sp.style, sp.tone, sp.link});
                row(std::move(runs), line);
            }
        }
        flush();
    }

    void paragraph(const std::vector<std::string>& text, const std::vector<u32>& nos, u32 line, u8 ink, u8 style) {
        std::vector<Span> spans;
        Span as;
        as.ink = ink;
        as.style = style;
        parse_inline(joined(text), as, refs, spans);
        if (picture && *picture && pictures_only(spans)) {
            pictures(spans, line, false);
            return;
        }
        const auto lines = wrap(spans, room());
        // (each row's source line: the paragraph's lines, as far as rows go)
        for (size_t k = 0; k < lines.size(); ++k) row(lines[k], nos.empty() ? line : nos[std::min(k * nos.size() / std::max<size_t>(1, lines.size()), nos.size() - 1)]);
    }

    void code(const Block& b) {
        const u32 room_cols = room();
        // A Mermaid diagram, drawn (a picture, in the middle); or the code,
        // if it cannot be (or is not drawn yet).
        if (lower(b.info.substr(0, b.info.find(' '))) == "mermaid") {
            std::string src;
            for (const std::string& l : b.text) src += l + "\n";
            auto line_of = [&](size_t k, size_t n) { return b.nos.empty() ? b.line : b.nos[std::min(k * b.nos.size() / std::max<size_t>(1, n), b.nos.size() - 1)]; };
            std::string note;
            MdPic pic;
            // (a line about it, wrapped to the room as a paragraph is)
            auto say = [&](const std::string& text, u8 ink) {
                Span sp;
                sp.text = text, sp.ink = ink, sp.style = kMdItalic;
                for (std::vector<MdRun>& l : wrap({sp}, room_cols)) row(std::move(l), b.line);
            };
            const u8 got = diagram && *diagram ? (*diagram)(src, room_cols, &pic, &note) : static_cast<u8>(kMdDiagramLater);
            if (got == kMdDiagramPicture && pic.cols && pic.rows) {
                pic.col = indent() + (room_cols > pic.cols ? (room_cols - pic.cols) / 2 : 0);
                for (u32 r = 0; r < pic.rows; ++r) {
                    MdRow& at = row({}, line_of(r, pic.rows));
                    at.pics.push_back(pic);
                    at.pics.back().part = r;
                }
                return;
            }
            if (got != kMdDiagramLater && !note.empty()) say("mermaid: " + note, kMdError);
        }
        const u32 from = indent();
        auto fill = [&](MdRow& r) {
            r.fill = kMdCodeTone;
            r.fill_from = from;
            r.fill_to = width;
        };
        // The language, at the top on the right.
        const std::string lang = b.info.substr(0, b.info.find(' '));
        std::vector<MdRun> top;
        if (named && !lang.empty() && room_cols > cols_of(lang) + 2) top.push_back({std::string(room_cols - cols_of(lang) - 1, ' ') + lang, kMdMuted, 0, kMdCodeTone});
        fill(row(std::move(top), b.line));
        // Its colours, when it is in a language there is a grammar for: an
        // ink for each byte of its lines (each ended by a newline).
        std::vector<u8> inks;
        std::vector<size_t> starts;
        if (colour && *colour && !lang.empty() && !b.text.empty()) {
            std::string src;
            for (const std::string& l : b.text) {
                starts.push_back(src.size());
                src += l;
                src += '\n';
            }
            if (!(*colour)(lang, src, &inks) || inks.size() != src.size()) inks.clear();
        }
        for (size_t k = 0; k < b.text.size(); ++k) {
            std::vector<u8> li;  // (the ink of each byte of l)
            std::string l = expand_tabs(b.text[k], inks.empty() ? nullptr : inks.data() + starts[k], &li);
            const u32 line = k < b.nos.size() ? b.nos[k] : b.line;
            size_t at = 0;
            do {  // (long lines wrap)
                const std::string_view rest = std::string_view(l).substr(at);
                size_t cut = room_cols > 2 ? fit(rest, room_cols - 2) : rest.size();
                if (cut == 0) cut = first_char(rest);  // (a wide character in one column: over the edge, whole)
                cut = std::min(cut, rest.size());
                if (cut < rest.size() && rest[cut] != ' ') {  // (after the last space that fits, as an editor wraps, if that is not far back)
                    const size_t space = rest.find_last_of(' ', cut - 1);
                    if (space != std::string_view::npos && space + 1 > cut / 2 && rest.find_first_not_of(' ') < space) cut = space + 1;
                }
                std::vector<MdRun> runs;
                if (li.empty()) {
                    runs.push_back({" " + std::string(rest.substr(0, cut)), kMdCode, 0, kMdCodeTone});
                } else {  // a run for each stretch of one ink
                    runs.push_back({" ", kMdCodePlain, 0, kMdCodeTone});
                    for (size_t a = at; a < at + cut;) {
                        size_t e = a + 1;
                        while (e < at + cut && li[e] == li[a]) ++e;
                        runs.push_back({l.substr(a, e - a), li[a], 0, kMdCodeTone});
                        a = e;
                    }
                }
                fill(row(std::move(runs), line));
                at += cut;
            } while (at < l.size());
        }
        fill(row({}, b.nos.empty() ? b.line : b.nos.back()));
    }

    void table(const Block& b) {
        const size_t ncols = b.align.size();
        std::vector<std::vector<std::vector<Span>>> cells(b.table.size());
        std::vector<u32> natural(ncols, 1);
        for (size_t r = 0; r < b.table.size(); ++r) {
            cells[r].resize(ncols);
            for (size_t c = 0; c < ncols; ++c) {
                Span as;
                if (r == 0) as.style = kMdBold;
                parse_inline(c < b.table[r].size() ? b.table[r][c] : std::string(), as, refs, cells[r][c]);
                u32 w = 0;
                for (const Span& sp : cells[r][c]) w += cols_of(sp.text);
                natural[c] = std::max(natural[c], w);
            }
        }
        // Each column as wide as its widest cell, the widest made narrower until it fits.
        const u32 frame = static_cast<u32>(3 * ncols + 1);
        const u32 room_cols = room() > frame ? room() - frame : static_cast<u32>(ncols);
        std::vector<u32> w = natural;
        auto total = [&] {
            u32 t = 0;
            for (const u32 x : w) t += x;
            return t;
        };
        while (total() > room_cols) {
            const auto widest = std::max_element(w.begin(), w.end());
            if (*widest <= 3) break;
            --*widest;
        }
        auto border = [&](const char* l, const char* m, const char* r, u32 line) {
            std::string s = l;
            for (size_t c = 0; c < ncols; ++c) {
                for (u32 k = 0; k < w[c] + 2; ++k) s += "\xE2\x94\x80";
                s += c + 1 < ncols ? m : r;
            }
            row({{s, kMdBorder}}, line);
        };
        const char* bar = "\xE2\x94\x82";
        border("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90", b.line);
        for (size_t r = 0; r < cells.size(); ++r) {
            const u32 line = r < b.nos.size() ? b.nos[r] : b.line;
            std::vector<std::vector<std::vector<MdRun>>> wrapped(ncols);
            size_t height = 1;
            for (size_t c = 0; c < ncols; ++c) {
                wrapped[c] = wrap(cells[r][c], w[c]);
                height = std::max(height, wrapped[c].size());
            }
            for (size_t k = 0; k < height; ++k) {
                std::vector<MdRun> runs{{bar, kMdBorder}};
                for (size_t c = 0; c < ncols; ++c) {
                    std::vector<MdRun> cell = k < wrapped[c].size() ? wrapped[c][k] : std::vector<MdRun>{};
                    const u32 used = cols_of_runs(cell);
                    const u32 spare = w[c] > used ? w[c] - used : 0;
                    const u32 before = b.align[c] == 1 ? spare : b.align[c] == 0 ? spare / 2 : 0;
                    runs.push_back({std::string(1 + before, ' '), kMdText, 0, r == 0 ? kMdHeaderTone : kMdNoTone});
                    for (MdRun& run : cell) {
                        if (r == 0 && run.tone == kMdNoTone) run.tone = kMdHeaderTone;
                        runs.push_back(std::move(run));
                    }
                    runs.push_back({std::string(spare - before + 1, ' '), kMdText, 0, r == 0 ? kMdHeaderTone : kMdNoTone});
                    runs.push_back({bar, kMdBorder});
                }
                row(std::move(runs), line);
            }
            if (r == 0) border("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4", line);
        }
        border("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98", b.nos.empty() ? b.line : b.nos.back());
    }

    // HTML: its text, without the tags (and comments); its pictures drawn,
    // in the middle if it says align="center" (as a README's top often does).
    void html(const Block& b, u8 ink) {
        std::string all;
        for (const std::string& l : b.text) all += l + "\n";
        // Comments, scripts and styles go, the lines they were on staying
        // (as blank ones), so that each line keeps its place in the file.
        auto cut = [&](std::string_view open, std::string_view close) {
            for (size_t c; (c = lower(all).find(open)) != std::string::npos;) {
                size_t e = lower(all).find(close, c + open.size());
                e = e == std::string::npos ? all.size() : e + close.size();
                const auto lines = std::count(all.begin() + static_cast<std::ptrdiff_t>(c), all.begin() + static_cast<std::ptrdiff_t>(e), '\n');
                all.replace(c, e - c, static_cast<size_t>(lines), '\n');
            }
        };
        cut("<!--", "-->");
        cut("<script", "</script>");
        cut("<style", "</style>");
        const std::string low = lower(all);
        const bool centre = low.find("align=\"center\"") != std::string::npos || low.find("align=center") != std::string::npos || low.find("align='center'") != std::string::npos;
        std::vector<Span> pics;  // pictures on lines of their own, gathered (a row of them across lines)
        size_t k = 0;
        for (size_t at = 0; at < all.size(); ++k) {
            const size_t nl = all.find('\n', at);
            const std::string_view l = std::string_view(all).substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? all.size() : nl + 1;
            const u32 line = k < b.nos.size() ? b.nos[k] : b.line;
            Span as;
            as.ink = ink;
            std::vector<Span> spans;
            parse_inline(l, as, refs, spans);
            if (pictures_only(spans)) {
                for (Span& sp : spans)
                    if (!sp.image.empty()) pics.push_back(std::move(sp));
                continue;
            }
            bool text = false;
            for (const Span& sp : spans) text = text || (!sp.brk && sp.text.find_first_not_of(" \t") != std::string::npos);
            if (!text) continue;  // (nothing but tags)
            if (!pics.empty()) pictures(pics, line, centre), pics.clear();
            for (std::vector<MdRun>& r : wrap(spans, room())) {
                if (centre) {
                    const u32 w = cols_of_runs(r);
                    if (room() > w) r.insert(r.begin(), MdRun(std::string((room() - w) / 2, ' ')));
                }
                row(std::move(r), line);
            }
        }
        if (!pics.empty()) pictures(pics, b.nos.empty() ? b.line : b.nos.back(), centre);
    }

    void block(const Block& b, bool tight, u8 ink) {
        switch (b.kind) {
            case Block::kPara: paragraph(b.text, b.nos, b.line, ink, 0); break;
            case Block::kHeading: {
                const u8 hink = b.level == 1 ? kMdHeading1 : b.level == 2 ? kMdHeading2 : b.level == 3 ? kMdHeading3 : kMdHeading;
                paragraph(b.text, {}, b.line, hink, kMdBold);
                if (b.level <= 2) {  // a line under the top two levels
                    std::string rule;
                    for (u32 k = 0; k < room(); ++k) rule += b.level == 1 ? "\xE2\x94\x81" : "\xE2\x94\x80";
                    row({{rule, b.level == 1 ? kMdHeading1 : kMdMuted}}, b.line);
                }
                break;
            }
            case Block::kCode: code(b); break;
            case Block::kRule: {
                std::string rule;
                for (u32 k = 0; k < room(); ++k) rule += "\xE2\x94\x80";
                row({{rule, kMdMuted}}, b.line);
                break;
            }
            case Block::kHtml: html(b, ink); break;
            case Block::kQuote: {
                // An alert is a callout: a heavier bar and a title in its kind's
                // colour, and what it says under that, in the text's own.
                static const struct {
                    const char* title;
                    u8 ink;
                } kAlerts[] = {{"Note", kMdAnsiBlue}, {"Tip", kMdAnsiGreen}, {"Important", kMdAnsiMagenta}, {"Warning", kMdAnsiYellow}, {"Caution", kMdError}};
                const bool alert = b.level >= 1 && b.level <= 5;
                const u8 bar_ink = alert ? kAlerts[b.level - 1].ink : static_cast<u8>(kMdAccent);
                const char* bar = alert ? "\xE2\x96\x8C " : "\xE2\x96\x8E ";
                levels.push_back({{{bar, bar_ink}}, {{bar, bar_ink}}, false});
                if (alert) row({{kAlerts[b.level - 1].title, bar_ink, kMdBold}}, b.line);
                blocks(b.kids, false, alert ? kMdText : kMdQuote);
                levels.pop_back();
                break;
            }
            case Block::kList: {
                int depth = 0;
                for (const Level& l : levels) depth += l.list ? 1 : 0;
                static const char* const kBullets[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};
                const int last = b.start + static_cast<int>(b.kids.size()) - 1;
                const u32 numw = b.ordered ? static_cast<u32>(std::to_string(std::max(last, b.start)).size()) + 1 : 1;
                for (size_t k = 0; k < b.kids.size(); ++k) {
                    const Block& item = b.kids[k];
                    if (k > 0 && b.loose) gap(item.line);
                    std::string mark;
                    u8 mark_ink = kMdAccent;
                    if (item.task >= 0) {
                        mark = item.task ? "\xE2\x98\x91" : "\xE2\x98\x90";
                        mark_ink = item.task ? kMdAccent : kMdMuted;
                    } else if (b.ordered) {
                        const std::string num = std::to_string(b.start + static_cast<int>(k)) + ".";
                        mark = std::string(numw > num.size() ? numw - num.size() : 0, ' ') + num;
                    } else {
                        mark = kBullets[depth % 3];
                    }
                    const u32 mw = cols_of(mark);
                    levels.push_back({{{mark + " ", mark_ink}}, {{std::string(mw + 1, ' '), kMdText}}, false, true});
                    if (item.kids.empty()) row({}, item.line);
                    blocks(item.kids, !b.loose, item.task == 1 ? static_cast<u8>(kMdMuted) : ink);
                    levels.pop_back();
                }
                break;
            }
            case Block::kItem: blocks(b.kids, tight, ink); break;
            case Block::kTable: table(b); break;
        }
    }
};

}  // namespace

std::vector<MdRow> md_render(std::string_view text, u32 width, const MdPictureFn& picture, const MdCodeFn& colour, const MdDiagramFn& diagram) {
    width = std::max<u32>(width, 12);
    std::vector<Line> lines;
    u32 no = 0;
    for (size_t at = 0; at <= text.size();) {
        const size_t nl = text.find('\n', at);
        std::string_view l = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (l.ends_with('\r')) l.remove_suffix(1);
        lines.push_back({l, no++});
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
    // Front matter (--- at the very top, to the next ---) is not shown: when
    // what is between looks like it (keys and values, list items, comments),
    // not a rule at the top and another further down.
    if (!lines.empty() && trim(lines[0].text) == "---")
        for (size_t k = 1; k < lines.size(); ++k) {
            const std::string_view t = trim(lines[k].text);
            if (t == "---" || t == "...") {
                lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(k + 1));
                break;
            }
            const bool yaml = t.empty() || t.starts_with('#') || t.starts_with("- ") || t.find(':') != std::string_view::npos || indent_of(lines[k].text) > 0;
            if (!yaml) break;
        }
    Refs refs;
    // (links can be defined after they are used: their definitions are gathered first)
    for (const Line& l : lines) {
        const std::string_view s = trim(l.text);
        const size_t close = s.find("]:");
        if (s.starts_with('[') && close != std::string_view::npos && close > 1 && s[1] != '^') {
            std::string_view dest = trim(s.substr(close + 2));
            if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);
            if (!dest.empty()) refs.set(lower(s.substr(1, close - 1)), std::string(dest));
        }
    }
    const std::vector<Block> blocks = parse_blocks(lines, refs);
    std::vector<MdRow> rows;
    Layout layout{rows, width, refs, {}, &picture, &colour, &diagram};
    layout.blocks(blocks, false, kMdText);
    return rows;
}

std::vector<MdRow> md_code(std::string_view lang, std::string_view code, u32 width, const MdCodeFn& colour) {
    Block b;
    b.kind = Block::kCode;
    b.info = std::string(lang);
    for (size_t at = 0; at < code.size();) {
        size_t nl = code.find('\n', at);
        if (nl == std::string_view::npos) nl = code.size();
        std::string_view l = code.substr(at, nl - at);
        if (l.ends_with('\r')) l.remove_suffix(1);
        b.nos.push_back(static_cast<u32>(b.text.size()));
        b.text.emplace_back(l);
        at = nl + 1;
    }
    std::vector<MdRow> rows;
    const Refs refs;
    const MdPictureFn none;
    Layout layout{rows, std::max<u32>(width, 12), refs, {}, &none, &colour};
    layout.named = false;
    layout.code(b);
    return rows;
}

std::vector<MdRow> md_table(const std::vector<std::vector<std::string>>& cells, const std::vector<int>& align, u32 width) {
    Block b;
    b.kind = Block::kTable;
    b.table = cells;
    b.align = align;
    size_t cols = 0;
    for (const std::vector<std::string>& r : cells) cols = std::max(cols, r.size());
    b.align.resize(std::max<size_t>(cols, 1), -1);
    for (u32 r = 0; r < cells.size(); ++r) b.nos.push_back(r);
    std::vector<MdRow> rows;
    const Refs refs;
    Layout layout{rows, std::max<u32>(width, 12), refs, {}, nullptr, nullptr};
    layout.table(b);
    return rows;
}

std::string md_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (std::ispunct(static_cast<unsigned char>(c))) out += '\\';
        out += c == '\n' || c == '\t' || c == '\r' ? ' ' : c;
    }
    return out;
}

std::string md_row_text(const MdRow& r) {
    std::string out;
    for (const MdRun& run : r.runs) out += run.text;
    for (const MdPic& p : r.pics)
        if (p.part == 0) out += "picture: " + p.src + " " + std::to_string(p.cols) + "x" + std::to_string(p.rows);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace blit
