#pragma once
// Text storage: a piece tree.
//
// The document is a sequence of pieces. A piece is a pointer + length into
// memory that never changes: either the original file (memory mapped, never
// copied) or an append-only block of typed/pasted text. Pieces live in the
// leaves of a B+tree whose inner nodes carry byte and newline counts, so
// offset lookup, line lookup, insert and erase are all O(log n).
//
// Pieces are kept short (about 4 KB), which bounds the scan needed to find a
// line inside one piece.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct Piece {
    const char* p;
    u32 len;
    u32 nl;
};

namespace detail {
inline constexpr int kLeafCap = 64;
inline constexpr int kInnerCap = 32;
inline constexpr int kMaxHeight = 16;

struct Node {
    u16 n = 0;
};
struct Leaf : Node {
    Leaf* prev = nullptr;
    Leaf* next = nullptr;
    Piece ps[kLeafCap];
};
struct Inner : Node {
    u64 bytes[kInnerCap];
    u64 nls[kInnerCap];
    Node* kid[kInnerCap];
};
struct Path {
    Inner* node[kMaxHeight];
    u16 idx[kMaxHeight];
};
struct Pos {
    Leaf* leaf;
    u32 idx;
    u32 local;
};
struct Entry {
    Node* node;
    u64 bytes;
    u64 nls;
};

// Fixed-size block allocator: nodes come from big slabs and are recycled
// through a free list, so node churn never touches malloc.
template <class T>
class Pool {
public:
    Pool() = default;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    ~Pool() { release(); }
    T* alloc();
    void free(T* t) {
        *reinterpret_cast<void**>(t) = free_;
        free_ = t;
    }
    void reserve(size_t count);
    void release();
    size_t bytes() const {
        size_t b = 0;
        for (const Slab& s : slabs_) b += s.count * sizeof(T);
        return b;
    }

private:
    struct Slab {
        void* mem;
        size_t count;
    };
    std::vector<Slab> slabs_;
    T* bump_ = nullptr;
    T* bump_end_ = nullptr;
    void* free_ = nullptr;
    size_t next_slab_ = 32;
};
}  // namespace detail

class Buffer {
public:
#ifndef BLIT_MAX_PIECE
#define BLIT_MAX_PIECE 4096
#endif
    static constexpr u32 kMaxPiece = BLIT_MAX_PIECE;

    // Sequential read cursor. chunk() is the contiguous run of bytes at the
    // cursor and is empty only at end of text.
    class Reader {
    public:
        std::string_view chunk() const {
            if (!leaf_ || idx_ >= leaf_->n) return {};
            const Piece& pc = leaf_->ps[idx_];
            return {pc.p + local_, pc.len - local_};
        }
        // Moves forward by k bytes, k <= chunk().size().
        void advance(u32 k) {
            local_ += k;
            if (local_ >= leaf_->ps[idx_].len) next_piece();
        }
        bool at_end() const { return !leaf_ || idx_ >= leaf_->n; }

    private:
        friend class Buffer;
        void next_piece() {
            local_ = 0;
            if (++idx_ >= leaf_->n && leaf_->next) {
                leaf_ = leaf_->next;
                idx_ = 0;
            }
        }
        const detail::Leaf* leaf_ = nullptr;
        u32 idx_ = 0;
        u32 local_ = 0;
    };

    Buffer();
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Replaces the contents with a view of [data, data+n). The memory must
    // stay valid and unchanged for the life of the buffer.
    void load_view(const char* data, u64 n);
    // Replaces the contents with the file at path (memory mapped).
    bool load_file(const char* path, std::string* err);
    // Replaces the contents with these bytes (a file read from another machine), kept by the buffer.
    void load_bytes(std::string_view bytes);
    // Writes the contents to path through a temp file and an atomic rename.
    bool save_file(const char* path, std::string* err) const;

    u64 size() const { return bytes_; }
    u64 line_count() const { return nls_ + 1; }
    size_t piece_count() const;
    // Memory used by the tree itself (not the text it points at).
    size_t index_bytes() const { return leaves_.bytes() + inners_.bytes(); }

    void insert(u64 off, std::string_view text);
    // Removes [off, off+len). The removed pieces are appended to *removed.
    void erase(u64 off, u64 len, std::vector<Piece>* removed = nullptr);
    void insert_pieces(u64 off, std::span<const Piece> ps);

    // Offset of the first byte of a line. line >= line_count() gives size().
    u64 line_start(u64 line) const;
    // Line containing the byte at off (off == size() gives the last line).
    u64 line_of(u64 off) const;

    Reader reader(u64 off) const;
    Reader line_reader(u64 line, u64* off_out = nullptr) const;
    // Copies up to n bytes starting at off into dst; returns the count.
    u64 read(u64 off, char* dst, u64 n) const;
    std::string text(u64 off, u64 n) const;

    // The document as a flat list of pieces. The bytes they point at never
    // change, so another thread can read this copy while editing goes on.
    void snapshot(std::vector<Piece>* out) const;

    // Checks every structural invariant; aborts with a message on failure.
    void verify() const;

private:
    using Node = detail::Node;
    using Leaf = detail::Leaf;
    using Inner = detail::Inner;
    using Path = detail::Path;
    using Pos = detail::Pos;
    using Entry = detail::Entry;

    Pos locate(u64 off, bool sticky_left, Path& path) const;
    void bump(const Path& path, i64 dbytes, i64 dnls);
    void insert_at(Pos pos, Path& path, std::span<const Piece> ps);
    void spread_up(Path& path, int level, std::vector<Entry>& es);
    void remove_leaf(Path& path);
    void merge_small(Path& path);
    void collapse_root();
    void append_text(std::string_view s, std::vector<Piece>& out);
    void clear();
    void drop_map();
    void free_tree(Node* nd, int depth);

    Node* root_ = nullptr;
    int height_ = 0;  // number of inner levels above the leaves
    u64 bytes_ = 0;
    u64 nls_ = 0;

    detail::Pool<Leaf> leaves_;
    detail::Pool<Inner> inners_;

    // Append-only storage for inserted text.
    std::vector<char*> blocks_;
    char* add_tail_ = nullptr;
    size_t add_room_ = 0;

    // Original file mapping.
    void* map_ = nullptr;
    size_t map_len_ = 0;
    bool map_is_mmap_ = false;

    mutable std::vector<Piece> scratch_;
};

}  // namespace blit
