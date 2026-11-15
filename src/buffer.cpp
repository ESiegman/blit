#include "buffer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "scan.hpp"
#include "utf8.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#endif

namespace blit {
using namespace detail;

// ---------------------------------------------------------------- Pool

template <class T>
T* Pool<T>::alloc() {
    if (free_) {
        void* p = free_;
        free_ = *reinterpret_cast<void**>(p);
        return new (p) T();
    }
    if (bump_ == bump_end_) {
        reserve(next_slab_);
        if (next_slab_ < 4096) next_slab_ *= 2;
    }
    return new (static_cast<void*>(bump_++)) T();
}

template <class T>
void Pool<T>::reserve(size_t count) {
    // Whatever is left of the current slab goes to the free list.
    while (bump_ != bump_end_) free(bump_++);
    void* mem = std::malloc(count * sizeof(T));
    if (!mem) std::abort();
    slabs_.push_back({mem, count});
    bump_ = static_cast<T*>(mem);
    bump_end_ = bump_ + count;
}

template <class T>
void Pool<T>::release() {
    for (Slab& s : slabs_) std::free(s.mem);
    slabs_.clear();
    bump_ = bump_end_ = nullptr;
    free_ = nullptr;
    next_slab_ = 32;
}

// ---------------------------------------------------------------- helpers

namespace {

// Even split of m items over nn groups: group j gets base + (j < extra).
struct Split {
    u32 nn, base, extra;
    Split(size_t m, u32 cap)
        : nn(static_cast<u32>((m + cap - 1) / cap)),
          base(static_cast<u32>(m / nn)),
          extra(static_cast<u32>(m % nn)) {}
    u32 count(u32 j) const { return base + (j < extra ? 1 : 0); }
};

Entry fill_inner(Inner* nd, const Entry* es, u32 cnt) {
    u64 sb = 0, sn = 0;
    for (u32 i = 0; i < cnt; ++i) {
        nd->kid[i] = es[i].node;
        nd->bytes[i] = es[i].bytes;
        nd->nls[i] = es[i].nls;
        sb += es[i].bytes;
        sn += es[i].nls;
    }
    nd->n = static_cast<u16>(cnt);
    return {nd, sb, sn};
}

Entry fill_leaf(Leaf* lf, const Piece* ps, u32 cnt) {
    u64 sb = 0, sn = 0;
    std::memcpy(lf->ps, ps, cnt * sizeof(Piece));
    for (u32 i = 0; i < cnt; ++i) {
        sb += ps[i].len;
        sn += ps[i].nl;
    }
    lf->n = static_cast<u16>(cnt);
    return {lf, sb, sn};
}

// Merges ps[i] and ps[i+1] when they are adjacent in memory.
void try_merge(Leaf* lf, u32 i) {
    if (i + 1 >= lf->n) return;
    Piece& a = lf->ps[i];
    const Piece& b = lf->ps[i + 1];
    if (a.p + a.len != b.p || a.len + b.len > Buffer::kMaxPiece) return;
    a.len += b.len;
    a.nl += b.nl;
    std::memmove(&lf->ps[i + 1], &lf->ps[i + 2], (lf->n - i - 2) * sizeof(Piece));
    --lf->n;
}

void remove_entry(Inner* nd, u32 i) {
    u32 tail = nd->n - i - 1;
    std::memmove(&nd->bytes[i], &nd->bytes[i + 1], tail * sizeof(u64));
    std::memmove(&nd->nls[i], &nd->nls[i + 1], tail * sizeof(u64));
    std::memmove(&nd->kid[i], &nd->kid[i + 1], tail * sizeof(Node*));
    --nd->n;
}

void unlink(Leaf* lf) {
    if (lf->prev) lf->prev->next = lf->next;
    if (lf->next) lf->next->prev = lf->prev;
}

// Largest split point <= want that does not cut a UTF-8 sequence.
size_t char_boundary(const char* s, size_t want) {
    size_t k = want;
    for (int back = 0; back < 3 && k > 0 && is_cont(static_cast<u8>(s[k])); ++back) --k;
    return (k == 0 || is_cont(static_cast<u8>(s[k]))) ? want : k;
}

}  // namespace

// ---------------------------------------------------------------- mapped files
//
// The original file is memory mapped, never copied. The price of that is
// that another program can shrink the file underneath us, and touching a
// page past its new end raises SIGBUS. Instead of dying (and taking unsaved
// edits along), the handler below backs such a page with zeros and lets the
// read continue: the lost text shows up as blanks.

#if !defined(_WIN32)
namespace {

struct Mapping {
    std::atomic<char*> base{nullptr};
    std::atomic<size_t> len{0};
};
constinit Mapping g_maps[64];
constinit std::atomic<bool> g_guard_installed{false};
struct sigaction g_old_bus, g_old_segv;

void on_bad_page(int sig, siginfo_t* info, void* ctx) {
    char* addr = static_cast<char*>(info->si_addr);
    for (Mapping& m : g_maps) {
        char* base = m.base.load(std::memory_order_acquire);
        if (!base || addr < base || addr >= base + m.len.load(std::memory_order_acquire)) continue;
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        char* at = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(addr) & ~(page - 1));
        if (mmap(at, page, PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != MAP_FAILED) return;
    }
    // Not ours: hand over to whoever was there before, or the default.
    struct sigaction* old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) return old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        return old->sa_handler(sig);
    }
    signal(sig, SIG_DFL);
}

void guard_mapping(char* base, size_t len) {
    if (!g_guard_installed.exchange(true)) {
        struct sigaction sa{};
        sa.sa_sigaction = on_bad_page;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS, &sa, &g_old_bus);
        sigaction(SIGSEGV, &sa, &g_old_segv);  // some systems report it this way
    }
    for (Mapping& m : g_maps) {
        char* expected = nullptr;
        if (m.base.load() == nullptr) {
            m.len.store(len);
            if (m.base.compare_exchange_strong(expected, base)) return;
        }
    }
}

void unguard_mapping(char* base) {
    for (Mapping& m : g_maps)
        if (m.base.load() == base) m.base.store(nullptr);
}

}  // namespace
#endif

// ---------------------------------------------------------------- lifetime

Buffer::Buffer() { root_ = leaves_.alloc(); }

void Buffer::drop_map() {
#if !defined(_WIN32)
    if (map_ && map_is_mmap_) {
        unguard_mapping(static_cast<char*>(map_));
        munmap(map_, map_len_);
    }
#endif
    if (map_ && !map_is_mmap_) std::free(map_);
    map_ = nullptr;
    map_len_ = 0;
}

Buffer::~Buffer() {
    for (char* b : blocks_) std::free(b);
    drop_map();
}

void Buffer::clear() {
    leaves_.release();
    inners_.release();
    for (char* b : blocks_) std::free(b);
    blocks_.clear();
    add_tail_ = nullptr;
    add_room_ = 0;
    drop_map();
    root_ = leaves_.alloc();
    height_ = 0;
    bytes_ = nls_ = 0;
}

// ---------------------------------------------------------------- loading

void Buffer::load_view(const char* data, u64 n) {
    clear();
    if (n == 0) return;

    const u64 K = kMaxPiece;
    const u64 np = (n + K - 1) / K;
    auto start_of = [&](u64 i) -> u64 { return i >= np ? n : char_boundary(data, i * K); };

    std::vector<Piece> ps(np);
    auto fill = [&](u64 lo, u64 hi) {
        u64 s = start_of(lo);
        for (u64 i = lo; i < hi; ++i) {
            u64 e = start_of(i + 1);
            ps[i] = {data + s, static_cast<u32>(e - s), count_nl(data + s, e - s)};
            s = e;
        }
    };
    unsigned hw = std::thread::hardware_concurrency();
    u64 nt = std::min<u64>(hw ? hw : 1, n / (8u << 20));
    if (nt <= 1) {
        fill(0, np);
    } else {
        std::vector<std::thread> ts;
        for (u64 t = 0; t < nt; ++t) ts.emplace_back(fill, np * t / nt, np * (t + 1) / nt);
        for (auto& t : ts) t.join();
    }

    // Leaves, then inner levels bottom-up.
    Split sp(np, kLeafCap);
    leaves_.free(static_cast<Leaf*>(root_));
    leaves_.reserve(sp.nn);
    std::vector<Entry> es(sp.nn);
    Leaf* prev = nullptr;
    u64 at = 0;
    for (u32 j = 0; j < sp.nn; ++j) {
        Leaf* lf = leaves_.alloc();
        u32 cnt = sp.count(j);
        es[j] = fill_leaf(lf, &ps[at], cnt);
        at += cnt;
        lf->prev = prev;
        if (prev) prev->next = lf;
        prev = lf;
    }
    std::vector<Entry> up;
    while (es.size() > 1) {
        Split si(es.size(), kInnerCap);
        up.clear();
        size_t a = 0;
        for (u32 j = 0; j < si.nn; ++j) {
            up.push_back(fill_inner(inners_.alloc(), &es[a], si.count(j)));
            a += si.count(j);
        }
        es.swap(up);
        ++height_;
    }
    root_ = es[0].node;
    bytes_ = es[0].bytes;
    nls_ = es[0].nls;
}

bool Buffer::load_file(const char* path, std::string* err) {
    auto fail = [&](const char* what) {
        if (err) *err = std::string(what) + ": " + path;
        return false;
    };
#if !defined(_WIN32)
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return fail("cannot open");
    struct stat st;
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return fail("cannot stat");
    }
    if (S_ISREG(st.st_mode)) {
        size_t n = static_cast<size_t>(st.st_size);
        void* m = n ? mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        ::close(fd);
        if (n && m == MAP_FAILED) return fail("cannot map");
        if (n) guard_mapping(static_cast<char*>(m), n);
        load_view(static_cast<const char*>(m), n);
        map_ = m;
        map_len_ = n;
        map_is_mmap_ = true;
        return true;
    }
    ::close(fd);
#endif
    // Pipes, devices and platforms without mmap: read everything.
    FILE* f = std::fopen(path, "rb");
    if (!f) return fail("cannot open");
    size_t cap = 1 << 16, n = 0;
    char* data = static_cast<char*>(std::malloc(cap));
    for (;;) {
        size_t got = std::fread(data + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
        if (n == cap) data = static_cast<char*>(std::realloc(data, cap *= 2));
    }
    std::fclose(f);
    load_view(data, n);
    map_ = data;
    map_len_ = n;
    map_is_mmap_ = false;
    return true;
}

void Buffer::load_bytes(std::string_view bytes) {
    char* data = static_cast<char*>(std::malloc(bytes.empty() ? 1 : bytes.size()));
    std::memcpy(data, bytes.data(), bytes.size());
    load_view(data, bytes.size());
    map_ = data;
    map_len_ = bytes.size();
    map_is_mmap_ = false;
}

bool Buffer::save_file(const char* path, std::string* err) const {
#if !defined(_WIN32)
    // Write through symlinks instead of replacing the link with a file.
    char real[PATH_MAX];
    if (realpath(path, real)) path = real;
#endif
    std::string tmp = std::string(path) + ".blit-tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot write: " + tmp;
        return false;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    const Node* nd = root_;
    for (int l = 0; l < height_; ++l) nd = static_cast<const Inner*>(nd)->kid[0];
    bool ok = true;
    // Pieces that sit next to each other in memory go out as one write.
    const char* run = nullptr;
    size_t run_len = 0;
    for (const Leaf* lf = static_cast<const Leaf*>(nd); lf; lf = lf->next) {
        for (u32 i = 0; i < lf->n; ++i) {
            const Piece& pc = lf->ps[i];
            if (run && run + run_len == pc.p) {
                run_len += pc.len;
                continue;
            }
            if (run_len) ok = ok && std::fwrite(run, 1, run_len, f) == run_len;
            run = pc.p;
            run_len = pc.len;
        }
    }
    if (run_len) ok = ok && std::fwrite(run, 1, run_len, f) == run_len;
    ok = (std::fflush(f) == 0) && ok;
#if !defined(_WIN32)
    struct stat st;
    if (ok && stat(path, &st) == 0) fchmod(fileno(f), st.st_mode & 07777);
#endif
    ok = (std::fclose(f) == 0) && ok;
#if defined(_WIN32)
    if (ok) std::remove(path);
#endif
    if (ok) ok = std::rename(tmp.c_str(), path) == 0;
    if (!ok) {
        std::remove(tmp.c_str());
        if (err) *err = std::string("cannot save: ") + path;
    }
    return ok;
}

// ---------------------------------------------------------------- lookup

Buffer::Pos Buffer::locate(u64 off, bool sticky, Path& path) const {
    Node* nd = root_;
    for (int l = 0; l < height_; ++l) {
        Inner* in = static_cast<Inner*>(nd);
        const u32 last = in->n - 1u;
        u32 i = 0;
        if (sticky) {
            while (i < last && off > in->bytes[i]) off -= in->bytes[i++];
        } else {
            while (i < last && off >= in->bytes[i]) off -= in->bytes[i++];
        }
        path.node[l] = in;
        path.idx[l] = static_cast<u16>(i);
        nd = in->kid[i];
    }
    Leaf* lf = static_cast<Leaf*>(nd);
    if (lf->n == 0) return {lf, 0, 0};
    const u32 last = lf->n - 1u;
    u32 i = 0;
    if (sticky) {
        while (i < last && off > lf->ps[i].len) off -= lf->ps[i++].len;
    } else {
        while (i < last && off >= lf->ps[i].len) off -= lf->ps[i++].len;
    }
    return {lf, i, static_cast<u32>(off)};
}

void Buffer::bump(const Path& path, i64 db, i64 dn) {
    for (int l = 0; l < height_; ++l) {
        path.node[l]->bytes[path.idx[l]] += static_cast<u64>(db);
        path.node[l]->nls[path.idx[l]] += static_cast<u64>(dn);
    }
    bytes_ += static_cast<u64>(db);
    nls_ += static_cast<u64>(dn);
}

u64 Buffer::line_start(u64 line) const {
    u64 off;
    line_reader(line, &off);
    return off;
}

Buffer::Reader Buffer::line_reader(u64 line, u64* off_out) const {
    Reader r;
    if (line == 0 || line > nls_) {
        u64 off = line == 0 ? 0 : bytes_;
        if (off_out) *off_out = off;
        return reader(off);
    }
    u64 k = line - 1;  // the newline that ends the previous line
    u64 off = 0;
    const Node* nd = root_;
    for (int l = 0; l < height_; ++l) {
        const Inner* in = static_cast<const Inner*>(nd);
        u32 i = 0;
        while (k >= in->nls[i]) {
            k -= in->nls[i];
            off += in->bytes[i];
            ++i;
        }
        nd = in->kid[i];
    }
    const Leaf* lf = static_cast<const Leaf*>(nd);
    u32 i = 0;
    while (k >= lf->ps[i].nl) {
        k -= lf->ps[i].nl;
        off += lf->ps[i].len;
        ++i;
    }
    u32 local = find_nth_nl(lf->ps[i].p, lf->ps[i].len, static_cast<u32>(k)) + 1;
    if (off_out) *off_out = off + local;
    r.leaf_ = lf;
    r.idx_ = i;
    r.local_ = local;
    if (local >= lf->ps[i].len) r.next_piece();
    return r;
}

u64 Buffer::line_of(u64 off) const {
    if (off >= bytes_) return nls_;
    u64 line = 0;
    const Node* nd = root_;
    for (int l = 0; l < height_; ++l) {
        const Inner* in = static_cast<const Inner*>(nd);
        u32 i = 0;
        while (off >= in->bytes[i]) {
            off -= in->bytes[i];
            line += in->nls[i];
            ++i;
        }
        nd = in->kid[i];
    }
    const Leaf* lf = static_cast<const Leaf*>(nd);
    u32 i = 0;
    while (off >= lf->ps[i].len) {
        off -= lf->ps[i].len;
        line += lf->ps[i].nl;
        ++i;
    }
    return line + count_nl(lf->ps[i].p, off);
}

Buffer::Reader Buffer::reader(u64 off) const {
    Path path;
    Pos pos = locate(std::min(off, bytes_), false, path);
    Reader r;
    r.leaf_ = pos.leaf;
    r.idx_ = pos.idx;
    r.local_ = pos.local;
    if (pos.leaf->n && pos.local >= pos.leaf->ps[pos.idx].len) r.next_piece();
    return r;
}

u64 Buffer::read(u64 off, char* dst, u64 n) const {
    if (off >= bytes_) return 0;
    n = std::min(n, bytes_ - off);
    Reader r = reader(off);
    u64 done = 0;
    while (done < n) {
        std::string_view c = r.chunk();
        u32 k = static_cast<u32>(std::min<u64>(c.size(), n - done));
        std::memcpy(dst + done, c.data(), k);
        done += k;
        r.advance(k);
    }
    return done;
}

std::string Buffer::text(u64 off, u64 n) const {
    if (off >= bytes_) return {};
    n = std::min(n, bytes_ - off);
    std::string s;
    s.resize_and_overwrite(static_cast<size_t>(n), [&](char* p, size_t m) { return static_cast<size_t>(read(off, p, m)); });
    return s;
}

void Buffer::snapshot(std::vector<Piece>* out) const {
    out->clear();
    const Node* nd = root_;
    for (int l = 0; l < height_; ++l) nd = static_cast<const Inner*>(nd)->kid[0];
    for (const Leaf* lf = static_cast<const Leaf*>(nd); lf; lf = lf->next) out->insert(out->end(), lf->ps, lf->ps + lf->n);
}

size_t Buffer::piece_count() const {
    const Node* nd = root_;
    for (int l = 0; l < height_; ++l) nd = static_cast<const Inner*>(nd)->kid[0];
    size_t c = 0;
    for (const Leaf* lf = static_cast<const Leaf*>(nd); lf; lf = lf->next) c += lf->n;
    return c;
}

// ---------------------------------------------------------------- insert

void Buffer::append_text(std::string_view s, std::vector<Piece>& out) {
    if (s.size() > add_room_) {
        size_t cap = std::max<size_t>(1u << 20, s.size());
        char* b = static_cast<char*>(std::malloc(cap));
        if (!b) std::abort();
        blocks_.push_back(b);
        add_tail_ = b;
        add_room_ = cap;
    }
    char* dst = add_tail_;
    std::memcpy(dst, s.data(), s.size());
    add_tail_ += s.size();
    add_room_ -= s.size();
    size_t at = 0;
    while (at < s.size()) {
        size_t take = s.size() - at;
        if (take > kMaxPiece) take = char_boundary(dst + at, kMaxPiece);
        out.push_back({dst + at, static_cast<u32>(take), count_nl(dst + at, take)});
        at += take;
    }
}

void Buffer::insert(u64 off, std::string_view s) {
    if (s.empty()) return;
    off = std::min(off, bytes_);
    Path path;
    Pos pos = locate(off, true, path);

    // Typing: the piece under the cursor ends at the write head, so the new
    // bytes extend it in place.
    if (pos.leaf->n && s.size() <= add_room_) {
        Piece& pc = pos.leaf->ps[pos.idx];
        if (pos.local == pc.len && pc.p + pc.len == add_tail_ && pc.len + s.size() <= kMaxPiece) {
            std::memcpy(add_tail_, s.data(), s.size());
            add_tail_ += s.size();
            add_room_ -= s.size();
            u32 nl = count_nl(s.data(), s.size());
            pc.len += static_cast<u32>(s.size());
            pc.nl += nl;
            bump(path, static_cast<i64>(s.size()), nl);
            return;
        }
    }
    scratch_.clear();
    append_text(s, scratch_);
    insert_at(pos, path, scratch_);
}

void Buffer::insert_pieces(u64 off, std::span<const Piece> ps) {
    if (ps.empty()) return;
    Path path;
    Pos pos = locate(std::min(off, bytes_), true, path);
    insert_at(pos, path, ps);
}

void Buffer::insert_at(Pos pos, Path& path, std::span<const Piece> ps) {
    Leaf* leaf = pos.leaf;
    u64 add_b = 0, add_n = 0;
    for (const Piece& p : ps) {
        add_b += p.len;
        add_n += p.nl;
    }

    u32 at = 0;
    Piece right{};
    bool has_right = false;
    if (leaf->n) {
        Piece& pc = leaf->ps[pos.idx];
        if (pos.local == 0) {
            at = pos.idx;
        } else if (pos.local == pc.len) {
            at = pos.idx + 1;
        } else {
            u32 lnl = count_nl(pc.p, pos.local);
            right = {pc.p + pos.local, pc.len - pos.local, pc.nl - lnl};
            pc.len = pos.local;
            pc.nl = lnl;
            has_right = true;
            at = pos.idx + 1;
        }
    }
    const u32 k = static_cast<u32>(ps.size()) + (has_right ? 1 : 0);

    if (leaf->n + k <= static_cast<u32>(kLeafCap)) {
        std::memmove(&leaf->ps[at + k], &leaf->ps[at], (leaf->n - at) * sizeof(Piece));
        std::memcpy(&leaf->ps[at], ps.data(), ps.size() * sizeof(Piece));
        if (has_right) leaf->ps[at + ps.size()] = right;
        leaf->n = static_cast<u16>(leaf->n + k);
        try_merge(leaf, at + k - 1);
        if (at > 0) try_merge(leaf, at - 1);
        bump(path, static_cast<i64>(add_b), static_cast<i64>(add_n));
        return;
    }

    // The leaf overflows: spread its pieces over as many leaves as needed.
    std::vector<Piece> all;
    all.reserve(leaf->n + k);
    all.insert(all.end(), leaf->ps, leaf->ps + at);
    all.insert(all.end(), ps.begin(), ps.end());
    if (has_right) all.push_back(right);
    all.insert(all.end(), leaf->ps + at, leaf->ps + leaf->n);

    Split sp(all.size(), kLeafCap);
    std::vector<Entry> es;
    es.reserve(sp.nn);
    Leaf* after = leaf->next;
    Leaf* prev = leaf;
    size_t a = 0;
    for (u32 j = 0; j < sp.nn; ++j) {
        Leaf* lf = leaf;
        if (j > 0) {
            lf = leaves_.alloc();
            lf->prev = prev;
            prev->next = lf;
            prev = lf;
        }
        es.push_back(fill_leaf(lf, &all[a], sp.count(j)));
        a += sp.count(j);
    }
    prev->next = after;
    if (after) after->prev = prev;

    bytes_ += add_b;
    nls_ += add_n;
    spread_up(path, height_ - 1, es);
}

// es replaces the child at path.idx[level] of path.node[level]; es[0].node is
// that child. Splits inner nodes on the way up as needed.
void Buffer::spread_up(Path& path, int level, std::vector<Entry>& es) {
    std::vector<Entry> all;
    for (; level >= 0; --level) {
        Inner* nd = path.node[level];
        const u32 ci = path.idx[level];
        const u32 k = static_cast<u32>(es.size()) - 1;
        const u32 m = nd->n + k;
        if (m <= static_cast<u32>(kInnerCap)) {
            u64 db = 0 - nd->bytes[ci], dn = 0 - nd->nls[ci];
            const u32 tail = nd->n - ci - 1;
            std::memmove(&nd->bytes[ci + 1 + k], &nd->bytes[ci + 1], tail * sizeof(u64));
            std::memmove(&nd->nls[ci + 1 + k], &nd->nls[ci + 1], tail * sizeof(u64));
            std::memmove(&nd->kid[ci + 1 + k], &nd->kid[ci + 1], tail * sizeof(Node*));
            for (u32 j = 0; j <= k; ++j) {
                nd->kid[ci + j] = es[j].node;
                nd->bytes[ci + j] = es[j].bytes;
                nd->nls[ci + j] = es[j].nls;
                db += es[j].bytes;
                dn += es[j].nls;
            }
            nd->n = static_cast<u16>(m);
            for (int l = level - 1; l >= 0; --l) {
                path.node[l]->bytes[path.idx[l]] += db;
                path.node[l]->nls[path.idx[l]] += dn;
            }
            return;
        }
        all.clear();
        all.reserve(m);
        for (u32 j = 0; j < ci; ++j) all.push_back({nd->kid[j], nd->bytes[j], nd->nls[j]});
        all.insert(all.end(), es.begin(), es.end());
        for (u32 j = ci + 1; j < nd->n; ++j) all.push_back({nd->kid[j], nd->bytes[j], nd->nls[j]});
        Split sp(all.size(), kInnerCap);
        es.clear();
        size_t a = 0;
        for (u32 j = 0; j < sp.nn; ++j) {
            es.push_back(fill_inner(j == 0 ? nd : inners_.alloc(), &all[a], sp.count(j)));
            a += sp.count(j);
        }
    }
    // The root itself split: grow new levels until one node remains.
    while (es.size() > 1) {
        Split sp(es.size(), kInnerCap);
        all.clear();
        size_t a = 0;
        for (u32 j = 0; j < sp.nn; ++j) {
            all.push_back(fill_inner(inners_.alloc(), &es[a], sp.count(j)));
            a += sp.count(j);
        }
        es.swap(all);
        ++height_;
    }
    root_ = es[0].node;
}

// ---------------------------------------------------------------- erase

void Buffer::erase(u64 off, u64 len, std::vector<Piece>* out) {
    if (off >= bytes_) return;
    len = std::min(len, bytes_ - off);
    while (len > 0) {
        Path path;
        Pos pos = locate(off, false, path);
        Leaf* leaf = pos.leaf;
        u32 i = pos.idx;
        u64 rb = 0, rn = 0;

        if (pos.local > 0) {
            Piece& pc = leaf->ps[i];
            const u32 lnl = count_nl(pc.p, pos.local);
            if (pos.local + len < pc.len) {
                // The range sits inside one piece: keep both ends.
                const u32 cut = static_cast<u32>(len);
                const u32 mnl = count_nl(pc.p + pos.local, cut);
                Piece mid{pc.p + pos.local, cut, mnl};
                Piece right{mid.p + cut, pc.len - pos.local - cut, pc.nl - lnl - mnl};
                if (out) out->push_back(mid);
                pc.len = pos.local;
                pc.nl = lnl;
                bump(path, -static_cast<i64>(mid.len + right.len), -static_cast<i64>(mid.nl + right.nl));
                insert_at({leaf, i, pc.len}, path, {&right, 1});
                return;
            }
            Piece tail{pc.p + pos.local, pc.len - pos.local, pc.nl - lnl};
            if (out) out->push_back(tail);
            rb += tail.len;
            rn += tail.nl;
            len -= tail.len;
            pc.len = pos.local;
            pc.nl = lnl;
            ++i;
        }

        u32 j = i;
        while (j < leaf->n && len >= leaf->ps[j].len) {
            rb += leaf->ps[j].len;
            rn += leaf->ps[j].nl;
            len -= leaf->ps[j].len;
            ++j;
        }
        if (j > i) {
            if (out) out->insert(out->end(), &leaf->ps[i], &leaf->ps[j]);
            std::memmove(&leaf->ps[i], &leaf->ps[j], (leaf->n - j) * sizeof(Piece));
            leaf->n = static_cast<u16>(leaf->n - (j - i));
        }

        if (len > 0 && i < leaf->n) {
            Piece& q = leaf->ps[i];
            const u32 cut = static_cast<u32>(len);
            const u32 hnl = count_nl(q.p, cut);
            if (out) out->push_back({q.p, cut, hnl});
            q.p += cut;
            q.len -= cut;
            q.nl -= hnl;
            rb += cut;
            rn += hnl;
            len = 0;
        }

        bump(path, -static_cast<i64>(rb), -static_cast<i64>(rn));
        if (leaf->n == 0) {
            remove_leaf(path);
        } else {
            if (i > 0 && i < leaf->n) try_merge(leaf, i - 1);
            if (leaf->n < kLeafCap / 4) merge_small(path);
        }
    }
}

// Drops the (now empty) leaf at the end of path, and any inner nodes that
// become empty with it.
void Buffer::remove_leaf(Path& path) {
    if (height_ == 0) return;  // an empty root leaf is the empty document
    Leaf* lf = static_cast<Leaf*>(path.node[height_ - 1]->kid[path.idx[height_ - 1]]);
    unlink(lf);
    leaves_.free(lf);
    for (int level = height_ - 1;; --level) {
        Inner* nd = path.node[level];
        remove_entry(nd, path.idx[level]);
        if (nd->n > 0) break;
        inners_.free(nd);
        if (level == 0) {
            root_ = leaves_.alloc();
            height_ = 0;
            return;
        }
    }
    collapse_root();
}

// Folds an underfull leaf into a neighbour under the same parent.
void Buffer::merge_small(Path& path) {
    if (height_ == 0) return;
    Inner* par = path.node[height_ - 1];
    u32 ci = path.idx[height_ - 1];
    if (ci + 1 >= par->n) {
        if (ci == 0) return;
        --ci;
    }
    Leaf* a = static_cast<Leaf*>(par->kid[ci]);
    Leaf* b = static_cast<Leaf*>(par->kid[ci + 1]);
    if (a->n + b->n > kLeafCap * 3 / 4) return;
    std::memcpy(&a->ps[a->n], b->ps, b->n * sizeof(Piece));
    const u32 seam = a->n - 1u;
    a->n = static_cast<u16>(a->n + b->n);
    try_merge(a, seam);
    par->bytes[ci] += par->bytes[ci + 1];
    par->nls[ci] += par->nls[ci + 1];
    remove_entry(par, ci + 1);
    unlink(b);
    leaves_.free(b);
    collapse_root();
}

void Buffer::collapse_root() {
    while (height_ > 0 && root_->n == 1) {
        Inner* r = static_cast<Inner*>(root_);
        root_ = r->kid[0];
        inners_.free(r);
        --height_;
    }
}

// ---------------------------------------------------------------- verify

void Buffer::verify() const {
    auto die = [](const char* what) {
        std::fprintf(stderr, "buffer invariant broken: %s\n", what);
        std::abort();
    };
    const Leaf* chain = nullptr;
    const Leaf* prev = nullptr;
    auto walk = [&](auto&& self, const Node* nd, int depth, u64& sb, u64& sn) -> void {
        sb = sn = 0;
        if (depth == height_) {
            const Leaf* lf = static_cast<const Leaf*>(nd);
            if (lf->n == 0 && nd != root_) die("empty leaf");
            if (lf->n > kLeafCap) die("leaf overflow");
            if (lf->prev != prev) die("leaf prev link");
            if (prev && prev->next != lf) die("leaf next link");
            if (!chain) chain = lf;
            prev = lf;
            for (u32 i = 0; i < lf->n; ++i) {
                const Piece& p = lf->ps[i];
                if (p.len == 0) die("empty piece");
                if (p.nl != count_nl(p.p, p.len)) die("piece newline count");
                sb += p.len;
                sn += p.nl;
            }
            return;
        }
        const Inner* in = static_cast<const Inner*>(nd);
        if (in->n == 0) die("empty inner");
        if (in->n > kInnerCap) die("inner overflow");
        for (u32 i = 0; i < in->n; ++i) {
            u64 b, n;
            self(self, in->kid[i], depth + 1, b, n);
            if (b != in->bytes[i]) die("inner byte sum");
            if (n != in->nls[i]) die("inner newline sum");
            sb += b;
            sn += n;
        }
    };
    u64 b, n;
    walk(walk, root_, 0, b, n);
    if (prev && prev->next) die("leaf chain tail");
    if (b != bytes_) die("total bytes");
    if (n != nls_) die("total newlines");
    if (height_ > 0 && root_->n < 2) die("root not collapsed");
}

}  // namespace blit
