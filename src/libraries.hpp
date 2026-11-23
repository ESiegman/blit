#pragma once
// The libraries a new project can be made with, and where they are looked
// up: vcpkg's ports, the ROCm packages installed, pkg-config's modules (all
// here, or on the machine the project is made on), and PyPI's packages.
// For CMake, how each is used comes as the lines of a usage note: its
// find_package and its target_link_libraries (with "main" for the target).
#include <string>
#include <vector>

namespace blit {

struct VcpkgPort {
    std::string name, version, description;
};

// vcpkg's folder, for a project made in `where` (another machine's: looked
// for there), as a path of that machine's; "" if it is not to be found.
std::string vcpkg_root(const std::string& where);
// Its ports, by name (descriptions too, when `descriptions`: a file each).
std::vector<VcpkgPort> vcpkg_ports(const std::string& root, bool descriptions);
// How CMake uses a port: the lines of its usage notes that are CMake
// (find_package, target_link_libraries ... with "main" for the target);
// "" if it has none (vcpkg says how when it installs it).
std::string vcpkg_usage(const std::string& root, const std::string& port);
// The commit vcpkg's folder is at (a manifest's builtin-baseline), or "".
std::string vcpkg_baseline(const std::string& root);

// ROCm, where a project made in `where` is to be built: its folder as that
// machine has it, or "" (ROCM_PATH, /opt/rocm, the newest /opt/rocm-*).
std::string rocm_root(const std::string& where);
// The packages it has for CMake (lib/cmake/*), and how one is used (its
// imported targets, read from its targets file).
std::vector<VcpkgPort> rocm_packages(const std::string& root);
std::string rocm_usage(const std::string& root, const std::string& package);
// The GPUs there (rocm_agent_enumerator: gfx942 ..., the CPU's gfx000 left
// out), "a;b", or "" if none are found.
std::string gpu_targets(const std::string& where, const std::string& rocm);

// pkg-config's modules on the machine `where` is on.
std::vector<VcpkgPort> pkgconfig_modules(const std::string& where);

// PyPI's most downloaded packages (15,000 of them, with downloads a month as
// the note), kept in `cache` and fetched again when it is a week old. None
// (none fetched, nor kept from before): `why`, curl's own words or that
// curl is not installed here. (It waits for the fetch: not on the main
// thread.)
std::vector<VcpkgPort> pypi_top(const std::string& cache, std::string* why = nullptr);

}  // namespace blit
