// Modern C++ on BaseOS: the runtime pieces DOSBox Staging relies on.
//   build/toolchain/bin/i686-baseos-g++ -std=c++20 -O2 modern.cpp -o modern
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int fail(const std::string &what) {
    std::cout << "modern: " << what << " failed" << std::endl;
    return 1;
}

// A queue with a condition variable, as an emulator hands audio between threads.
template <typename T> class Channel {
public:
    void push(T value) {
        { std::lock_guard lock(mutex_); items_.push(std::move(value)); }
        ready_.notify_one();
    }
    T pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !items_.empty(); });
        T value = std::move(items_.front());
        items_.pop();
        return value;
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<T> items_;
};

int main() {
    // Threads, atomics, a condition variable and futures.
    std::atomic<long> total{0};
    std::vector<std::jthread> workers;
    for (int i = 1; i <= 4; ++i)
        workers.emplace_back([&total, i] { for (int k = 0; k < 25000; ++k) total += i; });
    workers.clear();
    if (total != 250000) return fail("threads");
    Channel<int> channel;
    std::thread producer([&channel] { for (int i = 1; i <= 100; ++i) channel.push(i); channel.push(0); });
    int sum = 0;
    for (int value; (value = channel.pop());) sum += value;
    producer.join();
    if (sum != 5050) return fail("condition variable");
    auto answer = std::async(std::launch::async, [] { return 6 * 7; });
    if (answer.get() != 42) return fail("future");

    // Timed waits come back after about the right time.
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(50ms);
    auto slept = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (slept < 45ms || slept > 2000ms) return fail("sleep_for");

    // std::filesystem over the native volume.
    fs::path base = "/modern-test";
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base / "config" / "deep");
    { std::ofstream out(base / "config" / "dosbox.conf"); out << "[cpu]\ncycles=auto\n"; }
    { std::ofstream out(base / "config" / "deep" / "empty.txt"); }
    std::vector<std::string> names;
    for (const auto &entry : fs::recursive_directory_iterator(base))
        names.push_back(entry.path().lexically_relative(base).generic_string());
    std::sort(names.begin(), names.end());
    if (names != std::vector<std::string>{"config", "config/deep", "config/deep/empty.txt", "config/dosbox.conf"})
        return fail("directory listing");
    if (fs::file_size(base / "config" / "dosbox.conf") != 18) return fail("file_size");
    fs::rename(base / "config" / "dosbox.conf", base / "dosbox.conf");
    std::ifstream in(base / "dosbox.conf");
    std::string line, text;
    while (std::getline(in, line)) text += line + ";";
    if (text != "[cpu];cycles=auto;") return fail("ifstream");
    if (!fs::is_directory(base / "config") || fs::exists(base / "missing")) return fail("status");
    if (fs::remove_all(base) != 5 || fs::exists(base)) return fail("remove_all");

    // Formatting, streams, exceptions, smart pointers and randomness.
    std::ostringstream stream;
    stream << std::hex << 48879 << std::dec << ' ' << 3.25;
    if (stream.str() != "beef 3.25") return fail("ostringstream");
    if (std::format("{:>5}|{:04x}|{:.2f}", "ab", 255, 2.5) != "   ab|00ff|2.50") return fail("format");
    try {
        throw std::runtime_error("expected");
    } catch (const std::exception &e) {
        if (std::string(e.what()) != "expected") return fail("exception");
    }
    auto shared = std::make_shared<std::map<std::string, int>>();
    (*shared)["sb16"] = 220;
    std::optional<int> port = shared->count("sb16") ? std::optional((*shared)["sb16"]) : std::nullopt;
    std::mt19937 random(1234);
    std::uniform_int_distribution<int> die(1, 6);
    int rolls = 0;
    for (int i = 0; i < 1000; ++i) rolls += die(random);
    if (!port || *port != 220 || rolls < 3000 || rolls > 4000) return fail("library");

    std::cout << std::format("modern c++ ok: {} threads, sum {}, slept {} ms", 4, sum, slept.count()) << std::endl;
    return 0;
}
