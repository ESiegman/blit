#include "keymap.hpp"

#include <algorithm>
#include <cstdlib>

#include "utf8.hpp"

namespace blit {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

const struct {
    const char* name;
    u8 kind;
    u32 ch;
} kNamed[] = {{"Esc", MapKey::kEsc, 0},       {"CR", MapKey::kEnter, 0},       {"Enter", MapKey::kEnter, 0},   {"Return", MapKey::kEnter, 0},
              {"BS", MapKey::kBackspace, 0},  {"Tab", MapKey::kTab, 0},        {"Left", MapKey::kLeft, 0},     {"Right", MapKey::kRight, 0},
              {"Up", MapKey::kUp, 0},         {"Down", MapKey::kDown, 0},      {"Home", MapKey::kHome, 0},     {"End", MapKey::kEnd, 0},
              {"Del", MapKey::kDelete, 0},    {"Delete", MapKey::kDelete, 0},  {"PageUp", MapKey::kPageUp, 0}, {"PageDown", MapKey::kPageDown, 0},
              {"Space", MapKey::kChar, ' '},  {"lt", MapKey::kChar, '<'},      {"Bar", MapKey::kChar, '|'},    {"Bslash", MapKey::kChar, '\\'}};

// A key's name without its modifiers: "Tab", "F5", or one character.
bool named_key(std::string_view name, MapKey* k) {
    for (const auto& x : kNamed)
        if (same_name(name, x.name)) {
            k->kind = x.kind;
            k->ch = x.ch;
            return true;
        }
    if (name.size() >= 2 && name.size() <= 3 && lower(name[0]) == 'f') {
        u32 n = 0;
        for (size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return false;
            n = n * 10 + static_cast<u32>(name[i] - '0');
        }
        if (n < 1 || n > 24) return false;
        k->kind = MapKey::kFn;
        k->ch = n;
        return true;
    }
    const Decoded d = decode_utf8(reinterpret_cast<const u8*>(name.data()), name.size());
    if (name.empty() || d.len != name.size()) return false;
    k->kind = MapKey::kChar;
    k->ch = d.cp;
    return true;
}

// Shift is a modifier of a key held with Ctrl, Alt or Cmd, and of the keys
// that are not characters; of a character alone it is the character. With
// Ctrl a capital is the same key as the small letter, as in Vim (<C-S> is
// <C-s>; Shift is there only if it is written, <C-S-s>); with Alt or Cmd
// alone a capital is the letter with Shift (<M-J> is <M-S-j>).
void settle(MapKey* k) {
    if (k->kind != MapKey::kChar) return;
    const bool upper = k->ch >= 'A' && k->ch <= 'Z';
    if (k->mods & MapKey::kCtrl) {
        if (upper) k->ch += 32;
    } else if (k->mods & (MapKey::kAlt | MapKey::kCmd)) {
        if (upper) k->ch += 32, k->mods |= MapKey::kShift;
    } else if (k->mods & MapKey::kShift) {
        if (k->ch >= 'a' && k->ch <= 'z') k->ch -= 32;
        k->mods = static_cast<u8>(k->mods & ~MapKey::kShift);
    }
}

}  // namespace

void append_utf8(std::string* out, u32 cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parse_keys(std::string_view s, std::string_view leader, std::vector<MapKey>* out, std::string* err) {
    out->clear();
    for (size_t i = 0; i < s.size();) {
        const size_t close = s[i] == '<' ? s.find('>', i) : std::string_view::npos;
        if (close != std::string_view::npos && close > i + 1) {
            std::string_view name = s.substr(i + 1, close - i - 1);
            if (same_name(name, "leader")) {
                std::vector<MapKey> lead;
                if (!parse_keys(leader.empty() ? "\\" : leader, "\\", &lead, err)) return false;
                out->insert(out->end(), lead.begin(), lead.end());
                i = close + 1;
                continue;
            }
            MapKey k;
            // Modifiers: one letter and a dash each, in any order.
            while (name.size() > 2 && name[1] == '-') {
                const char m = lower(name[0]);
                const u8 bit = m == 'c' ? MapKey::kCtrl : (m == 'm' || m == 'a') ? MapKey::kAlt : m == 'd' ? MapKey::kCmd : m == 's' ? MapKey::kShift : 0;
                if (!bit) break;
                k.mods |= bit;
                name.remove_prefix(2);
            }
            if (!named_key(name, &k)) {
                if (err) *err = "unknown key <" + std::string(s.substr(i + 1, close - i - 1)) + "> (a \"<\" by itself is written <lt>)";
                return false;
            }
            settle(&k);
            out->push_back(k);
            i = close + 1;
            continue;
        }
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + i, s.size() - i);
        MapKey k;
        k.ch = d.cp;
        if (k.ch == '\n') k.kind = MapKey::kEnter, k.ch = 0;
        else if (k.ch == '\t') k.kind = MapKey::kTab, k.ch = 0;
        else if (k.ch == 27) k.kind = MapKey::kEsc, k.ch = 0;
        out->push_back(k);
        i += d.len;
    }
    return true;
}

std::string key_notation(std::span<const MapKey> keys) {
    std::string out;
    for (const MapKey& k : keys) {
        std::string name;
        for (const auto& x : kNamed)
            if (name.empty() && x.kind == k.kind && x.ch == k.ch) name = x.name;
        if (k.kind == MapKey::kFn) name = "F" + std::to_string(k.ch);
        if (name.empty() && !k.mods) {
            append_utf8(&out, k.ch);
            continue;
        }
        out += '<';
        if (k.mods & MapKey::kCtrl) out += "C-";
        if (k.mods & MapKey::kAlt) out += "M-";
        if (k.mods & MapKey::kCmd) out += "D-";
        if (k.mods & MapKey::kShift) out += "S-";
        if (name.empty()) append_utf8(&out, k.ch);
        else out += name;
        out += '>';
    }
    return out;
}

u8 Keymaps::modes_of(std::string_view letters) {
    if (letters.empty()) return kNormal | kVisual | kPending;
    u8 m = 0;
    for (const char c : letters) {
        switch (c) {
            case 'n': m |= kNormal; break;
            case 'i': m |= kInsert; break;
            case 'v':
            case 'x': m |= kVisual; break;
            case 'o': m |= kPending; break;
            case 'c': m |= kCommand; break;
            default: return 0;
        }
    }
    return m;
}

std::vector<int> Keymaps::remove(u8 modes, std::span<const MapKey> lhs) {
    std::vector<int> gone;
    for (size_t i = 0; i < maps_.size();) {
        Map& m = maps_[i];
        if ((m.modes & modes) && std::equal(m.lhs.begin(), m.lhs.end(), lhs.begin(), lhs.end())) {
            m.modes = static_cast<u8>(m.modes & ~modes);
            if (!m.modes) {
                if (m.fn >= 0) gone.push_back(m.fn);
                maps_.erase(maps_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
    index();
    return gone;
}

std::vector<int> Keymaps::set(Map m) {
    std::vector<int> gone = remove(m.modes, m.lhs);
    if (!m.lhs.empty() && m.modes) maps_.push_back(std::move(m));
    index();
    return gone;
}

std::vector<int> Keymaps::clear() {
    std::vector<int> gone;
    for (const Map& m : maps_)
        if (m.fn >= 0) gone.push_back(m.fn);
    maps_.clear();
    index();
    return gone;
}

void Keymaps::index() {
    for (int m = 0; m < 5; ++m) first_[m][0] = first_[m][1] = 0, other_[m] = false;
    for (const Map& map : maps_) {
        const MapKey& k = map.lhs.front();
        for (int m = 0; m < 5; ++m) {
            if (!(map.modes & (1u << m))) continue;
            if (k.kind == MapKey::kChar && k.mods == 0 && k.ch < 128) first_[m][k.ch >> 6] |= 1ull << (k.ch & 63);
            else other_[m] = true;
        }
    }
}

Keymaps::Match Keymaps::match(u8 mode, std::span<const MapKey> typed, const Map** exact) const {
    bool is = false, more = false;
    for (const Map& m : maps_) {
        if (!(m.modes & mode) || m.lhs.size() < typed.size()) continue;
        if (!std::equal(typed.begin(), typed.end(), m.lhs.begin())) continue;
        if (m.lhs.size() == typed.size()) {
            is = true;
            if (exact) *exact = &m;
        } else {
            more = true;
        }
    }
    return is ? (more ? kBoth : kExact) : more ? kPrefix : kNone;
}

}  // namespace blit
