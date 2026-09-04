// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Node naming for input-device buses. Primary ("mic") keeps legacy node names.

#pragma once

#include <cctype>
#include <cstdio>
#include <string>

namespace waveline {

inline constexpr const char *kPrimaryMasterId = "mic";
inline constexpr size_t kMaxMasterBuses = 8;

inline bool isPrimaryMaster(const std::string &id) { return id == kPrimaryMasterId; }

inline std::string masterGainNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-gain" : "waveline-" + id + "-gain";
}

inline std::string masterCaptureSelectorNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-capture-selector"
                               : "waveline-" + id + "-capture-selector";
}

inline std::string masterSynthNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-synth" : "waveline-" + id + "-synth";
}

inline bool isMidiBusType(const std::string &type) { return type == "midi"; }

inline std::string masterNcNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-nc" : "waveline-" + id + "-nc";
}

inline std::string masterFxNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-fx" : "waveline-" + id + "-fx";
}

inline std::string masterDynNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-dynamics" : "waveline-" + id + "-dynamics";
}

inline std::string masterCreativeNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic-creative" : "waveline-" + id + "-creative";
}

inline std::string masterSourceNode(const std::string &id) {
    return isPrimaryMaster(id) ? "waveline-mic" : "waveline-" + id;
}

inline std::string masterPathId(const std::string &id) { return id; }

inline std::string masterMonitorPath(const std::string &id) {
    return "waveline-" + id + "-monitor";
}

// PipeWire ALSA node prefixes for known capture devices. Used to auto-name
// master strips when a bus is added (Wave:3 → "Wave:3", etc.).
inline constexpr const char *kWave3CapturePrefix =
    "alsa_input.usb-Elgato_Systems_Elgato_Wave_3";
inline constexpr const char *kC922CapturePrefix =
    "alsa_input.usb-046d_C922_Pro_Stream_Webcam";
inline constexpr const char *kMeet4kCapturePrefix =
    "alsa_input.usb-Remo_Tech_Co.__Ltd._OBSBOT_Meet_4K";
inline constexpr const char *kModMicCapturePrefix =
    "alsa_input.usb-Antlion_Audio_Antlion_Wireless_Microphone";
inline constexpr const char *kRocksmithCapturePrefix =
    "alsa_input.usb-Hercules_Rocksmith_USB_Guitar_Adapter";
inline constexpr const char *kPciCapturePrefix = "alsa_input.pci-";

// Yuan / Elgato capture cards (from sc0710 PCI ID table).
inline constexpr const char *kSc0710VendorId = "12ab";
inline constexpr const char *kSc0710Mk2ProductId = "0710";   // 4K60 Pro MK.2
inline constexpr const char *kSc0710ProProductId = "0380";   // 4K Pro

inline bool startsWith(const std::string &s, const char *prefix) {
    const std::string p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline bool isWave3CaptureMatch(const std::string &match) {
    return !match.empty() && startsWith(match, kWave3CapturePrefix);
}

// Read a PCI sysfs id file ("0x0710\n" → "0710").
inline std::string readPciSysfsId(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    char buf[32] = {};
    const char *got = std::fgets(buf, sizeof(buf), f);
    std::fclose(f);
    if (!got) return {};
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (s.size() < 4)
        s.insert(s.begin(), '0');
    return s;
}

// alsa_input.pci-0000_0b_00.0.stereo-fallback → "0000:0b:00.0"
inline std::string pciBdfFromAlsaNode(const std::string &node) {
    if (!startsWith(node, kPciCapturePrefix)) return {};
    const std::string rest =
        node.substr(std::char_traits<char>::length(kPciCapturePrefix));
    std::string domain, bus, slot, func;
    std::size_t i = 0;
    auto takeHex = [&](std::string &out, char stop) -> bool {
        const std::size_t start = i;
        while (i < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i])))
            ++i;
        if (i == start) return false;
        out = rest.substr(start, i - start);
        if (stop && (i >= rest.size() || rest[i] != stop)) return false;
        if (stop) ++i;
        return true;
    };
    if (!takeHex(domain, '_')) return {};
    if (!takeHex(bus, '_')) return {};
    if (!takeHex(slot, '.')) return {};
    if (!takeHex(func, '\0')) return {};
    return domain + ":" + bus + ":" + slot + "." + func;
}

// Friendly capture-device brand for a PipeWire capture node name.
inline std::string masterCaptureBrand(const std::string &match) {
    if (match.empty()) return "Waveline";
    if (startsWith(match, kWave3CapturePrefix)) return "Wave:3";
    if (startsWith(match, kC922CapturePrefix)) return "C922";
    if (startsWith(match, kMeet4kCapturePrefix)) return "Meet 4K";
    if (startsWith(match, kModMicCapturePrefix)) return "ModMic";
    if (startsWith(match, kRocksmithCapturePrefix)) return "Tone Cable";

    const std::string bdf = pciBdfFromAlsaNode(match);
    if (!bdf.empty()) {
        const std::string base = "/sys/bus/pci/devices/" + bdf + "/";
        const std::string vendor = readPciSysfsId(base + "vendor");
        const std::string product = readPciSysfsId(base + "device");
        if (vendor == kSc0710VendorId) {
            if (product == kSc0710Mk2ProductId) return "4K60 MK.2";
            if (product == kSc0710ProProductId) return "4K Pro";
        }
    }
    return "Waveline";
}

}  // namespace waveline
