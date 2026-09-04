// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "steamdetector.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace waveline {
namespace {

constexpr const char *kSteamLaunch = "SteamLaunch AppId=";
constexpr const char *kSteamAppsCommon = "steamapps/common/";
constexpr const char *kSteamCgroup = "app-steam-app";

std::string procPath(uint32_t pid, const char *file) {
    return "/proc/" + std::to_string(pid) + "/" + file;
}

bool readFile(const std::string &path, std::string &out) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[16384];
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(sizeof(buf))) {
        const ssize_t n = read(fd, buf + total, sizeof(buf) - total);
        if (n < 0) {
            close(fd);
            return false;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    if (total <= 0) return false;
    out.assign(buf, total);
    return true;
}

bool binaryUnderSteamApps(const std::string &binary) {
    return binary.find(kSteamAppsCommon) != std::string::npos;
}

bool cmdlineIsSteamReaper(const std::string &cmdline) {
    // /proc/pid/cmdline uses NUL between argv entries; reaper is argv[1]="SteamLaunch"
    // argv[2]="AppId=…", not one contiguous "SteamLaunch AppId=" string.
    std::string flat;
    flat.reserve(cmdline.size());
    for (unsigned char c : cmdline) flat += (c == '\0') ? ' ' : static_cast<char>(c);
    return flat.find(kSteamLaunch) != std::string::npos;
}

bool cgroupIsSteamGame(const std::string &cgroup) {
    const auto pos = cgroup.find(kSteamCgroup);
    if (pos == std::string::npos) return false;
    const char *p = cgroup.c_str() + pos + std::strlen(kSteamCgroup);
    return *p && std::isdigit(static_cast<unsigned char>(*p));
}

bool environHasSteamAppId(const std::string &environ) {
    const auto pos = environ.find("SteamAppId=");
    if (pos == std::string::npos) return false;
    const char *p = environ.c_str() + pos + 11;
    if (!*p || !std::isdigit(static_cast<unsigned char>(*p))) return false;
    const long id = std::strtol(p, nullptr, 10);
    return id > 0;
}

uint32_t readParentPid(uint32_t pid) {
    std::string buf;
    if (!readFile(procPath(pid, "status"), buf)) return 0;
    for (size_t pos = 0; pos < buf.size();) {
        const size_t end = buf.find('\n', pos);
        if (buf.compare(pos, 5, "PPid:") == 0) {
            return static_cast<uint32_t>(
                std::strtoul(buf.c_str() + pos + 5, nullptr, 10));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return 0;
}

bool pidLooksLikeSteamGame(uint32_t pid) {
    if (pid == 0) return false;

    std::string buf;
    if (readFile(procPath(pid, "cgroup"), buf) && cgroupIsSteamGame(buf)) return true;

    buf.clear();
    if (readFile(procPath(pid, "environ"), buf) && environHasSteamAppId(buf)) return true;

    buf.clear();
    if (readFile(procPath(pid, "cmdline"), buf) && cmdlineIsSteamReaper(buf)) return true;

    return false;
}

bool isUnderSteamReaper(uint32_t pid) {
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
        if (pidLooksLikeSteamGame(pid)) return true;
        const uint32_t parent = readParentPid(pid);
        if (parent == 0 || parent == pid) return false;
        pid = parent;
    }
    return false;
}

uint32_t steamAppIdFromReaperCmdline(const std::string &cmdline) {
    std::string flat;
    flat.reserve(cmdline.size());
    for (unsigned char c : cmdline) flat += (c == '\0') ? ' ' : static_cast<char>(c);
    const auto pos = flat.find("AppId=");
    if (pos == std::string::npos) return 0;
    const char *p = flat.c_str() + pos + 6;
    if (!*p || !std::isdigit(static_cast<unsigned char>(*p))) return 0;
    const long id = std::strtol(p, nullptr, 10);
    return id > 0 ? static_cast<uint32_t>(id) : 0;
}

uint32_t steamAppIdFromPid(uint32_t pid) {
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
        std::string buf;
        if (readFile(procPath(pid, "cmdline"), buf)) {
            const uint32_t id = steamAppIdFromReaperCmdline(buf);
            if (id != 0) return id;
        }
        const uint32_t parent = readParentPid(pid);
        if (parent == 0 || parent == pid) return 0;
        pid = parent;
    }
    return 0;
}

}  // namespace

bool isSteamGameProcess(uint32_t pid, const std::string &binary) {
    if (!binary.empty() && binaryUnderSteamApps(binary)) return true;
    if (pid == 0) return false;
    return isUnderSteamReaper(pid);
}

uint32_t steamAppIdForProcess(uint32_t pid) {
    if (pid == 0) return 0;
    return steamAppIdFromPid(pid);
}

}  // namespace waveline
