#pragma once
// Finding files by name and lines by what is in them, under one folder.
//
// The folder is read once into a list of paths (in the background; files a
// .gitignore leaves out are left out here too, and so are the usual build
// folders). A search for names ranks that list with the fuzzy matcher; a
// search for text reads the files, several at a time, and reports the lines
// that have every word typed in them. Both run on a worker thread and can be
// overtaken: a new search drops the one under way. Nothing here knows about
// windows; whoever uses it is told (set_notify) when there is something new
// to show and reads it with results().
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fuzzy.hpp"

namespace blit {

struct FinderOptions {
    // More to leave out, in .gitignore's own syntax ("*.log", "/dist/").
    std::vector<std::string> ignore;
    bool gitignore = true;           // leave out what .gitignore files say
    bool hidden = true;              // show files and folders whose names start with a dot
    u32 max_files = 500000;          // a folder with more is listed this far
    u64 max_file_bytes = 8u << 20;   // a search for text passes over larger files
};

// The files under a folder: their paths from it, one after another.
struct FileIndex {
    std::string root;
    std::string names;
    std::vector<u32> offs;  // where each path starts in names; one more at the end
    bool truncated = false; // there were more than max_files
    u32 remote_walk = 0;    // a folder on another machine: the list as its agent keeps it (remote.hpp), searched there
    std::string error;      // (one that could not be listed: why, as a search of it says)
    size_t size() const { return offs.empty() ? 0 : offs.size() - 1; }
    std::string_view path(size_t i) const { return std::string_view(names).substr(offs[i], offs[i + 1] - offs[i]); }
};

struct FindHit {
    u32 file = 0;           // which file of the index
    u32 line = 0, col = 0;  // a search for text: the line (from 0) and the byte in it where the first word is
    int score = 0;          // a search for names: how good the match is
    std::string text;       // a search for text: the line, cut if it is very long
    u32 count = 1;          // a search for a phrase or a pattern: how many matches start on the line
};

// How a search for text reads what is typed: words, each anywhere on the
// line (as it comes); the whole of it as it stands (a phrase, for
// replacing); or a regular expression in Vim's syntax (regex.hpp). Small
// letters match either case unless `exact`.
struct TextQuery {
    bool regex = false, phrase = false, exact = false;
    bool operator==(const TextQuery&) const = default;
};

struct FindResults {
    std::shared_ptr<const FileIndex> index;
    std::vector<FindHit> hits;  // names: best first; text: in the order of the files
    u64 matched = 0;            // how many there are in all (hits holds only so many)
    u64 files_hit = 0;          // text: in how many files
    bool text = false;          // these answer a search for text
    bool indexing = false;      // the folder is still being read
    bool searching = false;     // the search is still under way: these are the hits so far
    bool capped = false;        // text: it stopped at kMaxLines; there may be more
    u64 serial = 0;             // of the search they answer
    std::string note;           // why there is nothing (a pattern another machine cannot be asked for, or no connection to it)
};

class Finder {
public:
    static constexpr size_t kMaxNames = 1000;  // hits kept from a search for names
    static constexpr size_t kMaxLines = 3000;  // and from a search for text

    Finder();
    ~Finder();
    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    // Called on the worker thread whenever results() has something new.
    void set_notify(std::function<void()> fn);
    void set_options(FinderOptions o);
    // Files that come first among those a search for names finds as good as
    // each other (the ones open, and those opened lately): paths from the folder.
    void set_preferred(std::vector<std::string> paths);
    // Reads the folder (again). Searches made meanwhile are answered from
    // the list as it was, and once more when the new one is there.
    void index(const std::string& root);
    // Starts a search; the number it returns is in the results that answer it.
    u64 find_names(std::string_view typed);
    u64 find_text(std::string_view typed, TextQuery how = {});
    void cancel();

    std::shared_ptr<const FindResults> results() const;
    std::shared_ptr<const FileIndex> files() const;  // the list as last read (null before the first reading is done)
    bool idle() const;
    void wait_idle() const;  // for tests
    // Waits, but no longer than so many milliseconds. True if it has finished.
    bool wait_idle_for(int ms) const;

    // The pieces, for tests and benchmarks.
    static std::shared_ptr<FileIndex> walk(const std::string& root, const FinderOptions& o, const std::atomic<u64>* epoch = nullptr, u64 mine = 0);
    static bool glob(std::string_view pattern, std::string_view text);
    // `among`, if given: only these files (in the order of the list) are looked at. `matched`, if given, gets every file that matched, in that order.
    // `preferred` (in order): files that come first among equals.
    static void rank_names(const FileIndex& ix, std::span<const FuzzyTerm> terms, FindResults* out, const std::atomic<u64>* epoch = nullptr, u64 mine = 0,
                           const std::vector<u32>* among = nullptr, std::vector<u32>* matched = nullptr, std::span<const u32> preferred = {});
    // Lines of `text` that have every term in them, appended to `out` (at most `room` of them). Returns false for a binary file.
    static bool grep(std::string_view text, std::span<const FuzzyTerm> terms, u32 file, size_t room, std::vector<FindHit>* out);
#if !defined(BLIT_AGENT)
    // The lines of `text` a phrase or a pattern is on (TextQuery), each with
    // how many matches start there; false for a binary file.
    static bool grep(std::string_view text, std::string_view typed, TextQuery how, u32 file, size_t room, std::vector<FindHit>* out);
#endif

private:
    void run(std::stop_token stop);       // the searches
    void run_walk(std::stop_token stop);  // the reading of the folder
    void search_names(u64 serial, const std::string& typed, std::shared_ptr<const FileIndex> ix, u64 epoch);
    void search_text(u64 serial, const std::string& typed, TextQuery how, std::shared_ptr<const FileIndex> ix, u64 epoch);
    void publish(std::shared_ptr<FindResults> r);

    mutable std::mutex mu_;
    mutable std::condition_variable_any wake_, settled_;
    std::function<void()> notify_;
    FinderOptions opt_;
    // What is wanted (under mu_): a folder to read, a search to make.
    std::string want_root_;
    bool want_index_ = false;
    u64 want_serial_ = 0, done_serial_ = 0;
    bool want_text_ = false;
    TextQuery want_how_{};
    std::string want_typed_;
    bool busy_ = false, indexing_ = false;
    bool live_ = false;  // somebody still wants the answer to the last search (it is made again when the folder has been read again)
    std::atomic<u64> index_epoch_{0}, search_epoch_{0};  // a newer request than the one being worked on has come
    // set_preferred's paths (under mu_), and, kept by the worker thread,
    // those files as places in the list they were looked for in.
    std::vector<std::string> pref_;
    u64 pref_gen_ = 0, pref_done_ = ~0ull;
    std::shared_ptr<const FileIndex> pref_ix_;
    std::vector<u32> pref_files_;
    std::shared_ptr<const FileIndex> index_;
    std::shared_ptr<const FindResults> results_;
    // Searches of each kind that were made to the end, kept by the worker
    // thread: what was typed and the files each found. Something typed onto
    // the end of one can only be found in those files (every word of it
    // holds a word typed then), so only they are looked through. Several
    // are kept, each typed onto the one before (the letters as they were
    // typed), so that after Backspace the search is the shorter one's and
    // not one through every file again.
    struct Base {
        std::string typed;
        std::shared_ptr<const FileIndex> index;
        std::vector<u32> files;
        u64 when_ns = 0;  // text: when every file was last looked through
    };
    struct Bases {
        std::vector<Base> kept;  // shortest first
        // The longest kept that `typed` is typed onto, or null.
        const Base* under(std::string_view typed, const std::shared_ptr<const FileIndex>& ix, u64 now_ns, u64 max_age_ns) const;
        // A search made to the end: those it is not typed onto go.
        void add(Base b);
    };
    Bases names_bases_, text_bases_;
    std::jthread worker_, walker_;  // (last: they stop before anything they use is gone)
};

}  // namespace blit
