// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "device/deviceprofile.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace waveline {

namespace {

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// The profile files are shell-sourceable, so a value may or may not be quoted
// depending on whether it has spaces in it. Both spellings mean the same thing
// and neither is worth rejecting.
std::string unquote(const std::string &s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

std::string homeDir() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return xdg;
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config";
    return {};
}

}  // namespace

std::string DeviceProfile::configPath() {
    const std::string base = homeDir();
    if (base.empty()) return {};
    return base + "/waveline/profile.conf";
}

DeviceProfile DeviceProfile::load() {
    DeviceProfile p;

    const std::string path = configPath();
    if (path.empty()) return p;
    std::ifstream in(path);
    if (!in) return p;

    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string val = unquote(trim(t.substr(eq + 1)));

        if (key == "PROFILE_ID") p.id = val;
        else if (key == "PROFILE_LABEL") p.label = val;
        else if (key == "BRAND") p.brand = val;
        else if (key == "ALSA_NODE_MATCH") p.alsaNodeMatch = val;
        else if (key == "HARDWARE_CONTROLS") p.hardwareControls = (val == "1");
        // Every other key in the file belongs to install.sh -- paths to
        // patches and drop-ins that mean nothing at runtime. Ignored rather
        // than rejected, so the installer can grow keys without this needing
        // to know about them (including legacy EDITION= lines).
    }

    // A profile that names no brand would publish nodes called " Stream Mix".
    if (trim(p.brand).empty()) p.brand = "Waveline";
    return p;
}

}  // namespace waveline
