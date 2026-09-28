// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "desktopentries.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace waveline {
namespace {

// How often the directories are checked for changes. A new install shows up
// within this long; between checks a lookup is a map find.
constexpr auto kRescanInterval = std::chrono::seconds(5);

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string baseName(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "/usr/bin/music (deleted)" is what the kernel reports for a binary that was
// replaced while it ran -- an update -- and it is still that program.
std::string withoutDeleted(std::string path) {
    static const std::string kDeleted = " (deleted)";
    if (path.size() > kDeleted.size()
        && path.compare(path.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0)
        path.resize(path.size() - kDeleted.size());
    return path;
}

// Programs that run other programs. A launcher entry whose Exec is one of
// these says nothing about any other process that happens to be running it.
bool isRuntime(const std::string &base) {
    static const std::set<std::string> kRuntimes = {
        "sh", "bash", "zsh", "dash", "env", "python", "python2", "python3", "perl",
        "ruby", "node", "java", "javaw", "mono", "dotnet", "wine", "wine64",
        "wine-preloader", "wine64-preloader", "electron", "flatpak", "snap",
        "gjs", "qml", "qmlscene", "appimagelauncher", "xdg-open", "steam",
    };
    std::string b = lower(base);
    // python3.12, electron31 and the like.
    while (!b.empty() && (std::isdigit(static_cast<unsigned char>(b.back())) || b.back() == '.'))
        b.pop_back();
    return kRuntimes.count(b) > 0;
}

// Exec's words, with the spec's double quoting undone.
std::vector<std::string> splitExec(const std::string &exec) {
    std::vector<std::string> words;
    std::string word;
    bool quoted = false, any = false;
    for (size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (quoted) {
            if (c == '\\' && i + 1 < exec.size()) word += exec[++i];
            else if (c == '"') quoted = false;
            else word += c;
        } else if (c == '"') {
            quoted = any = true;
        } else if (c == ' ' || c == '\t') {
            if (any || !word.empty()) words.push_back(word);
            word.clear();
            any = false;
        } else {
            word += c;
            any = true;
        }
    }
    if (any || !word.empty()) words.push_back(word);
    return words;
}

// The program an Exec line runs: past "env" and its VAR=value assignments,
// and for a Flatpak export, the --command it runs inside the sandbox.
std::string programOf(const std::string &exec) {
    const std::vector<std::string> words = splitExec(exec);
    size_t i = 0;
    if (i < words.size() && baseName(words[i]) == "env") {
        ++i;
        while (i < words.size() && (words[i].find('=') != std::string::npos
                                    || (!words[i].empty() && words[i][0] == '-')))
            ++i;
    }
    if (i >= words.size()) return {};
    if (baseName(words[i]) == "flatpak") {
        for (size_t j = i + 1; j < words.size(); ++j)
            if (words[j].rfind("--command=", 0) == 0) return words[j].substr(10);
        return {};
    }
    return words[i];
}

struct Parsed {
    DesktopEntry entry;
    std::string program;   // Exec's program, as written
    std::string wmClass;   // StartupWMClass, lowercased
    bool noDisplay = false;
};

// The [Desktop Entry] group only; actions and the rest are skipped.
bool parseFile(const std::string &path, const std::string &id,
               const std::vector<std::string> &langs, Parsed &out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool inMain = false;
    std::string type;
    bool hidden = false;
    int nameRank = INT_MAX;   // lower wins: a language match beats plain Name
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            inMain = line == "[Desktop Entry]";
            continue;
        }
        if (!inMain) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        std::string value = line.substr(eq + 1);
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);

        if (key == "Type") type = value;
        else if (key == "Hidden") hidden = value == "true";
        else if (key == "NoDisplay") out.noDisplay = value == "true";
        else if (key == "Icon") out.entry.icon = value;
        else if (key == "Exec") out.program = programOf(value);
        else if (key == "StartupWMClass") out.wmClass = lower(value);
        else if (key == "Categories") {
            size_t start = 0;
            while (start < value.size()) {
                const auto semi = value.find(';', start);
                const std::string cat = value.substr(start, semi - start);
                if (!cat.empty()) out.entry.categories.push_back(cat);
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        } else if (key == "Name" && nameRank > int(langs.size())) {
            out.entry.name = value;
            nameRank = int(langs.size());
        } else if (key.rfind("Name[", 0) == 0 && key.back() == ']') {
            const std::string lang = key.substr(5, key.size() - 6);
            const auto it = std::find(langs.begin(), langs.end(), lang);
            if (it != langs.end() && int(it - langs.begin()) < nameRank) {
                out.entry.name = value;
                nameRank = int(it - langs.begin());
            }
        }
    }
    if (hidden || type != "Application" || out.entry.name.empty()) return false;
    out.entry.id = id;
    return true;
}

// LANG=en_GB.UTF-8 -> {"en_GB", "en"}.
std::vector<std::string> currentLanguages() {
    std::vector<std::string> langs;
    const char *env = std::getenv("LC_ALL");
    if (!env || !*env) env = std::getenv("LC_MESSAGES");
    if (!env || !*env) env = std::getenv("LANG");
    if (!env || !*env) return langs;
    std::string lang = env;
    lang = lang.substr(0, lang.find_first_of(".@"));
    if (lang.empty() || lang == "C" || lang == "POSIX") return langs;
    langs.push_back(lang);
    if (const auto us = lang.find('_'); us != std::string::npos)
        langs.push_back(lang.substr(0, us));
    return langs;
}

// XDG_DATA_HOME first, then XDG_DATA_DIRS: an entry in an earlier directory
// shadows one of the same id in a later one, as it does for the launcher.
std::vector<std::string> applicationDirs() {
    std::vector<std::string> dirs;
    if (const char *h = std::getenv("XDG_DATA_HOME"); h && *h)
        dirs.push_back(std::string(h) + "/applications");
    else if (const char *home = std::getenv("HOME"); home && *home)
        dirs.push_back(std::string(home) + "/.local/share/applications");
    std::string data = "/usr/local/share:/usr/share";
    if (const char *d = std::getenv("XDG_DATA_DIRS"); d && *d) data = d;
    // A systemd user unit often runs without the Flatpak exports in its
    // XDG_DATA_DIRS; they are where Flatpak apps' entries live.
    if (const char *home = std::getenv("HOME"); home && *home)
        data += std::string(":") + home + "/.local/share/flatpak/exports/share";
    data += ":/var/lib/flatpak/exports/share";
    size_t start = 0;
    while (start <= data.size()) {
        const auto colon = data.find(':', start);
        const std::string d = data.substr(start, colon - start);
        if (!d.empty()) {
            const std::string apps = d + "/applications";
            if (std::find(dirs.begin(), dirs.end(), apps) == dirs.end()) dirs.push_back(apps);
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return dirs;
}

class Index {
public:
    std::optional<DesktopEntry> forProcess(uint32_t pid, const std::string &processBinary) {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshIfStale();

        std::vector<std::string> paths;
        if (pid != 0) {
            char link[64];
            std::snprintf(link, sizeof(link), "/proc/%u/exe", pid);
            char buf[PATH_MAX];
            const ssize_t n = readlink(link, buf, sizeof(buf) - 1);
            if (n > 0) paths.push_back(withoutDeleted(std::string(buf, size_t(n))));
        }
        if (!processBinary.empty()) paths.push_back(withoutDeleted(processBinary));

        for (const std::string &path : paths) {
            if (const auto it = byPath_.find(path); it != byPath_.end())
                return entries_[it->second].entry;
        }
        for (const std::string &path : paths) {
            const std::string base = lower(baseName(path));
            if (base.empty() || isRuntime(base)) continue;
            for (const auto *map : {&byProgram_, &byWmClass_, &byId_}) {
                if (const auto it = map->find(base); it != map->end())
                    return entries_[it->second].entry;
            }
        }
        return std::nullopt;
    }

private:
    // One stat per directory, at most every kRescanInterval.
    void refreshIfStale() {
        const auto now = std::chrono::steady_clock::now();
        if (scanned_ && now - lastCheck_ < kRescanInterval) return;
        lastCheck_ = now;

        std::string stamp;
        const std::vector<std::string> dirs = applicationDirs();
        for (const std::string &d : dirs) {
            struct stat st {};
            if (stat(d.c_str(), &st) == 0)
                stamp += d + ':' + std::to_string(st.st_mtim.tv_sec) + '.'
                       + std::to_string(st.st_mtim.tv_nsec) + ';';
        }
        if (scanned_ && stamp == stamp_) return;
        stamp_ = stamp;
        scanned_ = true;
        rebuild(dirs);
    }

    void rebuild(const std::vector<std::string> &dirs) {
        entries_.clear();
        byPath_.clear();
        byProgram_.clear();
        byWmClass_.clear();
        byId_.clear();
        const std::vector<std::string> langs = currentLanguages();
        std::set<std::string> seen;
        for (const std::string &dir : dirs) scanDir(dir, dir, langs, seen);
    }

    // Subdirectories become part of the id, per the spec: kde4/foo.desktop is
    // kde4-foo.
    void scanDir(const std::string &root, const std::string &dir,
                 const std::vector<std::string> &langs, std::set<std::string> &seen) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        while (dirent *e = readdir(d)) {
            const std::string file = e->d_name;
            if (file == "." || file == "..") continue;
            const std::string path = dir + '/' + file;
            struct stat st {};
            if (stat(path.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                scanDir(root, path, langs, seen);
                continue;
            }
            if (file.size() <= 8 || file.compare(file.size() - 8, 8, ".desktop") != 0) continue;
            std::string id = path.substr(root.size() + 1);
            id.resize(id.size() - 8);
            std::replace(id.begin(), id.end(), '/', '-');
            if (!seen.insert(id).second) continue;   // shadowed by an earlier dir
            Parsed parsed;
            if (!parseFile(path, id, langs, parsed)) continue;
            add(std::move(parsed));
        }
        closedir(d);
    }

    // A visible entry beats a NoDisplay one for the same key, and between two
    // visible ones the first found -- the user's own, then the system's.
    void claim(std::map<std::string, size_t> &map, const std::string &key, size_t index) {
        if (key.empty()) return;
        const auto it = map.find(key);
        if (it == map.end() || (entries_[it->second].noDisplay && !entries_[index].noDisplay))
            map[key] = index;
    }

    void add(Parsed parsed) {
        entries_.push_back(std::move(parsed));
        const size_t index = entries_.size() - 1;
        const Parsed &p = entries_[index];
        const std::string base = lower(baseName(p.program));
        if (!base.empty() && !isRuntime(base)) {
            if (p.program[0] == '/') claim(byPath_, p.program, index);
            claim(byProgram_, base, index);
        }
        claim(byWmClass_, p.wmClass, index);
        claim(byId_, lower(p.entry.id), index);
    }

    std::mutex mutex_;
    std::vector<Parsed> entries_;
    std::map<std::string, size_t> byPath_, byProgram_, byWmClass_, byId_;
    std::string stamp_;
    bool scanned_ = false;
    std::chrono::steady_clock::time_point lastCheck_;
};

Index &index() {
    static Index idx;
    return idx;
}

}  // namespace

std::optional<DesktopEntry> desktopEntryForProcess(uint32_t pid,
                                                   const std::string &processBinary) {
    return index().forProcess(pid, processBinary);
}

std::string channelForCategories(const std::vector<std::string> &categories) {
    const auto has = [&categories](const char *c) {
        return std::find(categories.begin(), categories.end(), c) != categories.end();
    };
    if (has("WebBrowser")) return "browser";
    if (has("Game")) return "game";
    if (has("Chat") || has("InstantMessaging") || has("VideoConference")
        || has("Telephony") || has("IRCClient"))
        return "voice";
    // A player, not an editor or a recorder: Voice Memos is Audio too.
    if (has("Player") || has("Music")) {
        if (has("Video") && !has("Audio")) return "video";
        return "music";
    }
    return {};
}

}  // namespace waveline
