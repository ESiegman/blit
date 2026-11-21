#pragma once
// Getting tree-sitter grammars. blit ships none: it fetches a grammar's
// source from its own repository at a fixed revision, compiles it with the
// system's C compiler, and keeps the result, with the grammar's highlight
// query, in its own data folder:
//
//   ~/.local/share/blit/parsers/cpp.so
//   ~/.local/share/blit/queries/cpp/highlights.scm
//
// Nothing here runs unless asked for: by `blit --install-grammar`, or by the
// settings ([syntax] install / auto_install). Fetching and compiling take
// seconds, so inside the editor they happen on a thread of their own, one
// grammar at a time.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.hpp"

namespace blit {

// The grammars blit knows where to get.
const std::vector<GrammarSource>& builtin_grammars();
// Those from the settings file (each filled in from the built-in grammar of
// the same name, if there is one), then the remaining built-in ones.
std::vector<GrammarSource> grammar_list(const std::vector<GrammarSource>& from_settings);
const GrammarSource* find_grammar(const std::vector<GrammarSource>& list, std::string_view name);
// The grammar a language's files are parsed with: its own, or the one its
// `parser` names (through an alias of an alias). Installing a language
// installs that one.
std::string grammar_parser(const std::vector<GrammarSource>& list, std::string_view name);
// Every alias in the list with the grammar it uses: {"metal", "cpp"}.
std::vector<std::pair<std::string, std::string>> grammar_parsers(const std::vector<GrammarSource>& list);

// The folder grammars are installed into ($XDG_DATA_HOME/blit, usually
// ~/.local/share/blit); parsers/ and queries/ are below it.
std::string grammar_dir();

enum class GrammarState { kMissing, kOutdated, kInstalled };
// Outdated: installed, but from another revision than the list asks for.
GrammarState grammar_state(const GrammarSource& g);

// Fetches, builds and installs a grammar, and first any grammar whose
// queries it inherits. Blocks for as long as that takes (seconds; a big
// grammar's compile can take half a minute). `say` hears about each step.
bool install_grammar(const std::vector<GrammarSource>& list, const std::string& name, std::string* err,
                     const std::function<void(const std::string&)>& say = {});

// For the command line. Installs the named grammars ("all": every one in
// the list), saying what it does on stdout; returns the exit code.
int install_grammars_command(const std::vector<GrammarSource>& list, std::vector<std::string> names);
// Prints the list, with what is installed.
void print_grammars(const std::vector<GrammarSource>& list);

// Installs grammars in the background, one at a time.
class GrammarInstaller {
public:
    struct Done {
        std::string name;
        bool ok = false;
        std::string message;  // why not, when it failed
    };

    GrammarInstaller();
    GrammarInstaller(const GrammarInstaller&) = delete;
    GrammarInstaller& operator=(const GrammarInstaller&) = delete;
    ~GrammarInstaller();

    // Called on the installer's thread when poll() has something.
    void set_notify(std::function<void()> fn);
    // Queues a grammar. Asking again for one that is queued, under way, or
    // has failed since the program started does nothing.
    void request(const std::vector<GrammarSource>& list, const std::string& name);
    // Forgets failures, so that grammars can be asked for again.
    void forgive();
    std::vector<Done> poll();
    // The grammar being installed right now, or "".
    std::string current() const;

private:
    struct State;
    std::shared_ptr<State> s_;
};

}  // namespace blit
