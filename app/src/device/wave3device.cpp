// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "wave3device.h"

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace waveline {
namespace {

// Offsets whose function is unknown, and offsets the firmware owns to report
// live dial state. Writing either fights the device; see docs/protocol.md.
const std::set<int> kReserved = {2, 3, 6};
const std::set<int> kFirmwareOwned = {kDialLo, kDialHi, kDialFlag, kDialMode};

std::optional<std::string> readSysfs(const fs::path &p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string s;
    std::getline(f, s);
    return s;
}

std::optional<long> readSysfsHex(const fs::path &p) {
    auto s = readSysfs(p);
    if (!s) return std::nullopt;
    try { return std::stol(*s, nullptr, 16); } catch (...) { return std::nullopt; }
}

std::optional<long> readSysfsDec(const fs::path &p) {
    auto s = readSysfs(p);
    if (!s) return std::nullopt;
    try { return std::stol(*s, nullptr, 10); } catch (...) { return std::nullopt; }
}

std::optional<std::string> findAlsaCardIndex() {
    const fs::path base = "/proc/asound";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(base, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0) continue;
        const auto usbid = readSysfs(entry.path() / "usbid");
        if (!usbid || *usbid != "0fd9:0070") continue;
        return name.substr(4);
    }
    return std::nullopt;
}

std::optional<std::string> findAlsaCardForPrefix(const std::string &prefix) {
    if (prefix.empty()) return std::nullopt;
    const fs::path base = "/proc/asound";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(base, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0) continue;
        const auto usbid = readSysfs(entry.path() / "usbid");
        if (!usbid || *usbid != "0fd9:0070") continue;
        const auto id = readSysfs(entry.path() / "id");
        if (id && prefix.find(*id) != std::string::npos) return name.substr(4);
    }
    return std::nullopt;
}

fs::path usbDeviceForAlsaCard(const std::string &cardIdx) {
    std::error_code ec;
    const fs::path dev = fs::path("/sys/class/sound/card") / cardIdx / "device";
    if (!fs::exists(dev, ec)) return {};
    const fs::path usb = fs::canonical(dev, ec);
    if (ec) return {};
    // card device symlink points at the USB interface; walk up to the device.
    fs::path p = usb;
    for (int i = 0; i < 4 && !p.empty(); ++i) {
        if (fs::exists(p / "idVendor", ec) && fs::exists(p / "idProduct", ec))
            return p;
        p = p.parent_path();
    }
    return {};
}

std::vector<fs::path> listWave3UsbDevices() {
    std::vector<fs::path> out;
    const fs::path base = "/sys/bus/usb/devices";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(base, ec)) {
        auto vid = readSysfsHex(entry.path() / "idVendor");
        auto pid = readSysfsHex(entry.path() / "idProduct");
        if (!vid || !pid || *vid != kVendorId || *pid != kProductId) continue;
        out.push_back(entry.path());
    }
    return out;
}

Result openUsbDevice(const fs::path &found, std::string &nodePath, int &fd) {
    const fs::path drv = found / (found.filename().string() + ":1." +
                                  std::to_string(kInterface)) / "driver";
    std::error_code ec;
    if (fs::exists(drv, ec)) {
        const std::string name = fs::read_symlink(drv, ec).filename().string();
        if (!ec && name != "usbfs")
            return {Error::Refused,
                    "interface 3 has kernel driver '" + name + "' bound"};
    }

    auto bus = readSysfsDec(found / "busnum");
    auto dev = readSysfsDec(found / "devnum");
    if (!bus || !dev) return {Error::NotFound, "cannot resolve usbfs node"};

    char path[64];
    std::snprintf(path, sizeof(path), "/dev/bus/usb/%03ld/%03ld", *bus, *dev);
    nodePath = path;

    fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            return {Error::Permission,
                    std::string(path) + " is not writable. Install the udev rule:\n"
                    "  sudo install -Dm644 udev/60-waveline-control.rules "
                    "/etc/udev/rules.d/\n  sudo udevadm control --reload-rules\n"
                    "  sudo udevadm trigger --subsystem-match=usb "
                    "--attr-match=idVendor=0fd9 --attr-match=idProduct=0070"};
        return {Error::Io, std::string("open ") + path + ": " + std::strerror(errno)};
    }
    return {};
}

// Sets one of the card's ALSA mixer controls by name. Both the mic preamp and
// the headphone level are reachable only this way: SET_CUR on the matching UAC
// feature unit through the vendor wIndex is accepted by the firmware and then
// does nothing, which is a great deal more confusing than an outright refusal.
Result setAlsaControl(const std::optional<std::string> &card, const char *name,
                      int alsaValue) {
    const auto idx = card ? card : findAlsaCardIndex();
    if (!idx)
        return {Error::NotFound, "Wave:3 ALSA card not found"};

    const std::string val = std::to_string(alsaValue);
    const std::string sel = std::string("name=") + name;
    const pid_t pid = fork();
    if (pid < 0) return {Error::Io, std::strerror(errno)};
    if (pid == 0) {
        execlp("amixer", "amixer", "-c", idx->c_str(), "-q", "cset", sel.c_str(),
               val.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {Error::Io, std::string("amixer failed to set ") + name};
    return {};
}

Result setAlsaMicGain(const std::optional<std::string> &card, int alsaValue) {
    const auto idx = card ? card : findAlsaCardIndex();
    if (!idx)
        return {Error::NotFound, "Wave:3 ALSA card not found"};

    const std::string val = std::to_string(alsaValue);
    const pid_t pid = fork();
    if (pid < 0) return {Error::Io, std::strerror(errno)};
    if (pid == 0) {
        execlp("amixer", "amixer", "-c", idx->c_str(), "-q", "cset",
               "name=Mic Capture Volume", val.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {Error::Io, "amixer failed to set Mic Capture Volume"};
    return {};
}

}  // namespace

Device::~Device() { close(); }

int Device::mixToPercent(int raw) {
    if (raw > kMixMax) raw = kMixMax;
    if (raw < kMixMin) raw = kMixMin;
    return static_cast<int>(std::lround(100.0 * raw / kMixMax));
}

int Device::percentToMix(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return static_cast<int>(std::lround(kMixMin + (kMixMax - kMixMin) * percent / 100.0));
}

Result Device::open() { return openMatching({}); }

Result Device::openMatching(const std::string &alsaNodePrefix) {
    if (fd_ >= 0) return {};

    fs::path found;
    std::optional<std::string> card;

    if (!alsaNodePrefix.empty()) {
        card = findAlsaCardForPrefix(alsaNodePrefix);
        if (card) found = usbDeviceForAlsaCard(*card);
    }

    if (found.empty()) {
        const std::vector<fs::path> devices = listWave3UsbDevices();
        if (devices.empty()) {
            const fs::path base = "/sys/bus/usb/devices";
            std::error_code ec;
            bool sawDfu = false;
            for (const auto &entry : fs::directory_iterator(base, ec)) {
                auto vid = readSysfsHex(entry.path() / "idVendor");
                auto pid = readSysfsHex(entry.path() / "idProduct");
                if (!vid || !pid || *vid != kVendorId) continue;
                if (*pid == kProductIdDfu) sawDfu = true;
            }
            if (sawDfu)
                return {Error::NotFound,
                        "device is in DFU/bootloader mode. Unplug it for 30 seconds "
                        "and replug; nothing here writes firmware."};
            return {Error::NotFound, "Elgato Wave:3 not found on USB"};
        }
        found = devices.front();
        if (!card) {
            for (const auto &d : devices) {
                const fs::path base = "/proc/asound";
                std::error_code ec;
                for (const auto &entry : fs::directory_iterator(base, ec)) {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("card", 0) != 0) continue;
                    if (usbDeviceForAlsaCard(name.substr(4)) == d) {
                        card = name.substr(4);
                        break;
                    }
                }
                if (card) break;
            }
        }
    }

    int fd = -1;
    if (auto r = openUsbDevice(found, nodePath_, fd); !r) return r;
    fd_ = fd;
    alsaCardIndex_ = card ? card : findAlsaCardIndex();
    return {};
}

void Device::close() {
    if (fd_ < 0) return;
    release();
    ::close(fd_);
    fd_ = -1;
    alsaCardIndex_.reset();
}

Result Device::claim() {
    if (fd_ < 0) return {Error::NotFound, "device not open"};
    if (claimed_) return {};
    unsigned int iface = kInterface;
    if (::ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        if (errno == EBUSY)
            return {Error::Busy, "interface 3 is held by another process"};
        if (errno == ENODEV)
            return {Error::NotFound, "device disconnected"};
        return {Error::Io, std::string("claim: ") + std::strerror(errno)};
    }
    claimed_ = true;
    return {};
}

void Device::release() {
    if (!claimed_ || fd_ < 0) return;
    unsigned int iface = kInterface;
    ::ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &iface);  // closing releases anyway
    claimed_ = false;
}

Result Device::transfer(uint8_t requestType, uint8_t request, uint16_t value,
                        uint16_t index, void *data, uint16_t length) {
    // Structural interlock: the vendor request type is not reachable from here.
    if (requestType != kClassIn && requestType != kClassOut)
        return {Error::Refused,
                "only class requests are permitted; vendor-type requests have "
                "rebooted this device into its bootloader"};
    if (fd_ < 0) return {Error::NotFound, "device not open"};

    usbdevfs_ctrltransfer xfer{};
    xfer.bRequestType = requestType;
    xfer.bRequest = request;
    xfer.wValue = value;
    xfer.wIndex = index;
    xfer.wLength = length;
    xfer.timeout = 1000;
    xfer.data = data;

    const int n = ::ioctl(fd_, USBDEVFS_CONTROL, &xfer);
    if (n < 0) {
        if (errno == ENODEV) return {Error::NotFound, "device disconnected"};
        if (errno == EBUSY) return {Error::Busy, "interface busy"};
        return {Error::Io, std::strerror(errno)};
    }
    if (n != length)
        return {Error::Protocol, "short transfer: " + std::to_string(n) + " of " +
                                 std::to_string(length)};
    return {};
}

Result Device::readConfig(std::vector<uint8_t> &out) {
    out.assign(kConfigLen, 0);
    return transfer(kClassIn, kReqGet, 0x0000, kWIndex, out.data(), kConfigLen);
}

Result Device::writeConfig(const std::vector<uint8_t> &cfg) {
    if (static_cast<int>(cfg.size()) != kConfigLen)
        return {Error::Protocol, "config must be 16 bytes"};
    std::vector<uint8_t> copy = cfg;
    return transfer(kClassOut, kReqSet, 0x0000, kWIndex, copy.data(), kConfigLen);
}

Result Device::setByte(int offset, uint8_t value) {
    if (offset < 0 || offset >= kConfigLen)
        return {Error::Refused, "offset out of range"};
    if (kReserved.count(offset))
        return {Error::Refused,
                "offset " + std::to_string(offset) + " has unknown function; "
                "\"no observed effect\" is not the same as no effect"};
    if (kFirmwareOwned.count(offset))
        return {Error::Refused,
                "offset " + std::to_string(offset) + " is written by the firmware "
                "to report live dial state"};

    // Re-read immediately before writing: the block carries firmware-owned
    // bytes, and echoing a stale snapshot of them back fights the dial.
    std::vector<uint8_t> cfg;
    if (auto r = readConfig(cfg); !r) return r;
    if (cfg[offset] == value) return {};  // nothing to do
    cfg[offset] = value;
    return writeConfig(cfg);
}

Result Device::uacGet(uint8_t entity, uint8_t selector, void *out, uint16_t len) {
    return transfer(kClassIn, kUacGetCur, static_cast<uint16_t>(selector << 8),
                    static_cast<uint16_t>((entity << 8) | kInterface), out, len);
}

Result Device::uacSet(uint8_t entity, uint8_t selector, const void *in,
                      uint16_t len) {
    return transfer(kClassOut, kUacSetCur, static_cast<uint16_t>(selector << 8),
                    static_cast<uint16_t>((entity << 8) | kInterface),
                    const_cast<void *>(in), len);
}

Result Device::readState(State &out) {
    std::vector<uint8_t> cfg;
    if (auto r = readConfig(cfg); !r) return r;

    out.raw = cfg;
    out.micMuted = cfg[kMicMute] != 0;
    out.hpMuted = cfg[kHpMute] != 0;
    out.clipguard = cfg[kClipguard] != 0;
    out.monitorMix = cfg[kMonitorMix];
    out.monitorPercent = mixToPercent(cfg[kMonitorMix]);
    out.dialValue = cfg[kDialLo] | (cfg[kDialHi] << 8);

    const int mode = cfg[kDialMode];
    out.dialMode = (mode >= 1 && mode <= 3) ? static_cast<DialMode>(mode)
                                            : DialMode::MicGain;

    // Volumes come from the feature units, not the config block. Offset 8
    // stores whole dB while ALSA and the firmware both work in 1/256 dB, so
    // reading it there loses half-decibel settings.
    int16_t raw = 0;
    if (auto r = uacGet(kFuMic, kUacVolume, &raw, 2); r)
        out.micGainDb = raw / 256.0;
    if (auto r = uacGet(kFuHeadphone, kUacVolume, &raw, 2); r)
        out.hpVolumeDb = raw / 256.0;

    uint8_t meter[kMeterLen] = {};
    if (auto r = transfer(kClassIn, kReqGet, 0x0001, kWIndex, meter, kMeterLen); r) {
        out.telemetry = static_cast<uint32_t>(meter[0]) |
                        (static_cast<uint32_t>(meter[1]) << 8) |
                        (static_cast<uint32_t>(meter[2]) << 16) |
                        (static_cast<uint32_t>(meter[3]) << 24);
    }
    return {};
}

Result Device::readInfo(DeviceInfo &out) {
    uint8_t buf[kInfoLen] = {};
    if (auto r = transfer(kClassIn, kReqGet, 0x000A, kWIndex, buf, kInfoLen); !r)
        return r;
    out.apiVersion = std::to_string(buf[0]) + "." + std::to_string(buf[1]);
    out.firmwareVersion = std::to_string(buf[6]) + "." + std::to_string(buf[7]) +
                          "." + std::to_string(buf[8]);
    return {};
}

Result Device::setClipguard(bool on) { return setByte(kClipguard, on ? 1 : 0); }
Result Device::setMicMute(bool muted) { return setByte(kMicMute, muted ? 1 : 0); }

Result Device::setMicGainDb(double db) {
    if (db < kMicGainMinDb) db = kMicGainMinDb;
    if (db > kMicGainMaxDb) db = kMicGainMaxDb;
    // Verified on hardware: SET_CUR on FU6 via the vendor wIndex is accepted
    // but does not move the gain. The kernel's ALSA control (numid 6) is the
    // path that actually reaches the firmware -- same as `amixer` and
    // PipeWire's volume slider in the analog profile.
    const int alsaVal =
        std::clamp(static_cast<int>(std::lround(db * 2.0)), 0, 80);
    return setAlsaMicGain(alsaCardIndex_, alsaVal);
}

Result Device::setHpMute(bool muted) { return setByte(kHpMute, muted ? 1 : 0); }

Result Device::setHpVolumeDb(double db) {
    if (db < kHpVolumeMinDb) db = kHpVolumeMinDb;
    if (db > kHpVolumeMaxDb) db = kHpVolumeMaxDb;
    // 'PCM Playback Volume' is 0..120 in half-decibel steps from -60 dB, which
    // is the same scale the config block's byte 8 reports as a signed dB value.
    const int alsaVal = std::clamp(
        static_cast<int>(std::lround((db - kHpVolumeMinDb) * 2.0)), 0, 120);
    return setAlsaControl(alsaCardIndex_, "PCM Playback Volume", alsaVal);
}

Result Device::setMonitorPercent(int percent) {
    return setByte(kMonitorMix, static_cast<uint8_t>(percentToMix(percent)));
}

}  // namespace waveline
