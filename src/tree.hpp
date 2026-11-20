#pragma once
// The file tree shown in the side panel: a folder's contents as nested
// nodes, read from disk on a thread of its own so that a folder with ten
// thousand entries (or a slow disk) never holds up a frame.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common.hpp"

namespace blit {

// How a file or folder is pictured: a glyph from a "Nerd Font" when the
// font has them, else one of the icons blit draws (Font::Icon), and the ink
// to draw it in.
struct FileIcon {
    u32 nerd = 0;   // code point in a Nerd Font
    u32 drawn = 0;  // Font::Icon
    u32 ink = 0;
};
FileIcon file_icon(std::string_view name, bool folder, bool open);

class FileTree {
public:
    struct Node {
        std::string name;
        bool dir = false;
        bool open = false;     // a folder showing its contents
        bool loaded = false;   // its contents have been read at least once
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> kids;
    };
    struct Row {
        Node* node;
        u32 depth;
    };

    FileTree();
    FileTree(const FileTree&) = delete;
    FileTree& operator=(const FileTree&) = delete;
    ~FileTree();

    // Called on the reader's thread when poll() has something.
    void set_notify(std::function<void()> fn);

    // The top folder. Its contents are read in the background and appear
    // when poll() says so.
    void set_root(const std::string& path);
    const std::string& root() const { return root_path_; }
    bool listed() const { return root_ && root_->loaded; }  // (the top folder has been read once)
    std::string path_of(const Node* n) const;

    // The rows on show: everything under an open folder, top to bottom.
    const std::vector<Row>& rows();
    // Opens or closes a folder.
    void toggle(Node* folder);
    // Reads a folder again (every open one, if null): something changed.
    void refresh(Node* folder = nullptr);
    // Reads a folder again, and every open one under it.
    void refresh_under(Node* folder);
    // Takes in folders that have been read. True if the rows changed.
    bool poll();
    // Counts up whenever the rows may have changed.
    u64 generation() const { return generation_; }
    // The node for a path, if that part of the tree has been read. Nodes
    // come and go as folders are read again, so keep paths, not pointers.
    Node* find(const std::string& path) { return node_at(path); }

    // File operations. Each returns false with a reason; on success the
    // folder concerned is read again. On another machine they wait for the
    // network, so they are done on the reader's thread, in turn with the
    // folders it reads: true then says only that the name will do, and what
    // came of it is a Change, taken in by poll() (the tree shows it then).
    bool create(Node* folder, const std::string& name, bool as_folder, std::string* err);
    bool rename(Node* node, const std::string& new_name, std::string* err);
    // Into another folder, under its own name (a folder on this machine).
    bool move(Node* node, Node* folder, std::string* err);
    // Moves to the system's Trash where there is one (macOS, Linux), else
    // deletes (files and empty folders only).
    bool trash(Node* node, std::string* err);
    // True if trash() moves things to a Trash they can be got back from.
    static bool has_trash();

    // What a change made on the reader's thread came to.
    struct Change {
        enum : u8 { kCreated, kRenamed, kTrashed };
        u8 kind = kCreated;
        std::string path, to;  // what was made, renamed (to: its new path) or put in the Trash
        std::string error;     // empty: it was done
    };
    std::vector<Change> take_changes() { return std::exchange(changes_, {}); }
    // Folders being read, or changes being made, on the reader's thread.
    bool busy() const;

private:
    struct Reader;
    struct Op;
    void request(Node* folder);
    void queue(Op op);
    Node* node_at(const std::string& path);

    std::string root_path_;
    std::unique_ptr<Node> root_;
    std::vector<Row> rows_;
    bool rows_stale_ = true;
    u64 generation_ = 0;
    std::vector<Change> changes_;
    std::shared_ptr<Reader> reader_;
};

}  // namespace blit
