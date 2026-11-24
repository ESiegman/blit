#pragma once
// A board's stream as a tab (the embedded design's E6): an RTT channel or
// the board's semihosting output, a terminal tab whose program is a link
// rather than a port (appterm.cpp runs it, apprtt.cpp opens it). What comes
// is kept and drawn as streamtab.hpp says, so the serial tab's times, hex,
// log and crash decoding are its too. Its source is one of:
//
//   - a loopback link (loopback.hpp): a TCP server a tool runs (OpenOCD's,
//     pyOCD's or J-Link's RTT channel, a semihosting console), connected
//     to again whenever it closes; or a port blit listens on, that a tool
//     connects out to (QEMU's semihosting chardev). Typing goes back down
//     it: an RTT down-channel, semihosting's input;
//   - bytes handed over (push): probe-rs's DAP events;
//   - a file a tool writes, read as it grows (QEMU's own output, where its
//     semihosting goes), from its start again when it is made anew.
//
// A defmt channel's bytes go through `defmt-print -e ELF --json` (rtt.hpp)
// instead of onto the tab: each record it gives is drawn as a line (level
// in colour, timestamp, message, place), and the keys 1 to 5 show only
// trace and up, debug and up, ... error alone, drawing again from the
// records kept (none is dropped).
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "loopback.hpp"
#include "rtt.hpp"
#include "streamtab.hpp"

namespace blit {

struct LinkSpec {
    std::string board;  // the run configuration it is of ("" none: opened by hand)
    StreamPlan plan;    // what it is (rtt.hpp): kind, name, channel, port, listening, greeting, input, defmt
    std::string host = "127.0.0.1";
    bool pushed = false;     // fed by push(), not a link
    std::string elf;         // defmt's ELF
    std::string dir;         // defmt-print's folder (the project's: its places are relative to it)
    LoopbackDial dial;       // (the link's way of connecting: empty, TCP here)
};

class LinkTab final : public StreamTab {
public:
    LinkTab(LinkSpec spec, SerialReturn ret, std::mutex& mu, Term& term);
    ~LinkTab() override;

    // The link (or defmt-print) started: once `told` is set.
    void start();

    std::string title() override;  // "RTT 0: Terminal", with "(waiting)" while the link is down
    std::string name() override;   // "RTT 0", "Board output"
    std::string dump() override;
    const LinkSpec& spec() const { return spec_; }
    // It is that board's stream of that kind (and channel).
    bool is(const std::string& board, const std::string& kind, int channel) const;
    // Named again (a channel's name found once the control block was read).
    void rename(const std::string& name);
    // The same stream at another port, or as another name's (a session that
    // chose its ports again): the link made again.
    void retarget(const StreamPlan& plan);
    bool open();  // the link is up (pushed: a session feeds it)
    // Typing: down the link (false: it is down, or the stream takes none).
    bool send(std::string_view bytes);
    void retry_now();
    // Bytes from a session's events (probe-rs's), from the main thread; and
    // the session that fed them gone (a gap line), or back.
    void push(std::string_view bytes);
    void push_gone(const std::string& why);
    void push_back();
    // defmt's: the lowest level shown (1 trace .. 5 error), drawn again.
    bool defmt() const { return spec_.plan.defmt; }
    void set_level(int lowest);
    int level();

private:
    void got(const char* p, size_t n);          // (the link's thread)
    void link_open();
    void link_closed(const std::string& why);
    void defmt_feed(const char* p, size_t n);   // (to defmt-print)
    void defmt_run(std::stop_token stop);       // (its thread: its records read)
    void defmt_record(const DefmtRecord& r);
    void make_link();
    void tail_run(std::stop_token stop);  // (a file's: read as it grows)

    LinkSpec spec_;
    std::unique_ptr<Loopback> link_;
    std::jthread tail_;
    enum : u8 { kWaiting, kOpen, kGone } state_ = kWaiting;  // (mu_)
    std::string waiting_;  // (mu_) what the title says it waits for

    // defmt-print: started at start(), fed on the link's thread, read on its own.
    std::mutex defmt_mu_;
    Spawned defmt_;
    std::string defmt_in_;  // (defmt_mu_) what waits to be written to it
    int defmt_wake_[2] = {-1, -1};
    std::jthread defmt_thread_;
    std::mutex order_;  // (held while a record is kept and drawn, and while all are drawn again: none twice)
    std::deque<DefmtRecord> records_;  // (mu_)
    int lowest_ = 1;                   // (mu_)
    int schema_ = 0;
    std::string defmt_err_;  // its last error lines
};

}  // namespace blit
