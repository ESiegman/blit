#include "kernel.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

#include "fsio.hpp"
#include "json.hpp"
#include "remote.hpp"

extern char** environ;

namespace blit {

namespace {

extern const char kHelper[];

inline int spawn_chdir(posix_spawn_file_actions_t* fa, const char* dir) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
    return posix_spawn_file_actions_addchdir(fa, dir);
#else
    return posix_spawn_file_actions_addchdir_np(fa, dir);
#endif
}

// The helper's file's name: named by what is in it, so that another blit's
// (an older one's) is never this one's.
std::string helper_name() {
    u64 h = 1469598103934665603ull;
    for (const char c : std::string_view(kHelper)) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    char name[40];
    std::snprintf(name, sizeof name, "kernel-%016llx.py", static_cast<unsigned long long>(h));
    return name;
}

// The helper written to blit's cache here (once; put in place whole): its path, or "".
std::string helper_here(std::string* err) {
    const std::string dir = cache_dir();
    if (dir.empty()) return *err = "no folder for blit's cache", std::string();
    const std::string path = dir + "/" + helper_name();
    if (fs_kind(path) == 1) return path;
    const std::string part = path + ".part" + std::to_string(::getpid());
    if (!fs_mkdirs(dir, err) || !fs_write(part, kHelper, err)) return {};
    if (std::rename(part.c_str(), path.c_str()) != 0) return *err = std::strerror(errno), std::string();
    return path;
}

// The Python there, chosen as the Python console's is here (appterm.cpp):
// the folder's virtual environment's, uv's for a uv project, else python3.
constexpr const char* kChoose =
    "if [ -x .venv/bin/python ]; then exec .venv/bin/python \"$@\"; fi; "
    "if [ -e uv.lock ] || [ -e pyproject.toml ]; then exec uv run python \"$@\"; fi; "
    "exec python3 \"$@\"";

}  // namespace

// The helper's end of things, shared by the Kernel (on the main thread),
// the thread that reads it, and the one that reaps it.
struct Kernel::Link {
    std::function<void()> notify;
    // Here: its process (the leader of a group of its own: uv, the Python
    // it runs, and what the cells start, end together). There: the
    // program the agent runs (mu, set by the reader once it has started).
    pid_t pid = -1;
    std::shared_ptr<Remote> conn;
    RemoteProc proc;
    int out = -1, err = -1;  // (the reader's)
    std::string remote_dir, notebook_dir;  // (on another machine: where it runs, and the notebook's folder there)
    // Writing. Until it has started (there), what is sent waits in `pending`.
    std::mutex write_mu;
    int in = -1;
    bool up = false;
    std::string pending;
    // What it has said, for poll().
    mutable std::mutex mu;
    std::deque<Event> queue;
    std::string errors;  // (the last of what it wrote as errors before it caught its own)
    std::atomic<i64> helper_pid{-1};  // (as it says)
    std::atomic<bool> ended{false}, stopping{false};
    std::jthread reader;

    bool start_remote(std::stop_token stop);
    void read_loop(std::stop_token stop);
    void heard(std::string_view line);
    void send(const std::string& line);
    void end();
};

Kernel::~Kernel() { stop(); }

bool Kernel::start(const std::string& python, const std::string& dir, const std::string& notebook_dir, std::function<void()> notify, std::string* err) {
    auto l = std::make_shared<Link>();
    l->notify = std::move(notify);
    std::string host, there;
    if (split_remote(dir, &host, &there)) {
        l->remote_dir = dir;  // (connected to, and started there, on the reader's thread)
        std::string nb_host;
        if (!split_remote(notebook_dir, &nb_host, &l->notebook_dir)) l->notebook_dir = there;
    } else {
        const std::string helper = helper_here(err);
        if (helper.empty()) return *err = "cannot write the kernel's helper: " + *err, false;
        int in[2], out[2], er[2];
        if (::pipe(in) || ::pipe(out) || ::pipe(er)) return *err = "cannot make a pipe", false;
        for (const int fd : {in[1], out[0], er[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_adddup2(&fa, er[1], 2);
        if (!dir.empty()) spawn_chdir(&fa, dir.c_str());
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        short flags = POSIX_SPAWN_SETPGROUP;
        posix_spawnattr_setpgroup(&attr, 0);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__)
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
        posix_spawnattr_setflags(&attr, flags);
        std::vector<std::string> words = {"/bin/sh", "-c", "exec " + python + " \"$@\"", "sh", helper, notebook_dir};
        std::vector<char*> args;
        for (std::string& w : words) args.push_back(w.data());
        args.push_back(nullptr);
        const int rc = ::posix_spawn(&l->pid, args[0], &fa, &attr, args.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]), ::close(out[1]), ::close(er[1]);
        if (rc != 0) {
            ::close(in[1]), ::close(out[0]), ::close(er[0]);
            l->pid = -1;
            return *err = std::string("cannot run ") + python + ": " + std::strerror(rc), false;
        }
        l->in = in[1], l->out = out[0], l->err = er[0];
        l->up = true;
    }
    ::signal(SIGPIPE, SIG_IGN);
    Link* raw = l.get();  // (alive until the reader has been joined)
    l->reader = std::jthread([raw](std::stop_token stop) { raw->read_loop(stop); });
    link_ = std::move(l);
    return true;
}

// On another machine: the connection (waited for, as a worker may, but not
// once it has failed, nor past a request to stop), the helper written to
// blit's cache there, and started; then the cells asked for meanwhile are sent.
bool Kernel::Link::start_remote(std::stop_token stop) {
    std::string host, there;
    split_remote(remote_dir, &host, &there);
    std::shared_ptr<Remote> c = Remote::get(host);
    bool ready = false;
    for (int waited = 0; !ready && waited < kRemoteWaitMs && !stop.stop_requested() && c->state() != Remote::kFailed; waited += 200) ready = c->wait_ready(200);
    auto failed = [&](const std::string& why) {
        std::lock_guard lock(mu);
        errors += why + "\n";
        return false;
    };
    if (stop.stop_requested()) return false;
    if (!ready) return failed(remote_error(remote_dir));
    const std::string cache = c->home() + "/.cache/blit", helper = cache + "/" + helper_name();
    std::string why;
    RemoteStat st;
    if (!c->stat(helper, &st) || st.kind != 1) {
        const std::string part = helper + ".part";
        if (!c->make_dir(cache, true, &why) || !c->write(part, kHelper, &why) || !c->rename(part, helper, &why))
            return failed("cannot write the kernel's helper there: " + why);
    }
    RemoteProc p;
    if (!c->spawn({"/bin/sh", "-c", kChoose, "sh", helper, notebook_dir}, there, {}, &p, &why)) return failed("cannot start the kernel there: " + why);
    {
        std::lock_guard lock(mu);
        conn = std::move(c);
        proc = p;
    }
    out = p.out, err = p.err;
    std::lock_guard lock(write_mu);
    in = p.in;
    up = true;
    for (size_t at = 0; at < pending.size();) {
        const ssize_t k = ::write(in, pending.data() + at, pending.size() - at);
        if (k > 0) at += static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else break;
    }
    pending.clear();
    return true;
}

// A line it said, as an event.
void Kernel::Link::heard(std::string_view line) {
    json::Doc doc;
    if (!doc.parse(std::string(line)) || !doc.root().is_object()) return;
    const json::Value v = doc.root();
    const std::string_view type = v["type"].str();
    Event e;
    e.cell = static_cast<u64>(v["id"].integer());
    if (type == "ready") {
        e.kind = Event::kReady;
        e.flag = v["ipython"].boolean();
        e.text = v["version"].str();
        helper_pid = v["pid"].integer(-1);
    } else if (type == "start") {
        e.kind = Event::kStart;
        e.count = v["count"].integer(-1);
    } else if (type == "output") {
        if (!v["output"].is_object()) return;
        e.kind = Event::kOutput;
        json::dump(e.text, v["output"]);
    } else if (type == "clear") {
        e.kind = Event::kClear;
        e.flag = v["wait"].boolean();
    } else if (type == "done") {
        e.kind = Event::kDone;
        e.count = v["count"].integer(-1);
        e.flag = v["ok"].boolean();
    } else {
        return;
    }
    std::lock_guard lock(mu);
    queue.push_back(std::move(e));
}

// Lines, as they come (and what it writes as errors, the last of it kept,
// for why it ended).
void Kernel::Link::read_loop(std::stop_token stop) {
    if (!remote_dir.empty() && !start_remote(stop)) {
        ended = true;
        {
            std::lock_guard lock(mu);
            Event e;
            e.text = errors;
            if (!stopping) queue.push_back(std::move(e));
        }
        if (notify) notify();
        return;
    }
    std::string text;
    char buf[65536];
    pollfd fds[2] = {{out, POLLIN, 0}, {err, POLLIN, 0}};
    nfds_t watched = err >= 0 ? 2 : 1;
    while (!stop.stop_requested()) {
        const int r = ::poll(fds, watched, 1000);  // (idle, it says nothing: seldom woken)
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (watched == 2 && fds[1].revents) {
            const ssize_t k = ::read(err, buf, sizeof buf);
            if (k <= 0) {
                watched = 1;
            } else {
                std::lock_guard lock(mu);
                errors.append(buf, static_cast<size_t>(k));
                if (errors.size() > 4096) errors.erase(0, errors.size() - 4096);
            }
        }
        if (!fds[0].revents) continue;
        const ssize_t k = ::read(out, buf, sizeof buf);
        if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) break;
        if (k < 0) continue;
        text.append(buf, static_cast<size_t>(k));
        size_t at = 0;
        for (size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1) heard(std::string_view(text).substr(at, nl - at));
        text.erase(0, at);
        if (at && notify) notify();
    }
    ended = true;
    {
        std::lock_guard lock(mu);
        Event e;
        e.text = errors;
        if (!stopping) queue.push_back(std::move(e));
    }
    if (notify) notify();
}

void Kernel::Link::send(const std::string& line) {
    std::lock_guard lock(write_mu);
    if (!up) {  // (not started there yet: sent when it has, unless it never will be)
        if (!ended) pending += line;
        return;
    }
    size_t at = 0;
    while (at < line.size() && in >= 0) {
        const ssize_t k = ::write(in, line.data() + at, line.size() - at);
        if (k > 0) at += static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else break;
    }
}

// Its programs reaped, once they have gone (on a thread of its own).
void Kernel::Link::end() {
    reader.request_stop();  // (one still connecting stops; one starting the helper there finishes that first)
    if (reader.joinable()) reader.join();
    std::shared_ptr<Remote> c;
    u32 id;
    {
        std::lock_guard lock(mu);
        c = conn, id = proc.id;
    }
    if (c && id) c->kill(id, SIGKILL);  // (it may have started there since)
    {
        std::lock_guard lock(write_mu);
        if (in >= 0) ::close(in);
        in = -1;
        up = false;
    }
    if (out >= 0) ::close(out);
    if (err >= 0) ::close(err);
    if (pid > 0) ::waitpid(pid, nullptr, 0);
    if (c && id && c->wait(id, 3000) == -2) c->wait(id, 3000);
}

void Kernel::run(u64 cell, std::string_view code) {
    if (!link_) return;
    std::string line = "{\"op\":\"run\",\"id\":" + std::to_string(cell) + ",\"code\":";
    json::quote(line, code);
    line += "}\n";
    link_->send(line);
}

void Kernel::interrupt() {
    if (!link_ || link_->ended) return;
    const i64 helper = link_->helper_pid;
    if (link_->remote_dir.empty() && link_->pid > 0) {  // (here: Ctrl+C to the helper itself, not to a uv that runs it)
        ::kill(helper > 0 ? static_cast<pid_t>(helper) : link_->pid, SIGINT);
        return;
    }
    link_->send("{\"op\":\"interrupt\"}\n");  // (there: it sends itself one)
}

std::vector<Kernel::Event> Kernel::poll() {
    std::vector<Event> out;
    if (!link_) return out;
    std::lock_guard lock(link_->mu);
    out.assign(std::make_move_iterator(link_->queue.begin()), std::make_move_iterator(link_->queue.end()));
    link_->queue.clear();
    return out;
}

bool Kernel::ended() const { return !link_ || link_->ended; }
i64 Kernel::pid() const { return link_ ? link_->helper_pid.load() : -1; }
bool Kernel::remote() const { return link_ && !link_->remote_dir.empty(); }

void Kernel::stop() {
    if (!link_) return;
    std::shared_ptr<Link> l = std::move(link_);
    l->stopping = true;
    // Ended now, from here (a thread just started may not run before blit
    // itself has gone): here its whole group; there, its program, whose
    // input closing ends the helper too, should uv be what was killed.
    if (l->pid > 0) ::kill(-l->pid, SIGKILL);
    {
        std::lock_guard lock(l->mu);
        if (l->conn && l->proc.id) l->conn->kill(l->proc.id, SIGKILL);
    }
    {
        std::lock_guard lock(l->write_mu);
        if (l->in >= 0) ::close(l->in);
        l->in = -1;
        l->up = false;
    }
    std::thread([l = std::move(l)] { l->end(); }).detach();
}

std::string_view Kernel::helper() { return kHelper; }

namespace {

// clang-format off
const char kHelper[] = R"PY(# blit's notebook kernel: runs a notebook's cells for blit, one at a time,
# in IPython's shell when it can be imported, else in plain Python. blit
# and it talk in JSON lines over its standard input and output (the
# program's own output and errors are caught on their way there).
import ast, base64, builtins, json, linecache, os, signal, sys, threading, time, traceback

out_lock = threading.Lock()
proto = os.fdopen(os.dup(1), "wb", buffering=0)  # (blit's, kept before the program's output is caught)
listen = os.fdopen(os.dup(0), "rb", buffering=0)
null = os.open(os.devnull, os.O_RDONLY)
os.dup2(null, 0)  # (the program reads nothing of blit's)
os.close(null)
sys.stdin = open(os.devnull)


def plain(o):  # (what JSON has no word for: bytes as base64, as a notebook keeps a picture)
    if isinstance(o, (bytes, bytearray, memoryview)):
        return base64.b64encode(bytes(o)).decode("ascii")
    return str(o)


def finite(o):  # (NaN and the infinities, which JSON has no word for, as null)
    if isinstance(o, float):
        return o if o == o and abs(o) != float("inf") else None
    if isinstance(o, dict):
        return {k: finite(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [finite(v) for v in o]
    return o


def send(msg):  # (a line of strict JSON, which blit reads: never NaN, which json.dumps writes by default)
    try:
        line = json.dumps(msg, default=plain, allow_nan=False)
    except ValueError:
        try:
            line = json.dumps(finite(msg), default=plain, allow_nan=False)
        except Exception as e:  # (a value that refers to itself, say: said, in place of it)
            if msg.get("type") != "output":
                return
            line = json.dumps({"type": "output", "id": msg.get("id", 0), "output": {"output_type": "stream", "name": "stderr",
                                                                                     "text": "blit could not send an output: %s\n" % e}})
    with out_lock:
        try:
            proto.write(line.encode() + b"\n")
        except OSError:
            os._exit(0)


def quiet():  # (threads of the helper's own take no Ctrl+C: it goes to the cell's)
    try:
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT})
    except (AttributeError, ValueError, OSError):
        pass


# ------------------------------------------------------------ output

cell = 0  # the cell running (blit's number for it)
io_lock = threading.RLock()
pending = {"stdout": [], "stderr": []}
pipes = {}  # the descriptors' read ends, and a decoder for each


def flush_streams():
    with io_lock:
        for name, (fd, dec) in pipes.items():
            while True:
                try:
                    b = os.read(fd, 65536)
                except BlockingIOError:
                    break
                except OSError:
                    break
                if not b:
                    break
                pending[name].append(dec.decode(b))
        for name in ("stdout", "stderr"):
            if pending[name]:
                text = "".join(pending[name])
                pending[name].clear()
                if text:
                    send({"type": "output", "id": cell, "output": {"output_type": "stream", "name": name, "text": text}})


class Stream:
    def __init__(self, name, fd):
        self.name, self.fd = name, fd
        self.encoding, self.errors = "utf-8", "replace"

    def write(self, s):
        if not isinstance(s, str):
            raise TypeError("write() argument must be str, not " + type(s).__name__)
        with io_lock:
            pending[self.name].append(s)
        return len(s)

    def writelines(self, lines):
        for s in lines:
            self.write(s)

    def flush(self):
        flush_streams()

    def isatty(self):
        return False

    def fileno(self):
        return self.fd

    def readable(self):
        return False

    def writable(self):
        return True

    def seekable(self):
        return False

    @property
    def closed(self):
        return False

    @property
    def buffer(self):  # (bytes written to it go out by the descriptor)
        return os.fdopen(os.dup(self.fd), "wb", buffering=0, closefd=True)


def catch(fd, name):  # the descriptor's output (a C library's, a program's the cell runs) caught too
    import codecs, fcntl
    r, w = os.pipe()
    os.dup2(w, fd)
    os.close(w)
    fcntl.fcntl(r, fcntl.F_SETFL, fcntl.fcntl(r, fcntl.F_GETFL) | os.O_NONBLOCK)
    pipes[name] = (r, codecs.getincrementaldecoder("utf-8")("replace"))


catch(1, "stdout")
catch(2, "stderr")
sys.stdout, sys.stderr = Stream("stdout", 1), Stream("stderr", 2)
sys.__stdout__, sys.__stderr__ = sys.stdout, sys.stderr


busy = threading.Event()  # (a cell is running: only then is there output to send)


def flusher():  # output sent as it comes, a few times a second
    quiet()
    while True:
        busy.wait()
        time.sleep(0.05)
        flush_streams()


# ------------------------------------------------------------ the shell

running = False


def on_interrupt(signum, frame):
    if running:
        raise KeyboardInterrupt


signal.signal(signal.SIGINT, on_interrupt)
os.chdir(sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else os.getcwd())
sys.path[0] = ""  # (the notebook's folder, not the helper's)
sys.argv = [""]

os.environ["COLUMNS"] = "58"  # (as wide as a traceback's rules and a progress bar are drawn: a notebook's view beside the script)
shell = None
try:
    import importlib.util
    if importlib.util.find_spec("matplotlib_inline"):  # (plots shown in the notebook, as Jupyter shows them)
        os.environ.setdefault("MPLBACKEND", "module://matplotlib_inline.backend_inline")
    from IPython.core.interactiveshell import InteractiveShell
    from IPython.core.displayhook import DisplayHook
    from IPython.core.displaypub import DisplayPublisher
    from traitlets import Type

    class Result(DisplayHook):
        def start_displayhook(self):
            self.bundle = None

        def write_output_prompt(self):
            pass

        def write_format_data(self, format_dict, md_dict=None):
            self.bundle = (format_dict, md_dict or {})

        def finish_displayhook(self):
            flush_streams()
            if self.bundle:
                send({"type": "output", "id": cell, "output": {"output_type": "execute_result", "execution_count": count,
                                                                "data": self.bundle[0], "metadata": self.bundle[1]}})
            self.bundle = None

    class Shown(DisplayPublisher):
        def publish(self, data, metadata=None, source=None, *, transient=None, update=False, **kw):
            flush_streams()
            send({"type": "output", "id": cell, "output": {"output_type": "display_data", "data": data, "metadata": metadata or {}}})

        def clear_output(self, wait=False):
            flush_streams()
            send({"type": "clear", "id": cell, "wait": bool(wait)})

    class Shell(InteractiveShell):
        displayhook_class = Type(Result)
        display_pub_class = Type(Shown)

        def _showtraceback(self, etype, evalue, stb):
            flush_streams()
            send({"type": "output", "id": cell, "output": {"output_type": "error", "ename": getattr(etype, "__name__", str(etype)),
                                                            "evalue": str(evalue), "traceback": list(stb)}})

        def enable_gui(self, gui=None):
            pass

    shell = Shell.instance()
    for scheme in ("Linux", "linux"):  # (a traceback in colour)
        try:
            shell.colors = scheme
            break
        except Exception:
            pass
except Exception:
    shell = None

namespace = {"__name__": "__main__", "__builtins__": builtins}
count = 0


def bundle(value):  # a value's richest forms, as IPython would give them (plain Python: those it says it has)
    data = {"text/plain": repr(value)}
    for mime, name in (("text/html", "_repr_html_"), ("text/markdown", "_repr_markdown_"), ("text/latex", "_repr_latex_"),
                       ("image/png", "_repr_png_"), ("image/jpeg", "_repr_jpeg_"), ("image/svg+xml", "_repr_svg_"), ("application/json", "_repr_json_")):
        method = getattr(type(value), name, None)
        if not callable(method):
            continue
        try:
            got = method(value)
        except Exception:
            continue
        if isinstance(got, tuple):
            got = got[0]
        if got is not None:
            data[mime] = got
    return data


def display(*values, **kw):
    flush_streams()
    for v in values:
        send({"type": "output", "id": cell, "output": {"output_type": "display_data", "data": bundle(v), "metadata": {}}})


def run_plain(code):
    global count
    count += 1
    name = "<cell %d>" % count
    linecache.cache[name] = (len(code), None, code.splitlines(True), name)  # (its lines shown in a traceback)
    send({"type": "start", "id": cell, "count": count})
    namespace.setdefault("display", display)
    try:
        tree = ast.parse(code, name, "exec")
        last = tree.body.pop() if tree.body and isinstance(tree.body[-1], ast.Expr) else None
        exec(compile(tree, name, "exec"), namespace)
        if last is not None:
            value = eval(compile(ast.Expression(last.value), name, "eval"), namespace)
            if value is not None:
                namespace["_"] = value
                flush_streams()
                send({"type": "output", "id": cell, "output": {"output_type": "execute_result", "execution_count": count, "data": bundle(value), "metadata": {}}})
        return True
    except BaseException as e:
        # (Only the cell's calls and those it made: not the helper's, nor
        # those that read it, before it ran.)
        frames = [f for f in traceback.extract_tb(e.__traceback__) if f.filename != __file__]
        while frames and not frames[0].filename.startswith("<cell"):
            frames.pop(0)
        lines = ["Traceback (most recent call last):\n"] + traceback.StackSummary.from_list(frames).format() if frames else []
        lines += traceback.format_exception_only(type(e), e)
        flush_streams()
        send({"type": "output", "id": cell, "output": {"output_type": "error", "ename": type(e).__name__, "evalue": str(e),
                                                        "traceback": [l.rstrip("\n") for l in lines]}})
        return False


def run_ipython(code):
    global count
    count = shell.execution_count
    send({"type": "start", "id": cell, "count": count})
    result = shell.run_cell(code, store_history=True)
    return result.error_before_exec is None and result.error_in_exec is None


# ------------------------------------------------------------ blit

import queue

todo = queue.Queue()
main_thread = threading.main_thread().ident


def reader():  # what blit asks; when blit has gone, so does this
    quiet()
    for line in listen:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if msg.get("op") == "run":
            todo.put(msg)
        elif msg.get("op") == "interrupt" and running:
            signal.pthread_kill(main_thread, signal.SIGINT)
    os._exit(0)


threading.Thread(target=reader, daemon=True).start()
threading.Thread(target=flusher, daemon=True).start()
send({"type": "ready", "pid": os.getpid(), "ipython": shell is not None, "version": sys.version.split()[0]})
while True:
    msg = todo.get()
    cell = msg.get("id", 0)
    ok = False
    busy.set()
    try:
        running = True
        ok = run_ipython(msg.get("code", "")) if shell else run_plain(msg.get("code", ""))
    except KeyboardInterrupt:  # (between the cell's end and here)
        pass
    finally:
        running = False
        busy.clear()
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except Exception:
        pass
    flush_streams()
    send({"type": "done", "id": cell, "ok": ok, "count": count})
)PY";
// clang-format on

}  // namespace

}  // namespace blit
