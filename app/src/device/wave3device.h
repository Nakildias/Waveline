// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Elgato Wave:3 vendor control transport.
//
// A C++ port of scripts/wave3_usb.py, kept deliberately free of Qt and of
// PipeWire so that both the GUI and the daemon can link it. The protocol and
// the reasoning behind every constant here are documented in docs/protocol.md;
// what follows only repeats the parts that constrain the code.
//
// Safety rules are enforced structurally rather than by convention:
//
//   * class request type only (0xA1 / 0x21). Vendor type (0xC1) once rebooted
//     a device into its bootloader, so it is not reachable from this API.
//   * only the three wValue IDs this firmware answers.
//   * config offsets 2, 3 and 6 are never written (function unknown), nor are
//     0, 1, 7 and 12 (written by the firmware to report live dial state).
//   * writes are read-modify-write, re-reading immediately beforehand.
//
// Interface 3 is claimed explicitly. Leaving it to usbfs's implicit claim logs
// a kernel warning on every single use.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace waveline {

inline constexpr uint16_t kVendorId = 0x0FD9;
inline constexpr uint16_t kProductId = 0x0070;
inline constexpr uint16_t kProductIdDfu = 0x0071;

// Vendor interface: 0xFF/0xF0, zero endpoints, no kernel driver.
inline constexpr uint8_t kInterface = 3;
// wIndex high byte is the entity the firmware looks at; low byte is the
// interface number the kernel checks. See docs/protocol.md.
inline constexpr uint16_t kWIndex = (0x33 << 8) | kInterface;

inline constexpr uint8_t kClassIn = 0xA1;
inline constexpr uint8_t kClassOut = 0x21;
inline constexpr uint8_t kReqGet = 0x85;
inline constexpr uint8_t kReqSet = 0x05;

// UAC feature units, reachable with wIndex = (entity << 8) | kInterface.
inline constexpr uint8_t kFuHeadphone = 5;
inline constexpr uint8_t kFuMic = 6;
inline constexpr uint8_t kUacMute = 0x01;
inline constexpr uint8_t kUacVolume = 0x02;
inline constexpr uint8_t kUacGetCur = 0x81;
inline constexpr uint8_t kUacSetCur = 0x01;

// Mic gain range from the UAC feature unit (matches ALSA numid 6).
inline constexpr double kMicGainMinDb = 0.0;
inline constexpr double kMicGainMaxDb = 40.0;

// Headphone level, i.e. ALSA 'PCM Playback Volume' (numid 4): 0..120 in
// half-decibel steps. The config block reports the same value at byte 8 as a
// signed dB integer.
inline constexpr double kHpVolumeMinDb = -60.0;
inline constexpr double kHpVolumeMaxDb = 0.0;

// Config block offsets.
enum Offset : int {
    kDialLo = 0,
    kDialHi = 1,
    kMicMute = 4,
    kClipguard = 5,
    kDialFlag = 7,
    kHpVolume = 8,
    kHpMute = 9,
    kMonitorIndicator = 10,
    kMonitorMix = 11,   // verified: 0 = all PC, 91 = all mic
    kDialMode = 12,
    kBrightness = 15,   // stores a value but has no effect on this model
};

inline constexpr int kConfigLen = 16;
inline constexpr int kMeterLen = 8;
inline constexpr int kInfoLen = 51;

// Range the physical dial itself produces. The byte accepts 0-255 with no
// clamping, but staying inside the hardware's own range is the safe choice.
inline constexpr int kMixMin = 0;
inline constexpr int kMixMax = 91;

enum class DialMode { MicGain = 1, HeadphoneVolume = 2, MonitorMix = 3 };

struct State {
    bool micMuted = false;
    bool hpMuted = false;
    bool clipguard = false;
    int monitorMix = 0;        // raw byte, 0..91
    int monitorPercent = 0;    // percent of your own voice
    int dialValue = 0;
    DialMode dialMode = DialMode::MicGain;
    double micGainDb = 0.0;    // from feature unit 6
    double hpVolumeDb = 0.0;   // from feature unit 5
    // wValue=0x0001 is documented upstream as two uint32 level meters
    // (input, playback). It is not: the 8 bytes are one 4-byte value repeated,
    // and it responds to neither mic mute, mic gain nor playback. Kept as raw
    // telemetry of unidentified meaning. Real levels come from PipeWire.
    uint32_t telemetry = 0;
    std::vector<uint8_t> raw;
};

struct DeviceInfo {
    std::string apiVersion;
    std::string firmwareVersion;
};

// Why an operation failed. Busy is separated because it is routine: a poller
// and a one-shot command contend for the interface constantly, and the right
// response is to try again rather than to report a fault.
enum class Error { None, NotFound, Permission, Busy, Io, Protocol, Refused };

struct Result {
    Error error = Error::None;
    std::string message;
    explicit operator bool() const { return error == Error::None; }
};

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    // Locates the device via sysfs and opens its usbfs node. Does not claim.
    Result open();
    // Opens the Wave:3 whose ALSA capture node matches `alsaNodePrefix`
    // (PipeWire node name prefix, e.g. alsa_input.usb-Elgato_...). Empty
    // prefix opens the first Wave:3 found, same as open().
    Result openMatching(const std::string &alsaNodePrefix);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    // Interface 3 is exclusive, so hold it only while actually transferring.
    Result claim();
    void release();

    Result readState(State &out);
    Result readInfo(DeviceInfo &out);

    Result setClipguard(bool on);
    Result setMicMute(bool muted);
    Result setMicGainDb(double db);
    Result setHpMute(bool muted);
    // The headphone jack's own level, -60..0 dB. Goes out through ALSA for the
    // same reason setMicGainDb does.
    Result setHpVolumeDb(double db);
    // percent is the share of your own voice: 0 = all PC, 100 = all mic.
    Result setMonitorPercent(int percent);

    const std::string &nodePath() const { return nodePath_; }

    static int mixToPercent(int raw);
    static int percentToMix(int percent);

private:
    Result transfer(uint8_t requestType, uint8_t request, uint16_t value,
                    uint16_t index, void *data, uint16_t length);
    Result readConfig(std::vector<uint8_t> &out);
    Result writeConfig(const std::vector<uint8_t> &cfg);
    // Read-modify-write of exactly one byte, refusing protected offsets.
    Result setByte(int offset, uint8_t value);
    Result uacGet(uint8_t entity, uint8_t selector, void *out, uint16_t len);
    Result uacSet(uint8_t entity, uint8_t selector, const void *in, uint16_t len);

    int fd_ = -1;
    bool claimed_ = false;
    std::string nodePath_;
    std::optional<std::string> alsaCardIndex_;
};

// A claim guard: the interface is exclusive and must not be held across a
// blocking UI operation, so scope it tightly.
class ClaimGuard {
public:
    explicit ClaimGuard(Device &dev) : dev_(dev) { result_ = dev.claim(); }
    ~ClaimGuard() { if (result_) dev_.release(); }
    ClaimGuard(const ClaimGuard &) = delete;
    ClaimGuard &operator=(const ClaimGuard &) = delete;
    const Result &result() const { return result_; }
    explicit operator bool() const { return static_cast<bool>(result_); }

private:
    Device &dev_;
    Result result_;
};

}  // namespace waveline
