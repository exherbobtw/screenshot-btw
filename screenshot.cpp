/*
 * i like yuri
 * simple wayland ss tool in c++
 */
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

static std::string shot_dir() {
    const char *env = std::getenv("SHOT_DIR");
    if (env && *env) return env;
    const char *home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Pictures/Screenshots";
}

static int run(const std::vector<std::string> &cmd, std::string *out = nullptr, std::string *err = nullptr) {
    int pipe_out[2], pipe_err[2];
    if (pipe(pipe_out) || pipe(pipe_err)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        close(pipe_out[0]); close(pipe_out[1]);
        close(pipe_err[0]); close(pipe_err[1]);
        std::vector<char *> argv;
        for (auto &a : cmd) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_out[1]); close(pipe_err[1]);
    if (out) { char buf[4096]; ssize_t n; while ((n = read(pipe_out[0], buf, sizeof(buf))) > 0) out->append(buf, n); }
    if (err) { char buf[4096]; ssize_t n; while ((n = read(pipe_err[0], buf, sizeof(buf))) > 0) err->append(buf, n); }
    close(pipe_out[0]); close(pipe_err[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool which(const std::string &name) {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (fs::exists(fs::path(dir) / name)) return true;
    }
    return false;
}

static void notify(const std::string &msg) {
    if (which("notify-send")) run({"notify-send", "-a", "shot", msg});
}

static std::string active_window() {
    std::string out;
    if (run({"hyprctl", "-j", "activewindow"}, &out) != 0) return "";
    size_t at_pos = out.find("\"at\"");
    size_t sz_pos = out.find("\"size\"");
    if (at_pos == std::string::npos || sz_pos == std::string::npos) return "";
    auto parse_pair = [&](size_t pos) {
        size_t b = out.find('[', pos);
        size_t e = out.find(']', b);
        if (b == std::string::npos || e == std::string::npos) return std::pair<int,int>{0,0};
        std::string inner = out.substr(b + 1, e - b - 1);
        int x = 0, y = 0;
        sscanf(inner.c_str(), "%d, %d", &x, &y);
        return std::pair<int,int>{x, y};
    };
    auto at = parse_pair(at_pos);
    auto sz = parse_pair(sz_pos);
    return std::to_string(at.first) + "," + std::to_string(at.second) + " " +
           std::to_string(sz.first) + "x" + std::to_string(sz.second);
}

static bool copy_to_clipboard(const fs::path &path) {
    if (!which("wl-copy")) return false;
    std::ifstream f(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    pid_t pid = fork();
    if (pid == 0) {
        int pipefd[2];
        pipe(pipefd);
        if (fork() == 0) {
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            execlp("wl-copy", "wl-copy", "--type", "image/png", nullptr);
            _exit(127);
        }
        close(pipefd[0]);
        write(pipefd[1], data.data(), data.size());
        close(pipefd[1]);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    return true;
}

static std::pair<int,int> image_size(const fs::path &path) {
    std::string out;
    if (run({"magick", "identify", "-format", "%w %h", path.string()}, &out) != 0) return {0, 0};
    int w = 0, h = 0;
    sscanf(out.c_str(), "%d %d", &w, &h);
    return {w, h};
}

static void postprocess(const fs::path &path, int radius, bool decorate, const std::string &label) {
    auto [w, h] = image_size(path);
    std::vector<std::string> ops;
    if (radius > 0) {
        int r = radius;
        ops = {"(", "+clone", "-alpha", "transparent", "-background", "none",
               "-channel", "RGBA", "-fuzz", "2%", "-fill", "none",
               "-draw", "roundrectangle 0,0," + std::to_string(w-1) + "," + std::to_string(h-1) + "," + std::to_string(r) + "," + std::to_string(r),
               ")", "-alpha", "set", "-compose", "DstIn", "-composite"};
    }
    if (decorate) {
        std::string text = label.empty() ? path.stem().string() : label;
        int pts = std::max(14, w / 60);
        ops.insert(ops.end(), {"-gravity", "SouthEast", "-background", "#00000000", "-fill", "white",
                               "-pointsize", std::to_string(pts), "label:" + text, "-geometry", "+24+24", "-compose", "over", "-composite"});
    }
    if (ops.empty()) return;
    fs::path tmp = path;
    tmp.replace_extension(".tmp.png");
    std::vector<std::string> cmd = {"magick", path.string()};
    cmd.insert(cmd.end(), ops.begin(), ops.end());
    cmd.push_back(tmp.string());
    run(cmd);
    fs::rename(tmp, path);
}

int main(int argc, char **argv) {
    bool area = false, window = false, clipboard = false, decorate = false, no_open = false;
    int monitor = -1, radius = 0;
    float delay = 0;
    std::string output, label, filename;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) { std::cerr << name << " requires an argument\n"; exit(1); }
            return argv[++i];
        };
        if (a == "-a" || a == "--area") area = true;
        else if (a == "-w" || a == "--window") window = true;
        else if (a == "-m" || a == "--monitor") monitor = std::stoi(next("--monitor"));
        else if (a == "-d" || a == "--delay") delay = std::stof(next("--delay"));
        else if (a == "-o" || a == "--output") output = next("--output");
        else if (a == "-c" || a == "--clipboard") clipboard = true;
        else if (a == "-r" || a == "--radius") radius = std::stoi(next("--radius"));
        else if (a == "-t" || a == "--decorate") decorate = true;
        else if (a == "--label") label = next("--label");
        else if (a == "-f" || a == "--filename") filename = next("--filename");
        else if (a == "--no-open") no_open = true;
        else { std::cerr << "unknown arg: " << a << "\n"; return 1; }
    }

    std::string geometry;
    if (monitor >= 0) {
        geometry = "";
    } else if (area) {
        std::string out;
        if (run({"slurp"}, &out) != 0) {
            std::cerr << "cancelled\n";
            return 1;
        }
        geometry = out;
        while (!geometry.empty() && (geometry.back() == '\n' || geometry.back() == '\r')) geometry.pop_back();
    } else if (window) {
        geometry = active_window();
    }

    fs::path dir = shot_dir();
    fs::create_directories(dir);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d_%H-%M-%S", localtime(&t));

    fs::path out;
    if (!output.empty()) out = fs::path(output);
    else out = dir / ((filename.empty() ? timebuf : filename) + ".png");
    fs::create_directories(out.parent_path());

    std::vector<std::string> cmd = {"grim"};
    if (monitor >= 0) cmd.push_back("-o"), cmd.push_back(std::to_string(monitor));
    if (!geometry.empty()) cmd.push_back("-g"), cmd.push_back(geometry);
    cmd.push_back(out.string());

    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds((int)(delay * 1000)));

    std::string err;
    if (run(cmd, nullptr, &err) != 0) {
        std::cerr << (err.empty() ? "capture failed" : err) << "\n";
        return 1;
    }

    postprocess(out, radius, decorate, label);

    if (clipboard && !copy_to_clipboard(out))
        notify("wl-copy not found, skipping clipboard");

    if (!no_open && which("xdg-open")) {
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", out.c_str(), nullptr);
            _exit(127);
        }
    }

    std::cout << out.string() << "\n";
    return 0;
}
