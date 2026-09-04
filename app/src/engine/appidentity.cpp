// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "appidentity.h"

#include "pwengine.h"
#include "steamdetector.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <limits.h>
#include <vector>

namespace waveline {
namespace {

constexpr const char *kSteamAppsCommon = "steamapps/common/";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isGenericName(const std::string &name) {
    static const char *kGeneric[] = {
        "java", "sdl application", "wine", "wine64-preloader", "wine-preloader",
        "python", "python3", "dotnet", "mono", "bash", "/bin/bash", "sh",
        "electron", "steam", "steam.exe", "gamelauncher", "reaper",
        "unrealgame", "unity", "unityplayer", "godot", "godotengine",
        "webrtc voiceengine",
        nullptr,
    };
    const std::string hay = lower(name);
    for (const char **g = kGeneric; *g; ++g) {
        if (hay == *g) return true;
    }
    return false;
}

std::string fileBaseName(std::string path) {
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) path.erase(0, slash + 1);
    return path;
}

std::string prettifyBinaryName(std::string base) {
    for (const char *suffix :
         {".bin.x86_64", ".x86_64", ".i386", ".i686", ".exe", ".bin", ".x64"}) {
        const size_t len = std::strlen(suffix);
        if (base.size() > len &&
            lower(base.substr(base.size() - len)) == lower(std::string(suffix))) {
            base.resize(base.size() - len);
            break;
        }
    }
    if (!base.empty()) base[0] = static_cast<char>(std::toupper(base[0]));
    return base;
}

std::string steamFolderFromBinary(const std::string &binary) {
    const auto pos = binary.find(kSteamAppsCommon);
    if (pos == std::string::npos) return {};
    const char *start = binary.c_str() + pos + std::strlen(kSteamAppsCommon);
    const char *end = std::strchr(start, '/');
    if (!end || end == start) return {};
    return std::string(start, end - start);
}

bool readCmdline(uint32_t pid, std::string &out) {
    if (pid == 0) return false;
    const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
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

std::string flatCmdline(const std::string &cmdline) {
    std::string flat;
    flat.reserve(cmdline.size());
    for (unsigned char c : cmdline) flat += (c == '\0') ? ' ' : static_cast<char>(c);
    return flat;
}

std::string minecraftLabel(uint32_t pid) {
    std::string cmdline;
    if (!readCmdline(pid, cmdline)) return {};
    const std::string flat = flatCmdline(cmdline);

    const auto jar = flat.find("minecraft-");
    if (jar != std::string::npos) {
        const auto end = flat.find("-client.jar", jar);
        if (end != std::string::npos && end > jar + 10) {
            return "Minecraft " + flat.substr(jar + 10, end - jar - 10);
        }
    }

    const auto inst = flat.find("/instances/");
    if (inst != std::string::npos) {
        const char *start = flat.c_str() + inst + 11;
        const char *end = std::strchr(start, '/');
        if (end && end > start) {
            std::string name(start, end - start);
            if (!name.empty()) return "Minecraft (" + name + ")";
        }
    }

    if (flat.find("PrismLauncher") != std::string::npos ||
        flat.find("org.prismlauncher") != std::string::npos)
        return "Minecraft";

    return {};
}

std::string minecraftIdentity(uint32_t pid) {
    std::string cmdline;
    if (!readCmdline(pid, cmdline)) return {};
    const std::string flat = flatCmdline(cmdline);

    const auto jar = flat.find("minecraft-");
    if (jar != std::string::npos) {
        const auto end = flat.find("-client.jar", jar);
        if (end != std::string::npos && end > jar + 10)
            return "minecraft:" + flat.substr(jar + 10, end - jar - 10);
    }

    const auto inst = flat.find("/instances/");
    if (inst != std::string::npos) {
        const char *start = flat.c_str() + inst + 11;
        const char *end = std::strchr(start, '/');
        if (end && end > start) return "minecraft:" + std::string(start, end - start);
    }

    if (flat.find("minecraft") != std::string::npos ||
        flat.find("PrismLauncher") != std::string::npos)
        return "minecraft:launcher";

    return {};
}

std::string labelFromBinary(const std::string &binary) {
    if (binary.empty()) return {};
    const std::string folder = steamFolderFromBinary(binary);
    if (!folder.empty()) return folder;
    return prettifyBinaryName(fileBaseName(binary));
}

std::string labelFromPidExe(uint32_t pid) {
    if (pid == 0) return {};
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    char buf[PATH_MAX];
    const ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return labelFromBinary(buf);
}

std::string steamRootPath() {
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/share/Steam";
    return {};
}

bool readFileString(const std::string &path, std::string &out) {
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

// Read a quoted value from a Steam .acf manifest (appmanifest_*.acf).
std::string acfField(const std::string &acf, const char *key) {
    const std::string needle = std::string("\"") + key + "\"";
    const auto pos = acf.find(needle);
    if (pos == std::string::npos) return {};
    const auto open = acf.find('"', pos + needle.size());
    if (open == std::string::npos) return {};
    const auto close = acf.find('"', open + 1);
    if (close == std::string::npos || close <= open + 1) return {};
    return acf.substr(open + 1, close - open - 1);
}

std::string steamTitleFromAppId(uint32_t appId) {
    if (appId == 0) return {};
    const std::string root = steamRootPath();
    if (root.empty()) return {};
    std::string acf;
    if (!readFileString(root + "/steamapps/appmanifest_" + std::to_string(appId) + ".acf",
                        acf))
        return {};
    return acfField(acf, "name");
}

std::string steamFolderFromCmdline(uint32_t pid) {
    std::string cmdline;
    if (!readCmdline(pid, cmdline)) return {};
    const std::string flat = flatCmdline(cmdline);

    const auto steamPos = flat.find(kSteamAppsCommon);
    if (steamPos != std::string::npos) {
        const char *start = flat.c_str() + steamPos + std::strlen(kSteamAppsCommon);
        const char *end = std::strchr(start, '/');
        if (end && end > start) return std::string(start, end - start);
    }

    for (const char *marker : {"S:\\common\\", "S:/common/", "s:\\common\\", "s:/common/"}) {
        const auto pos = flat.find(marker);
        if (pos == std::string::npos) continue;
        const char *start = flat.c_str() + pos + std::strlen(marker);
        const char *end = std::strchr(start, '\\');
        if (!end) end = std::strchr(start, '/');
        if (end && end > start) return std::string(start, end - start);
    }
    return {};
}

std::string rawAppName(const PwNode &node) {
    if (!node.appName.empty()) return node.appName;
    return node.name;
}

std::string exePathFromPid(uint32_t pid) {
    if (pid == 0) return {};
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    char buf[PATH_MAX];
    const ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
}

bool pathLooksLikeDiscord(const std::string &path) {
    if (path.empty()) return false;
    const std::string hay = lower(path);
    return hay.find("discord") != std::string::npos ||
           hay.find("discordapp") != std::string::npos ||
           hay.find("com.discordapp") != std::string::npos;
}

bool cmdlineLooksLikeDiscord(const std::string &cmdline) {
    if (cmdline.empty()) return false;
    const std::string flat = lower(flatCmdline(cmdline));
    return flat.find("discord") != std::string::npos ||
           flat.find("discordapp") != std::string::npos ||
           flat.find("com.discordapp") != std::string::npos;
}

uint32_t parentPid(uint32_t pid) {
    if (pid == 0) return 0;
    std::string status;
    if (!readFileString("/proc/" + std::to_string(pid) + "/status", status)) return 0;
    const auto pos = status.find("PPid:");
    if (pos == std::string::npos) return 0;
    const char *p = status.c_str() + pos + 5;
    while (*p == '\t' || *p == ' ') ++p;
    const long ppid = std::strtol(p, nullptr, 10);
    return ppid > 0 ? static_cast<uint32_t>(ppid) : 0;
}

bool isDiscordProcess(uint32_t pid) {
    if (pid == 0) return false;
    if (pathLooksLikeDiscord(exePathFromPid(pid))) return true;

    std::string comm;
    if (readFileString("/proc/" + std::to_string(pid) + "/comm", comm)) {
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\0'))
            comm.pop_back();
        if (pathLooksLikeDiscord(comm)) return true;
    }

    std::string cmdline;
    if (readCmdline(pid, cmdline) && cmdlineLooksLikeDiscord(cmdline)) return true;
    return false;
}

std::string discordFromProcessTree(uint32_t pid) {
    for (int depth = 0; pid != 0 && depth < 32; ++depth) {
        if (isDiscordProcess(pid)) return "Discord";
        pid = parentPid(pid);
    }
    return {};
}

void addCandidate(std::vector<std::string> &keys, std::string key) {
    if (key.empty()) return;
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
    keys.push_back(std::move(key));
}

}  // namespace

bool isGenericAppLabel(const std::string &name) {
    return isGenericName(name);
}

bool isStableIdentityKey(const std::string &key) {
    if (key.empty()) return false;
    for (unsigned char c : key) {
        if (c < 0x20 || c > 0x7e) return false;
    }
    if (key.rfind("node:", 0) == 0) return false;
    if (key.rfind("steam:", 0) == 0) return true;
    if (key.rfind("steamdir:", 0) == 0) return true;
    if (key.rfind("minecraft:", 0) == 0) return true;
    if (key.rfind("discord:", 0) == 0) return true;
    if (key.rfind("bin:", 0) == 0) return !isGenericName(key.substr(4));
    if (key.rfind("name:", 0) == 0) return !isGenericName(key.substr(5));
    return false;
}

std::vector<std::string> appIdentityKeyCandidates(const PwNode &node) {
    std::vector<std::string> keys;

    if (const uint32_t appId = steamAppIdForProcess(node.processId))
        addCandidate(keys, "steam:" + std::to_string(appId));

    if (!discordFromProcessTree(node.processId).empty()) addCandidate(keys, "discord:app");

    if (const std::string folder = steamFolderFromBinary(node.processBinary);
        !folder.empty())
        addCandidate(keys, "steamdir:" + lower(folder));

    if (const std::string folder = steamFolderFromCmdline(node.processId);
        !folder.empty())
        addCandidate(keys, "steamdir:" + lower(folder));

    if (const std::string mc = minecraftIdentity(node.processId); !mc.empty())
        addCandidate(keys, mc);

    if (const std::string exePath = exePathFromPid(node.processId); !exePath.empty()) {
        if (const std::string folder = steamFolderFromBinary(exePath); !folder.empty())
            addCandidate(keys, "steamdir:" + lower(folder));
        if (const std::string base = lower(fileBaseName(exePath)); !base.empty() &&
                                                                  !isGenericName(base))
            addCandidate(keys, "bin:" + base);
    }

    if (!node.processBinary.empty() && !isGenericName(node.processBinary))
        addCandidate(keys, "bin:" + lower(fileBaseName(node.processBinary)));

    const std::string raw = rawAppName(node);
    if (!raw.empty() && !isGenericName(raw))
        addCandidate(keys, "name:" + lower(raw));

    if (!node.processBinary.empty())
        addCandidate(keys, "bin:" + lower(fileBaseName(node.processBinary)));
    if (!raw.empty()) addCandidate(keys, "name:" + lower(raw));
    addCandidate(keys, "node:" + std::to_string(node.id));
    return keys;
}

std::string appIdentityKey(const PwNode &node) {
    for (const std::string &key : appIdentityKeyCandidates(node)) {
        if (isStableIdentityKey(key)) return key;
    }
    const auto keys = appIdentityKeyCandidates(node);
    return keys.empty() ? std::string() : keys.front();
}

std::string appDisplayName(const PwNode &node) {
    PwNode n = node;
    if ((n.appName.empty() || isGenericAppLabel(n.appName)) &&
        !n.name.empty() && isGenericAppLabel(n.name))
        n.appName.clear();  // prefer process metadata over "java" in node.name

    if (const uint32_t appId = steamAppIdForProcess(n.processId)) {
        if (const std::string title = steamTitleFromAppId(appId); !title.empty())
            return title;
    }

    if (const std::string folder = steamFolderFromBinary(n.processBinary);
        !folder.empty())
        return folder;

    if (const std::string folder = steamFolderFromCmdline(n.processId); !folder.empty())
        return folder;

    if (const std::string discord = discordFromProcessTree(n.processId); !discord.empty())
        return discord;

    const std::string raw = rawAppName(n);

    if (!raw.empty() && !isGenericName(raw)) {
        const std::string folder = steamFolderFromBinary(node.processBinary);
        if (!folder.empty() && lower(raw) != lower(folder))
            return prettifyBinaryName(fileBaseName(raw));
        if (folder.empty()) return prettifyBinaryName(fileBaseName(raw));
    }

    if (const std::string mc = minecraftLabel(node.processId); !mc.empty()) return mc;

    if (const std::string fromBin = labelFromBinary(node.processBinary);
        !fromBin.empty() && !isGenericName(fromBin))
        return fromBin;

    if (const std::string fromExe = labelFromPidExe(node.processId);
        !fromExe.empty() && !isGenericName(fromExe))
        return fromExe;

    if (!raw.empty()) return prettifyBinaryName(fileBaseName(raw));
    return "Unknown application";
}

std::string appMergeKey(const PwNode &node) {
    for (const std::string &key : appIdentityKeyCandidates(node)) {
        if (isStableIdentityKey(key)) return key;
    }
    const std::string display = appDisplayName(node);
    if (!display.empty() && display != "Unknown application")
        return "display:" + lower(display);
    return "node:" + std::to_string(node.id);
}

}  // namespace waveline
