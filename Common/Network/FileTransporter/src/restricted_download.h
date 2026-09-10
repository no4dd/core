/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <vector>

namespace NSNetwork { namespace NSFileTransport { namespace RestrictedDownload {

// Server allowlists contain origins ending in '/', never arbitrary string prefixes.
inline bool parseUrl(const std::string& url, std::string& origin)
{
    if (url.empty() || url.size() > 16384) return false;
    for (unsigned char c : url)
        if (c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '%') return false;
    const size_t scheme = url.compare(0, 7, "http://") == 0 ? 7 :
        (url.compare(0, 8, "https://") == 0 ? 8 : 0);
    if (!scheme) return false;
    const size_t slash = url.find('/', scheme);
    if (slash == std::string::npos) return false;
    const std::string authority = url.substr(scheme, slash - scheme);
    const size_t colon = authority.find(':');
    const std::string host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253) return false;
    size_t begin = 0;
    bool numeric = true;
    unsigned labels = 0;
    while (begin < host.size()) {
        size_t end = host.find('.', begin);
        if (end == std::string::npos) end = host.size();
        const std::string label = host.substr(begin, end - begin);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (char c : label) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            if (c < '0' || c > '9') numeric = false;
        }
        ++labels;
        begin = end + 1;
    }
    if (host.back() == '.') return false;
    // curl also recognizes non-canonical hexadecimal and shortened numeric IPs.
    const std::string last = host.substr(host.find_last_of('.') == std::string::npos ? 0 : host.find_last_of('.') + 1);
    if (!numeric && (std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        (last.size() > 2 && last.compare(0, 2, "0x") == 0))) return false;
    if (numeric) {
        if (labels != 4) return false;
        begin = 0;
        while (begin < host.size()) {
            size_t end = host.find('.', begin);
            if (end == std::string::npos) end = host.size();
            const std::string part = host.substr(begin, end - begin);
            if (part.size() > 3 || (part.size() > 1 && part[0] == '0') || std::stoi(part) > 255) return false;
            begin = end + 1;
        }
    }
    if (colon != std::string::npos) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port[0] == '0') return false;
        for (char c : port) if (c < '0' || c > '9') return false;
        if (std::stoi(port) > 65535) return false;
    }
    const size_t query = url.find('?', slash);
    const std::string path = url.substr(slash, query == std::string::npos ? query : query - slash);
    begin = 1;
    while (begin < path.size()) {
        size_t end = path.find('/', begin);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        begin = end + 1;
    }
    origin = url.substr(0, slash + 1);
    return true;
}

inline bool allowed(const std::string& url, const std::string& allowList)
{
    if (allowList.empty() || allowList.size() > 4096) return false;
    std::string origin;
    if (!parseUrl(url, origin)) return false;
    bool match = false;
    size_t start = 0;
    while (start < allowList.size()) {
        size_t end = allowList.find(';', start);
        if (end == std::string::npos) end = allowList.size();
        const std::string entry = allowList.substr(start, end - start);
        std::string parsed;
        if (!parseUrl(entry, parsed) || parsed != entry) return false;
        match = match || entry == origin;
        start = end + 1;
    }
    return allowList.back() != ';' && match;
}

struct Limits {
    size_t bytes = 256 * 1024 * 1024;
    unsigned milliseconds = 180000;
};

inline int download(const std::string& url, const std::string& output, const std::string& allowList,
    const std::function<bool()>& aborted = {}, const Limits& limits = {})
{
    if (!allowed(url, allowList) || output.empty() || !limits.bytes || !limits.milliseconds) return 1;
    const auto cancelled = [&]() noexcept {
        try { return aborted && aborted(); } catch (...) { return true; }
    };
    if (cancelled()) return 1;
    // The scoped transport has no fallback. Its fixed executable and empty environment
    // cannot select another proxy, config file, protocol or downloader.
    const char* executable = "/usr/bin/curl";
    if (access(executable, X_OK) != 0) return 1;
    int body[2], statusPipe[2];
    if (pipe(body) != 0) return 1;
    if (pipe(statusPipe) != 0) { close(body[0]); close(body[1]); return 1; }
    int file = open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        close(body[0]); close(body[1]); close(statusPipe[0]); close(statusPipe[1]); return 1;
    }
    const std::string seconds = std::to_string((limits.milliseconds + 999) / 1000);
    const char* arguments[] = { executable, "-q", "--globoff", "--silent", "--proxy", "", "--noproxy", "*",
        "--proto", "=http,https", "--proto-redir", "=http,https", "--max-redirs", "0",
        "--connect-timeout", "10", "--max-time", seconds.c_str(), "--speed-limit", "1", "--speed-time", "15",
        "--path-as-is", "--write-out", "%{stderr}%{http_code}", "--url", url.c_str(), nullptr };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.milliseconds);
    const long descriptorLimit = sysconf(_SC_OPEN_MAX);
    if (descriptorLimit < 0) {
        close(body[0]); close(body[1]); close(statusPipe[0]); close(statusPipe[1]);
        close(file); unlink(output.c_str()); return 1;
    }
    const pid_t child = fork();
    if (child == 0) {
        if (dup2(body[1], STDOUT_FILENO) < 0 || dup2(statusPipe[1], STDERR_FILENO) < 0) _exit(126);
        int nullFd = open("/dev/null", O_RDONLY);
        if (nullFd < 0 || dup2(nullFd, STDIN_FILENO) < 0) _exit(126);
        close(nullFd); close(body[0]); close(body[1]); close(statusPipe[0]); close(statusPipe[1]); close(file);
        // Do not leak unrelated service sockets or files into the downloader.
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3U, ~0U, 0) != 0)
#endif
            for (long fd = 3; fd < descriptorLimit; ++fd) close(static_cast<int>(fd));
        char* const environment[] = { nullptr };
        execve(executable, const_cast<char* const*>(arguments), environment);
        _exit(127);
    }
    close(body[1]); close(statusPipe[1]);
    if (child < 0) {
        close(body[0]); close(statusPipe[0]); close(file); unlink(output.c_str()); return 1;
    }
    fcntl(body[0], F_SETFL, O_NONBLOCK);
    fcntl(statusPipe[0], F_SETFL, O_NONBLOCK);
    bool failed = false, bodyOpen = true, statusOpen = true;
    std::string httpStatus;
    size_t total = 0;
    while (bodyOpen || statusOpen) {
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        pollfd descriptors[2] = {{bodyOpen ? body[0] : -1, POLLIN, 0},
                                {statusOpen ? statusPipe[0] : -1, POLLIN, 0}};
        const int ready = poll(descriptors, 2, 50);
        if (ready < 0) { if (errno == EINTR) continue; failed = true; break; }
        for (int i = 0; i < 2 && !failed; ++i) {
            if (!descriptors[i].revents) continue;
            char buffer[16384];
            const ssize_t count = read(descriptors[i].fd, buffer, sizeof(buffer));
            if (count == 0) { (i == 0 ? bodyOpen : statusOpen) = false; continue; }
            if (count < 0) { if (errno != EAGAIN && errno != EINTR) failed = true; continue; }
            if (i == 1) {
                httpStatus.append(buffer, static_cast<size_t>(count));
                if (httpStatus.size() > 3) failed = true;
            } else {
                if (static_cast<size_t>(count) > limits.bytes - total) { failed = true; break; }
                total += static_cast<size_t>(count);
                ssize_t written = 0;
                while (written < count) {
                    const ssize_t n = write(file, buffer + written, static_cast<size_t>(count - written));
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { failed = true; break; }
                    written += n;
                }
            }
        }
    }
    close(body[0]); close(statusPipe[0]);
    int childStatus = 0;
    pid_t reaped = 0;
    while (!failed) {
        reaped = waitpid(child, &childStatus, WNOHANG);
        if (reaped == child) break;
        if (reaped < 0 && errno == EINTR) continue;
        if (reaped < 0) { failed = true; break; }
        if (cancelled() || std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        poll(nullptr, 0, 10);
    }
    if (failed) {
        if (!(reaped < 0 && errno == ECHILD)) {
            kill(child, SIGKILL);
            do { reaped = waitpid(child, &childStatus, 0); } while (reaped < 0 && errno == EINTR);
        }
    }
    if (close(file) != 0) failed = true;
    const bool ok = !failed && reaped == child && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0 &&
        httpStatus.size() == 3 && httpStatus[0] == '2' && httpStatus[1] >= '0' && httpStatus[1] <= '9' &&
        httpStatus[2] >= '0' && httpStatus[2] <= '9';
    if (!ok) unlink(output.c_str());
    return ok ? 0 : 1;
}

}}} // namespace NSNetwork::NSFileTransport::RestrictedDownload
