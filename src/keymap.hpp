#pragma once
// Key mappings: a run of keys that stands for other keys, or for a function
// of a script's (applua.cpp). This is the table of them and the question
// asked of every key typed, "does a mapping start with this?"; it knows
// nothing of windows, of Vim or of Lua.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct MapKey {
    // (The kinds up to kDelete are VimKey's, in its order.)
    enum : u8 { kChar, kEsc, kEnter, kBackspace, kTab, kLeft, kRight, kUp, kDown, kHome, kEnd, kPageUp, kPageDown, kDelete, kFn };
    enum : u8 { kCtrl = 1, kAlt = 2, kCmd = 4, kShift = 8 };
    u8 kind = kChar;
    u8 mods = 0;
    // kChar: the character. With Ctrl, Alt or Cmd it is the key's own
    // letter, in lower case, and Shift is one of the mods; without them
    // Shift is in the character ("A", "!"). kFn: which one (1 for F1).
    u32 ch = 0;
    bool operator==(const MapKey&) const = default;
};

// Keys in Vim's notation: "gx", "<leader>ff", "<C-s>", "<D-k>" (Cmd),
// "<M-j>" or "<A-j>" (Alt), "<S-Tab>", "<C-S-p>", "<F5>", "<Space>",
// "<lt>" for a "<". False, with what was wrong in *err, for a name between
// < and > that is not a key's.
bool parse_keys(std::string_view notation, std::string_view leader, std::vector<MapKey>* out, std::string* err = nullptr);
// And back again, for showing them.
std::string key_notation(std::span<const MapKey> keys);
// A character, as UTF-8, onto the end of a string.
void append_utf8(std::string* out, u32 cp);

class Keymaps {
public:
    enum Mode : u8 { kNormal = 1, kInsert = 2, kVisual = 4, kPending = 8, kCommand = 16, kAllModes = 31 };
    struct Map {
        std::vector<MapKey> lhs;
        u8 modes = 0;
        int fn = -1;        // a script's function, by the number the script engine knows it by; -1: keys
        std::string keys;   // what is typed in its place, in the notation above
        bool remap = false; // and whether those keys may be mappings themselves
        std::string desc;
    };
    enum Match : u8 {
        kNone,    // no mapping starts with the keys typed
        kPrefix,  // some do, and need more keys
        kExact,   // they are a mapping
        kBoth,    // they are a mapping, and the start of longer ones
    };

    // "n", "i", "v" or "x", "o", "c"; "" for n, v and o together; several
    // letters for several modes ("nv"). 0 if a letter is not a mode's.
    static u8 modes_of(std::string_view letters);

    // Puts a mapping in, in place of whatever the same keys were in those
    // modes. Returns the functions of mappings that went (for the script
    // engine to let go of).
    std::vector<int> set(Map m);
    std::vector<int> remove(u8 modes, std::span<const MapKey> lhs);
    std::vector<int> clear();
    bool empty() const { return maps_.empty(); }
    const std::vector<Map>& all() const { return maps_; }

    Match match(u8 mode, std::span<const MapKey> typed, const Map** exact = nullptr) const;
    // Whether any mapping of the mode starts with this key: one or two
    // loads for a key that is plain text.
    bool starts(u8 mode, const MapKey& k) const {
        const int m = mode_index(mode);
        if (k.kind == MapKey::kChar && k.mods == 0 && k.ch < 128) return (first_[m][k.ch >> 6] >> (k.ch & 63)) & 1;
        return other_[m];
    }

private:
    static int mode_index(u8 mode) { return mode & kNormal ? 0 : mode & kInsert ? 1 : mode & kVisual ? 2 : mode & kPending ? 3 : 4; }
    void index();

    std::vector<Map> maps_;
    u64 first_[5][2] = {};   // per mode: the plain characters mappings start with
    bool other_[5] = {};     // per mode: some mapping starts with another kind of key
};

}  // namespace blit
