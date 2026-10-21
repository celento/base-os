// A C++ program for the BaseOS ELF runtime, built with the cross toolchain:
//   build/toolchain/bin/i686-baseos-g++ -O2 hello.cpp -o hello-cpp
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct Counter {
    static int live;
    Counter() { ++live; }
    ~Counter() { --live; }
};
int Counter::live = 0;

static Counter global_counter;  // runs a static constructor and destructor

int main() {
    std::vector<std::string> words{"kernel", "shell", "apps", "toolchain"};
    std::sort(words.begin(), words.end());
    std::map<std::string, size_t> lengths;
    for (const auto &word : words) lengths[word] = word.size();
    auto owned = std::make_unique<Counter>();
    std::printf("Hello from C++ on BaseOS: %s first, %zu words, %d counters\n",
                words.front().c_str(), lengths.size(), Counter::live);
    try {
        throw std::runtime_error("exceptions work");
    } catch (const std::exception &error) {
        std::printf("%s\n", error.what());
    }
    return Counter::live == 2 ? 0 : 1;
}
