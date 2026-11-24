#pragma once
// What the wave tab lists and how (appwave.cpp): its rows (signals of a
// dump, groups of them, a bus opened into its bits), each value written in
// the row's radix, and the view as it is kept beside the project
// (.blit/waves/<dump>.json): rows by hierarchical path, radix, groups,
// colours, names of values, markers, the cursor and the stretch of time on
// show.
// Knows no window: the tab, the tile painter (wavetile.hpp) and the tests
// share it.
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wave.hpp"

namespace blit {

// How a row's values are written. The order is the one `r` goes round in.
enum class WaveRadix : u8 { kHex, kUnsigned, kSigned, kBinary, kOctal, kAscii, kEnum, kFixed, kFloat };
inline constexpr u32 kWaveRadixCount = 9;

std::string_view wave_radix_name(WaveRadix r);  // "hex", "unsigned", ... as the view file has them
bool wave_radix_parse(std::string_view s, WaveRadix* out);

// Names for a signal's values ("IDLE" for 0), given in the view.
struct WaveEnum {
    std::vector<std::pair<u64, std::string>> names;  // by value
    const std::string* find(u64 v) const;
    // "IDLE START DATA" (from 0 on) or "0=IDLE 4=DATA"; false if neither.
    static bool parse(std::string_view s, WaveEnum* out);
    std::string text() const;  // as parse() takes it
};

struct WaveFormat {
    WaveRadix radix = WaveRadix::kHex;
    i32 bit = -1;      // one bit of the lane (counted from its lowest), or -1: all of it
    u8 frac = 0;       // fixed point: bits after the point
    bool sign = true;  // fixed point: Qm.n (signed) or UQm.n
    std::shared_ptr<const WaveEnum> names;  // kEnum
    // "Q8.8" or "UQ4.12" (m + n the signal's width); false if not one.
    static bool parse_q(std::string_view s, u32 width, WaveFormat* f);
    std::string q_text(u32 width) const;
};

// Change i of a lane as text in a format: 0 1 x z for one bit; for a bus
// its digits (x or z for a digit wholly so, X or Z for one partly so, and
// x or z alone in a radix with no digits of its own); a real or a string
// as the lane has it; an event as "".
void wave_value(const Lane& lane, u64 i, const WaveFormat& f, std::string* out);
// The radix a signal starts in: binary for one bit, the one the dump asks for (an ILA
// capture's CSV), signed for an integer, hex for another bus.
WaveRadix wave_radix_default(const WaveVar& v);

// The colours a row can be given, each one of the theme's own by its name
// there (as the view file has it too): 0 is none, drawn as rows are. They
// are the ones apart from each other, and from a signal's own colour (the
// theme's diff.plus, else info), in each of blit's themes.
inline constexpr u8 kWaveColours = 5;
std::string_view wave_colour_name(u8 c);  // "error", "warning", "hint", "accent", "keyword"; "none"
bool wave_colour_parse(std::string_view s, u8* out);

// One signal in the list.
struct WaveSignal {
    u32 var = 0;
    WaveFormat fmt;
    bool open = false;  // a bus shown bit by bit under it
    u8 colour = 0;      // given it (0: none, the group's if it is in one that has one)
};

// One entry of the list: a signal, or a group of them under a name.
struct WaveItem {
    bool group = false;
    WaveSignal sig;                   // (a signal)
    std::string name;                 // (a group: its name, whether its rows show, its colour, and its signals)
    bool open = true;
    u8 colour = 0;
    std::vector<WaveSignal> members;
};

// A row as the tab shows it: a signal (a group's member, or not), one bit of
// a bus opened, or a group's heading.
struct WaveRow {
    enum Kind : u8 { kSignal, kBit, kGroup };
    Kind kind = kSignal;
    u32 item = 0;
    u32 member = ~0u;  // (a group's signal: which)
    u32 bit = 0;       // (kBit: which bit of the lane, from its lowest)
    u8 depth = 0;      // how far it is indented
};
std::vector<WaveRow> wave_rows(const std::vector<WaveItem>& items, const Wave& w);
// A row's signal (null for a group's heading), and its format (a bit's: binary, of that bit).
const WaveSignal* wave_row_signal(const std::vector<WaveItem>& items, const WaveRow& r);
WaveSignal* wave_row_signal(std::vector<WaveItem>& items, const WaveRow& r);
WaveFormat wave_row_format(const std::vector<WaveItem>& items, const WaveRow& r);
// The colour a row is drawn in: its signal's own, else its group's (a bit's: its bus's); a group's heading, the group's.
u8 wave_row_colour(const std::vector<WaveItem>& items, const WaveRow& r);
// A variable as the names column and the picker say it: "tb.u_cnt.count[7:0]".
std::string wave_var_name(const Wave& w, u32 var);
// What the names column says: "tb.u_cnt.count[7:0]", "count[3]", a group's name.
std::string wave_row_name(const std::vector<WaveItem>& items, const WaveRow& r, const Wave& w);

// The list a dump starts with when no view of it is kept: each scope's
// variables (not those seen already under another name: aliases share a
// lane) as a group named by the scope's path, at most `most` signals; a
// dump of one scope with no groups.
std::vector<WaveItem> wave_default_items(const Wave& w, u32 most = 64);

// The view as it is kept (.blit/waves/<dump-stem>.json, data, not Lua).
struct WaveSaved {
    std::vector<WaveItem> items;
    std::map<std::string, std::shared_ptr<const WaveEnum>> enums;  // by signal path
    u64 marks[26] = {};
    u32 marked = 0;          // which of marks[] are set (bit k: letter 'a' + k)
    u64 cursor = 0;
    bool has_cursor = false;
    double from = 0, to = 0;  // the stretch of time on show (to <= from: the whole run)
};
std::string wave_view_json(const Wave& w, const WaveSaved& s);
// Rows whose paths the dump has not got are left out (`dropped` counts them).
bool wave_view_parse(const Wave& w, std::string_view text, WaveSaved* out, u32* dropped);
// Where a dump's view is kept: root (the project's folder, else the
// dump's) + "/.blit/waves/" + the dump's name without its extension + ".json".
std::string wave_view_path(const std::string& root, const std::string& dump);

}  // namespace blit
