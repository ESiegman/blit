#include "linktab.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "missing.hpp"

namespace blit {

namespace {

constexpr size_t kRecordsMost = 200'000;  // defmt's records kept, at most (the oldest go)

}  // namespace

LinkTab::LinkTab(LinkSpec spec, SerialReturn ret, std::mutex& mu, Term& term) : StreamTab(ret, mu, term), spec_(std::move(spec)) {
    waiting_ = spec_.pushed               ? std::string("waiting for the session")
               : !spec_.plan.file.empty() ? "waiting for " + spec_.plan.file
               : spec_.plan.listen        ? "waiting for a connection on " + std::to_string(spec_.plan.port)
                                          : "waiting for " + LoopbackEnd{spec_.host, spec_.plan.port}.text();
}

LinkTab::~LinkTab() {
    link_.reset();  // (its thread stopped first: it calls into the tab)
    if (tail_.joinable()) {
        tail_.request_stop();
        tail_.join();
    }
    if (defmt_thread_.joinable()) {
        defmt_thread_.request_stop();
        if (defmt_wake_[1] >= 0) {
            const char c = 1;
            [[maybe_unused]] const ssize_t k = ::write(defmt_wake_[1], &c, 1);
        }
        defmt_thread_.join();
    }
    spawn_end(defmt_);
    for (const int fd : defmt_wake_)
        if (fd >= 0) ::close(fd);
}

void LinkTab::make_link() {
    Loopback::Options o;
    o.mode = spec_.plan.listen ? Loopback::Mode::kListen : Loopback::Mode::kConnect;
    o.end = {spec_.host, spec_.plan.port};
    o.dial = spec_.dial;
    o.greeting = spec_.plan.greeting;
    Loopback::Events e;
    e.data = [this](const char* p, size_t n) { got(p, n); };
    e.opened = [this] { link_open(); };
    e.closed = [this](const std::string& why) { link_closed(why); };
    link_ = std::make_unique<Loopback>(std::move(o), std::move(e));
}

void LinkTab::start() {
    {
        std::lock_guard lock(mu_);
        status("[" + waiting_ + "]");
    }
    if (spec_.plan.defmt) {
        std::string why;
        defmt_ = spawn_piped({"defmt-print", "-e", spec_.elf, "--json", "stdin"}, spec_.dir, true, true, true, &why);
        if (defmt_.pid < 0) {
            if (why.find("is not installed") != std::string::npos) why += " (cargo install defmt-print)";
            std::lock_guard lock(mu_);
            say(why, 1);
            note("[" + why + "]", wall_ns());
        } else if (::pipe(defmt_wake_) == 0) {
            for (const int fd : defmt_wake_) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            defmt_thread_ = std::jthread([this](std::stop_token stop) { defmt_run(stop); });
        }
    }
    if (!spec_.plan.file.empty()) tail_ = std::jthread([this](std::stop_token stop) { tail_run(stop); });
    else if (!spec_.pushed) make_link();
    if (told) told();
}

// A file a tool writes: read from its start as it grows (looked at every
// 30 ms); made anew (another inode, or shorter than what was read: the
// next session's), read from its start again, a line marking the gap.
void LinkTab::tail_run(std::stop_token stop) {
    int fd = -1;
    u64 at = 0;
    ino_t ino = 0;
    char buf[65536];
    while (!stop.stop_requested()) {
        struct stat st;
        if (fd < 0) {
            fd = ::open(spec_.plan.file.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0 && ::fstat(fd, &st) == 0) {
                ino = st.st_ino;
                at = 0;
                link_open();
            }
        } else if (::stat(spec_.plan.file.c_str(), &st) != 0 || st.st_ino != ino || (::fstat(fd, &st) == 0 && static_cast<u64>(st.st_size) < at)) {
            ::close(fd);
            fd = -1;
            link_closed("started again");
            continue;
        } else {
            for (ssize_t k; (k = ::pread(fd, buf, sizeof buf, static_cast<off_t>(at))) > 0;) {
                at += static_cast<u64>(k);
                got(buf, static_cast<size_t>(k));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (fd >= 0) ::close(fd);
}

// ---------------------------------------------------------------- asked by the main thread

std::string LinkTab::title() {
    std::lock_guard lock(mu_);
    return spec_.plan.name + (state_ == kOpen ? "" : " (waiting)");
}

std::string LinkTab::name() {
    std::lock_guard lock(mu_);
    return spec_.plan.name;
}

std::string LinkTab::dump() {
    std::lock_guard lock(mu_);
    static const char* const kState[] = {"waiting", "open", "gone"};
    std::string out = std::string(kState[state_]) + " " + spec_.plan.kind + (spec_.plan.channel >= 0 ? " " + std::to_string(spec_.plan.channel) : std::string()) +
                      (spec_.pushed               ? std::string(" events")
                       : !spec_.plan.file.empty() ? std::string(" file")
                                                  : (spec_.plan.listen ? " listening " : " port ") + std::to_string(link_ ? link_->port() : spec_.plan.port)) +
                      (spec_.board.empty() ? std::string() : " of " + spec_.board) + " kept " + std::to_string(kept_bytes()) + " gaps " + std::to_string(gap_count());
    if (link_) out += " opens " + std::to_string(link_->opens());
    if (spec_.plan.defmt) out += " defmt from " + std::string(defmt_level_name(lowest_)) + " records " + std::to_string(records_.size());
    if (stamps_on()) out += " times";
    if (hex_on()) out += " hex";
    return out;
}

bool LinkTab::is(const std::string& board, const std::string& kind, int channel) const {
    return spec_.board == board && spec_.plan.kind == kind && (kind != "rtt" || spec_.plan.channel == channel);
}

void LinkTab::rename(const std::string& name) {
    std::lock_guard lock(mu_);
    spec_.plan.name = name;
}

void LinkTab::retarget(const StreamPlan& plan) {
    if (spec_.pushed || !spec_.plan.file.empty() || (plan.port == spec_.plan.port && plan.listen == spec_.plan.listen && plan.greeting == spec_.plan.greeting)) return;
    link_.reset();
    {
        std::lock_guard lock(mu_);
        const std::string name = spec_.plan.name;
        const bool defmt = spec_.plan.defmt;
        spec_.plan = plan;
        spec_.plan.name = name;
        spec_.plan.defmt = defmt;  // (its records' engine stays as it started)
    }
    make_link();
}

bool LinkTab::open() {
    std::lock_guard lock(mu_);
    return state_ == kOpen;
}

bool LinkTab::send(std::string_view bytes) {
    if (!spec_.plan.input || !link_) return false;
    return link_->send(bytes);
}

void LinkTab::retry_now() {
    if (link_) link_->retry_now();
}

void LinkTab::push(std::string_view bytes) {
    {
        std::lock_guard lock(mu_);
        if (state_ != kOpen) {
            if (state_ == kGone) gap_end(wall_ns());
            else status_end({});
            state_ = kOpen;
        }
    }
    got(bytes.data(), bytes.size());
}

void LinkTab::push_gone(const std::string& why) { link_closed(why); }

void LinkTab::push_back() { link_open(); }

// ---------------------------------------------------------------- the link's thread

void LinkTab::got(const char* p, size_t n) {
    if (spec_.plan.defmt) {
        defmt_feed(p, n);
        return;
    }
    const std::string reply = take(p, n, wall_ns());
    if (!reply.empty() && link_ && spec_.plan.input) link_->send(reply);  // (the Term's answers: where the cursor is)
    if (told) told();
}

void LinkTab::link_open() {
    {
        std::lock_guard lock(mu_);
        if (state_ == kGone) gap_end(wall_ns());
        else status_end({});
        state_ = kOpen;
    }
    if (told) told();
}

void LinkTab::link_closed(const std::string& why) {
    const std::string where = spec_.pushed || !spec_.plan.file.empty() ? std::string()
                              : spec_.plan.listen                      ? "waiting for a connection on " + std::to_string(spec_.plan.port)
                                                                       : "trying " + LoopbackEnd{spec_.host, spec_.plan.port}.text();
    bool was_open;
    {
        std::lock_guard lock(mu_);
        was_open = state_ == kOpen;
    }
    if (was_open) scan_gone();  // (a line cut short is none; a dump it was in is over)
    {
        std::lock_guard lock(mu_);
        if (was_open) {
            state_ = kGone;
            gap_begin(why, wall_ns(), where);
        } else if (state_ == kGone) {
            // (Still gone, tried again: its line says what it waits for already.)
        } else if (why.starts_with("waiting for")) {
            waiting_ = why;
            status("[" + why + "]");
        } else {
            status("[" + waiting_ + ": " + why + ", trying again]");
        }
    }
    if (told) told();
}

// ---------------------------------------------------------------- defmt

void LinkTab::defmt_feed(const char* p, size_t n) {
    {
        std::lock_guard lock(defmt_mu_);
        if (defmt_.pid < 0) return;
        defmt_in_.append(p, n);
        if (defmt_in_.size() > (16u << 20)) defmt_in_.erase(0, defmt_in_.size() - (16u << 20));  // (defmt-print not keeping up: the oldest go)
    }
    const char c = 1;
    [[maybe_unused]] const ssize_t k = ::write(defmt_wake_[1], &c, 1);
}

// defmt-print's output read a line at a time (its records, after a schema
// line), its errors kept for when it ends, and what came written to it.
void LinkTab::defmt_run(std::stop_token stop) {
    std::string out_line, err;
    char buf[65536];
    bool out_open = true, err_open = defmt_.err >= 0;
    while (!stop.stop_requested() && out_open) {
        bool pending;
        int in;
        {
            std::lock_guard lock(defmt_mu_);
            pending = !defmt_in_.empty();
            in = defmt_.in;
        }
        pollfd p[4] = {{defmt_.out, POLLIN, 0}, {err_open ? defmt_.err : -1, POLLIN, 0}, {pending ? in : -1, POLLOUT, 0}, {defmt_wake_[0], POLLIN, 0}};
        if (::poll(p, 4, 1000) < 0 && errno != EINTR) break;
        if (p[3].revents)
            while (::read(defmt_wake_[0], buf, sizeof buf) > 0) {
            }
        if (p[2].revents & (POLLOUT | POLLERR | POLLHUP)) {
            std::lock_guard lock(defmt_mu_);
            const ssize_t k = ::write(defmt_.in, defmt_in_.data(), defmt_in_.size());
            if (k > 0) defmt_in_.erase(0, static_cast<size_t>(k));
            else if (k < 0 && errno != EAGAIN && errno != EINTR) defmt_in_.clear();
        }
        if (p[1].revents) {
            const ssize_t k = ::read(defmt_.err, buf, sizeof buf);
            if (k > 0) {
                err.append(buf, static_cast<size_t>(k));
                if (err.size() > 8192) err.erase(0, err.size() - 8192);
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                err_open = false;
            }
        }
        if (p[0].revents) {
            const ssize_t k = ::read(defmt_.out, buf, sizeof buf);
            if (k > 0) {
                out_line.append(buf, static_cast<size_t>(k));
                size_t at = 0;
                for (size_t nl; (nl = out_line.find('\n', at)) != std::string::npos; at = nl + 1) {
                    DefmtRecord r;
                    int schema = 0;
                    if (defmt_parse(std::string_view(out_line).substr(at, nl - at), &r, &schema)) {
                        defmt_record(r);
                    } else if (schema) {
                        schema_ = schema;
                        if (schema != 1) {
                            std::lock_guard lock(mu_);
                            say("defmt-print speaks its JSON schema " + std::to_string(schema) + "; blit reads schema 1", 1);
                        }
                    }
                }
                out_line.erase(0, at);
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                out_open = false;
            }
        }
    }
    if (stop.stop_requested()) return;
    // It ended: why, from the last line of its errors (a defmt version it
    // does not read, an ELF with no .defmt), on the tab and said.
    for (int waited = 0; waited < 500 && err_open; waited += 20) {
        const ssize_t k = ::read(defmt_.err, buf, sizeof buf);
        if (k > 0) err.append(buf, static_cast<size_t>(k));
        else if (k == 0 || errno != EAGAIN) break;
        else ::usleep(20'000);
    }
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r' || err.back() == ' ')) err.pop_back();
    std::string last = err.substr(err.rfind('\n') == std::string::npos ? 0 : err.rfind('\n') + 1);
    std::string why = "defmt-print ended" + (last.empty() ? std::string() : ": " + last);
    {
        std::lock_guard lock(mu_);
        say(why, 1);
        note("[" + why + "]", wall_ns());
    }
    {
        std::lock_guard lock(defmt_mu_);
        spawn_end(defmt_);
    }
    if (told) told();
}

void LinkTab::defmt_record(const DefmtRecord& r) {
    std::lock_guard order(order_);
    std::string line;
    u64 ns;
    {
        std::lock_guard lock(mu_);
        records_.push_back(r);
        if (records_.size() > kRecordsMost) records_.pop_front();
        if (r.level && r.level < lowest_) return;
        line = defmt_line(r);
        ns = r.host_ns ? r.host_ns : wall_ns();
    }
    // (Its line looked at for crashes and logged, as any stream's.)
    take(line.data(), line.size(), ns);
    if (told) told();
}

void LinkTab::set_level(int lowest) {
    std::lock_guard order(order_);
    std::lock_guard lock(mu_);
    if (lowest < 1 || lowest > 5 || lowest == lowest_) return;
    lowest_ = lowest;
    forget();
    for (const DefmtRecord& r : records_) {
        if (r.level && r.level < lowest_) continue;
        const std::string line = defmt_line(r);
        take_quiet(line.data(), line.size(), r.host_ns ? r.host_ns : wall_ns());
    }
}

int LinkTab::level() {
    std::lock_guard lock(mu_);
    return lowest_;
}

}  // namespace blit
