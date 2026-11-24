#pragma once
// What a monitor tab keeps and draws of a stream of bytes from a device: a
// terminal tab whose program is a serial port (serialtab.hpp), an RTT
// channel, a board's semihosting output (linktab.hpp). What came is kept
// as it came (the bytes, when each part of them came, and when the stream
// went and came back), and drawn from that onto the tab's Term: as text,
// each line a line whether it ends with CR LF or LF alone; with the host's
// time before each line (a column turned on and off, the times stored as
// the lines came); or as hex beside the text, the same bytes. Turning
// either on or off draws the tab again from what is kept. A log to a file,
// and a recogniser of whole lines (the crash decoder's place), go with it.
//
// Threads: the stream's reader hands bytes over (take), says when the
// stream goes (gone) and is back (back), and what it is waiting for
// (waiting); the main thread asks. All that is shared is under the tab's
// mutex, which the Term's drawing holds too, but the recogniser of lines
// (StreamScan), which has a mutex of its own and is told the lines before
// the tab's is taken.
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

class Term;

// What Return sends: CR (a terminal's own), LF, or both.
enum class SerialReturn : u8 { kCR, kLF, kCRLF };
const char* serial_return_name(SerialReturn r);  // "CR", "LF", "CR LF"
bool serial_return_parse(std::string_view s, SerialReturn* r);  // "cr", "lf", "crlf", "cr lf" (any case)

// A recogniser of lines (the crash decoder's place, E7: appterm.cpp's
// StreamCrash): told each whole line the device sends (without its line
// end, escapes and all), on the reader's thread as it comes, before the
// tab's mutex is taken (so that it may take its time: read an ELF); the
// lines it adds, plain text, go after that line, dim, in the tab, what is
// kept of it, and its log. Told when the stream goes, too (a reset): what
// it was in the middle of is over. Attached with StreamTab::set_scan; with
// none, lines are not put together at all.
struct StreamScan {
    virtual ~StreamScan() = default;
    virtual void line(std::string_view text, u64 wall_ns, std::vector<std::string>* add) = 0;
    virtual void gone() {}
};
using SerialScan = StreamScan;

class StreamTab {
public:
    // A tab drawing onto `term`, whose mutex is `mu` (the TermView's).
    StreamTab(SerialReturn ret, std::mutex& mu, Term& term);
    virtual ~StreamTab();
    StreamTab(const StreamTab&) = delete;
    StreamTab& operator=(const StreamTab&) = delete;

    // ---- the main thread (each takes the mutex)

    virtual std::string title() = 0;  // the tab's name, with what it is waiting for
    virtual std::string name() = 0;   // what it is called, for a log's name: "cu.usbmodem1101", "RTT 0 Terminal"
    virtual std::string dump() = 0;   // (the tests: state, settings)
    SerialReturn return_mode();
    void set_return(SerialReturn r);
    std::string return_bytes();
    bool stamps();
    void set_stamps(bool on);  // (the tab drawn again)
    bool hex();
    void set_hex(bool on);
    // Everything that comes written to a file as well, from now: each line
    // after its date and time; false (why) when it cannot be written.
    bool log_to(const std::string& path, std::string* why);
    void log_stop();
    std::string log_path();
    // What the reader has to say (a line that could not be set, a reset
    // done), each with its level for notify; taken once.
    std::vector<std::pair<std::string, int>> take_news();
    void set_scan(std::unique_ptr<StreamScan> scan);
    void clear();  // what is kept and shown goes (Cmd+K), but a gap still open

    // ---- the reader's thread

    // Called when something changed that the main thread should see (the
    // TermView's wake), set before the reader starts.
    std::function<void()> told;
    int wake_fd() const { return wake_r_; }
    void drain();  // (the wake pipe emptied)
    // What came, at this wall-clock time: kept, logged, drawn. What the
    // Term answers the device is returned, to be sent.
    std::string take(const char* p, size_t n, u64 wall_ns);

    static u64 wall_ns();  // (now, by the wall clock: what the times are)
    static constexpr const char* kReleasedWhy = "released for flashing";  // (a gap's why, for a port let go of for a flash)

protected:
    struct Event {
        u64 at = 0;              // the place in what came: after the bytes before it
        u64 ns = 0, back = 0;    // a gap: when the stream went, and came back (0: not yet)
        u8 kind = 0;             // kGap, kNote
        std::string text;        // a gap: why; a note: its line
    };
    enum : u8 { kGap, kNote };
    struct Chunk {
        u64 at = 0, ns = 0;  // the bytes from `at` came at ns
    };

    // (Each with the mutex held.)
    // The stream went, at ns, for `why` (one dim line says so, rewritten as
    // what is waited for changes); `waiting` what the tab waits for.
    void gap_begin(const std::string& why, u64 ns, const std::string& waiting);
    // And it is back: the gap's line says how long it was (true), or
    // there was no gap open and only the waiting line goes (false).
    bool gap_end(u64 ns);
    bool gap_open() const { return !events_.empty() && events_.back().kind == kGap && !events_.back().back; }
    // The last event's line, a gap's, with what is waited for ("" when the
    // last is no gap).
    std::string last_gap_text(const std::string& waiting) const;
    void status(const std::string& text);  // (the last line, rewritten as what is waited for changes)
    void status_end(const std::string& last);  // (and that line made `last`, or taken away: "")
    std::string gap_text(const Event& e, const std::string& waiting = {}) const;
    // A dim line of blit's own among what came (a waiting line taken away
    // first), kept with it and logged: drawn again with it.
    void note(const std::string& text, u64 ns);
    // What the reader has to say, for notify.
    void say(std::string text, int level) { news_.emplace_back(std::move(text), level); }
    // Bytes kept and drawn as take()'s are, but neither looked at by the
    // recogniser nor logged (a stream drawn again from records of its own:
    // defmt's, with a level filter).
    void take_quiet(const char* p, size_t n, u64 ns);
    void poke();
    // The kept bytes and events dropped, and the Term emptied (a filter
    // changed: what is kept is made again by the caller).
    void forget();
    // (The tests' dumps: how much is kept, the gaps among it, the log's path.)
    size_t kept_bytes() const { return bytes_.size(); }
    size_t gap_count() const;
    const std::string& log_file() const { return log_path_; }
    bool stamps_on() const { return stamps_; }
    bool hex_on() const { return hex_; }
    SerialReturn ret_now() const { return ret_; }

    std::mutex& mu_;
    Term& term_;
    std::vector<std::pair<std::string, int>> news_;

private:
    void keep(const char* p, size_t n, u64 ns);
    void trim();
    void render(const char* p, size_t n, u64 ns);
    void render_text(const char* p, size_t n, u64 ns);
    void render_hex(const char* p, size_t n, u64 ns);
    void hex_end_row();
    void end_line();  // (the drawing at the start of a line)
    void stamp(u64 ns);
    void dim_line(const std::string& text, bool open);
    void rebuild();
    void log_bytes(const char* p, size_t n, u64 ns);
    void log_line(const std::string& text, u64 ns);
    void scan_lines(const char* p, size_t n, u64 ns);  // (scan_mu_: into notes_)

    int wake_r_ = -1, wake_w_ = -1;
    SerialReturn ret_ = SerialReturn::kCR;
    bool stamps_ = false, hex_ = false;

    // What came, kept: the bytes (`base_` is the place of the first, in all
    // that came since the tab opened), when each part came, and the gaps
    // and notes, all in order of place.
    std::string bytes_;
    u64 base_ = 0;
    std::deque<Chunk> chunks_;
    std::deque<Event> events_;

    // Drawing: at the start of a line; the last byte drawn a CR; the hex
    // row being filled (its place, its time, its bytes so far).
    bool line_start_ = true, after_cr_ = false;
    u64 render_at_ = 0;
    bool hex_open_ = false;
    std::string hex_row_;
    size_t hex_n_ = 16;  // (the bytes in the row being filled: 16, or 8 where the tab is too narrow for 16)
    std::string scratch_;
    // The status line on the tab's last row while the stream is down: the
    // row it starts on, counted as the Term counts lines (~0: none).
    u64 status_row_ = ~0ull;

    FILE* log_ = nullptr;
    std::string log_path_;
    bool log_line_start_ = true;

    // The recogniser, and the line it is to be told once it ends: the
    // reader's, under their own mutex (set from the main thread), never
    // under the tab's. The lines it added to a piece of what came, each
    // after which byte of it (the reader's).
    std::mutex scan_mu_;
    std::unique_ptr<StreamScan> scan_;
    std::string scan_line_;
    std::vector<std::string> scan_add_;
    std::vector<std::pair<size_t, std::string>> notes_;

protected:
    // A gone stream's: the recogniser told (a line cut short by it is none,
    // a dump it was in is over). Without the tab's mutex.
    void scan_gone();
};

}  // namespace blit
