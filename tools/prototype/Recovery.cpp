#include "Recovery.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace pg::editor {
namespace {

constexpr const char* kHeader = "# prototype autosave of ";
constexpr const char* kExample = "# example ";

int processId() {
#ifndef _WIN32
    return static_cast<int>(::getpid());
#else
    return 0;
#endif
}

/// Whether the process `pid` -- an editor that kept an autosave -- runs.
bool running(int pid) {
    if (pid <= 0) return false;
#ifndef _WIN32
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#else
    return pid == processId();
#endif
}

/// A name for a file of: letters, digits, - and _.
std::string tidy(const std::string& s) {
    std::string out;
    for (const char c : s) out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return out.empty() ? std::string("untitled") : out;
}

}  // namespace

std::string defaultRecoveryFolder() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
        return (fs::path(state) / "prototype" / "recovery").string();
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (fs::path(home) / ".local" / "state" / "prototype" / "recovery").string();
    }
    return (fs::temp_directory_path() / "prototype-recovery").string();
}

std::string writeAutosave(const std::string& folder, const std::string& extension, const std::string& of,
                          const std::string& example, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string stem = !of.empty() ? fs::path(of).stem().string() : !example.empty() ? example : "untitled";
    const fs::path path = fs::path(folder) / (tidy(stem) + "-" + std::to_string(processId()) + extension);
    // Where it is saved from wherever the next editor starts.
    const std::string where = of.empty() ? std::string() : fs::absolute(of, ec).lexically_normal().string();
    std::string whole = kHeader + where + "\n";
    if (!example.empty()) whole += kExample + example + "\n";
    whole += text;
    // Whole or not at all, as a saved file: written beside, then renamed.
    const fs::path part = path.string() + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(whole.data(), static_cast<std::streamsize>(whole.size())) || !out.flush()) {
            error = path.string() + ": cannot write it";
            fs::remove(part, ec);
            return {};
        }
    }
    fs::rename(part, path, ec);
    if (ec) {
        error = path.string() + ": cannot write it (" + ec.message() + ")";
        fs::remove(part, ec);
        return {};
    }
    return path.string();
}

std::vector<Kept> leftBehind(const std::string& folder) {
    std::vector<Kept> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return out;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        const fs::path p = e.path();
        const std::string ext = p.extension().string();
        if (ext != ".pgsim" && ext != ".pgsg") continue;
        // scene-4182: whose -- one still running keeps its own.
        const std::string stem = p.stem().string();
        const size_t dash = stem.rfind('-');
        if (dash == std::string::npos) continue;
        const int pid = std::atoi(stem.c_str() + dash + 1);
        if (running(pid)) continue;
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        if (text.rfind(kHeader, 0) != 0) continue;  // not ours
        Kept k;
        k.path = p.string();
        k.extension = ext;
        size_t end = text.find('\n');
        k.of = text.substr(std::char_traits<char>::length(kHeader), end - std::char_traits<char>::length(kHeader));
        size_t next = end == std::string::npos ? text.size() : end + 1;
        if (text.compare(next, std::char_traits<char>::length(kExample), kExample) == 0) {
            end = text.find('\n', next);
            k.example = text.substr(next + std::char_traits<char>::length(kExample),
                                    (end == std::string::npos ? text.size() : end) - next - std::char_traits<char>::length(kExample));
            next = end == std::string::npos ? text.size() : end + 1;
        }
        k.text = text.substr(next);
        std::error_code tec;
        const auto when = fs::last_write_time(p, tec);
        k.secondsAgo = tec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(now - when).count();
        out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const Kept& a, const Kept& b) { return a.secondsAgo < b.secondsAgo; });
    return out;
}

std::string keptName(const Kept& k) {
    if (!k.of.empty()) return fs::path(k.of).filename().string();
    if (!k.example.empty()) return k.example + " (example)";
    return "untitled" + k.extension;
}

}  // namespace pg::editor
