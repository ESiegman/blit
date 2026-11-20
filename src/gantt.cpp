// Gantt charts and timelines, laid out along time (a Gantt chart's tasks
// as bars against a date axis) or along a row of periods (a timeline's,
// each with its events under it), for the Markdown preview's pictures
// (diagram.hpp).
//
// A Gantt chart's dates are read by its dateFormat (Day.js's tokens) and
// written by its axisFormat (strftime's, as D3 has them) with a calendar
// of blit's own: days from 1970 by Howard Hinnant's arithmetic, and no
// time zones (a date is the same day wherever it is read, as the pictures
// are the same pixels).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#include "diagram.hpp"

namespace blit {

namespace {

// ---------------------------------------------------------------- the calendar

constexpr i64 kSecond = 1000, kMinute = 60 * kSecond, kHour = 60 * kMinute, kDay = 24 * kHour;
// (times kept within years 0 to 9999, and a little either side)
constexpr i64 kEarliest = -62'200'000'000'000, kLatest = 253'500'000'000'000;

i64 floor_div(i64 a, i64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    const i64 era = floor_div(y, 400);
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Civil {
    i64 y = 1970;
    int mo = 1, d = 1, h = 0, mi = 0, s = 0, ms = 0;
};

Civil civil(i64 t) {
    Civil c;
    const i64 day = floor_div(t, kDay);
    i64 in = t - day * kDay;
    c.h = static_cast<int>(in / kHour), in %= kHour;
    c.mi = static_cast<int>(in / kMinute), in %= kMinute;
    c.s = static_cast<int>(in / kSecond), c.ms = static_cast<int>(in % kSecond);
    const i64 z = day + 719468;
    const i64 era = floor_div(z, 146097);
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    c.d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    c.mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    c.y = yoe + era * 400 + (c.mo <= 2);
    return c;
}

bool leap(i64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(i64 y, int m) { return m == 2 ? (leap(y) ? 29 : 28) : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31; }
i64 time_of(const Civil& c) { return days_from_civil(c.y, c.mo, c.d) * kDay + c.h * kHour + c.mi * kMinute + c.s * kSecond + c.ms; }
int weekday(i64 t) { return static_cast<int>((floor_div(t, kDay) % 7 + 11) % 7); }  // 0: Sunday (1 January 1970 was a Thursday)

i64 add_months(i64 t, i64 n) {
    Civil c = civil(t);
    const i64 m = c.y * 12 + (c.mo - 1) + n;
    c.y = floor_div(m, 12), c.mo = static_cast<int>(m - c.y * 12) + 1;
    c.d = std::min(c.d, days_in(c.y, c.mo));
    return time_of(c);
}

const char* const kMonths[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
const char* const kDays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

int day_named(std::string_view s) {
    const std::string l = diagram_lower(diagram_trim(s));
    for (int i = 0; i < 7; ++i)
        if (l == kDays[i]) return i;
    return -1;
}

// A date read by a Day.js format (YYYY-MM-DD, DD/MM/YYYY HH:mm, X, ...):
// each number from one digit to as many as its token has, the rest as it
// is written. Parts it does not name are those of 1 January 1970.
bool parse_with(std::string_view s, std::string_view fmt, i64* out) {
    Civil c;
    bool pm = false, ampm = false;
    size_t at = 0;
    auto number = [&](size_t most, i64* v) {
        size_t n = 0;
        i64 x = 0;
        while (at < s.size() && n < most && std::isdigit(static_cast<unsigned char>(s[at]))) x = x * 10 + (s[at++] - '0'), ++n;
        *v = x;
        return n > 0;
    };
    for (size_t f = 0; f < fmt.size();) {
        auto token = [&](std::string_view t) { return fmt.substr(f).starts_with(t) ? (f += t.size(), true) : false; };
        i64 v = 0;
        if (fmt[f] == '[') {  // [literal]
            const size_t close = fmt.find(']', f);
            const std::string_view lit = fmt.substr(f + 1, close == std::string_view::npos ? std::string_view::npos : close - f - 1);
            if (s.substr(at, lit.size()) != lit) return false;
            at += lit.size();
            f = close == std::string_view::npos ? fmt.size() : close + 1;
        } else if (token("YYYY")) {
            if (!number(4, &v)) return false;
            c.y = v;
        } else if (token("YY")) {
            if (!number(2, &v)) return false;
            c.y = v < 69 ? 2000 + v : 1900 + v;  // (as Day.js has it)
        } else if (token("MMMM") || token("MMM")) {
            int found = -1;
            size_t len = 0;
            for (int m = 0; m < 12; ++m) {
                const std::string_view name = kMonths[m];
                for (const size_t n : {name.size(), size_t{3}})
                    if (at + n <= s.size() && diagram_lower(s.substr(at, n)) == name.substr(0, n) && n > len) found = m, len = n;
            }
            if (found < 0) return false;
            c.mo = found + 1, at += len;
        } else if (token("MM") || token("M")) {
            if (!number(2, &v)) return false;
            c.mo = static_cast<int>(v);
        } else if (token("Do")) {
            if (!number(2, &v)) return false;
            c.d = static_cast<int>(v);
            while (at < s.size() && std::isalpha(static_cast<unsigned char>(s[at]))) ++at;  // (1st, 2nd, ...)
        } else if (token("DD") || token("D")) {
            if (!number(2, &v)) return false;
            c.d = static_cast<int>(v);
        } else if (token("HH") || token("H")) {
            if (!number(2, &v)) return false;
            c.h = static_cast<int>(v);
        } else if (token("hh") || token("h")) {
            if (!number(2, &v)) return false;
            c.h = static_cast<int>(v), ampm = true;
        } else if (token("mm") || token("m")) {
            if (!number(2, &v)) return false;
            c.mi = static_cast<int>(v);
        } else if (token("ss") || token("s")) {
            if (!number(2, &v)) return false;
            c.s = static_cast<int>(v);
        } else if (token("SSS") || token("SS") || token("S")) {
            const size_t from = at;
            if (!number(3, &v)) return false;
            for (size_t n = at - from; n < 3; ++n) v *= 10;
            c.ms = static_cast<int>(v);
        } else if (token("A") || token("a")) {
            const std::string l = diagram_lower(s.substr(at, 2));
            if (l != "am" && l != "pm") return false;
            pm = l == "pm", ampm = true, at += 2;
        } else if (token("X") || token("x")) {  // seconds (milliseconds) from 1970
            const bool ms = fmt[f - 1] == 'x';
            const bool minus = at < s.size() && s[at] == '-';
            if (minus) ++at;
            if (!number(15, &v)) return false;
            const i64 t = (minus ? -v : v) * (ms ? 1 : kSecond);
            if (t < kEarliest || t > kLatest) return false;
            if (at < s.size() && s[at] == '.') {  // (a part of a second: left out)
                ++at;
                i64 part = 0;
                number(9, &part);
            }
            c = civil(t);
        } else if (token("ZZ") || token("Z")) {  // a zone: read and left out
            if (at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
            number(2, &v);
            if (at < s.size() && s[at] == ':') ++at;
            number(2, &v);
        } else {
            // Anything else is itself; a space matches any run of them.
            if (fmt[f] == ' ') {
                if (at >= s.size() || s[at] != ' ') return false;
                while (at < s.size() && s[at] == ' ') ++at;
                while (f < fmt.size() && fmt[f] == ' ') ++f;
                continue;
            }
            if (at >= s.size() || s[at] != fmt[f]) return false;
            ++at, ++f;
        }
    }
    if (at != s.size()) return false;
    if (ampm) {
        if (c.h < 1 || c.h > 12) return false;
        c.h = c.h % 12 + (pm ? 12 : 0);
    }
    if (c.mo < 1 || c.mo > 12 || c.d < 1 || c.d > days_in(c.y, c.mo) || c.h > 23 || c.mi > 59 || c.s > 59) return false;
    *out = time_of(c);
    return true;
}

// By the chart's dateFormat, else as ISO 8601 has it.
bool parse_date(std::string_view s, std::string_view fmt, i64* out) {
    s = diagram_trim(s);
    if (s.empty()) return false;
    if (parse_with(s, diagram_trim(fmt), out)) return true;
    for (const char* f : {"YYYY-MM-DD", "YYYY-MM-DD HH:mm", "YYYY-MM-DD HH:mm:ss", "YYYY-MM-DD[T]HH:mm", "YYYY-MM-DD[T]HH:mm:ss", "YYYY-MM-DD[T]HH:mm:ss.SSS"})
        if (parse_with(s, f, out)) return true;
    return false;
}

// A length: a number and its unit (ms, s, m, h, d, w; M and y by the calendar).
bool parse_duration(std::string_view s, i64 from, i64* end) {
    s = diagram_trim(s);
    size_t n = 0;
    while (n < s.size() && (std::isdigit(static_cast<unsigned char>(s[n])) || s[n] == '.')) ++n;
    if (n == 0 || n > 12) return false;
    const std::string num(s.substr(0, n));
    char* stop = nullptr;
    const double v = std::strtod(num.c_str(), &stop);
    if (stop != num.c_str() + num.size() || !(v >= 0) || v > 1e7) return false;
    const std::string_view unit = diagram_trim(s.substr(n));
    i64 per = 0;
    if (unit == "ms") per = 1;
    else if (unit == "s") per = kSecond;
    else if (unit == "m") per = kMinute;
    else if (unit == "h") per = kHour;
    else if (unit == "d") per = kDay;
    else if (unit == "w") per = 7 * kDay;
    else if (unit == "M" || unit == "y") {
        const i64 months = static_cast<i64>(std::llround(v * (unit == "y" ? 12 : 1)));
        if (months > 120000) return false;
        *end = add_months(from, months);
        return *end <= kLatest;
    } else {
        return false;
    }
    *end = from + static_cast<i64>(std::llround(v * static_cast<double>(per)));
    return *end <= kLatest;
}

// A date written by an axisFormat: %Y %y %m %d %e %b %B %a %A %H %I %M %S
// %L %p %j %%, %-d (no padding), %_d (spaces).
std::string format_date(i64 t, std::string_view fmt) {
    static const char* const kShortMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* const kLongMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* const kShortDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kLongDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const Civil c = civil(t);
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        char pad = 0;  // (the code's own)
        char k = fmt[++i];
        if ((k == '-' || k == '_' || k == '0') && i + 1 < fmt.size()) pad = k, k = fmt[++i];
        auto num = [&](i64 v, int width, char fill) {
            std::string d = std::to_string(v);
            const char p = pad == '-' ? 0 : pad == '_' ? ' ' : pad == '0' ? '0' : fill;
            if (p)
                while (static_cast<int>(d.size()) < width) d.insert(d.begin(), p);
            out += d;
        };
        switch (k) {
            case 'Y': num(c.y, 4, '0'); break;
            case 'y': num(((c.y % 100) + 100) % 100, 2, '0'); break;
            case 'm': num(c.mo, 2, '0'); break;
            case 'd': num(c.d, 2, '0'); break;
            case 'e': num(c.d, 2, ' '); break;
            case 'H': num(c.h, 2, '0'); break;
            case 'I': num(c.h % 12 ? c.h % 12 : 12, 2, '0'); break;
            case 'M': num(c.mi, 2, '0'); break;
            case 'S': num(c.s, 2, '0'); break;
            case 'L': num(c.ms, 3, '0'); break;
            case 'j': num(floor_div(t, kDay) - days_from_civil(c.y, 1, 1) + 1, 3, '0'); break;
            case 'p': out += c.h < 12 ? "AM" : "PM"; break;
            case 'b': out += kShortMonths[c.mo - 1]; break;
            case 'B': out += kLongMonths[c.mo - 1]; break;
            case 'a': out += kShortDays[weekday(t)]; break;
            case 'A': out += kLongDays[weekday(t)]; break;
            case '%': out += '%'; break;
            default: out += '%', out += k; break;
        }
    }
    return out;
}

// A date as the axis says it: written by axisFormat, its runs of spaces
// one space each (%e pads a day to two places, 'Oct  1': Mermaid's labels
// are SVG text, where the two are one).
std::string axis_label(i64 t, std::string_view fmt) {
    const std::string all = format_date(t, fmt);
    std::string out;
    for (const char c : all)
        if (c != ' ' || (!out.empty() && out.back() != ' ')) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---------------------------------------------------------------- drawing

// Lines of text, their block centred on cy, each from x (align 0),
// centred on it (1) or ending at it (2).
void text_block(Drawing* d, const DiagramFont& font, const std::vector<std::string>& lines, float x, float cy, float size, DiagramColour c, int align, bool bold) {
    const float line = font.line(size), th = static_cast<float>(lines.size()) * line;
    float y = cy - th / 2 + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
    for (const std::string& l : lines) {
        d->text(l, x, y, size, c, font, align, bold);
        y += line;
    }
}

float widest(const std::vector<std::string>& lines, const DiagramFont& font, float size, bool bold = false) {
    float w = 0;
    for (const std::string& l : lines) w = std::max(w, font.width(l, size, bold));
    return w;
}

// A label's lines, wrapped to `most` points.
std::vector<std::string> wrapped(std::string_view s, float most, float size, const DiagramFont& font) {
    return diagram_wrap(diagram_label(s), most, size, font);
}

// A name without spaces, for the tests' geometry.
std::string geo_name(std::string_view s) {
    std::string out(diagram_trim(s));
    for (char& c : out)
        if (c == ' ' || c == '\t') c = '_';
    return out.empty() ? "_" : out;
}

// A title over a drawing, in the middle: bold, larger.
float title_band(Drawing* d, const DiagramStyle& style, const std::string& title, float top, float w) {
    if (title.empty()) return 0;
    const DiagramFont& font = *style.font;
    const float u = style.unit(), size = style.size * 1.2f;
    const std::vector<std::string> lines = diagram_wrap(diagram_label(title), std::max(w - 20 * u, 80 * u), size, font);
    text_block(d, font, lines, w / 2, top + static_cast<float>(lines.size()) * font.line(size) / 2, size, style.palette.of(kMdText), 1, true);
    return static_cast<float>(lines.size()) * font.line(size) + 10 * u;
}

// ---------------------------------------------------------------- Gantt charts

struct Task {
    std::string name, id;
    int no = 0, section = 0;
    bool done = false, active = false, crit = false, milestone = false;
    std::string start, end;  // as written ("" for the start: after the task before)
    int before = -1;         // the task before it (its start, when none is written)
    i64 from = 0, to = 0, shown_to = 0;
    bool known = false, manual_end = false;
};

struct Gantt {
    std::string title, date_format = "YYYY-MM-DD", axis_format, tick_interval, today_css;
    bool today = false, inclusive = false, top_axis = false, weekends = false;
    int weekend_from = 6;  // Saturday (and Sunday); or Friday (and Saturday)
    int week_from = 0;     // Sunday
    bool excluded_day[7] = {};
    std::vector<std::pair<int, std::string>> excludes, includes;  // dates, read once the format is known
    std::set<i64> excluded_dates, included_dates;
    std::vector<std::string> sections;  // (the first unnamed: tasks before any section)
    std::vector<Task> tasks;
};

std::vector<std::string> split(std::string_view s, char by) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t c = s.find(by, at);
        out.emplace_back(diagram_trim(s.substr(at, c == std::string_view::npos ? std::string_view::npos : c - at)));
        if (c == std::string_view::npos) break;
        at = c + 1;
    }
    return out;
}

bool parse_gantt(const std::vector<DiagramLine>& lines, Gantt& g, std::string* err) {
    g.sections.emplace_back();
    bool described = false;  // (in accDescr { ... })
    for (size_t k = 1; k < lines.size(); ++k) {
        const std::string_view s = diagram_trim(lines[k].text);
        const std::string l = diagram_lower(s);
        auto fail = [&](const std::string& why) {
            *err = "line " + std::to_string(lines[k].no) + ": " + why;
            return false;
        };
        auto word = [&](std::string_view w) { return l.starts_with(w) && (l.size() == w.size() || l[w.size()] == ' ' || l[w.size()] == '\t' || l[w.size()] == ':'); };
        auto rest = [&](size_t n) { return std::string(diagram_trim(s.substr(std::min(n, s.size())))); };
        if (described) {
            described = s.find('}') == std::string_view::npos;
            continue;
        }
        if (word("title")) g.title = rest(5);
        else if (word("dateformat")) g.date_format = rest(10);
        else if (word("axisformat")) g.axis_format = rest(10);
        else if (word("tickinterval")) g.tick_interval = rest(12);
        else if (word("excludes") || word("includes")) {
            // (weekends, days' names and dates, apart by commas or spaces)
            const bool ex = word("excludes");
            std::string list = rest(8);
            std::replace(list.begin(), list.end(), '\t', ' ');
            std::replace(list.begin(), list.end(), ',', ' ');
            for (const std::string& w : split(list, ' ')) {
                const std::string lw = diagram_lower(w);
                if (lw.empty()) continue;
                if (ex && lw == "weekends") g.weekends = true;
                else if (const int d = day_named(lw); ex && d >= 0) g.excluded_day[d] = true;
                else (ex ? g.excludes : g.includes).emplace_back(lines[k].no, w);
            }
        } else if (word("todaymarker")) {
            g.today_css = rest(11);
            g.today = diagram_lower(g.today_css) != "off";
        } else if (word("inclusiveenddates")) g.inclusive = true;
        else if (word("topaxis")) g.top_axis = true;
        else if (word("weekday")) {
            const int d = day_named(rest(7));
            if (d < 0) return fail("weekday is a day's name");
            g.week_from = d;
        } else if (word("weekend")) {
            const int d = day_named(rest(7));
            if (d != 5 && d != 6) return fail("weekend is friday or saturday");
            g.weekend_from = d;
        } else if (word("section")) g.sections.push_back(rest(7));
        else if (word("displaymode") || word("click") || word("acctitle") || word("accdescr") || l == "gantt") {
            described = word("accdescr") && s.find('{') != std::string_view::npos && s.find('}') == std::string_view::npos;
        } else if (const size_t colon = s.find(':'); colon != std::string_view::npos) {
            if (g.tasks.size() >= 2000) return fail("too many tasks to draw");
            Task t;
            t.no = lines[k].no;
            t.name = std::string(diagram_trim(s.substr(0, colon)));
            t.section = static_cast<int>(g.sections.size()) - 1;
            std::string data(s.substr(colon + 1));
            if (const size_t cut = data.find_first_of("#;"); cut != std::string::npos) data.resize(cut);
            std::vector<std::string> parts = split(data, ',');
            for (bool more = true; more && !parts.empty();) {
                const std::string p = diagram_lower(parts.front());
                more = p == "done" || p == "active" || p == "crit" || p == "milestone" || p == "vert";
                if (!more) break;
                t.done |= p == "done", t.active |= p == "active", t.crit |= p == "crit", t.milestone |= p == "milestone" || p == "vert";
                parts.erase(parts.begin());
            }
            if (parts.empty() || (parts.size() == 1 && parts[0].empty())) return fail("\"" + t.name + "\" needs its length or its end");
            if (parts.size() > 3) return fail("a task is name : [tags,] [id,] [start,] end or length");
            if (parts.size() == 3) t.id = parts[0];
            if (parts.size() >= 2) t.start = parts[parts.size() - 2];
            t.end = parts.back();
            t.before = static_cast<int>(g.tasks.size()) - 1;
            if (t.start.empty() && t.before < 0) return fail("the first task needs a start");
            if (t.id.empty()) t.id = "task" + std::to_string(g.tasks.size() + 1);
            g.tasks.push_back(std::move(t));
        } else {
            return fail("\"" + std::string(s.substr(0, 24)) + "\" is not a task (name : when)");
        }
    }
    if (g.tasks.empty()) {
        *err = "no tasks in it";
        return false;
    }
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [no, text] : pass ? g.includes : g.excludes) {
            i64 t = 0;
            if (parse_date(text, g.date_format, &t)) (pass ? g.included_dates : g.excluded_dates).insert(floor_div(t, kDay));
        }
    return true;
}

bool excluded(const Gantt& g, i64 t) {
    const i64 day = floor_div(t, kDay);
    if (g.included_dates.contains(day)) return false;
    const int wd = weekday(t);
    if (g.weekends && (wd == g.weekend_from || wd == (g.weekend_from + 1) % 7)) return true;
    return g.excluded_day[wd] || g.excluded_dates.contains(day);
}

bool any_excluded(const Gantt& g) {
    bool any = g.weekends || !g.excluded_dates.empty();
    for (const bool d : g.excluded_day) any = any || d;
    return any;
}

// Each task's start and end: in as many passes as after and until need
// (a task may follow one named further down).
bool resolve(Gantt& g, std::string* err) {
    std::map<std::string, int> by_id;
    for (size_t i = 0; i < g.tasks.size(); ++i) by_id.emplace(g.tasks[i].id, static_cast<int>(i));
    const bool skips = any_excluded(g);
    for (bool progress = true; progress;) {
        progress = false;
        for (Task& t : g.tasks) {
            if (t.known) continue;
            auto fail = [&](const std::string& why) {
                *err = "line " + std::to_string(t.no) + ": " + why;
                return false;
            };
            // Its start: after tasks (the latest of their ends), a date, or the task before's end.
            i64 from = 0;
            const std::string ls = diagram_lower(t.start);
            if (t.start.empty()) {
                const Task& b = g.tasks[static_cast<size_t>(t.before)];
                if (!b.known) continue;
                from = b.to;
            } else if (ls.starts_with("after ")) {
                bool ready = true, any = false;
                for (const std::string& id : split(t.start.substr(6), ' ')) {
                    if (id.empty()) continue;
                    const auto it = by_id.find(id);
                    if (it == by_id.end()) return fail("no task " + id + " to come after");
                    const Task& o = g.tasks[static_cast<size_t>(it->second)];
                    ready = ready && o.known;
                    from = any ? std::max(from, o.to) : o.to, any = true;
                }
                if (!any) return fail("after which task?");
                if (!ready) continue;
            } else if (!parse_date(t.start, g.date_format, &from)) {
                return fail("\"" + t.start + "\" is not a date (dateFormat " + g.date_format + ")");
            }
            // Its end: a date, a length, or until tasks (the earliest of their starts).
            i64 to = 0;
            const std::string le = diagram_lower(t.end);
            t.manual_end = false;
            if (parse_with(t.end, diagram_trim(g.date_format), &to)) {
                t.manual_end = true;
                if (g.inclusive) to += kDay;
            } else if (parse_duration(t.end, from, &to)) {
            } else if (le.starts_with("until ")) {
                bool ready = true, any = false;
                for (const std::string& id : split(t.end.substr(6), ' ')) {
                    if (id.empty()) continue;
                    const auto it = by_id.find(id);
                    if (it == by_id.end()) return fail("no task " + id + " to end at");
                    const Task& o = g.tasks[static_cast<size_t>(it->second)];
                    ready = ready && o.known;
                    to = any ? std::min(to, o.from) : o.from, any = true;
                }
                if (!any) return fail("until which task?");
                if (!ready) continue;
                t.manual_end = true;
            } else if (parse_date(t.end, g.date_format, &to)) {
                t.manual_end = true;
                if (g.inclusive) to += kDay;
            } else {
                return fail("\"" + t.end + "\" is neither a date nor a length (3d, 2w, 12h)");
            }
            if (to < from) to = from;
            t.from = from, t.to = to, t.shown_to = to;
            // A length skips the days left out (as Mermaid does: its bar
            // ends before them, and what comes after it after them).
            if (skips && !t.manual_end) {
                i64 at = from, end = to, shown = to;
                bool out = false;
                for (int n = 0; at <= end && n < 200000; ++n) {
                    if (!out) shown = end;
                    out = excluded(g, at);
                    if (out) end += kDay;
                    at += kDay;
                }
                t.to = end, t.shown_to = std::max(from, shown);
            }
            t.known = progress = true;
        }
    }
    for (const Task& t : g.tasks)
        if (!t.known) {
            *err = "line " + std::to_string(t.no) + ": its after or until goes round in a circle";
            return false;
        }
    return true;
}

// Ticks along the axis: every `step` of a unit, on the calendar's own
// boundaries (midnight, the week's first day, the 1st, 1 January).
enum Unit : u8 { kMs, kSec, kMin, kHourU, kDayU, kWeek, kMonth, kYear };

// The finest unit an axisFormat tells apart: ticks finer than it would
// all say the same (6 hours apart, each '2024-01-01').
Unit finest(std::string_view fmt) {
    Unit u = kYear;
    bool any = false;
    for (size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] != '%') continue;
        char k = fmt[++i];
        if ((k == '-' || k == '_' || k == '0') && i + 1 < fmt.size()) k = fmt[++i];
        Unit of = kYear;
        switch (k) {
            case 'H': case 'I': case 'M': case 'S': case 'L': case 'p': of = kMs; break;
            case 'd': case 'e': case 'j': case 'a': case 'A': of = kDayU; break;
            case 'm': case 'b': case 'B': of = kMonth; break;
            case 'Y': case 'y': of = kYear; break;
            default: continue;
        }
        u = any ? std::min(u, of) : of, any = true;
    }
    return any ? u : kDayU;  // (no dates in it at all: as days)
}

std::vector<i64> ticks(i64 t0, i64 t1, Unit unit, i64 step, int week_from, size_t most) {
    std::vector<i64> out;
    if (step <= 0) return out;
    auto fixed = [&](i64 per) {
        for (i64 t = floor_div(t0 + per * step - 1, per * step) * per * step; t <= t1 && out.size() <= most; t += per * step) out.push_back(t);
    };
    switch (unit) {
        case kMs: fixed(1); break;
        case kSec: fixed(kSecond); break;
        case kMin: fixed(kMinute); break;
        case kHourU: fixed(kHour); break;
        case kDayU: fixed(kDay); break;
        case kWeek: {
            i64 t = floor_div(t0 + kDay - 1, kDay) * kDay;
            while (weekday(t) != week_from) t += kDay;
            for (; t <= t1 && out.size() <= most; t += 7 * kDay * step) out.push_back(t);
            break;
        }
        case kMonth:
        case kYear: {
            Civil c = civil(t0);
            c.d = 1, c.h = c.mi = c.s = c.ms = 0;
            if (unit == kYear) c.mo = 1;
            i64 t = time_of(c);
            const i64 months = unit == kYear ? 12 * step : step;
            // (on months, and years, that are multiples of the step)
            while (t < t0 || (unit == kYear ? civil(t).y % step : (civil(t).mo - 1) % step) != 0) {
                t = add_months(t, unit == kYear ? 12 : 1);
                if (t > t1) break;
            }
            for (; t <= t1 && out.size() <= most; t = add_months(t, months)) out.push_back(t);
            break;
        }
    }
    return out;
}

void draw_gantt(Gantt& g, const DiagramStyle& style, Drawing* d, DiagramGeometry* geo) {
    const DiagramFont& font = *style.font;
    const DiagramPalette& p = style.palette;
    const bool dark = p.dark();
    const float u = style.unit(), size = style.size, small = style.size * 0.86f;
    const float line = font.line(size), pad = 14 * u;
    // (none near the critical tasks' red: a section's tasks would look critical)
    const std::vector<u8> series = diagram_series(p, kMdError);
    auto ink_of = [&](int section) { return series[static_cast<size_t>(section) % series.size()]; };

    // The sections' names, in a column at the left (if any is named).
    bool named = false;
    for (const std::string& s : g.sections) named = named || !s.empty();
    const float name_most = 150 * u;
    std::vector<std::vector<std::string>> names(g.sections.size());
    float col = 0;
    if (named) {
        for (size_t i = 0; i < g.sections.size(); ++i) {
            names[i] = wrapped(g.sections[i], name_most, size, font);
            col = std::max(col, widest(names[i], font, size, true));
        }
        col = std::max(col, 40 * u) + 26 * u;
    }
    const float w = std::max(style.room, col + 2 * pad + 260 * u);
    // (room at the ends for half a milestone's diamond, at the first date or the last)
    const float bar_h = line + 9 * u, gap = 7 * u, band_pad = 6 * u, diamond = bar_h * 0.8f;
    bool milestones = false;
    for (const Task& t : g.tasks) milestones = milestones || t.milestone;
    const float x0 = pad + col + std::max(10 * u, milestones ? diamond / 2 + 3 * u : 0), x1 = w - pad - std::max(8 * u, milestones ? diamond / 2 + 3 * u : 0);

    i64 t0 = g.tasks[0].from, t1 = g.tasks[0].shown_to;
    for (const Task& t : g.tasks) t0 = std::min(t0, t.from), t1 = std::max({t1, t.shown_to, t.from});
    if (t1 <= t0) t1 = t0 + kDay;
    const double span = static_cast<double>(t1 - t0);
    auto x_of = [&](i64 t) { return x0 + static_cast<float>(static_cast<double>(t - t0) / span * (x1 - x0)); };

    // The axis: the coarsest ticks whose labels fit apart (or tickInterval's).
    const std::string axis_format = g.axis_format.empty() ? "%Y-%m-%d" : g.axis_format;
    std::vector<i64> marks;
    auto labels_fit = [&](const std::vector<i64>& m) {
        if (m.size() < 2) return true;
        float most = 0;
        for (const i64 t : m) most = std::max(most, font.width(axis_label(t, axis_format), small));
        return (x1 - x0) * static_cast<float>(static_cast<double>(m[1] - m[0]) / span) >= most + 14 * u;
    };
    if (!g.tick_interval.empty()) {
        const std::string ti = diagram_lower(g.tick_interval);
        size_t n = 0;
        while (n < ti.size() && std::isdigit(static_cast<unsigned char>(ti[n]))) ++n;
        const i64 step = n && n < 7 ? std::atoll(ti.substr(0, n).c_str()) : 0;
        const std::string unit = ti.substr(n);
        const Unit un = unit == "millisecond" ? kMs : unit == "second" ? kSec : unit == "minute" ? kMin : unit == "hour" ? kHourU : unit == "day" ? kDayU : unit == "week" ? kWeek : unit == "month" ? kMonth : kYear;
        if (step > 0 && (un != kYear || unit == "year")) marks = ticks(t0, t1, un, step, g.week_from, 400);
        if (marks.size() > 400) marks.clear();
    }
    if (marks.empty()) {
        static const std::pair<Unit, int> kSteps[] = {{kSec, 1},   {kSec, 5},   {kSec, 15},  {kSec, 30},  {kMin, 1},  {kMin, 5},  {kMin, 15},  {kMin, 30},
                                                      {kHourU, 1}, {kHourU, 3}, {kHourU, 6}, {kHourU, 12}, {kDayU, 1}, {kDayU, 2}, {kWeek, 1},  {kMonth, 1},
                                                      {kMonth, 3}, {kYear, 1},  {kYear, 2},  {kYear, 5},  {kYear, 10}, {kYear, 25}, {kYear, 50}, {kYear, 100}};
        const Unit least = finest(axis_format);
        for (const auto& [un, step] : kSteps) {
            if (un < least) continue;
            const i64 per = un == kSec ? kSecond : un == kMin ? kMinute : un == kHourU ? kHour : un == kDayU ? kDay : un == kWeek ? 7 * kDay : un == kMonth ? 28 * kDay : 365 * kDay;
            if (span / static_cast<double>(per * step) > 200) continue;
            marks = ticks(t0, t1, un, step, g.week_from, 400);
            if (labels_fit(marks)) break;
        }
    }

    // Rows: a task each. Its name is in its bar if it fits, else beside it
    // on the side that has the room; a name too long for either side is
    // wrapped beside the bar, or put on lines of its own above it, across
    // the chart (whichever makes the lower row: a long name beside a bar
    // late in the chart would be a column six lines tall).
    struct Row {
        float y = 0, h = 0;
        float bx = 0, bw = 0;           // the bar
        std::vector<std::string> label;
        int side = 0;                   // 0 in the bar, 1 right of it, -1 left of it, 2 above it
    };
    // (names beside bars and above them stay this far inside the bands)
    const float over_gap = 3 * u, chart_left = pad + col + 4 * u, chart_right = w - pad - 4 * u;
    std::vector<Row> rows(g.tasks.size());
    for (size_t i = 0; i < g.tasks.size(); ++i) {
        const Task& t = g.tasks[i];
        Row& r = rows[i];
        const float a = x_of(t.from), b = x_of(t.shown_to);
        if (t.milestone) {
            r.bx = (a + b) / 2 - diamond / 2, r.bw = diamond;
        } else {
            r.bx = a, r.bw = std::max(b - a, 3 * u);
            if (r.bx + r.bw > x1 + 3 * u) r.bx = x1 + 3 * u - r.bw;
        }
        const std::vector<std::string> one = diagram_label(t.name);
        const float tw = widest(one, font, size);
        const float right = chart_right - (r.bx + r.bw + 8 * u), left = r.bx - 8 * u - chart_left;
        if (!t.milestone && one.size() == 1 && tw + 16 * u <= r.bw) r.label = one, r.side = 0;
        else if (tw <= right) r.label = one, r.side = 1;
        else if (tw <= left) r.label = one, r.side = -1;
        else {
            r.side = right >= left ? 1 : -1;
            r.label = diagram_wrap(one, std::max(std::max(right, left), 40 * u), size, font);
            if (r.label.size() > 2) {
                std::vector<std::string> over = diagram_wrap(one, chart_right - chart_left, size, font);
                if (static_cast<float>(over.size()) * line + over_gap + bar_h < static_cast<float>(r.label.size()) * line + 4 * u) r.label = std::move(over), r.side = 2;
            }
        }
        r.h = r.side == 2 ? static_cast<float>(r.label.size()) * line + over_gap + bar_h : std::max(bar_h, static_cast<float>(r.label.size()) * line + 4 * u);
    }

    float y = pad;
    y += title_band(d, style, g.title, y, w);
    const float axis_h = 6 * u + font.line(small) + 4 * u;
    const float axis_top = y;
    if (g.top_axis) y += axis_h;
    const float bands_top = y;
    struct Band {
        float y = 0, h = 0;
    };
    std::vector<Band> bands(g.sections.size());
    {
        size_t i = 0;
        for (size_t s = 0; s < g.sections.size(); ++s) {
            float rows_h = 0;
            const size_t first = i;
            while (i < g.tasks.size() && g.tasks[i].section == static_cast<int>(s)) rows_h += rows[i].h + gap, ++i;
            if (i == first) continue;  // (a section with no tasks: not drawn)
            if (y > bands_top) y += 2 * u;  // (a hairline between bands)
            rows_h -= gap;
            const float name_h = named ? static_cast<float>(names[s].size()) * line : 0;
            const float h = std::max(rows_h, name_h) + 2 * band_pad;
            float ry = y + band_pad + (h - 2 * band_pad - rows_h) / 2;
            for (size_t k = first; k < i; ++k) rows[k].y = ry, ry += rows[k].h + gap;
            bands[s] = {y, h};
            y += h;
        }
    }
    const float bands_bottom = y;
    y += axis_h;
    d->w = w, d->h = y + pad;

    // Sections' bands, in turn lighter and darker, a stripe of their colour at the left.
    for (size_t s = 0, n = 0; s < g.sections.size(); ++s) {
        if (bands[s].h <= 0) continue;
        RasterPath band;
        band.rect(pad, bands[s].y, w - 2 * pad, bands[s].h, 0);
        d->shape(std::move(band), p.tone(ink_of(static_cast<int>(s)), (n++ % 2 ? 0.03f : 0.075f) * (dark ? 1.2f : 1)));
        if (named) {
            RasterPath stripe;
            stripe.rect(pad, bands[s].y + 3 * u, 3 * u, bands[s].h - 6 * u, 1.5f * u);
            d->shape(std::move(stripe), p.of(ink_of(static_cast<int>(s))));
            text_block(d, font, names[s], pad + 12 * u, bands[s].y + bands[s].h / 2, size, p.of(kMdText), 0, true);
        }
        if (geo) geo->boxes.push_back({"cluster", "section" + std::to_string(s), pad, bands[s].y, w - 2 * pad, bands[s].h});
    }
    // Days left out, shaded (where a day is wide enough to see).
    const float day_w = static_cast<float>(static_cast<double>(kDay) / span * (x1 - x0));
    if (any_excluded(g) && day_w >= 1.5f * u && span / kDay < 20000) {
        for (i64 t = floor_div(t0, kDay) * kDay; t < t1;) {
            if (!excluded(g, t)) {
                t += kDay;
                continue;
            }
            i64 e = t;
            while (e < t1 && excluded(g, e)) e += kDay;
            const float a = std::max(x0, x_of(t)), b = std::min(x1, x_of(e));
            if (b > a) {
                RasterPath r;
                r.rect(a, bands_top, b - a, bands_bottom - bands_top, 0);
                d->shape(std::move(r), p.tone(kMdMuted, dark ? 0.13f : 0.1f));
            }
            t = e;
        }
    }
    // The grid and the axis (or axes) with their labels.
    const DiagramColour grid = p.tone(kMdBorder, dark ? 0.22f : 0.2f), axis_text = p.tone(kMdText, 0.75f);
    float last_end = -1e9f, top_end = -1e9f;
    for (const i64 t : marks) {
        const float x = x_of(t);
        RasterPath gl;
        gl.move_to(x, bands_top), gl.line_to(x, bands_bottom + 5 * u);
        d->shape(std::move(gl), {}, grid, 1 * u);
        const std::string label = axis_label(t, axis_format);
        const float lw = font.width(label, small);
        const float lx = std::clamp(x - lw / 2, 2 * u, w - 2 * u - lw);
        if (lx < last_end + 6 * u) continue;
        last_end = lx + lw;
        d->text(label, lx, bands_bottom + 6 * u + font.ascent(small) + 1 * u, small, axis_text, font);
        if (g.top_axis && lx >= top_end + 6 * u) {
            RasterPath tick;
            tick.move_to(x, bands_top - 5 * u), tick.line_to(x, bands_top);
            d->shape(std::move(tick), {}, grid, 1 * u);
            d->text(label, lx, axis_top + font.ascent(small) + 1 * u, small, axis_text, font);
            top_end = lx + lw;
        }
    }
    {
        RasterPath base;
        base.move_to(x0, bands_bottom), base.line_to(x1, bands_bottom);
        d->shape(std::move(base), {}, p.tone(kMdBorder, dark ? 0.45f : 0.4f), 1.2f * u);
    }

    // The tasks: bars (diamonds for milestones) in their section's colour,
    // or grey when done, outlined when active, red when critical.
    for (size_t i = 0; i < g.tasks.size(); ++i) {
        const Task& t = g.tasks[i];
        const Row& r = rows[i];
        const u8 ink = t.crit ? static_cast<u8>(kMdError) : ink_of(t.section);
        DiagramColour fill = p.tone(ink, dark ? 0.4f : 0.32f), stroke = p.tone(ink, 0.85f);
        float sw = 1.2f * u;
        if (t.done) fill = p.tone(kMdMuted, dark ? 0.3f : 0.22f), stroke = t.crit ? p.of(kMdError) : p.tone(kMdMuted, 0.6f);
        if (t.active) fill = p.tone(ink, dark ? 0.16f : 0.12f), stroke = p.of(ink), sw = 2 * u;
        // (the bar at the row's middle, or at its foot under a name above it)
        const float cy = r.side == 2 ? r.y + r.h - bar_h / 2 : r.y + r.h / 2;
        RasterPath shape;
        float by = cy - bar_h / 2, bh = bar_h;
        if (t.milestone) {
            const float m = r.bw / 2;
            const RasterPoint pts[] = {{r.bx + m, cy - m}, {r.bx + 2 * m, cy}, {r.bx + m, cy + m}, {r.bx, cy}};
            shape.polygon(pts, 4);
            fill = t.done ? fill : p.tone(ink, dark ? 0.6f : 0.5f);
            by = cy - m, bh = 2 * m;
        } else {
            shape.rect(r.bx, by, r.bw, bh, std::min(4 * u, r.bw / 2));
        }
        d->shape(std::move(shape), fill, stroke, sw);
        const DiagramColour tc = t.done ? p.tone(kMdText, 0.8f) : p.of(kMdText);
        const float lw = widest(r.label, font, size), lh = static_cast<float>(r.label.size()) * line;
        float lx = 0, ly = cy;
        if (r.side == 0) {
            text_block(d, font, r.label, r.bx + r.bw / 2, cy, size, tc, 1, false);
        } else if (r.side == 2) {
            // (from the bar's start if it fits from there, else ending at the chart's end)
            lx = std::max(chart_left, std::min(r.bx, chart_right - lw)), ly = r.y + lh / 2;
            text_block(d, font, r.label, lx, ly, size, tc, 0, false);
        } else {
            lx = r.side > 0 ? r.bx + r.bw + 8 * u : r.bx - 8 * u - lw;
            text_block(d, font, r.label, lx, cy, size, tc, 0, false);
        }
        if (geo) {
            geo->boxes.push_back({"node", geo_name(t.id), r.bx, by, r.bw, bh});
            if (r.side != 0) geo->boxes.push_back({"label", geo_name(t.id), lx, ly - lh / 2, lw, lh});
        }
    }

    // Today, if todayMarker asks for it (stroke and stroke-width as it
    // says): at its midnight, the day the style says (so the picture is the
    // same all day, and is laid out again the next).
    if (g.today && style.today != kDiagramNoDay && style.today > kEarliest / kDay && style.today < kLatest / kDay) {
        const i64 t = style.today * kDay;
        if (t >= t0 && t <= t1) {
            DiagramColour mark = p.of(kMdError);
            float width = 2 * u;
            for (const std::string& part : split(g.today_css, ',')) {
                const size_t colon = part.find(':');
                if (colon == std::string::npos) continue;
                const std::string key = diagram_lower(diagram_trim(std::string_view(part).substr(0, colon)));
                const std::string_view v = diagram_trim(std::string_view(part).substr(colon + 1));
                if (key == "stroke") diagram_colour(v, &mark);
                else if (key == "stroke-width") width = std::clamp(static_cast<float>(std::atof(std::string(v).c_str())), 0.5f, 12.0f) * u;
                else if (key == "opacity") mark.a *= std::clamp(static_cast<float>(std::atof(std::string(v).c_str())), 0.0f, 1.0f);
            }
            RasterPath m;
            m.move_to(x_of(t), bands_top), m.line_to(x_of(t), bands_bottom);
            d->shape(std::move(m), {}, mark, width);
            if (geo) geo->boxes.push_back({"today", "today", x_of(t), bands_top, 0, bands_bottom - bands_top});
        }
    }
}

// ---------------------------------------------------------------- timelines

struct Period {
    std::string text;
    std::vector<std::string> events;
    int section = -1;
};

struct Timeline {
    std::string title;
    std::vector<std::string> sections;
    std::vector<Period> periods;
};

bool parse_timeline(const std::vector<DiagramLine>& lines, Timeline& t, std::string* err) {
    bool described = false;
    for (size_t k = 1; k < lines.size(); ++k) {
        const std::string_view s = diagram_trim(lines[k].text);
        const std::string l = diagram_lower(s);
        auto word = [&](std::string_view w) { return l.starts_with(w) && (l.size() == w.size() || l[w.size()] == ' ' || l[w.size()] == '\t' || l[w.size()] == ':'); };
        if (described) {
            described = s.find('}') == std::string_view::npos;
            continue;
        }
        if (word("title")) t.title = std::string(diagram_trim(s.substr(5)));
        else if (word("section")) t.sections.emplace_back(diagram_trim(s.substr(7)));
        else if (word("acctitle") || word("accdescr")) described = word("accdescr") && s.find('{') != std::string_view::npos && s.find('}') == std::string_view::npos;
        else {
            std::vector<std::string> parts = split(s, ':');
            if (parts[0].empty()) {  // : more events for the period before
                if (t.periods.empty()) {
                    *err = "line " + std::to_string(lines[k].no) + ": an event with no period before it";
                    return false;
                }
            } else {
                if (t.periods.size() >= 500) {
                    *err = "line " + std::to_string(lines[k].no) + ": too many periods to draw";
                    return false;
                }
                t.periods.push_back({parts[0], {}, static_cast<int>(t.sections.size()) - 1});
            }
            for (size_t i = 1; i < parts.size(); ++i)
                if (!parts[i].empty() && t.periods.back().events.size() < 200) t.periods.back().events.push_back(parts[i]);
        }
    }
    if (t.periods.empty()) {
        *err = "no periods in it";
        return false;
    }
    return true;
}

void draw_timeline(const Timeline& t, const DiagramStyle& style, Drawing* d, DiagramGeometry* geo) {
    const DiagramFont& font = *style.font;
    const DiagramPalette& p = style.palette;
    const bool dark = p.dark();
    const float u = style.unit(), size = style.size, line = font.line(size);
    const float pad = 14 * u, gap = 14 * u, px = 10 * u, py = 7 * u, row_gap = 26 * u;
    const std::vector<u8> series = diagram_series(p);
    const bool sectioned = !t.sections.empty();
    // A colour for each period: its section's, or its own when there are none.
    auto ink_of = [&](size_t i) {
        const int s = t.periods[i].section;
        return series[static_cast<size_t>(sectioned ? std::max(s, 0) : static_cast<int>(i)) % series.size()];
    };

    // Columns all as wide as the widest text needs (within bounds).
    float need = 0;
    for (const Period& pe : t.periods) {
        for (const std::string& l : diagram_label(pe.text)) need = std::max(need, font.width(l, size, true));
        for (const std::string& e : pe.events)
            for (const std::string& l : diagram_label(e)) need = std::max(need, font.width(l, size));
    }
    const float col = std::clamp(need + 2 * px, 90 * u, 190 * u), most = col - 2 * px;
    const size_t per_row = static_cast<size_t>(std::max(1.0f, std::floor((std::max(style.room, col + 2 * pad) - 2 * pad + gap) / (col + gap))));

    // Rows of columns: a section kept whole on a row where it can be.
    std::vector<std::pair<size_t, size_t>> row_spans;  // [from, to)
    for (size_t i = 0; i < t.periods.size();) {
        size_t end = std::min(t.periods.size(), i + per_row);
        if (sectioned && end < t.periods.size() && t.periods[end].section == t.periods[end - 1].section) {
            size_t start = end - 1;
            while (start > i && t.periods[start - 1].section == t.periods[end].section) --start;
            if (start > i) end = start;  // (the section that would be cut starts the next row)
        }
        row_spans.emplace_back(i, end);
        i = end;
    }

    float y = pad;
    const float width = std::min(per_row, t.periods.size()) * (col + gap) - gap + 2 * pad;
    y += title_band(d, style, t.title, y, width);
    d->w = width;
    std::vector<std::vector<std::string>> period_lines(t.periods.size());
    for (size_t i = 0; i < t.periods.size(); ++i) period_lines[i] = wrapped(t.periods[i].text, most, size, font);

    for (const auto& [from, to] : row_spans) {
        // Section headers over their columns.
        float header_h = 0;
        std::vector<std::pair<size_t, size_t>> runs;  // a section's columns on this row
        if (sectioned) {
            for (size_t i = from; i < to;) {
                size_t j = i;
                while (j < to && t.periods[j].section == t.periods[i].section) ++j;
                runs.emplace_back(i, j);
                i = j;
            }
            for (const auto& [a, b] : runs) {
                const int s = t.periods[a].section;
                if (s < 0) continue;
                const float w = static_cast<float>(b - a) * (col + gap) - gap;
                header_h = std::max(header_h, static_cast<float>(wrapped(t.sections[static_cast<size_t>(s)], w - 2 * px, size, font).size()) * line + 2 * py);
            }
        }
        float period_h = 0;
        for (size_t i = from; i < to; ++i) period_h = std::max(period_h, static_cast<float>(period_lines[i].size()) * line + 2 * py);
        const float top = y, periods_y = y + (header_h > 0 ? header_h + 10 * u : 0);
        const float axis_y = periods_y + period_h + 16 * u, events_y = axis_y + 16 * u;
        float bottom = axis_y + 6 * u;
        auto col_x = [&](size_t i) { return pad + static_cast<float>(i - from) * (col + gap); };

        // The axis: a line through the row, an arrow at its end.
        const DiagramColour axis = p.tone(kMdText, dark ? 0.55f : 0.6f);
        {
            const float xa = pad - 4 * u, xb = col_x(to - 1) + col + 4 * u;
            RasterPath a;
            a.move_to(xa, axis_y), a.line_to(xb - 6 * u, axis_y);
            d->shape(std::move(a), {}, axis, 2 * u, {}, RasterStroke::kRound);
            diagram_head(*d, kHeadArrow, {xb + 2 * u, axis_y}, {1, 0}, 9 * u, axis, 1.5f * u);
        }
        for (const auto& [a, b] : runs) {
            const int s = t.periods[a].section;
            if (s < 0) continue;
            const float x = col_x(a), w = static_cast<float>(b - a) * (col + gap) - gap;
            const u8 ink = ink_of(a);
            RasterPath box;
            box.rect(x, top, w, header_h, 6 * u);
            d->shape(std::move(box), p.tone(ink, dark ? 0.42f : 0.36f), p.tone(ink, 0.9f), 1.3f * u);
            text_block(d, font, wrapped(t.sections[static_cast<size_t>(s)], w - 2 * px, size, font), x + w / 2, top + header_h / 2, size, p.of(kMdText), 1, true);
        }
        for (size_t i = from; i < to; ++i) {
            const Period& pe = t.periods[i];
            const u8 ink = ink_of(i);
            const float x = col_x(i), cx = x + col / 2;
            const std::string pname = "p" + std::to_string(i + 1);
            RasterPath box;
            box.rect(x, periods_y, col, period_h, 6 * u);
            d->shape(std::move(box), p.tone(ink, dark ? 0.3f : 0.24f), p.tone(ink, 0.85f), 1.3f * u);
            text_block(d, font, period_lines[i], cx, periods_y + period_h / 2, size, p.of(kMdText), 1, true);
            if (geo) geo->boxes.push_back({"node", pname, x, periods_y, col, period_h});
            // Its events under it, joined by a dashed line through a dot on the axis.
            const DiagramColour joint = p.tone(ink, 0.8f);
            float ey = events_y, above = periods_y + period_h;
            std::string above_name = pname;
            RasterPath dot;
            dot.ellipse(cx, axis_y, 4.5f * u, 4.5f * u);
            d->shape(std::move(dot), p.of(ink), p.tone(ink, 0.3f), 1.5f * u);
            if (pe.events.empty()) {
                RasterPath stem;
                stem.move_to(cx, above), stem.line_to(cx, axis_y - 4.5f * u);
                d->shape(std::move(stem), {}, joint, 1.3f * u, {3 * u, 3 * u});
            }
            for (size_t k = 0; k < pe.events.size(); ++k) {
                const std::vector<std::string> el = wrapped(pe.events[k], most, size, font);
                const float eh = static_cast<float>(el.size()) * line + 2 * py;
                RasterPath join;
                join.move_to(cx, above), join.line_to(cx, ey);
                d->shape(std::move(join), {}, joint, 1.3f * u, {3 * u, 3 * u});
                RasterPath ev;
                ev.rect(x, ey, col, eh, 6 * u);
                d->shape(std::move(ev), p.tone(ink, dark ? 0.13f : 0.1f), p.tone(ink, 0.6f), 1.1f * u);
                text_block(d, font, el, cx, ey + eh / 2, size, p.of(kMdText), 1, false);
                const std::string ename = pname + "." + std::to_string(k + 1);
                if (geo) {
                    geo->boxes.push_back({"node", ename, x, ey, col, eh});
                    geo->lines.push_back({above_name, ename, {{cx, above}, {cx, (above + ey) / 2}, {cx, ey}}});
                }
                above = ey + eh, above_name = ename;
                ey += eh + 10 * u;
            }
            bottom = std::max(bottom, above);
        }
        if (geo)
            for (const auto& [a, b] : runs)
                if (t.periods[a].section >= 0)
                    geo->boxes.push_back({"cluster", "section" + std::to_string(t.periods[a].section + 1), col_x(a), top, static_cast<float>(b - a) * (col + gap) - gap, bottom - top});
        y = bottom + row_gap;
    }
    d->h = y - row_gap + pad;
}

}  // namespace

bool diagram_dated(std::string_view source) {
    const std::vector<DiagramLine> lines = diagram_lines(source);
    if (lines.empty()) return false;
    const std::string_view head = diagram_trim(lines[0].text);
    if (diagram_lower(head.substr(0, head.find_first_of(" \t"))) != "gantt") return false;
    Gantt g;
    std::string err;
    return parse_gantt(lines, g, &err) && g.today;
}

i64 diagram_today() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    return days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

bool gantt_layout(const std::vector<DiagramLine>& lines, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    Gantt g;
    if (!parse_gantt(lines, g, err) || !resolve(g, err)) return false;
    draw_gantt(g, style, out, geo);
    return true;
}

bool timeline_layout(const std::vector<DiagramLine>& lines, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    Timeline t;
    if (!parse_timeline(lines, t, err)) return false;
    draw_timeline(t, style, out, geo);
    return true;
}

}  // namespace blit
