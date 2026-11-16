#pragma once
// New projects: a kind (a CMake project, a Python package ...), the choices
// it offers (the language standard, what is built, the libraries it uses,
// tests, presets ...), and from them the files it is made of and what is
// run in it once they are written (git init, configuring). The start
// screen's New project tab is the form (appstart.cpp); init.lua may add
// kinds of its own with blit.project.add.
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace blit {

using ProjectValues = std::map<std::string, std::string>;  // a field's key -> its value ("true"/"false" for a switch)

struct ProjectField {
    // kLibraries: names (a space between), looked up in `source`: "vcpkg",
    // "rocm" (the packages installed), "pkg-config" or "pypi".
    enum Kind { kText, kSwitch, kChoice, kLibraries };
    std::string key, label;
    Kind kind = kText;
    std::vector<std::string> choices;
    std::string def;
    std::string source;
    // Shown only when another field has one of these values.
    std::string when_key;
    std::vector<std::string> when_values;
    bool shown(const ProjectValues& v) const;
};

struct ProjectFile {
    std::string path;  // relative to the project's folder
    std::string text;
};

struct ProjectPlan {
    std::vector<ProjectFile> files;
    std::string open;  // the file to open once it is made
    std::string then;  // run by sh in the new folder once the files are written ("" for nothing)
    // What the form leaves to the machine, filled in before plan is called:
    // "vcpkg_root", "vcpkg_baseline", "rocm_root", "uv_version", and for each
    // library chosen from vcpkg or ROCm, "usage:NAME" (libraries.hpp).
};

struct ProjectKind {
    std::string name, description;
    std::vector<ProjectField> fields;
    std::function<ProjectPlan(const ProjectValues&)> plan;  // values has "name" too
    bool scripted = false;  // init.lua's (blit.project.add): its create function makes it, not plan
};

const std::vector<ProjectKind>& project_kinds();
// A name fit for code (an identifier): letters, digits and _.
std::string project_ident(const std::string& name);
// The values a kind starts with.
ProjectValues project_defaults(const std::vector<ProjectField>& fields);

}  // namespace blit
