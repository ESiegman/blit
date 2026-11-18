// A program for the debugging tests' containers: a vector, a string and a
// map, as the standard library's pretty printers show them.
#include <map>
#include <string>
#include <vector>
int main() {
    std::vector<int> v{1, 2, 3};
    std::string s = "hello";
    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    return static_cast<int>(v.size() + s.size() + m.size());
}
