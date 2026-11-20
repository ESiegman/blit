#include "project.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace blit {

bool ProjectField::shown(const ProjectValues& v) const {
    if (when_key.empty()) return true;
    const auto it = v.find(when_key);
    return it != v.end() && std::find(when_values.begin(), when_values.end(), it->second) != when_values.end();
}

std::string project_ident(const std::string& name) {
    std::string out;
    for (const char c : name) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
    return out;
}

ProjectValues project_defaults(const std::vector<ProjectField>& fields) {
    ProjectValues v;
    for (const ProjectField& f : fields) v[f.key] = !f.def.empty() || f.kind != ProjectField::kChoice || f.choices.empty() ? f.def : f.choices[0];
    return v;
}

namespace {

using F = ProjectField;

// The fields, made: a choice (the first is the default unless one is
// given), a switch, a line of text, libraries from a lookup. `when`: shown
// only while another field has one of these values.
struct When {
    std::string key;
    std::vector<std::string> values;
};
F choice(std::string key, std::string label, std::vector<std::string> choices, std::string def = {}, When when = {}) {
    F f;
    f.key = std::move(key), f.label = std::move(label), f.kind = F::kChoice, f.choices = std::move(choices);
    f.def = def.empty() ? f.choices[0] : std::move(def);
    f.when_key = std::move(when.key), f.when_values = std::move(when.values);
    return f;
}
F flag(std::string key, std::string label, bool on, When when = {}) {
    F f;
    f.key = std::move(key), f.label = std::move(label), f.kind = F::kSwitch, f.def = on ? "true" : "false";
    f.when_key = std::move(when.key), f.when_values = std::move(when.values);
    return f;
}
F text(std::string key, std::string label, std::string def = {}, When when = {}) {
    F f;
    f.key = std::move(key), f.label = std::move(label), f.kind = F::kText, f.def = std::move(def);
    f.when_key = std::move(when.key), f.when_values = std::move(when.values);
    return f;
}
F libraries(std::string key, std::string label, std::string source, When when = {}) {
    F f;
    f.key = std::move(key), f.label = std::move(label), f.kind = F::kLibraries, f.source = std::move(source);
    f.when_key = std::move(when.key), f.when_values = std::move(when.values);
    return f;
}

struct Get {
    const ProjectValues& v;
    std::string operator[](const std::string& k) const {
        const auto it = v.find(k);
        return it == v.end() ? std::string() : it->second;
    }
    bool on(const std::string& k) const { return (*this)[k] == "true"; }
};

// Words in a list typed as "a b, c".
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (const char c : s + " ") {
        if (c == ' ' || c == ',' || c == '\t' || c == ';' || c == '\n') {
            if (!w.empty()) out.push_back(std::move(w));
            w.clear();
        } else {
            w += c;
        }
    }
    return out;
}

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s + "\n") {
        if (c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// A name for build systems' targets: what it was called, with what they cannot have made _.
std::string target_name(const std::string& name) {
    std::string out;
    for (const char c : name) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' ? c : '_';
    return out.empty() ? "app" : out;
}

// CMake keeps some target names for itself (CMP0037): a project called that
// has its target named otherwise, and its output is what it was called.
std::string cmake_target(const std::string& real) {
    for (const char* reserved : {"all", "clean", "help", "install", "package", "test", "edit_cache", "rebuild_cache", "list_install_components", "package_source", "install/local", "install/strip"})
        if (real == reserved) return real + "_app";
    return real;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// For sh: as it is when it is plain, else in single quotes.
std::string sh_word(const std::string& s) {
    bool plain = !s.empty();
    for (const char c : s) plain &= std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' || c == '_' || c == '-' || c == '=' || c == ':' || c == '+';
    if (plain) return s;
    std::string out = "'";
    for (const char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string joined(const std::vector<std::string>& list, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < list.size(); ++i) out += (i ? sep : "") + list[i];
    return out;
}

std::string clang_format(const std::string& style) {
    if (style.empty() || style == "None") return {};
    return "BasedOnStyle: " + style + "\nColumnLimit: 120\n";
}

// A line of C++ that prints: std::println when the standard has it, else
// std::cout. `fmt` has {} where each of args goes.
std::string say(bool println, const std::string& fmt, const std::vector<std::string>& args = {}) {
    if (println) {
        std::string s = "    std::println(\"" + fmt + "\"";
        for (const std::string& a : args) s += ", " + a;
        return s + ");\n";
    }
    std::string s = "    std::cout << ";
    size_t at = 0, i = 0;
    bool first = true;
    auto put = [&](const std::string& part, bool literal) {
        if (part.empty() && literal) return;
        s += (first ? "" : " << ") + (literal ? "\"" + part + "\"" : part);
        first = false;
    };
    for (size_t brace; (brace = fmt.find("{}", at)) != std::string::npos; at = brace + 2) {
        put(fmt.substr(at, brace - at), true);
        if (i < args.size()) put(args[i++], false);
    }
    put(fmt.substr(at), true);
    return s + " << '\\n';\n";
}

// vcpkg's name for a project: lower case letters and digits, in words joined by -.
std::string vcpkg_name(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "project" : out;
}

const std::vector<std::string> kWarnings = {"Standard", "Strict", "Off"};
const std::vector<std::string> kFormats = {"None", "LLVM", "Google", "Chromium", "Mozilla", "WebKit"};

// ---------------------------------------------------------------- C, C++ and HIP with CMake

enum class Lang { kCxx, kC, kHip };

// The API a new project starts with, declared in include/NAME/NAME.h(pp)
// and defined in src/ (or in the header, for a header-only library).
struct Api {
    std::string header, source, call;
};
Api api_of(Lang lang, const std::string& id, bool header_only) {
    Api a;
    const std::string include = id + "/" + id + (lang == Lang::kC ? ".h" : ".hpp");
    if (lang == Lang::kC) {
        const std::string guard = upper(id) + "_H";
        a.header = "#ifndef " + guard + "\n#define " + guard + "\n\nint " + id + "_add(int a, int b);\n\n#endif\n";
        a.source = "#include \"" + include + "\"\n\nint " + id + "_add(int a, int b) { return a + b; }\n";
        a.call = id + "_add(2, 3)";
    } else {
        a.header = "#pragma once\n\nnamespace " + id + " {\n\n" + (header_only ? "inline int add(int a, int b) { return a + b; }\n" : "int add(int a, int b);\n") +
                   "\n}  // namespace " + id + "\n";
        a.source = "#include \"" + include + "\"\n\nnamespace " + id + " {\n\nint add(int a, int b) { return a + b; }\n\n}  // namespace " + id + "\n";
        a.call = id + "::add(2, 3)";
    }
    return a;
}

// HIP's: what the GPUs are, and y = a * x + y on one; HIP_CHECK for every call.
struct HipFiles {
    std::string check, header, source, main, test;
};
HipFiles hip_files(const std::string& name, const std::string& id) {
    HipFiles h;
    h.check = "#pragma once\n\n#include <cstdio>\n#include <cstdlib>\n\n#include <hip/hip_runtime.h>\n\n"
              "// A HIP call that must not fail: if it does, where and why, and the program ends.\n"
              "#define HIP_CHECK(expr)                                                         \\\n"
              "    do {                                                                        \\\n"
              "        const hipError_t hip_check_error_ = (expr);                             \\\n"
              "        if (hip_check_error_ != hipSuccess) {                                   \\\n"
              "            std::fprintf(stderr, \"%s:%d: %s: %s\\n\", __FILE__, __LINE__, #expr,   \\\n"
              "                         hipGetErrorString(hip_check_error_));                  \\\n"
              "            std::exit(1);                                                       \\\n"
              "        }                                                                       \\\n"
              "    } while (0)\n";
    h.header = "#pragma once\n\n#include <vector>\n\nnamespace " + id +
               " {\n\n// How many GPUs there are (0 if there are none, or no driver).\nint device_count();\n// Each GPU: its name, its architecture, its compute units.\n"
               "void print_devices();\n// y = a * x + y, on the GPU.\nvoid saxpy(float a, const std::vector<float>& x, std::vector<float>& y);\n\n}  // namespace " +
               id + "\n";
    h.source = "#include <cstdio>\n\n#include \"" + id + "/" + id + ".hpp\"\n#include \"" + id + "/hip_check.hpp\"\n\nnamespace " + id +
               " {\n\nnamespace {\n\n__global__ void saxpy_kernel(float a, const float* x, float* y, int n) {\n"
               "    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);\n    if (i < n) y[i] = a * x[i] + y[i];\n}\n\n}  // namespace\n\n"
               "int device_count() {\n    int n = 0;\n    return hipGetDeviceCount(&n) == hipSuccess ? n : 0;\n}\n\n"
               "void print_devices() {\n    for (int d = 0; d < device_count(); ++d) {\n        hipDeviceProp_t p;\n        HIP_CHECK(hipGetDeviceProperties(&p, d));\n"
               "        std::printf(\"GPU %d: %s (%s), %d compute units, wavefront %d\\n\", d, p.name, p.gcnArchName, p.multiProcessorCount, p.warpSize);\n    }\n}\n\n"
               "void saxpy(float a, const std::vector<float>& x, std::vector<float>& y) {\n    const int n = static_cast<int>(x.size());\n"
               "    const size_t bytes = x.size() * sizeof(float);\n    float* dx = nullptr;\n    float* dy = nullptr;\n    HIP_CHECK(hipMalloc(&dx, bytes));\n"
               "    HIP_CHECK(hipMalloc(&dy, bytes));\n    HIP_CHECK(hipMemcpy(dx, x.data(), bytes, hipMemcpyHostToDevice));\n"
               "    HIP_CHECK(hipMemcpy(dy, y.data(), bytes, hipMemcpyHostToDevice));\n    constexpr int block = 256;\n"
               "    saxpy_kernel<<<(n + block - 1) / block, block>>>(a, dx, dy, n);\n    HIP_CHECK(hipGetLastError());\n"
               "    HIP_CHECK(hipMemcpy(y.data(), dy, bytes, hipMemcpyDeviceToHost));\n    HIP_CHECK(hipFree(dx));\n    HIP_CHECK(hipFree(dy));\n}\n\n}  // namespace " +
               id + "\n";
    const std::string check_run = "    const int n = 1 << 20;\n    std::vector<float> x(n, 1.0f), y(n, 2.0f);\n    " + id +
                                  "::saxpy(3.0f, x, y);\n    int wrong = 0;\n    for (const float v : y) wrong += std::fabs(v - 5.0f) > 1e-6f;\n";
    h.main = "#include <cmath>\n#include <cstdio>\n#include <vector>\n\n#include \"" + id + "/" + id + ".hpp\"\n\nint main() {\n    if (" + id +
             "::device_count() == 0) {\n        std::puts(\"No GPU found.\");\n        return 1;\n    }\n    std::puts(\"Hello from " + name + "\");\n    " + id +
             "::print_devices();\n" + check_run + "    std::printf(\"saxpy on %d floats: %s\\n\", n, wrong ? \"wrong\" : \"right\");\n    return wrong ? 1 : 0;\n}\n";
    h.test = "#include <cmath>\n#include <cstdio>\n#include <vector>\n\n#include \"" + id + "/" + id +
             ".hpp\"\n\n// 77: skipped (no GPU here), which CTest reports as such rather than as passing.\nint main() {\n    if (" + id +
             "::device_count() == 0) return 77;\n" + check_run + "    if (wrong) std::printf(\"saxpy: %d of %d wrong\\n\", wrong, n);\n    return wrong ? 1 : 0;\n}\n";
    return h;
}

std::string main_of(Lang lang, bool println, const std::string& name, const std::string& id, const Api& api) {
    const std::string include = id + "/" + id + (lang == Lang::kC ? ".h" : ".hpp");
    if (lang == Lang::kC)
        return "#include <stdio.h>\n\n#include \"" + include + "\"\n\nint main(void) {\n    printf(\"Hello from " + name + "\\n\");\n    printf(\"2 + 3 = %d\\n\", " + api.call +
               ");\n    return 0;\n}\n";
    return std::string(println ? "#include <print>\n" : "#include <iostream>\n") + "\n#include \"" + include + "\"\n\nint main() {\n" + say(println, "Hello from " + name) +
           say(println, "2 + 3 = {}", {api.call}) + "    return 0;\n}\n";
}

std::string tests_of(Lang lang, const std::string& tests, const std::string& id, const Api& api) {
    const std::string include = id + "/" + id + (lang == Lang::kC ? ".h" : ".hpp");
    if (tests == "GoogleTest") return "#include <gtest/gtest.h>\n\n#include \"" + include + "\"\n\nTEST(" + id + ", Adds) { EXPECT_EQ(" + api.call + ", 5); }\n";
    if (tests == "Catch2") return "#include <catch2/catch_test_macros.hpp>\n\n#include \"" + include + "\"\n\nTEST_CASE(\"adds\") { REQUIRE(" + api.call + " == 5); }\n";
    if (tests == "doctest")
        return "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include <doctest/doctest.h>\n\n#include \"" + include + "\"\n\nTEST_CASE(\"adds\") { CHECK(" + api.call + " == 5); }\n";
    const bool c = lang == Lang::kC;
    const std::string q = c ? "" : "std::";
    return std::string(c ? "#include <stdio.h>\n" : "#include <cstdio>\n") + "\n#include \"" + include + "\"\n\nint main(" + (c ? "void" : "") + ") {\n    if (" + api.call +
           " != 5) {\n        " + q + "puts(\"add(2, 3) is not 5\");\n        return 1;\n    }\n    " + q + "puts(\"ok\");\n    return 0;\n}\n";
}

// The flags of each warning level, for GCC and Clang.
std::string warning_flags(Lang lang, const std::string& level) {
    if (level == "Off") return {};
    std::string w = "-Wall -Wextra";
    if (lang != Lang::kHip) w += " -Wpedantic";
    if (level == "Strict") {
        w += " -Wshadow -Wconversion";
        if (lang == Lang::kC) w += " -Wstrict-prototypes -Wmissing-prototypes";
    }
    return w;
}

// A CMake project of C, C++ or HIP: headers in include/, sources in src/, a
// program as a library of what it does and a thin main (so tests have
// something to link); flags on a target of their own, never global;
// libraries from vcpkg (and ROCm's, and pkg-config's) by their usage notes.
ProjectPlan cmake_plan(const ProjectValues& values, Lang lang) {
    const Get v{values};
    ProjectPlan p;
    const std::string name = v["name"], real = target_name(name), t = cmake_target(real), id = project_ident(name);
    const bool hip = lang == Lang::kHip, c_lang = lang == Lang::kC;
    const std::string lang_cmake = c_lang ? "C" : "CXX", ext = c_lang ? "c" : "cpp", hdr = c_lang ? "h" : "hpp";
    const std::string target = v["target"];
    const bool program = target == "Program", header_only = target == "Header-only library";
    std::string tests = v["tests"];
    if (tests == "true") tests = "CTest";
    if (tests == "false") tests = "None";
    const bool testing = !tests.empty() && tests != "None";
    const std::string std_level = v["std"];
    const bool println = lang == Lang::kCxx && (std_level == "23" || std_level == "26");
    const std::vector<std::string> libs = words(v["libraries"]), rocm_libs = words(v["rocm_libraries"]), modules = words(v["pkgconfig"]);
    const bool framework = tests == "GoogleTest" || tests == "Catch2" || tests == "doctest";
    const bool vcpkg = !libs.empty() || framework;
    const std::string lib = program ? t + "_core" : t;  // what the libraries are linked to
    const std::string scope = header_only ? "INTERFACE" : "PUBLIC";
    const std::string sanitizers = v["sanitizers"];
    const std::string san_flags = sanitizers == "Thread"                ? "-fsanitize=thread"
                                  : sanitizers == "Address"             ? "-fsanitize=address -shared-libsan"
                                  : sanitizers.starts_with("Address")   ? "-fsanitize=address,undefined"
                                                                        : "";
    const std::string gen = v["generator"].empty() ? std::string("Ninja") : v["generator"];
    const bool own_gen = gen != "CMake's default";

    // What the libraries' usage notes say to write, for the target they go to.
    std::vector<std::string> finds, uses, unknown;
    auto use = [&](const std::string& usage, const std::string& lib_name, const std::string& as) {
        if (usage.empty()) {
            unknown.push_back(lib_name);
            return;
        }
        for (const std::string& line : lines_of(usage)) {
            if (line.starts_with("find_")) {
                if (std::find(finds.begin(), finds.end(), line) == finds.end()) finds.push_back(line);
                continue;
            }
            std::string u = line;
            if (const size_t at = u.find("(main "); at != std::string::npos) u.replace(at, 6, "(" + lib + " ");
            for (const char* sc : {" PRIVATE ", " PUBLIC ", " INTERFACE "})
                if (const size_t at = u.find(sc); at != std::string::npos) {
                    u.replace(at, std::string_view(sc).size(), " " + as + " ");
                    break;
                }
            uses.push_back(u);
        }
    };
    // (ROCm's are the GPU code's alone: their targets bring HIP's compile flags,
    // which the C++ that uses the project's own header must not get.)
    for (const std::string& l : rocm_libs) use(v["usage:" + l], l, "PRIVATE");
    for (const std::string& l : libs) use(v["usage:" + l], l, scope);
    if (!modules.empty()) {
        finds.push_back("find_package(PkgConfig REQUIRED)");
        for (const std::string& m : modules) {
            const std::string var = upper(project_ident(m));
            finds.push_back("pkg_check_modules(" + var + " REQUIRED IMPORTED_TARGET " + m + ")");
            uses.push_back("target_link_libraries(" + lib + " " + scope + " PkgConfig::" + var + ")");
        }
    }
    if (tests == "GoogleTest") finds.push_back("find_package(GTest CONFIG REQUIRED)");
    if (tests == "Catch2") finds.push_back("find_package(Catch2 3 CONFIG REQUIRED)");
    if (tests == "doctest") finds.push_back("find_package(doctest CONFIG REQUIRED)");

    // CMakeLists.txt
    std::string c = "cmake_minimum_required(VERSION 3.25)\n\n";
    if (vcpkg)
        c += "# The libraries in vcpkg.json come from vcpkg: its toolchain, from VCPKG_ROOT.\nif(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})\n"
             "  set(CMAKE_TOOLCHAIN_FILE \"$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake\" CACHE FILEPATH \"vcpkg's toolchain\")\nendif()\n\n";
    if (hip)
        c += "# ROCm: where ROCM_PATH says, else where it usually is. The GPUs to build for are the\n"
             "# presets' (CMAKE_HIP_ARCHITECTURES), or -DCMAKE_HIP_ARCHITECTURES=gfx942 and the like.\n"
             "if(DEFINED ENV{ROCM_PATH})\n  list(APPEND CMAKE_PREFIX_PATH \"$ENV{ROCM_PATH}\")\nelse()\n  list(APPEND CMAKE_PREFIX_PATH /opt/rocm)\nendif()\n\n";
    c += "project(" + t + " VERSION 0.1.0 LANGUAGES " + (hip ? "CXX HIP" : lang_cmake) + ")\n\n";
    for (const std::string& l : hip ? std::vector<std::string>{"CXX", "HIP"} : std::vector<std::string>{lang_cmake})
        c += "set(CMAKE_" + l + "_STANDARD " + std_level + ")\nset(CMAKE_" + l + "_STANDARD_REQUIRED ON)\nset(CMAKE_" + l + "_EXTENSIONS OFF)\n";
    if (v.on("clangd")) c += "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n";
    c += "\nif(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n  set(CMAKE_BUILD_TYPE " + std::string(hip ? "RelWithDebInfo" : "Debug") +
         " CACHE STRING \"Debug, Release, RelWithDebInfo or MinSizeRel\" FORCE)\nendif()\n";
    if (v.on("lto")) {
        c += "\n# Link-time optimization in Release builds, where the compiler can do it.\ninclude(CheckIPOSupported)\n";
        c += "check_ipo_supported(RESULT " + id + "_ipo OUTPUT " + id + "_ipo_why LANGUAGES " + lang_cmake + ")\n";
        c += "if(" + id + "_ipo)\n  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)\nelse()\n  message(STATUS \"No link-time optimization: ${" + id + "_ipo_why}\")\nendif()\n";
    }
    if (!finds.empty()) c += "\n" + joined(finds, "\n") + "\n";

    // The flags every target of the project's builds with.
    const std::string flags = t + "_flags";
    const bool with_flags = !header_only || testing;
    if (with_flags) {
        c += "\n# What every target here is built with: warnings, sanitizers, tuning.\nadd_library(" + flags + " INTERFACE)\n";
        const std::string langs = hip ? "CXX,HIP" : lang_cmake;
        if (const std::string w = warning_flags(lang, v["warnings"]); !w.empty()) {
            std::string list = w;
            std::replace(list.begin(), list.end(), ' ', ';');
            c += "if(MSVC)\n  target_compile_options(" + flags + " INTERFACE /W4)\nelse()\n  target_compile_options(" + flags + " INTERFACE \"$<$<COMPILE_LANGUAGE:" + langs +
                 ">:" + list + ">\")\nendif()\n";
        }
        if (!san_flags.empty()) {
            std::string list = san_flags + " -fno-omit-frame-pointer";
            std::replace(list.begin(), list.end(), ' ', ';');
            std::string link = san_flags;
            std::replace(link.begin(), link.end(), ' ', ';');
            c += "if(NOT MSVC)  # sanitizers, in Debug builds\n";
            c += "  target_compile_options(" + flags + " INTERFACE \"$<$<CONFIG:Debug>:" + list + ">\")\n";
            c += "  target_link_options(" + flags + " INTERFACE \"$<$<CONFIG:Debug>:" + link + ">\")\nendif()\n";
        }
        if (c_lang && v.on("hardening"))
            c += "if(NOT MSVC)  # hardening, in optimized builds\n  target_compile_definitions(" + flags +
                 " INTERFACE \"$<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>\")\n  target_compile_options(" + flags + " INTERFACE -fstack-protector-strong)\nendif()\n";
        if (v["tuning"].starts_with("This machine")) {
            const std::string check = c_lang ? "check_c_compiler_flag" : "check_cxx_compiler_flag";
            c += "# Release builds for this machine's processor.\ninclude(" + std::string(c_lang ? "CheckCCompilerFlag" : "CheckCXXCompilerFlag") + ")\n";
            c += check + "(-march=native " + id + "_march_native)\n" + check + "(-mcpu=native " + id + "_mcpu_native)\n";
            const std::string when = "$<AND:$<CONFIG:Release,RelWithDebInfo>,$<COMPILE_LANGUAGE:" + langs + ">>";
            c += "if(" + id + "_march_native)\n  target_compile_options(" + flags + " INTERFACE \"$<" + when + ":-march=native>\")\n";
            c += "elseif(" + id + "_mcpu_native)\n  target_compile_options(" + flags + " INTERFACE \"$<" + when + ":-mcpu=native>\")\nendif()\n";
        }
    }

    // The targets.
    const std::string source = hip ? "src/" + id + ".hip" : "src/" + id + "." + ext;
    c += "\n";
    if (header_only) {
        c += "add_library(" + t + " INTERFACE)\ntarget_include_directories(" + t + " INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)\n";
        c += "target_compile_features(" + t + " INTERFACE " + std::string(c_lang ? "c" : "cxx") + "_std_" + std_level + ")\n";
    } else if (program) {
        c += "add_library(" + lib + " STATIC " + source + ")\ntarget_include_directories(" + lib + " PUBLIC include)\n";
        c += "target_link_libraries(" + lib + " PRIVATE " + flags + ")\n";
        c += "\nadd_executable(" + t + " src/main." + ext + ")\ntarget_link_libraries(" + t + " PRIVATE " + flags + " " + lib + ")\n";
    } else {
        c += "add_library(" + t + (target == "Shared library" ? " SHARED " : " STATIC ") + source + ")\ntarget_include_directories(" + t +
             " PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)\n";
        c += "target_link_libraries(" + t + " PRIVATE $<BUILD_INTERFACE:" + flags + ">)\n";
    }
    if (t != real && !header_only) c += "set_target_properties(" + t + " PROPERTIES OUTPUT_NAME " + real + ")  # (" + real + " is a name CMake keeps for itself)\n";
    if (!uses.empty() || !unknown.empty()) {
        c += "\n# The libraries.\n";
        for (const std::string& u : uses) c += u + "\n";
        for (const std::string& l : unknown)
            c += "# " + l + ": vcpkg says what to find_package and link (target_link_libraries(" + lib + " " + scope + " ...)) when it installs it.\n";
    }
    if (!program && v.on("install"))
        c += "\ninclude(GNUInstallDirs)\ninstall(TARGETS " + t + " EXPORT " + t + "Targets)\ninstall(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\ninstall(EXPORT " +
             t + "Targets NAMESPACE " + t + ":: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/" + t + ")\n";

    // Tests.
    if (testing) {
        const std::string tt = t + "_tests";
        const std::string fw = tests == "GoogleTest" ? " GTest::gtest_main" : tests == "Catch2" ? " Catch2::Catch2WithMain" : tests == "doctest" ? " doctest::doctest" : "";
        c += "\nenable_testing()\nadd_executable(" + tt + " tests/test_" + id + "." + ext + ")\n";
        c += "target_link_libraries(" + tt + " PRIVATE " + flags + " " + lib + fw + ")\n";
        if (tests == "GoogleTest") c += "include(GoogleTest)\ngtest_discover_tests(" + tt + ")\n";
        else c += "add_test(NAME " + tt + " COMMAND " + tt + ")\n";
        if (hip)
            c += "set_tests_properties(" + tt + " PROPERTIES LABELS gpu SKIP_RETURN_CODE 77" + (sanitizers == "Address" ? " ENVIRONMENT HSA_XNACK=1" : "") +
                 ")  # (ctest -LE gpu: all but these)\n";
    }
    p.files.push_back({"CMakeLists.txt", c});

    // The sources.
    const Api api = api_of(lang, id, header_only);
    if (hip) {
        const HipFiles h = hip_files(name, id);
        p.files.push_back({"include/" + id + "/" + id + ".hpp", h.header});
        p.files.push_back({"include/" + id + "/hip_check.hpp", h.check});
        p.files.push_back({source, h.source});
        if (program) p.files.push_back({"src/main.cpp", h.main});
        if (testing) p.files.push_back({"tests/test_" + id + ".cpp", h.test});
        p.open = source;
    } else {
        p.files.push_back({"include/" + id + "/" + id + "." + hdr, api.header});
        p.open = "include/" + id + "/" + id + "." + hdr;
        if (!header_only) {
            p.files.push_back({source, api.source});
            p.open = source;
        }
        if (program) {
            p.files.push_back({"src/main." + ext, main_of(lang, println, name, id, api)});
            p.open = "src/main." + ext;
        }
        if (testing) p.files.push_back({"tests/test_" + id + "." + ext, tests_of(lang, tests, id, api)});
    }
    if (vcpkg) {
        std::vector<std::string> all = libs;
        if (tests == "GoogleTest") all.push_back("gtest");
        if (tests == "Catch2") all.push_back("catch2");
        if (tests == "doctest") all.push_back("doctest");
        std::string deps;
        for (size_t i = 0; i < all.size(); ++i) deps += (i ? ",\n    \"" : "\n    \"") + all[i] + "\"";
        const std::string baseline = v["vcpkg_baseline"];
        p.files.push_back({"vcpkg.json", "{\n  \"name\": \"" + vcpkg_name(name) + "\",\n  \"version-string\": \"0.1.0\",\n" +
                                             (baseline.empty() ? std::string() : "  \"builtin-baseline\": \"" + baseline + "\",\n") + "  \"dependencies\": [" + deps +
                                             "\n  ]\n}\n"});
    }

    // Presets: debug (with the sanitizers), release, and ci (warnings as errors).
    const std::string first = hip ? "release" : "debug";
    if (v.on("presets")) {
        std::string arch;
        for (const std::string& a : words(v["gpu"])) arch += (arch.empty() ? "" : ";") + a;
        std::string arch_xnack;  // (ASan on the GPU needs xnack+)
        for (const std::string& a : words(v["gpu"])) arch_xnack += (arch_xnack.empty() ? "" : ";") + a + ":xnack+";
        struct Preset {
            const char* name;
            const char* shown;
            const char* type;
            bool werror;
        };
        Preset presets[] = {{"debug", "Debug", "Debug", false},
                            {"release", hip ? "Release, with debug information" : "Release", hip ? "RelWithDebInfo" : "Release", false},
                            {"ci", "CI (warnings are errors)", "Release", true}};
        if (hip) std::swap(presets[0], presets[1]);  // (the one configured comes first: the Run button builds the first build preset)
        std::string pr = "{\n  \"version\": 6,\n  \"configurePresets\": [\n";
        for (size_t i = 0; i < std::size(presets); ++i) {
            const Preset& s = presets[i];
            std::string cache = "\"CMAKE_BUILD_TYPE\": \"" + std::string(s.type) + "\"";
            if (s.werror) cache += ", \"CMAKE_COMPILE_WARNING_AS_ERROR\": \"ON\"";
            if (hip && !arch.empty())
                cache += ", \"CMAKE_HIP_ARCHITECTURES\": \"" + (std::string(s.name) == "debug" && sanitizers == "Address" ? arch_xnack : arch) + "\"";
            pr += std::string("    {\n      \"name\": \"") + s.name + "\",\n      \"displayName\": \"" + s.shown + "\",\n" +
                  (own_gen ? "      \"generator\": \"" + gen + "\",\n" : "") + "      \"binaryDir\": \"${sourceDir}/build/" + s.name + "\",\n      \"cacheVariables\": {" +
                  cache + "}\n    }" + (i + 1 < std::size(presets) ? ",\n" : "\n");
        }
        pr += "  ],\n  \"buildPresets\": [\n";
        for (size_t i = 0; i < std::size(presets); ++i)
            pr += std::string("    {\"name\": \"") + presets[i].name + "\", \"configurePreset\": \"" + presets[i].name + "\"}" + (i + 1 < std::size(presets) ? ",\n" : "\n");
        pr += "  ]";
        if (testing) {
            pr += ",\n  \"testPresets\": [\n";
            for (size_t i = 0; i < std::size(presets); ++i)
                pr += std::string("    {\"name\": \"") + presets[i].name + "\", \"configurePreset\": \"" + presets[i].name +
                      "\", \"output\": {\"outputOnFailure\": true}}" + (i + 1 < std::size(presets) ? ",\n" : "\n");
            pr += "  ]";
        }
        pr += "\n}\n";
        p.files.push_back({"CMakePresets.json", pr});
        if (v.on("clangd")) p.files.push_back({".clangd", "CompileFlags:\n  CompilationDatabase: build/" + first + "\n"});
    }
    if (const std::string f = clang_format(v["format"]); !f.empty()) p.files.push_back({".clang-format", f});
    if (v.on("tidy"))
        p.files.push_back({".clang-tidy", c_lang ? "Checks: >\n  -*,\n  bugprone-*,\n  cert-*,\n  clang-analyzer-*,\n  misc-*,\n  -misc-include-cleaner,\n  performance-*,\n  portability-*\n"
                                                 : "Checks: >\n  -*,\n  bugprone-*,\n  clang-analyzer-*,\n  cppcoreguidelines-*,\n  -cppcoreguidelines-avoid-magic-numbers,\n"
                                                   "  misc-*,\n  -misc-include-cleaner,\n  modernize-*,\n  -modernize-use-trailing-return-type,\n  performance-*,\n  readability-*,\n"
                                                   "  -readability-magic-numbers,\n  -readability-identifier-length\n"});
    if (hip && v.on("profiling"))
        p.files.push_back({"scripts/profile.sh", "#!/bin/sh\n# The program, profiled with rocprofv3: each kernel's times, into profile/.\nset -e\ncd \"$(dirname \"$0\")/..\"\n"
                                                 "cmake --build --preset release\nrocprofv3 --kernel-trace --stats -d profile -o run -- ./build/release/" +
                                                     real + " \"$@\"\n"});
    if (v.on("git"))
        p.files.push_back({".gitignore", std::string("build/\n.cache/\ncompile_commands.json\nCMakeUserPresets.json\n") + (vcpkg ? "vcpkg_installed/\n" : "") +
                                             (hip && v.on("profiling") ? "profile/\n" : "")});
    const std::string configure = v.on("presets") ? "cmake --preset " + first : "cmake -S . -B build" + std::string(own_gen ? " -G " + sh_word(gen) : "");
    std::vector<std::string> env;
    if (vcpkg && !v["vcpkg_root"].empty()) env.push_back("VCPKG_ROOT=" + sh_word(v["vcpkg_root"]));
    if (hip && !v["rocm_root"].empty()) env.push_back("ROCM_PATH=" + sh_word(v["rocm_root"]));
    if (v.on("readme")) {
        const std::string build = v.on("presets") ? "cmake --build --preset " + first : "cmake --build build";
        const std::string test = testing ? (v.on("presets") ? "\nctest --preset " + first : "\nctest --test-dir build") : "";
        std::string notes;
        if (vcpkg) notes += "Its libraries come from vcpkg: VCPKG_ROOT says where vcpkg is.\n";
        if (hip) notes += "ROCM_PATH says where ROCm is (else /opt/rocm); the GPUs it is built for are the presets' CMAKE_HIP_ARCHITECTURES.\n";
        if (hip && v.on("profiling")) notes += "scripts/profile.sh profiles it with rocprofv3.\n";
        p.files.push_back({"README.md", "# " + name + "\n\n" + notes + (notes.empty() ? "" : "\n") + "```sh\n" + configure + "\n" + build + test + "\n```\n"});
    }
    if (v.on("configure")) p.then = (env.empty() ? std::string() : joined(env, " ") + " ") + configure;
    if (hip && v.on("profiling")) p.then = "chmod +x scripts/profile.sh" + (p.then.empty() ? std::string() : "; " + p.then);
    return p;
}

// ---------------------------------------------------------------- C with Meson or make

ProjectPlan meson_plan(const ProjectValues& values) {
    const Get v{values};
    ProjectPlan p;
    const std::string name = v["name"], t = target_name(name), id = project_ident(name);
    const std::string target = v["target"];
    const bool program = target == "Program", testing = v.on("tests");
    const std::string lib = project_ident(program ? t + "_core" : t);
    const std::vector<std::string> modules = words(v["pkgconfig"]);
    const std::string sanitizers = v["sanitizers"];
    const std::string san = sanitizers == "Thread" ? "thread" : sanitizers == "None" ? "none" : "address,undefined";
    std::string m = "project('" + t + "', 'c',\n  version: '0.1.0',\n  meson_version: '>=1.1',\n  default_options: ['c_std=c" + v["std"] + "', 'warning_level=" +
                    std::string(v["warnings"] == "Off" ? "1" : "3") + "', 'buildtype=debug', 'b_sanitize=" + san + "'])\n\ncc = meson.get_compiler('c')\n";
    if (v["warnings"] == "Strict")
        m += "add_project_arguments(cc.get_supported_arguments(['-Wshadow', '-Wconversion', '-Wstrict-prototypes', '-Wmissing-prototypes']), language: 'c')\n";
    if (v.on("hardening"))
        m += "if get_option('buildtype') != 'debug'\n  add_project_arguments(cc.get_supported_arguments(['-D_FORTIFY_SOURCE=3', '-fstack-protector-strong']), language: 'c')\nendif\n";
    if (v["tuning"].starts_with("This machine"))
        m += "if get_option('buildtype').startswith('release')\n  add_project_arguments(cc.first_supported_argument(['-march=native', '-mcpu=native']), language: 'c')\nendif\n";
    m += "\ninc = include_directories('include')\ndeps = [";
    for (size_t i = 0; i < modules.size(); ++i) m += (i ? ", " : "") + std::string("dependency('") + modules[i] + "')";
    m += "]\n\n";
    const std::string kind = target == "Shared library" ? "shared_library" : "static_library";
    m += lib + " = " + (program ? "static_library" : kind) + "('" + (program ? t + "_core" : t) + "', 'src/" + id + ".c', include_directories: inc, dependencies: deps" +
         (program ? "" : ", install: true") + ")\n";
    m += lib + "_dep = declare_dependency(link_with: " + lib + ", include_directories: inc, dependencies: deps)\n";
    if (program) m += "executable('" + t + "', 'src/main.c', dependencies: " + lib + "_dep, install: true)\n";
    if (!program) m += "install_subdir('include/" + id + "', install_dir: get_option('includedir'))\n";
    if (testing) m += "\ntest('" + id + "', executable('test_" + id + "', 'tests/test_" + id + ".c', dependencies: " + lib + "_dep))\n";
    p.files.push_back({"meson.build", m});
    const Api api = api_of(Lang::kC, id, false);
    p.files.push_back({"include/" + id + "/" + id + ".h", api.header});
    p.files.push_back({"src/" + id + ".c", api.source});
    p.open = "src/" + id + ".c";
    if (program) {
        p.files.push_back({"src/main.c", main_of(Lang::kC, false, name, id, api)});
        p.open = "src/main.c";
    }
    if (testing) p.files.push_back({"tests/test_" + id + ".c", tests_of(Lang::kC, "CTest", id, api)});
    if (v.on("clangd")) p.files.push_back({".clangd", "CompileFlags:\n  CompilationDatabase: build\n"});
    if (const std::string f = clang_format(v["format"]); !f.empty()) p.files.push_back({".clang-format", f});
    if (v.on("tidy"))
        p.files.push_back({".clang-tidy", "Checks: >\n  -*,\n  bugprone-*,\n  cert-*,\n  clang-analyzer-*,\n  misc-*,\n  -misc-include-cleaner,\n  performance-*,\n  portability-*\n"});
    if (v.on("git")) p.files.push_back({".gitignore", "build/\nbuild-*/\n.cache/\n"});
    if (v.on("readme"))
        p.files.push_back({"README.md", "# " + name + "\n\n```sh\nmeson setup build                 # debug, with the sanitizers\nmeson compile -C build" +
                                             std::string(testing ? "\nmeson test -C build" : "") +
                                             "\nmeson setup build-release --buildtype=release -Db_sanitize=none\n```\n"});
    if (v.on("configure")) p.then = "meson setup build";
    return p;
}

ProjectPlan make_plan(const ProjectValues& values) {
    const Get v{values};
    ProjectPlan p;
    const std::string name = v["name"], t = target_name(name), id = project_ident(name);
    const std::string target = v["target"];
    const bool program = target == "Program", shared = target == "Shared library", testing = v.on("tests");
    const std::vector<std::string> modules = words(v["pkgconfig"]);
    const std::string w = warning_flags(Lang::kC, v["warnings"]);
    const std::string san = v["sanitizers"] == "Thread" ? "-fsanitize=thread" : v["sanitizers"] == "None" ? "" : "-fsanitize=address,undefined";
    const bool native = v["tuning"].starts_with("This machine");
    std::string m = "# make: a debug build in build/debug; make BUILD=release: an optimized one in build/release.\nCC ?= cc\nBUILD ?= debug\n\n";
    m += "CFLAGS := -std=c" + v["std"] + (w.empty() ? "" : " " + w) + " -MMD -MP" + (shared ? " -fPIC" : "") + "\nCPPFLAGS := -Iinclude\nLDFLAGS :=\nLDLIBS :=\n";
    if (!modules.empty()) {
        const std::string mods = joined(modules, " ");
        m += "\n# Libraries, by pkg-config.\nCFLAGS += $(shell pkg-config --cflags " + mods + ")\nLDLIBS += $(shell pkg-config --libs " + mods + ")\n";
    }
    m += "\nifeq ($(BUILD),release)\n" +
         std::string(native ? "  # This machine's processor, with whichever of the flags the compiler has.\n"
                              "  NATIVE := $(shell for f in -march=native -mcpu=native; do $(CC) $$f -x c -fsyntax-only /dev/null >/dev/null 2>&1 && echo $$f && break; done)\n"
                            : "") +
         "  CFLAGS += -O2 -DNDEBUG" + std::string(native ? " $(NATIVE)" : "") +
         (v.on("hardening") ? "\n  CPPFLAGS += -D_FORTIFY_SOURCE=3\n  CFLAGS += -fstack-protector-strong" : "") + "\nelse\n  CFLAGS += -O0 -g" +
         (san.empty() ? "" : " " + san + " -fno-omit-frame-pointer") + "\n" + (san.empty() ? "" : "  LDFLAGS += " + san + "\n") + "endif\n";
    m += "\nOUT := build/$(BUILD)\nSRC := $(wildcard src/*.c)\nOBJ := $(SRC:src/%.c=$(OUT)/%.o)\nCORE := $(filter-out $(OUT)/main.o,$(OBJ))\n";
    if (program)
        m += "BIN := $(OUT)/" + t + "\n\nall: $(BIN)\n\n$(BIN): $(OBJ)\n\t$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)\n\nrun: $(BIN)\n\t./$(BIN)\n\n";
    else if (shared)
        m += "LIB := $(OUT)/lib" + t + ".so\n\nall: $(LIB)\n\n$(LIB): $(OBJ)\n\t$(CC) -shared $(LDFLAGS) $^ -o $@ $(LDLIBS)\n\n";
    else
        m += "LIB := $(OUT)/lib" + t + ".a\n\nall: $(LIB)\n\n$(LIB): $(OBJ)\n\tar rcs $@ $^\n\n";
    m += "$(OUT)/%.o: src/%.c | $(OUT)\n\t$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@\n\n$(OUT):\n\tmkdir -p $@\n\n";
    if (testing)
        m += "$(OUT)/test_" + id + ": tests/test_" + id + ".c $(CORE) | $(OUT)\n\t$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)\n\ntest: $(OUT)/test_" + id +
             "\n\t./$(OUT)/test_" + id + "\n\n";
    m += "release:\n\t$(MAKE) BUILD=release\n\nclean:\n\trm -rf build\n\n-include $(OBJ:.o=.d)\n\n.PHONY: all " + std::string(program ? "run " : "") +
         (testing ? "test " : "") + "release clean\n";
    p.files.push_back({"Makefile", m});
    const Api api = api_of(Lang::kC, id, false);
    p.files.push_back({"include/" + id + "/" + id + ".h", api.header});
    p.files.push_back({"src/" + id + ".c", api.source});
    p.open = "src/" + id + ".c";
    if (program) {
        p.files.push_back({"src/main.c", main_of(Lang::kC, false, name, id, api)});
        p.open = "src/main.c";
    }
    if (testing) p.files.push_back({"tests/test_" + id + ".c", tests_of(Lang::kC, "CTest", id, api)});
    if (v.on("clangd")) {
        std::string flags = "-std=c" + v["std"] + "\n-Iinclude\n";
        for (const std::string& f : words(w)) flags += f + "\n";
        p.files.push_back({"compile_flags.txt", flags});
    }
    if (const std::string f = clang_format(v["format"]); !f.empty()) p.files.push_back({".clang-format", f});
    if (v.on("tidy"))
        p.files.push_back({".clang-tidy", "Checks: >\n  -*,\n  bugprone-*,\n  cert-*,\n  clang-analyzer-*,\n  misc-*,\n  -misc-include-cleaner,\n  performance-*,\n  portability-*\n"});
    if (v.on("git")) p.files.push_back({".gitignore", "build/\n.cache/\n"});
    if (v.on("readme"))
        p.files.push_back({"README.md", "# " + name + "\n\n```sh\nmake          # build/debug, with the sanitizers\n" + std::string(testing ? "make test\n" : "") +
                                             "make release  # build/release\n```\n"});
    return p;
}

ProjectPlan c_plan(const ProjectValues& values) {
    const std::string build = Get{values}["build"];
    if (build == "Meson") return meson_plan(values);
    if (build == "Make") return make_plan(values);
    return cmake_plan(values, Lang::kC);
}

// ---------------------------------------------------------------- Python

ProjectPlan python_plan(const ProjectValues& values) {
    const Get v{values};
    ProjectPlan p;
    const std::string name = v["name"], pkg = lower(project_ident(name));
    const bool uv = v["manager"] != "pip";
    const std::string type = v["type"];
    const bool script = type == "Script", library = type == "Library", app = !script && !library;
    const bool src = v["layout"] != "Flat";
    const std::string dir = src ? "src/" + pkg : pkg;
    const std::string backend = script ? "None" : uv ? v["backend_uv"] : v["backend_pip"];
    const bool packaged = backend != "None" && !backend.empty();
    const std::vector<std::string> deps = words(v["deps"]), dev_deps = words(v["dev"]);
    std::string py = v["python"];
    if (py.empty()) py = "3.13";
    std::string pyshort = py;
    pyshort.erase(std::remove(pyshort.begin(), pyshort.end(), '.'), pyshort.end());
    const std::string tests = script ? "None" : v["tests"];
    const std::string types = v["types"];
    const bool ruff = v.on("ruff"), cli = app && v.on("cli") && packaged;
    std::vector<std::string> tools;  // the dev tools the choices need
    if (tests == "pytest") tools.push_back("pytest");
    if (ruff) tools.push_back("ruff");
    if (!types.empty() && types != "None") tools.push_back(types);
    auto quoted_list = [](const std::vector<std::string>& list) {
        std::string out;
        for (size_t i = 0; i < list.size(); ++i) out += (i ? ", \"" : "\"") + list[i] + "\"";
        return out;
    };
    const std::string run_py = uv ? "uv run" : ".venv/bin/python";
    // Under src/ with nothing to install it, the package is found by the path.
    const std::string path_env = src && !packaged ? "PYTHONPATH=src " : "";

    if (script) {
        // One file, with what it needs written in it (PEP 723): `uv run` reads that.
        p.files.push_back({pkg + ".py", "# /// script\n# requires-python = \">=" + py + "\"\n# dependencies = [" + quoted_list(deps) + "]\n# ///\n\"\"\"" + name +
                                            ".\"\"\"\n\n\ndef main() -> None:\n    print(\"Hello from " + name + "\")\n\n\nif __name__ == \"__main__\":\n    main()\n"});
        p.open = pkg + ".py";
        if (!uv && !deps.empty()) p.files.push_back({"requirements.txt", joined(deps, "\n") + "\n"});
    } else {
        std::string t;
        t += "[project]\nname = \"" + name + "\"\nversion = \"0.1.0\"\ndescription = \"\"\n" + std::string(v.on("readme") ? "readme = \"README.md\"\n" : "") +
             "requires-python = \">=" + py + "\"\ndependencies = [" + quoted_list(deps) + "]\n";
        if (cli) t += "\n[project.scripts]\n" + name + " = \"" + pkg + ".__main__:main\"\n";
        if (packaged) {
            if (backend == "uv_build") {
                const std::string ver = v["uv_version"];  // (uv_build's version is uv's)
                std::string bound;
                if (const size_t dot = ver.find('.'); dot != std::string::npos) {
                    const size_t dot2 = ver.find('.', dot + 1);
                    bound = ver.substr(0, dot + 1) + std::to_string(std::atoi(ver.substr(dot + 1, dot2 - dot - 1).c_str()) + 1);
                }
                t += "\n[build-system]\nrequires = [\"uv_build" + (ver.empty() || bound.empty() ? std::string() : ">=" + ver + ",<" + bound) +
                     "\"]\nbuild-backend = \"uv_build\"\n";
                if (!src) t += "\n[tool.uv.build-backend]\nmodule-root = \"\"\n";
            } else if (backend == "hatchling") {
                t += "\n[build-system]\nrequires = [\"hatchling\"]\nbuild-backend = \"hatchling.build\"\n\n[tool.hatch.build.targets.wheel]\npackages = [\"" + dir + "\"]\n";
            } else {
                t += "\n[build-system]\nrequires = [\"setuptools>=77\"]\nbuild-backend = \"setuptools.build_meta\"\n";
            }
        }
        {
            std::vector<std::string> dev = tools;
            dev.insert(dev.end(), dev_deps.begin(), dev_deps.end());
            if (!dev.empty()) t += "\n[dependency-groups]\ndev = [" + quoted_list(dev) + "]\n";
        }
        if (ruff) t += "\n[tool.ruff]\nline-length = 100\ntarget-version = \"py" + pyshort + "\"\n\n[tool.ruff.lint]\nselect = [\"E\", \"F\", \"I\", \"B\", \"UP\"]\n";
        if (tests == "pytest") t += "\n[tool.pytest.ini_options]\ntestpaths = [\"tests\"]\n" + std::string(src && !packaged ? "pythonpath = [\"src\"]\n" : "");
        if (types == "mypy") t += "\n[tool.mypy]\nstrict = true\n";
        if (types == "pyright" || types == "basedpyright") t += "\n[tool." + types + "]\ntypeCheckingMode = \"strict\"\n";
        p.files.push_back({"pyproject.toml", t});
        if (uv) p.files.push_back({".python-version", py + "\n"});
        if (!uv && !packaged) {
            if (!deps.empty()) p.files.push_back({"requirements.txt", joined(deps, "\n") + "\n"});
            std::vector<std::string> dev = tools;
            dev.insert(dev.end(), dev_deps.begin(), dev_deps.end());
            p.files.push_back({"requirements-dev.txt", std::string(deps.empty() ? "" : "-r requirements.txt\n") + joined(dev, "\n") + (dev.empty() ? "" : "\n")});
        }
        if (library) {
            p.files.push_back({dir + "/__init__.py", "\"\"\"" + name + ".\"\"\"\n\n\ndef add(a: int, b: int) -> int:\n    return a + b\n"});
            p.files.push_back({dir + "/py.typed", ""});
            p.open = dir + "/__init__.py";
        } else {
            p.files.push_back({dir + "/__init__.py", "\"\"\"" + name + ".\"\"\"\n"});
            p.files.push_back({dir + "/__main__.py", "def main() -> None:\n    print(\"Hello from " + name + "\")\n\n\nif __name__ == \"__main__\":\n    main()\n"});
            p.open = dir + "/__main__.py";
        }
        if (tests == "pytest") {
            p.files.push_back({"tests/test_" + pkg + ".py",
                               library ? "from " + pkg + " import add\n\n\ndef test_add() -> None:\n    assert add(2, 3) == 5\n"
                                       : "import pytest\n\nfrom " + pkg + ".__main__ import main\n\n\ndef test_main(capsys: pytest.CaptureFixture[str]) -> None:\n    main()\n    assert \"" +
                                             name + "\" in capsys.readouterr().out\n"});
        } else if (tests == "unittest") {
            p.files.push_back({"tests/__init__.py", ""});
            p.files.push_back({"tests/test_" + pkg + ".py",
                               "import unittest\n\n" +
                                   (library ? "from " + pkg + " import add\n\n\nclass TestAdd(unittest.TestCase):\n    def test_add(self) -> None:\n        self.assertEqual(add(2, 3), 5)\n"
                                            : "from " + pkg + ".__main__ import main\n\n\nclass TestMain(unittest.TestCase):\n    def test_main(self) -> None:\n        main()\n") +
                                   "\n\nif __name__ == \"__main__\":\n    unittest.main()\n"});
        }
    }
    if (v.on("git")) p.files.push_back({".gitignore", "__pycache__/\n*.egg-info/\n.venv/\ndist/\nbuild/\n.pytest_cache/\n.ruff_cache/\n.mypy_cache/\n"});
    const std::string test_cmd = tests == "pytest" ? run_py + (uv ? " pytest -q" : " -m pytest -q") : tests == "unittest" ? path_env + run_py + (uv ? " python" : "") + " -m unittest" : "";
    if (v.on("readme")) {
        std::string how;
        if (script) how = uv ? "uv run " + pkg + ".py" : ".venv/bin/python " + pkg + ".py";
        else how = (cli ? (uv ? "uv run " : ".venv/bin/") + name : path_env + run_py + (uv ? " python" : "") + " -m " + pkg) + (test_cmd.empty() ? "" : "\n" + test_cmd);
        p.files.push_back({"README.md", "# " + name + "\n\n```sh\n" + how + "\n```\n"});
    }

    // Once the files are written: the environment, and what goes in it.
    std::vector<std::string> then;
    const bool install = v.on("install");
    if (uv) {
        if (install && !script) {
            then.push_back("uv sync");
            if (!test_cmd.empty()) then.push_back(test_cmd);
        }
    } else {
        const std::string python = "python" + py;
        then.push_back("(command -v " + python + " >/dev/null && " + python + " -m venv .venv || python3 -m venv .venv)");
        if (install) {
            then.push_back(".venv/bin/python -m pip install -q -U pip");
            if (script) {
                if (!deps.empty()) then.push_back(".venv/bin/python -m pip install -r requirements.txt");
            } else if (packaged) {
                then.push_back(".venv/bin/python -m pip install -e ." + std::string(tools.empty() && dev_deps.empty() ? "" : " --group dev"));
            } else {
                then.push_back(".venv/bin/python -m pip install -r requirements-dev.txt");
            }
            if (!test_cmd.empty()) then.push_back(test_cmd);
        }
    }
    p.then = joined(then, " && ");
    return p;
}

const std::vector<std::string> kTests = {"CTest", "Catch2", "GoogleTest", "doctest", "None"};

}  // namespace

const std::vector<ProjectKind>& project_kinds() {
    static const std::vector<ProjectKind> kinds = [] {
        std::vector<ProjectKind> k;
        k.push_back({"C++",
                     "CMake, libraries from vcpkg",
                     {
                         choice("target", "Builds", {"Program", "Static library", "Shared library", "Header-only library"}),
                         choice("std", "C++ standard", {"26", "23", "20", "17"}, "23"),
                         libraries("libraries", "Libraries (vcpkg)", "vcpkg"),
                         choice("tests", "Tests", kTests),
                         choice("warnings", "Warnings", kWarnings),
                         choice("sanitizers", "Sanitizers (debug)", {"Address + undefined", "Thread", "None"}),
                         choice("tuning", "Release tuning", {"Portable", "This machine (-march=native)"}),
                         flag("lto", "Link-time optimization (release)", false),
                         choice("generator", "Generator", {"Ninja", "Unix Makefiles", "CMake's default"}),
                         flag("presets", "CMakePresets.json (debug, release, ci)", true),
                         flag("install", "Install rules", true, {"target", {"Static library", "Shared library", "Header-only library"}}),
                         flag("clangd", "compile_commands.json for clangd", true),
                         flag("tidy", ".clang-tidy", false),
                         choice("format", ".clang-format", kFormats),
                         flag("readme", "README.md", true),
                         flag("configure", "Configure once made", true),
                     },
                     [](const ProjectValues& v) { return cmake_plan(v, Lang::kCxx); }});
        k.push_back({"HIP",
                     "AMD GPUs with ROCm, CMake",
                     {
                         choice("target", "Builds", {"Program", "Static library", "Shared library"}),
                         text("gpu", "GPU targets (gfx942 ...)", "gfx942"),
                         choice("std", "C++ standard", {"17", "20", "23"}),
                         libraries("rocm_libraries", "ROCm libraries", "rocm"),
                         libraries("libraries", "Host libraries (vcpkg)", "vcpkg"),
                         flag("tests", "Tests (CTest, labelled gpu)", true),
                         choice("warnings", "Warnings", kWarnings),
                         choice("sanitizers", "Sanitizers (debug)", {"None", "Address"}),
                         flag("profiling", "rocprofv3 script", false),
                         choice("generator", "Generator", {"Ninja", "Unix Makefiles", "CMake's default"}),
                         flag("presets", "CMakePresets.json (debug, release, ci)", true),
                         flag("clangd", "compile_commands.json for clangd", true),
                         choice("format", ".clang-format", kFormats),
                         flag("readme", "README.md", true),
                         flag("configure", "Configure once made", true),
                     },
                     [](const ProjectValues& v) { return cmake_plan(v, Lang::kHip); }});
        k.push_back({"C",
                     "CMake, Meson or make",
                     {
                         choice("build", "Build system", {"CMake", "Meson", "Make"}),
                         choice("target", "Builds", {"Program", "Static library", "Shared library"}),
                         choice("std", "C standard", {"17", "23", "11", "99"}),
                         libraries("libraries", "Libraries (vcpkg)", "vcpkg", {"build", {"CMake"}}),
                         libraries("pkgconfig", "Libraries (pkg-config)", "pkg-config"),
                         flag("tests", "Tests", true),
                         choice("warnings", "Warnings", kWarnings),
                         choice("sanitizers", "Sanitizers (debug)", {"Address + undefined", "Thread", "None"}),
                         flag("hardening", "Hardening (optimized builds)", true),
                         choice("tuning", "Release tuning", {"Portable", "This machine (-march=native)"}),
                         choice("generator", "Generator", {"Ninja", "Unix Makefiles", "CMake's default"}, {}, {"build", {"CMake"}}),
                         flag("presets", "CMakePresets.json (debug, release, ci)", true, {"build", {"CMake"}}),
                         flag("install", "Install rules", true, {"build", {"CMake"}}),
                         flag("clangd", "Compile commands for clangd", true),
                         flag("tidy", ".clang-tidy", false),
                         choice("format", ".clang-format", kFormats),
                         flag("readme", "README.md", true),
                         flag("configure", "Configure once made", true, {"build", {"CMake", "Meson"}}),
                     },
                     c_plan});
        k.push_back({"Python",
                     "uv or pip, packages from PyPI",
                     {
                         choice("manager", "Manager", {"uv", "pip"}),
                         choice("type", "Type", {"Application", "Library", "Script"}),
                         choice("python", "Python at least", {"3.13", "3.14", "3.12", "3.11", "3.10"}),
                         choice("layout", "Layout", {"src/", "Flat"}, {}, {"type", {"Application", "Library"}}),
                         choice("backend_uv", "Build backend", {"uv_build", "hatchling", "setuptools", "None"}, {}, {"manager", {"uv"}}),
                         choice("backend_pip", "Build backend", {"hatchling", "setuptools", "None"}, {}, {"manager", {"pip"}}),
                         libraries("deps", "Dependencies (PyPI)", "pypi"),
                         libraries("dev", "Dev dependencies (PyPI)", "pypi", {"type", {"Application", "Library"}}),
                         flag("cli", "A command (and python -m)", true, {"type", {"Application"}}),
                         choice("tests", "Tests", {"pytest", "unittest", "None"}, {}, {"type", {"Application", "Library"}}),
                         flag("ruff", "Ruff (lint and format)", true),
                         choice("types", "Type checker", {"mypy", "pyright", "basedpyright", "ty", "None"}),
                         flag("readme", "README.md", true),
                         flag("install", "Install once made (network)", true),
                     },
                     python_plan});
        return k;
    }();
    return kinds;
}

}  // namespace blit
