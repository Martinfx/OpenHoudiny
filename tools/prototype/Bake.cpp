#include "Bake.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>

extern char** environ;

namespace pg::editor {

namespace fs = std::filesystem;

namespace {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The last line of a file that says anything.
std::string lastLine(const std::string& path) {
    std::ifstream in(path);
    std::string line, last;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") != std::string::npos) last = line;
    }
    return last;
}

/// This program: the bake runs it.
std::string self() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.string();
}

}  // namespace

Bake::~Bake() {
    // A bake outlives the editor: it is left running.
}

bool Bake::start(const std::string& text, const std::string& networkFolder, const std::string& folder, int frames,
                 int checkpoint, bool resume, std::string& error, const std::string& card) {
    if (running()) {
        error = "A bake is running already";
        return false;
    }
    const std::string exe = self();
    if (exe.empty()) {
        error = "Cannot find this program to run the bake with (/proc/self/exe)";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (!resume) {
        // What a cache there had is not this bake's: its frames, its note,
        // its checkpoint go -- nothing else in the folder.
        for (const fs::directory_entry& e : fs::directory_iterator(folder, ec)) {
            const std::string name = e.path().filename().string();
            static const std::regex ours(R"(frame\.\d+\.pgframe(\.part)?|cache\.txt(\.part)?|checkpoint\.pgstate(\.part)?)");
            if (e.is_regular_file() && std::regex_match(name, ours)) fs::remove(e.path(), ec);
        }
    }
    const std::string network = (fs::path(folder) / "network.pgsim").string();
    if (!sim::writeWhole(network, text, error)) return false;
    const std::string log = (fs::path(folder) / "bake.log").string();

    std::vector<std::string> args = {exe,       "sim",     network, "-", "--cache", folder, "--frames",
                                     std::to_string(frames), "--folder", networkFolder};
    if (checkpoint > 0) {
        args.push_back("--checkpoint");
        args.push_back(std::to_string(checkpoint));
    }
    if (resume) args.push_back("--resume");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | (resume ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&files, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    // A group of its own: Ctrl+C in the editor's terminal does not stop it.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    // The environment as the editor's, PG_GPU naming the card if one was.
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        if (card.empty() || std::strncmp(*e, "PG_GPU=", 7) != 0) env.emplace_back(*e);
    }
    if (!card.empty()) env.push_back("PG_GPU=" + card);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    const int failed = posix_spawn(&pid, exe.c_str(), &files, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    if (failed != 0) {
        error = "Cannot start the bake: " + std::string(std::strerror(failed));
        return false;
    }
    pid_ = pid;
    folder_ = folder;
    frames_ = frames;
    ended_ = failed_ = cancelled_ = false;
    why_.clear();
    progress_ = sim::CacheInfo();
    progress_.of = frames;
    startFrame_ = 0;
    if (resume) {
        sim::CacheInfo before;
        std::string ignored;
        if (sim::readCacheInfo(folder, before, ignored)) startFrame_ = before.checkpoint;
    }
    started_ = now();
    finished_ = 0.0;
    return true;
}

void Bake::cancel() {
    if (!running()) return;
    kill(pid_, SIGTERM);
    cancelled_ = true;
}

void Bake::poll() {
    if (!folder_.empty()) {
        sim::CacheInfo info;
        std::string error;
        if (sim::readCacheInfo(folder_, info, error)) progress_ = info;
    }
    if (!running()) return;
    int status = 0;
    const pid_t done = waitpid(pid_, &status, WNOHANG);
    if (done == 0) return;  // still at it
    pid_ = 0;
    ended_ = true;
    finished_ = now();
    const bool ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    failed_ = !ok || progress_.frames < frames_;
    if (failed_ && !cancelled_) {
        why_ = lastLine((fs::path(folder_) / "bake.log").string());
        if (why_.empty()) why_ = done > 0 && WIFSIGNALED(status) ? "it was stopped by signal " + std::to_string(WTERMSIG(status))
                                                                  : std::string("it stopped");
    }
}

double Bake::seconds() const {
    if (started_ == 0.0) return 0.0;
    return (finished_ > 0.0 ? finished_ : now()) - started_;
}

double Bake::secondsLeft() const {
    const int made = progress_.frames - startFrame_;
    if (made <= 0 || !running()) return 0.0;
    // As fast as it has gone so far -- reading the checkpoint included.
    return seconds() / made * std::max(0, frames_ - progress_.frames);
}

bool Bake::canResume(const std::string& folder, const std::string& text) {
    if (folder.empty()) return false;
    sim::CacheInfo info;
    std::string error;
    std::error_code ec;
    return sim::readCacheInfo(folder, info, error) && !info.done() && info.checkpoint > 0 &&
           fs::exists(sim::checkpointFile(folder), ec) && info.network == sim::networkHash(text);
}

std::string Bake::duration(double seconds) {
    char short_[32];
    if (seconds < 1.0) {
        std::snprintf(short_, sizeof short_, "%.0f ms", std::max(0.0, seconds) * 1000.0);
        return short_;
    }
    if (seconds < 10.0) {
        std::snprintf(short_, sizeof short_, "%.1f s", seconds);
        return short_;
    }
    const long s = std::lround(std::max(0.0, seconds));
    char text[48];
    if (s < 60) std::snprintf(text, sizeof text, "%ld s", s);
    else if (s < 3600) std::snprintf(text, sizeof text, "%ld min %ld s", s / 60, s % 60);
    else std::snprintf(text, sizeof text, "%ld h %ld min", s / 3600, s / 60 % 60);
    return text;
}

}  // namespace pg::editor
