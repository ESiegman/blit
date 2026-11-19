#include "tree.hpp"

#include "remote.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "font.hpp"
#include "grid.hpp"
#include "qos.hpp"

namespace blit {

// ---------------------------------------------------------------- icons

FileIcon file_icon(std::string_view name, bool folder, bool open) {
    // Inks: the palette entries with fixed meanings (grid.hpp) double as a
    // small set of colours to tell kinds of file apart.
    const u32 red = kInkDiagnostic, gold = kInkDiagnostic + 1, blue = kInkDiagnostic + 2, purple = kInkDiagnostic + 3,
              grey = kInkMuted, rose = kInkAccent, plain = 0;
    if (folder) return {open ? 0xE5FEu : 0xE5FFu, open ? Font::kIconFolderOpen : Font::kIconFolder, blue};

    std::string lower;
    for (const char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const struct {
        const char* name;
        FileIcon icon;
    } by_name[] = {
        {"makefile", {0xE615, Font::kIconData, grey}},     {"cmakelists.txt", {0xE615, Font::kIconData, grey}},
        {"dockerfile", {0xE615, Font::kIconData, blue}},   {".gitignore", {0xE702, Font::kIconData, red}},
        {".gitattributes", {0xE702, Font::kIconData, red}}, {".gitmodules", {0xE702, Font::kIconData, red}},
        {"license", {0xF15C, Font::kIconText, gold}},      {"readme", {0xEB1D, Font::kIconText, blue}},
    };
    for (const auto& e : by_name)
        if (lower == e.name) return e.icon;

    const size_t dot = lower.rfind('.');
    const std::string_view ext = dot == std::string::npos || dot == 0 ? std::string_view{} : std::string_view(lower).substr(dot + 1);
    static const struct {
        const char* exts;  // space separated
        FileIcon icon;
    } by_ext[] = {
        {"c", {0xE61E, Font::kIconCode, blue}},
        {"cpp cc cxx h hpp hh hxx ipp inl hip", {0xE61D, Font::kIconCode, blue}},
        {"py pyi", {0xE606, Font::kIconCode, gold}},
        {"lua", {0xE620, Font::kIconCode, purple}},
        {"sh bash zsh", {0xE691, Font::kIconCode, rose}},
        {"json jsonc", {0xE60B, Font::kIconData, gold}},
        {"toml yaml yml ini conf cfg cmake mk env", {0xE615, Font::kIconData, grey}},
        {"lock", {0xF023, Font::kIconData, grey}},
        {"md markdown", {0xEB1D, Font::kIconText, blue}},
        {"txt log rst", {0xF15C, Font::kIconText, plain}},
        {"png jpg jpeg gif bmp webp ico tiff svg", {0xF1C5, Font::kIconImage, purple}},
        {"zip tar gz tgz bz2 xz 7z rar", {0xF1C6, Font::kIconArchive, gold}},
        {"o a so dylib dll exe bin obj pyc wasm", {0xEAE8, Font::kIconBinary, grey}},
    };
    if (!ext.empty()) {
        for (const auto& e : by_ext) {
            std::string_view list = e.exts;
            while (!list.empty()) {
                const size_t sp = list.find(' ');
                if (list.substr(0, sp) == ext) return e.icon;
                list = sp == std::string_view::npos ? std::string_view{} : list.substr(sp + 1);
            }
        }
    }
    return {0xF15B, Font::kIconFile, grey};
}

// ---------------------------------------------------------------- reading folders

namespace {

struct Entry {
    std::string name;
    std::string key;  // the name in small letters, to sort by
    bool dir = false;
};

// Names the tree leaves out: version control's own folder and the Finder's
// droppings.
bool left_out(std::string_view name) { return name == ".git" || name == ".DS_Store" || name == "." || name == ".."; }

// Folders first, then by name whatever the case of its letters (and of two
// that differ only in that, capitals first).
bool before(const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir;
    if (const int c = a.key.compare(b.key)) return c < 0;
    return a.name < b.name;
}

// What a folder holds. The kind of each thing comes with its name (no
// looking up each one), but for a link, which is asked where it leads.
std::vector<Entry> read_folder(const std::string& path) {
    std::vector<Entry> out;
    if (is_remote(path)) {  // another machine's, asked of it (on the reader's thread: it may wait for the connection)
        std::string there;
        std::vector<RemoteEntry> list;
        const std::shared_ptr<Remote> r = remote_for(path, &there, kRemoteWaitMs);
        if (!r || !r->list(there, &list)) return out;
        for (RemoteEntry& x : list) {
            if (left_out(x.name)) continue;
            Entry e;
            e.name = std::move(x.name);
            e.dir = x.dir;
            e.key = e.name;
            for (char& c : e.key)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), before);
        return out;
    }
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    std::string full = path;
    if (full.empty() || full.back() != '/') full += '/';
    const size_t base = full.size();
    while (const dirent* de = ::readdir(d)) {
        if (left_out(de->d_name)) continue;
        Entry e;
        e.name = de->d_name;
        if (de->d_type == DT_DIR) {
            e.dir = true;
        } else if (de->d_type == DT_LNK || de->d_type == DT_UNKNOWN) {
            full.resize(base);
            full += e.name;
            struct stat st;
            e.dir = ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        e.key = e.name;
        for (char& c : e.key)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        out.push_back(std::move(e));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), before);
    return out;
}

// (Here: another machine's are asked about on the reader's thread, see FileTree::Op.)
// A link is there even when what it leads to is not: a rename must not put
// something over it.
bool exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

std::string base_name(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

// A path as a URL's is written (a .trashinfo's Path=): every byte but
// letters, digits, "-._~" and "/" as %XX.
std::string url_escaped(const std::string& path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char ch : path) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/')
            out += ch;
        else
            out += {'%', hex[c >> 4], hex[c & 15]};
    }
    return out;
}

// A name for one file or folder: no separators, not "." or "..".
bool usable_name(const std::string& name, std::string* err) {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos) {
        *err = name.empty() ? "a name is needed" : "\"" + name + "\" cannot be used as a name";
        return false;
    }
    return true;
}

}  // namespace

// A change to another machine's files, made on the reader's thread: it and
// the question whether the name is taken there are a round trip each.
struct FileTree::Op {
    Change change;               // (what it is to be, and then what it came to)
    std::string folder;          // read again once it is done
    bool as_folder = false;      // kCreated: a folder, not a file
    bool same_but_for_case = false;  // kRenamed: "Readme" to "README", which may be one name there
};

namespace {

void carry_out(FileTree::Change& c, bool as_folder, bool same_but_for_case) {
    std::string there, err;
    const std::shared_ptr<Remote> r = remote_for(c.path, &there, kRemoteWaitMs);
    if (!r) {
        c.error = remote_error(c.path);
        return;
    }
    RemoteStat st;
    switch (c.kind) {
        case FileTree::Change::kCreated:
            if (r->stat(there, &st) && st.kind != 0) c.error = "\"" + base_name(c.path) + "\" is already there";
            else if (as_folder ? !r->make_dir(there, false, &err) : !r->create(there, &err)) c.error = err;
            break;
        case FileTree::Change::kRenamed: {
            std::string host, there_to;
            split_remote(c.to, &host, &there_to);
            if (!same_but_for_case && r->stat(there_to, &st) && st.kind != 0) c.error = "\"" + base_name(c.to) + "\" is already there";
            else if (!r->rename(there, there_to, &err)) c.error = err;
            break;
        }
        case FileTree::Change::kTrashed:
            if (!r->trash(there, &err)) c.error = err;  // (into the Trash there, of the user blit's agent runs as)
            break;
        default: break;
    }
}

}  // namespace

struct FileTree::Reader {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Op> ops;                                                // changes to make (before folders are read)
    std::vector<std::string> wanted;                                   // folders to read
    std::vector<Change> changed;                                       // changes made
    std::vector<std::pair<std::string, std::vector<Entry>>> finished;  // folders read
    std::function<void()> notify;
    bool stop = false, running = false;
    bool working = false;  // reading a folder, or making a change, now

    static void work(std::shared_ptr<Reader> r) {
        thread_soon(Soon::kWaitedFor);
        for (;;) {
            std::string path;
            Op op;
            bool change = false;
            {
                std::unique_lock lock(r->mu);
                r->working = false;
                r->cv.wait(lock, [&] { return r->stop || !r->wanted.empty() || !r->ops.empty(); });
                if (r->stop) return;
                r->working = true;
                if (!r->ops.empty()) {
                    op = std::move(r->ops.front());
                    r->ops.pop_front();
                    change = true;
                } else {
                    path = std::move(r->wanted.front());
                    r->wanted.erase(r->wanted.begin());
                }
            }
            if (change) {
                carry_out(op.change, op.as_folder, op.same_but_for_case);
                std::function<void()> notify;
                {
                    std::lock_guard lock(r->mu);
                    if (r->stop) return;
                    if (op.change.error.empty() && std::find(r->wanted.begin(), r->wanted.end(), op.folder) == r->wanted.end()) r->wanted.push_back(op.folder);
                    r->changed.push_back(std::move(op.change));
                    notify = r->notify;
                }
                if (notify) notify();
                continue;
            }
            std::vector<Entry> entries = read_folder(path);
            std::function<void()> notify;
            {
                std::lock_guard lock(r->mu);
                if (r->stop) return;
                r->finished.emplace_back(std::move(path), std::move(entries));
                notify = r->notify;
            }
            if (notify) notify();
        }
    }
};

FileTree::FileTree() : reader_(std::make_shared<Reader>()) {}

FileTree::~FileTree() {
    {
        std::lock_guard lock(reader_->mu);
        reader_->stop = true;
        reader_->notify = nullptr;
    }
    reader_->cv.notify_all();  // the thread holds what it needs and ends by itself
}

void FileTree::set_notify(std::function<void()> fn) {
    std::lock_guard lock(reader_->mu);
    reader_->notify = std::move(fn);
}

void FileTree::request(Node* folder) {
    {
        std::lock_guard lock(reader_->mu);
        const std::string path = path_of(folder);
        if (std::find(reader_->wanted.begin(), reader_->wanted.end(), path) == reader_->wanted.end()) reader_->wanted.push_back(path);
        if (!reader_->running) {
            reader_->running = true;
            std::thread(Reader::work, reader_).detach();
        }
    }
    reader_->cv.notify_one();
}

void FileTree::queue(Op op) {
    {
        std::lock_guard lock(reader_->mu);
        reader_->ops.push_back(std::move(op));
        if (!reader_->running) {
            reader_->running = true;
            std::thread(Reader::work, reader_).detach();
        }
    }
    reader_->cv.notify_one();
}

bool FileTree::busy() const {
    std::lock_guard lock(reader_->mu);
    return reader_->working || !reader_->ops.empty() || !reader_->wanted.empty() || !reader_->changed.empty() || !reader_->finished.empty();
}

void FileTree::set_root(const std::string& path) {
    std::string clean = path;
    while (clean.size() > 1 && (clean.back() == '/' || clean.back() == '\\')) clean.pop_back();
    root_path_ = clean;
    root_ = std::make_unique<Node>();
    root_->dir = root_->open = true;
    const size_t slash = clean.find_last_of("/\\");
    root_->name = slash == std::string::npos || clean.size() == 1 ? clean : clean.substr(slash + 1);
    rows_stale_ = true;
    ++generation_;
    request(root_.get());
}

std::string FileTree::path_of(const Node* n) const {
    if (!n || n == root_.get()) return root_path_;
    std::string below;
    for (const Node* at = n; at && at != root_.get(); at = at->parent) below = "/" + at->name + below;
    return (root_path_ == "/" ? "" : root_path_) + below;
}

FileTree::Node* FileTree::node_at(const std::string& path) {
    if (!root_ || !path.starts_with(root_path_)) return nullptr;
    if (path.size() == root_path_.size()) return root_.get();
    if (root_path_ != "/" && path[root_path_.size()] != '/') return nullptr;
    Node* at = root_.get();
    std::string_view rest = std::string_view(path).substr(root_path_.size());
    while (!rest.empty() && at) {
        while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
        if (rest.empty()) break;
        const size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        Node* next = nullptr;
        for (const auto& k : at->kids)
            if (k->name == part) next = k.get();
        at = next;
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    }
    return at;
}

const std::vector<FileTree::Row>& FileTree::rows() {
    if (!rows_stale_) return rows_;
    rows_stale_ = false;
    rows_.clear();
    if (!root_) return rows_;
    // Depth first, without recursion: a tree can be deep.
    struct At {
        Node* node;
        size_t next;
        u32 depth;
    };
    std::vector<At> stack{{root_.get(), 0, 0}};
    while (!stack.empty()) {
        At& top = stack.back();
        if (top.next >= top.node->kids.size()) {
            stack.pop_back();
            continue;
        }
        Node* kid = top.node->kids[top.next++].get();
        const u32 depth = top.depth;
        rows_.push_back({kid, depth});
        if (kid->dir && kid->open) stack.push_back({kid, 0, depth + 1});
    }
    return rows_;
}

void FileTree::toggle(Node* folder) {
    if (!folder || !folder->dir) return;
    folder->open = !folder->open;
    if (folder->open) request(folder);  // also picks up what changed since it was last open
    rows_stale_ = true;
    ++generation_;
}

void FileTree::refresh(Node* folder) {
    if (!root_) return;
    if (folder) return request(folder);
    refresh_under(root_.get());
}

void FileTree::refresh_under(Node* folder) {
    if (!folder || !folder->dir) return;
    std::vector<Node*> todo{folder};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        request(n);
        for (const auto& k : n->kids)
            if (k->dir && k->open) todo.push_back(k.get());
    }
}

bool FileTree::poll() {
    std::vector<std::pair<std::string, std::vector<Entry>>> done;
    std::vector<Change> made;
    {
        std::lock_guard lock(reader_->mu);
        done.swap(reader_->finished);
        made.swap(reader_->changed);
    }
    bool changed = false;
    // Changes made there: a node renamed keeps its place (and whether it is
    // open) as one renamed here does; one in the Trash goes. (Their folders
    // are being read again.)
    for (Change& c : made) {
        if (c.error.empty() && (c.kind == Change::kRenamed || c.kind == Change::kTrashed)) {
            if (Node* node = node_at(c.path); node && node != root_.get() && node->parent) {
                if (c.kind == Change::kRenamed) {
                    node->name = base_name(c.to);
                } else {
                    Node* parent = node->parent;
                    std::erase_if(parent->kids, [&](const std::unique_ptr<Node>& k) { return k.get() == node; });
                }
                changed = true;
            }
        }
        changes_.push_back(std::move(c));
    }
    for (auto& [path, entries] : done) {
        Node* folder = node_at(path);
        if (!folder || !folder->dir) continue;  // it has gone, or the root moved on
        // Nodes that are still there keep their place in memory, and with
        // it whether they are open and what is under them.
        std::unordered_map<std::string_view, size_t> have;
        have.reserve(folder->kids.size());
        for (size_t i = 0; i < folder->kids.size(); ++i) have.emplace(folder->kids[i]->name, i);
        std::vector<std::unique_ptr<Node>> kids;
        kids.reserve(entries.size());
        for (Entry& e : entries) {
            std::unique_ptr<Node> kid;
            if (const auto it = have.find(e.name); it != have.end() && folder->kids[it->second] && folder->kids[it->second]->dir == e.dir)
                kid = std::move(folder->kids[it->second]);
            if (!kid) {
                kid = std::make_unique<Node>();
                kid->name = std::move(e.name);
                kid->dir = e.dir;
                kid->parent = folder;
            }
            kids.push_back(std::move(kid));
        }
        have.clear();  // its keys point into nodes about to go
        folder->kids = std::move(kids);
        folder->loaded = true;
        changed = true;
    }
    if (changed) {
        rows_stale_ = true;
        ++generation_;
    }
    return changed;
}

// ---------------------------------------------------------------- changing things

bool FileTree::create(Node* folder, const std::string& name, bool as_folder, std::string* err) {
    if (!folder || !folder->dir) folder = root_.get();
    if (!folder || !usable_name(name, err)) return false;
    const std::string path = path_of(folder) + "/" + name;
    if (is_remote(path)) {  // (another machine's: its agent makes it, on the reader's thread)
        Op op;
        op.change = {Change::kCreated, path, {}, {}};
        op.folder = path_of(folder);
        op.as_folder = as_folder;
        queue(std::move(op));
        if (!folder->open) {  // (read once the change is made, whether or not it was)
            folder->open = true;
            request(folder);
            rows_stale_ = true;
            ++generation_;
        }
        return true;
    }
    if (exists(path)) {
        *err = "\"" + name + "\" is already there";
        return false;
    }
    if (as_folder) {
        if (!SDL_CreateDirectory(path.c_str())) {
            *err = std::string("cannot create the folder: ") + SDL_GetError();
            return false;
        }
    } else {
        // "x": fail rather than empty a file that appeared in the meantime.
        FILE* f = std::fopen(path.c_str(), "wbx");
        if (!f) {
            *err = "cannot create the file";
            return false;
        }
        std::fclose(f);
    }
    folder->open = true;
    request(folder);
    return true;
}

bool FileTree::rename(Node* node, const std::string& new_name, std::string* err) {
    if (!node || node == root_.get() || !node->parent) return false;
    if (!usable_name(new_name, err)) return false;
    if (new_name == node->name) return true;
    const std::string from = path_of(node), to = path_of(node->parent) + "/" + new_name;
    // On a file system that ignores case, "Readme" and "README" are one
    // file: renaming between them must not be refused as "already there".
    bool same_but_for_case = new_name.size() == node->name.size();
    for (size_t i = 0; same_but_for_case && i < new_name.size(); ++i)
        same_but_for_case = std::tolower(static_cast<unsigned char>(new_name[i])) == std::tolower(static_cast<unsigned char>(node->name[i]));
    if (is_remote(from)) {  // (on the reader's thread; the node takes its new name when it is done)
        Op op;
        op.change = {Change::kRenamed, from, to, {}};
        op.folder = path_of(node->parent);
        op.same_but_for_case = same_but_for_case;
        queue(std::move(op));
        return true;
    }
    bool same_file = false;
    if (same_but_for_case) {
        struct stat a, b;
        same_file = ::lstat(from.c_str(), &a) == 0 && ::lstat(to.c_str(), &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }
    if (exists(to) && !(same_but_for_case && same_file)) {
        *err = "\"" + new_name + "\" is already there";
        return false;
    }
    if (!SDL_RenamePath(from.c_str(), to.c_str())) {
        *err = std::string("cannot rename: ") + SDL_GetError();
        return false;
    }
    node->name = new_name;  // shown at once; the re-read puts it in its sorted place
    rows_stale_ = true;
    ++generation_;
    request(node->parent);
    return true;
}

bool FileTree::move(Node* node, Node* folder, std::string* err) {
    if (!node || !folder || node == root_.get() || !node->parent || !folder->dir) return false;
    if (node->parent == folder) return true;
    const std::string from = path_of(node), into = path_of(folder), to = into + "/" + node->name;
    if (is_remote(from) || is_remote(into)) {
        *err = "moving is for files on this machine";
        return false;
    }
    for (const Node* up = folder; up; up = up->parent)
        if (up == node) {
            *err = "\"" + node->name + "\" cannot go inside itself";
            return false;
        }
    if (exists(to)) {
        *err = "\"" + node->name + "\" is already in " + folder->name;
        return false;
    }
    if (!SDL_RenamePath(from.c_str(), to.c_str())) {
        *err = std::string("cannot move: ") + SDL_GetError();
        return false;
    }
    rows_stale_ = true;
    ++generation_;
    request(node->parent);  // (both folders read again: the node goes from one, comes in the other)
    request(folder);
    return true;
}

bool FileTree::has_trash() {
#if defined(__APPLE__) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool FileTree::trash(Node* node, std::string* err) {
    if (!node || node == root_.get() || !node->parent) return false;
    const std::string from = path_of(node);
    if (is_remote(from)) {  // (on the reader's thread; the node goes when it is done)
        Op op;
        op.change = {Change::kTrashed, from, {}, {}};
        op.folder = path_of(node->parent);
        queue(std::move(op));
        return true;
    }
    const char* home = std::getenv("HOME");
    // (BLIT_TRASH_DIR names another folder to use as the Trash: the tests
    // must not put things in the real one.)
    const char* other = std::getenv("BLIT_TRASH_DIR");
    if (other && *other) home = other;
#if defined(__APPLE__)
    const std::string bin = other && *other ? std::string(other) : std::string(home ? home : "") + "/.Trash";
    const std::string info_dir;
#elif defined(__linux__)
    // The desktop's Trash: the thing itself in files/, where it came from in
    // info/. (The tests' folder takes the things straight, as a Mac's does.)
    std::string bin, info_dir;
    if (other && *other) {
        bin = other;
    } else {
        const char* data = std::getenv("XDG_DATA_HOME");
        const std::string base = (data && *data ? std::string(data) : std::string(home ? home : "") + "/.local/share") + "/Trash";
        bin = base + "/files", info_dir = base + "/info";
        SDL_CreateDirectory(info_dir.c_str());
    }
    SDL_CreateDirectory(bin.c_str());
#else
    const std::string bin, info_dir;
#endif
    if (bin.empty() || !home) {
        if (!SDL_RemovePath(from.c_str())) {
            *err = std::string("cannot delete (a folder has to be empty): ") + SDL_GetError();
            return false;
        }
    } else {
        // A name not taken in the Trash: "notes.txt", "notes 2.txt", ...
        // (On Linux neither in files/ nor in info/, whose NAME.trashinfo is
        // made first, as the desktop's own Trash does, to keep the name.)
        const size_t dot = node->dir ? std::string::npos : node->name.rfind('.');
        const std::string stem = dot == std::string::npos || dot == 0 ? node->name : node->name.substr(0, dot);
        const std::string ext = dot == std::string::npos || dot == 0 ? "" : node->name.substr(dot);
        std::string name = node->name, info;
        FILE* f = nullptr;
        for (int n = 2;; ++n) {
            if (!exists(bin + "/" + name)) {
                if (info_dir.empty()) break;
                info = info_dir + "/" + name + ".trashinfo";
                if ((f = std::fopen(info.c_str(), "wbx")) || errno != EEXIST) break;  // ("x": not over one there)
            }
            if (n >= 10000) break;
            name = stem + " " + std::to_string(n) + ext;
        }
        if (f) {
            // What the desktop needs to offer "restore" (the path written as a URL's is).
            char when[32];
            const std::time_t now = std::time(nullptr);
            std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
            std::fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", url_escaped(from).c_str(), when);
            std::fclose(f);
        }
        if (!SDL_RenamePath(from.c_str(), (bin + "/" + name).c_str())) {
            *err = std::string("cannot move it to the Trash: ") + SDL_GetError();
            if (f) std::remove(info.c_str());
            return false;
        }
    }
    Node* parent = node->parent;
    std::erase_if(parent->kids, [&](const std::unique_ptr<Node>& k) { return k.get() == node; });
    rows_stale_ = true;
    ++generation_;
    request(parent);
    return true;
}

}  // namespace blit
