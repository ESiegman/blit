// Dev containers: the working folder opened inside the container its
// devcontainer.json describes (.devcontainer/devcontainer.json, or
// .devcontainer.json), as a folder on another machine is over ssh: the
// container is made (its image, or one built from its Dockerfile) and
// started with docker, with the folder mounted at its workspaceFolder;
// postCreateCommand is run in it once, when it is made; and then
// "docker://NAME/workspace" is opened, blit's agent copied in and run
// through docker exec (remote.cpp). Made once, the same container is
// started again the next time; "Rebuild the dev container" makes it anew.
//
// Of devcontainer.json: image; build.dockerfile and build.context (or
// dockerFile and context); workspaceFolder; workspaceMount; runArgs;
// containerEnv; postCreateCommand (a string, or a program and its
// arguments). BLIT_DOCKER names another docker (the tests' stand-in).
#include <cctype>
#include <cstdlib>
#include <format>

#include "app.hpp"
#include "applua.hpp"
#include "fsio.hpp"
#include "json.hpp"

namespace blit {

namespace {

std::string docker_program() {
    const char* own = std::getenv("BLIT_DOCKER");
    return own && *own ? own : "docker";
}

// JSON with comments and trailing commas (as devcontainer.json may have) as JSON.
std::string plain_json(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {  // a string, as it is
            const size_t start = i++;
            while (i < s.size() && s[i] != '"') i += s[i] == '\\' ? 2 : 1;
            out.append(s.substr(start, std::min(i, s.size() - 1) - start + 1));
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            out += '\n';
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const size_t end = s.find("*/", i + 2);
            i = end == std::string_view::npos ? s.size() : end + 1;
        } else if (c == ',') {
            size_t j = i + 1;  // (a comma before a closing bracket is left out, comments between them or not)
            for (;;) {
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) ++j;
                if (s.substr(j, 2) == "//") j = std::min(s.find('\n', j), s.size());
                else if (s.substr(j, 2) == "/*") j = std::min(s.find("*/", j + 2), s.size() - 2) + 2;
                else break;
            }
            if (j < s.size() && (s[j] == '}' || s[j] == ']')) continue;
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

std::string folder_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string(".") : s == 0 ? std::string("/") : p.substr(0, s);
}

// A container's name for a folder: its name and a hash of its path.
std::string container_name(const std::string& root) {
    u64 h = 1469598103934665603ull;
    for (const char c : root) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    std::string name;
    for (const char c : base_name(root)) name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '-';
    if (name.empty()) name = "folder";
    return std::format("blit-{}-{:08x}", name, static_cast<u32>(h ^ (h >> 32)));
}

// A --mount's fields, as docker reads them: CSV, where a field in double
// quotes may hold commas, and "" in it is a quote.
std::vector<std::string> mount_fields(std::string_view s) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c != '"') out.back() += c;
            else if (i + 1 < s.size() && s[i + 1] == '"') out.back() += '"', ++i;
            else quoted = false;
        } else if (c == ',') {
            out.emplace_back();
        } else if (c == '"') {
            quoted = true;
        } else {
            out.back() += c;
        }
    }
    return out;
}

// The fields as a --mount (each with a comma or a quote in it in quotes).
std::string mount_of(const std::vector<std::string>& fields) {
    std::string out;
    for (const std::string& f : fields) {
        if (!out.empty()) out += ',';
        if (f.find_first_of(",\"\r\n") == std::string::npos) {
            out += f;
            continue;
        }
        out += '"';
        for (const char c : f) out += c == '"' ? std::string("\"\"") : std::string(1, c);
        out += '"';
    }
    return out;
}

std::string first_line(const std::string& err, const std::string& out) {
    const std::string& s = err.empty() ? out : err;
    std::string line = s.substr(0, s.find('\n'));
    return line.empty() ? std::string("it failed") : line;
}

}  // namespace

// What it needs to be made, from devcontainer.json (false: there is none,
// or it says nothing that can be used; err says why).
bool App::container_plan(const std::string& root, ContainerPlan* plan, std::string* err) {
    std::string file;
    for (const char* at : {"/.devcontainer/devcontainer.json", "/.devcontainer.json"})
        if (const char* k = kind_of(root + at); k && std::string_view(k) == "file") {
            file = root + at;
            break;
        }
    if (file.empty()) return *err = "No devcontainer.json in " + root, false;
    std::string text, why;
    if (!fs_read_whole(file, &text, &why, 1u << 20, {})) return *err = why, false;
    json::Doc doc;
    if (!doc.parse(plain_json(text)) || !doc.root().is_object()) return *err = file + " is not JSON", false;
    const json::Value c = doc.root();
    const std::string here = folder_of(file);
    auto resolve = [&](std::string_view p) { return p.starts_with('/') ? std::string(p) : here + "/" + std::string(p); };
    ContainerPlan& p = *plan;
    p = {};
    p.root = root;
    p.name = container_name(root);
    p.image = std::string(c["image"].str());
    const json::Value build = c["build"];
    std::string dockerfile(build["dockerfile"].str());
    if (dockerfile.empty()) dockerfile = std::string(c["dockerFile"].str());
    if (!dockerfile.empty()) {
        p.dockerfile = resolve(dockerfile);
        std::string context(build["context"].str());
        if (context.empty()) context = std::string(c["context"].str());
        p.context = context.empty() ? folder_of(p.dockerfile) : resolve(context);
        p.image = p.name + "-image";
    }
    if (p.image.empty()) return *err = file + " names no image and no Dockerfile", false;
    p.workspace = std::string(c["workspaceFolder"].str());
    if (p.workspace.empty()) p.workspace = "/workspaces/" + base_name(root);
    // (The paths put in field by field, then quoted as they need: a comma in
    // one would end its field.)
    const std::string_view mount = c["workspaceMount"].str();
    std::vector<std::string> fields = mount.empty() ? std::vector<std::string>{"type=bind", "source=" + root, "target=" + p.workspace} : mount_fields(mount);
    for (std::string& f : fields)
        for (const auto& [from, to] : {std::pair{"${localWorkspaceFolder}", root}, std::pair{"${containerWorkspaceFolder}", p.workspace},
                                       std::pair{"${localWorkspaceFolderBasename}", base_name(root)}})
            for (size_t at = 0; (at = f.find(from, at)) != std::string::npos; at += to.size()) f.replace(at, std::string_view(from).size(), to);
    p.mount = mount_of(fields);
    for (const json::Value a : c["runArgs"])
        if (a.is_string()) p.run_args.emplace_back(a.str());
    for (const json::Value e : c["containerEnv"])
        if (e.is_string()) p.env.push_back(std::string(e.key()) + "=" + std::string(e.str()));
    const json::Value post = c["postCreateCommand"];
    if (post.is_string() && !post.str().empty()) {
        p.post = {"/bin/sh", "-c", std::string(post.str())};
    } else if (post.is_array()) {
        for (const json::Value a : post)
            if (a.is_string()) p.post.emplace_back(a.str());
    }
    return true;
}

// "Open in the dev container": the container started (made first if it
// is not there), then its workspace opened.
void App::container_open(bool anew) {
    const std::string* root = panel_root();
    const std::string folder = root ? *root : default_workdir();
    if (is_remote(folder)) return notify("A dev container is for a folder on this machine");
    auto plan = std::make_shared<ContainerPlan>();
    std::string why;
    if (!container_plan(folder, plan.get(), &why)) return notify(why, 2);
    const std::string docker = docker_program();
    auto opened = [this, plan] {
        notify("In the dev container " + plan->name);
        project_open("docker://" + plan->name + plan->workspace);
    };
    auto make = [this, plan, docker, opened] {
        auto run = [this, plan, docker, opened] {
            std::vector<std::string> argv = {docker, "run", "-d", "--name", plan->name, "--label", "blit.folder=" + plan->root, "--mount", plan->mount, "-w", plan->workspace};
            for (const std::string& e : plan->env) argv.insert(argv.end(), {"-e", e});
            argv.insert(argv.end(), plan->run_args.begin(), plan->run_args.end());
            argv.insert(argv.end(), {plan->image, "/bin/sh", "-c", "trap 'exit 0' TERM; while sleep 1000; do :; done"});
            notify("Starting the dev container\xE2\x80\xA6");
            script_run(std::move(argv), plan->root, [this, plan, docker, opened](int code, const std::string& out, const std::string& err) {
                if (code != 0) return notify("The dev container did not start: " + first_line(err, out), 2);
                if (plan->post.empty()) return opened();
                std::vector<std::string> post = {docker, "exec", "-w", plan->workspace, plan->name};
                post.insert(post.end(), plan->post.begin(), plan->post.end());
                notify("Running its postCreateCommand\xE2\x80\xA6");
                script_run(std::move(post), plan->root, [this, opened](int code2, const std::string& out2, const std::string& err2) {
                    if (code2 != 0) notify("Its postCreateCommand failed: " + first_line(err2, out2), 1);
                    opened();
                });
            });
        };
        if (plan->dockerfile.empty()) return run();
        notify("Building the dev container's image\xE2\x80\xA6");
        script_run({docker, "build", "-t", plan->image, "-f", plan->dockerfile, plan->context}, plan->root, [this, run](int code, const std::string& out, const std::string& err) {
            if (code != 0) return notify("The dev container's image did not build: " + first_line(err, out), 2);
            run();
        });
    };
    // docker asked whether it is running, when what was asked of it failed
    // for another reason than that the container is not there: if it is
    // not, that is said (and not that an image did not build).
    auto unless_down = [this, plan, docker](std::function<void()> then) {
        script_run({docker, "info", "--format", "{{.ServerVersion}}"}, plan->root, [this, then](int code, const std::string&, const std::string& err) {
            if (code == 0) return then();
            if (err.starts_with("cannot run ")) return notify(err, 2);  // (no docker here at all)
            notify("docker is not running", 2, "docker is not running: " + err);
        });
    };
    if (anew) {  // (made anew: the one there goes first)
        script_run({docker, "rm", "-f", plan->name}, plan->root, [make, unless_down](int code, const std::string&, const std::string&) {
            if (code != 0) return unless_down(make);
            make();
        });
        return;
    }
    // Already made: started, if it is not running.
    script_run({docker, "inspect", "-f", "{{.State.Running}}", plan->name}, plan->root,
               [this, plan, docker, make, opened, unless_down](int code, const std::string& out, const std::string& err) {
                   if (code != 0 && err.find("No such") == std::string::npos) return unless_down(make);  // (docker may not be running)
                   if (code != 0) return make();
                   if (out.starts_with("true")) return opened();
                   script_run({docker, "start", plan->name}, plan->root, [this, opened](int code2, const std::string& out2, const std::string& err2) {
                       if (code2 != 0) return notify("The dev container did not start: " + first_line(err2, out2), 2);
                       opened();
                   });
               });
}

}  // namespace blit
