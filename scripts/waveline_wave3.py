# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
"""Endpoint-0 transport for the Elgato Wave:3 vendor control interface.

Python 3 standard library only -- no libusb binding, no pyusb. Control
transfers go through the USBDEVFS_CONTROL ioctl on /dev/bus/usb/<bus>/<dev>.

The safety rules from docs/protocol.md are enforced here rather than left to
the caller's discipline:

  * request type is restricted to class IN (0xA1) and class OUT (0x21).
    Vendor type (0xC1/0x41) is refused -- a vendor-mode ID scan has already
    rebooted one of these devices into its bootloader.
  * wValue is restricted to the three IDs this firmware answers.
  * config offsets 2, 3 and 6 are refused for writing (unknown function),
    as are 0, 1, 7 and 12 (written by the firmware to report dial state).
  * writes go through a read-modify-write that re-reads immediately first,
    so a stale snapshot is never echoed back.

Nothing in this module claims an interface. Control transfers to interface 3
only require that no driver own it, which none does.
"""

import ctypes
import errno
import fcntl
import os
import struct

VID = 0x0FD9
PID = 0x0070
PID_DFU = 0x0071

IFACE = 3           # vendor-specific, 0xFF/0xF0, no endpoints, no driver
ENTITY = 0x33       # what the firmware looks at, in the wIndex high byte
WINDEX = (ENTITY << 8) | IFACE   # 0x3303

BM_CLASS_IN = 0xA1
BM_CLASS_OUT = 0x21
REQ_GET = 0x85
REQ_SET = 0x05

# The only wValue IDs this firmware answers, and their payload lengths.
ID_CONFIG = 0x0000
ID_METER = 0x0001
ID_INFO = 0x000A
KNOWN_IDS = {ID_CONFIG: 16, ID_METER: 8, ID_INFO: 51}

# UAC feature units, reachable with wIndex = (entity << 8) | IFACE
FU_HP = 5
FU_MIC = 6
UAC_MUTE = 0x01
UAC_VOLUME = 0x02
UAC_GET_CUR, UAC_SET_CUR = 0x81, 0x01
UAC_GET_MIN, UAC_GET_MAX, UAC_GET_RES = 0x82, 0x83, 0x84

# Config block offsets. See docs/protocol.md.
CFG_DIAL_LO, CFG_DIAL_HI = 0, 1
CFG_MIC_MUTE = 4
CFG_CLIPGUARD = 5
CFG_DIAL_FLAG = 7
CFG_HP_VOLUME = 8
CFG_HP_MUTE = 9
CFG_MONITOR_IND = 10   # companion/indicator: 0xAB while monitoring, 0 at all-PC
CFG_MONITOR_MIX = 11   # VERIFIED: 0 = all PC ... 91 = all mic (see docs)
CFG_DIAL_MODE = 12
CFG_UNKNOWN_13 = 13
CFG_UNKNOWN_14 = 14    # upstream called this the monitor mix; refuted
CFG_BRIGHTNESS = 15    # writable, reads back, but has no effect on this model

# Offset 11 range, taken from what the physical dial itself produces:
# fully counter-clockwise (all mic) = 91, centred = 41, fully clockwise
# (all PC) = 0. The byte accepts 0-255 with no clamping, but staying inside
# the hardware's own range is the conservative choice.
MIX_MIN, MIX_MAX = 0, 91

# Deprecated aliases. Upstream documented offsets 10/11/13 as an RGB ring
# colour; testing showed this model's ring is a fixed white level bar that
# turns red for mute, and that offset 11 is the monitor mix. Kept so older
# probe scripts keep working.
CFG_RING_R, CFG_RING_G, CFG_RING_B = 10, 11, 13

CFG_LEN = 16

# Refused for writing: unknown function, or firmware-owned live state.
CFG_RESERVED = {2, 3, 6}
CFG_FIRMWARE_OWNED = {CFG_DIAL_LO, CFG_DIAL_HI, CFG_DIAL_FLAG, CFG_DIAL_MODE}

CFG_NAMES = {
    0: "dial value lo (firmware)",
    1: "dial value hi (firmware)",
    2: "reserved",
    3: "reserved",
    4: "mic mute",
    5: "clipguard",
    6: "reserved",
    7: "dial flag (firmware)",
    8: "headphone volume (s8 dB)",
    9: "headphone mute",
    10: "monitor indicator",
    11: "monitor mix (0=PC, 91=mic)",
    12: "dial mode (firmware)",
    13: "unknown",
    14: "unknown (not the monitor mix)",
    15: "led brightness (no effect)",
}

DIAL_MODES = {1: "mic gain", 2: "headphone volume", 3: "monitor mix"}


class Wave3Error(RuntimeError):
    pass


class Wave3Busy(Wave3Error):
    """Another process holds interface 3 right now. Usually transient."""


class _CtrlTransfer(ctypes.Structure):
    """struct usbdevfs_ctrltransfer from linux/usbdevice_fs.h"""
    _fields_ = [
        ("bRequestType", ctypes.c_uint8),
        ("bRequest", ctypes.c_uint8),
        ("wValue", ctypes.c_uint16),
        ("wIndex", ctypes.c_uint16),
        ("wLength", ctypes.c_uint16),
        ("timeout", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
    ]


def _ioc(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (ord(typ) << 8) | nr


# USBDEVFS_CONTROL = _IOWR('U', 0, struct usbdevfs_ctrltransfer)
USBDEVFS_CONTROL = _ioc(3, "U", 0, ctypes.sizeof(_CtrlTransfer))
# Both take a pointer to an unsigned int holding the interface number.
USBDEVFS_CLAIMINTERFACE = _ioc(2, "U", 15, 4)
USBDEVFS_RELEASEINTERFACE = _ioc(2, "U", 16, 4)


def find_device():
    """Locate the Wave:3 by scanning sysfs. Returns (node_path, sysfs_path).

    Does not hardcode a bus/device number -- those change when the device is
    replugged or moved to another port.
    """
    base = "/sys/bus/usb/devices"
    dfu_seen = False
    for name in sorted(os.listdir(base)):
        d = os.path.join(base, name)
        try:
            with open(os.path.join(d, "idVendor")) as f:
                vid = int(f.read().strip(), 16)
            with open(os.path.join(d, "idProduct")) as f:
                pid = int(f.read().strip(), 16)
        except (OSError, ValueError):
            continue
        if vid != VID:
            continue
        if pid == PID_DFU:
            dfu_seen = True
            continue
        if pid != PID:
            continue

        # Refuse if a *kernel* driver has claimed the vendor interface -- it
        # would be managing the same config block underneath us.
        #
        # "usbfs" is not such a driver. The kernel binds usbfs to an interface
        # whenever a userspace process addresses a control transfer to it, so
        # seeing usbfs here just means another waveline-hw (or any libusb program) has
        # touched it, possibly one that already exited. Treating that as a
        # conflict made `waveline-hw --status` refuse to run whenever the sync service
        # was up. Genuine contention shows up as EBUSY at claim time instead,
        # which is handled where it happens.
        drv = os.path.join(d, f"{name}:1.{IFACE}", "driver")
        if os.path.exists(drv):
            drv_name = os.path.basename(os.path.realpath(drv))
            if drv_name != "usbfs":
                raise Wave3Error(
                    f"interface {IFACE} has kernel driver '{drv_name}' bound; "
                    "refusing to share the control interface"
                )

        with open(os.path.join(d, "busnum")) as f:
            bus = int(f.read().strip())
        with open(os.path.join(d, "devnum")) as f:
            dev = int(f.read().strip())
        return f"/dev/bus/usb/{bus:03d}/{dev:03d}", d

    if dfu_seen:
        raise Wave3Error(
            f"device is in DFU/bootloader mode ({VID:04x}:{PID_DFU:04x}). "
            "Unplug it for 30 seconds and replug. Nothing here writes firmware."
        )
    raise Wave3Error(f"Elgato Wave:3 ({VID:04x}:{PID:04x}) not found on USB")


class Wave3:
    """Open handle on the Wave:3 usbfs node."""

    def __init__(self, path=None, sysfs=None):
        if path is None:
            path, sysfs = find_device()
        self.path = path
        self.sysfs = sysfs
        try:
            self.fd = os.open(path, os.O_RDWR)
        except PermissionError:
            raise Wave3Error(
                f"{path} is not writable by this user.\n"
                "Install the udev rule and re-trigger:\n"
                "  sudo install -Dm644 devices/elgato/microphones/wave3/udev/60-waveline-wave3.rules "
                "/etc/udev/rules.d/\n"
                "  sudo udevadm control --reload-rules\n"
                "  sudo udevadm trigger --subsystem-match=usb "
                "--attr-match=idVendor=0fd9 --attr-match=idProduct=0070"
            ) from None

        self.claimed = False
        self.claim()

    def claim(self):
        """Take interface 3.

        Without an explicit claim the kernel claims it implicitly on the first
        interface-recipient transfer and logs "usbfs: process N did not claim
        interface 3 before use" every single time -- once per invocation, which
        turns into steady kernel-log noise for anything long-running.
        """
        if self.claimed:
            return
        num = ctypes.c_uint(IFACE)
        try:
            fcntl.ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, num)
        except OSError as e:
            if e.errno == errno.EBUSY:
                raise Wave3Busy(
                    f"interface {IFACE} is held by another process "
                    "(another waveline-hw, or the waveline-sync service)"
                ) from None
            raise Wave3Error(f"claim interface {IFACE} failed: {e.strerror}") from None
        self.claimed = True

    def release(self):
        """Give interface 3 back, so other waveline-hw invocations can use it."""
        if not self.claimed or self.fd is None:
            return
        num = ctypes.c_uint(IFACE)
        try:
            fcntl.ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, num)
        except OSError:
            pass  # closing the fd releases it anyway
        self.claimed = False

    def close(self):
        if getattr(self, "fd", None) is not None:
            self.release()
            os.close(self.fd)
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---------------------------------------------------------- raw transport

    def _xfer(self, bm_request_type, b_request, w_value, w_index, buf, timeout=1000):
        if bm_request_type not in (BM_CLASS_IN, BM_CLASS_OUT):
            raise Wave3Error(
                f"refusing bmRequestType 0x{bm_request_type:02x}: only class "
                "requests (0xA1/0x21) are permitted. Vendor-type requests have "
                "rebooted this device into its bootloader."
            )
        xfer = _CtrlTransfer(
            bRequestType=bm_request_type,
            bRequest=b_request,
            wValue=w_value,
            wIndex=w_index,
            wLength=len(buf),
            timeout=timeout,
            data=ctypes.cast(buf, ctypes.c_void_p),
        )
        try:
            n = fcntl.ioctl(self.fd, USBDEVFS_CONTROL, xfer)
        except OverflowError:
            # Older CPython wants the request as a signed value.
            n = fcntl.ioctl(self.fd, USBDEVFS_CONTROL - (1 << 32), xfer)
        except OSError as e:
            raise Wave3Error(
                f"control transfer failed: {e.strerror} (errno {e.errno}); "
                f"bRequest=0x{b_request:02x} wValue=0x{w_value:04x}"
            ) from None
        return n

    # ------------------------------------------------------- class ID reads

    def read_id(self, w_value):
        """Read one of the three known class IDs."""
        if w_value not in KNOWN_IDS:
            raise Wave3Error(
                f"refusing wValue 0x{w_value:04x}: only "
                + ", ".join(f"0x{k:04x}" for k in sorted(KNOWN_IDS))
                + " are permitted. Scanning IDs is what triggered a DFU reset."
            )
        length = KNOWN_IDS[w_value]
        buf = ctypes.create_string_buffer(length)
        n = self._xfer(BM_CLASS_IN, REQ_GET, w_value, WINDEX, buf)
        if n != length:
            raise Wave3Error(
                f"short read on wValue 0x{w_value:04x}: got {n} of {length} bytes"
            )
        return bytes(buf.raw[:length])

    def read_config(self):
        return bytearray(self.read_id(ID_CONFIG))

    def read_meter(self):
        raw = self.read_id(ID_METER)
        inp, play = struct.unpack("<II", raw)
        return inp, play

    def read_info(self):
        raw = self.read_id(ID_INFO)
        serial = raw[27:47].split(b"\x00")[0].decode("ascii", "replace").strip()
        return {
            "api_version": f"{raw[0]}.{raw[1]}",
            "firmware_version": f"{raw[6]}.{raw[7]}.{raw[8]}",
            "serial": serial,
            "raw": raw,
        }

    # --------------------------------------------------------- config writes

    def write_config(self, cfg):
        """Write all 16 bytes. Callers should use set_byte() instead."""
        if len(cfg) != CFG_LEN:
            raise Wave3Error(f"config must be {CFG_LEN} bytes, got {len(cfg)}")
        buf = ctypes.create_string_buffer(bytes(cfg), CFG_LEN)
        n = self._xfer(BM_CLASS_OUT, REQ_SET, ID_CONFIG, WINDEX, buf)
        if n != CFG_LEN:
            raise Wave3Error(f"short write: {n} of {CFG_LEN} bytes")
        return n

    def set_byte(self, offset, value, allow_firmware_owned=False):
        """Read-modify-write a single config byte.

        Re-reads immediately before writing so firmware-owned bytes carry
        current values rather than a stale snapshot. Returns (before, after)
        copies of the full block, or (cfg, None) if no write was needed.
        """
        if not 0 <= offset < CFG_LEN:
            raise Wave3Error(f"offset {offset} out of range")
        if offset in CFG_RESERVED:
            raise Wave3Error(
                f"refusing to write offset {offset}: function unknown. "
                '"No observed effect" is not the same as no effect.'
            )
        if offset in CFG_FIRMWARE_OWNED and not allow_firmware_owned:
            raise Wave3Error(
                f"offset {offset} ({CFG_NAMES[offset]}) is written by the "
                "firmware to report live dial state; writing it fights the "
                "dial. Pass allow_firmware_owned=True only for a deliberate "
                "experiment."
            )
        if not 0 <= value <= 0xFF:
            raise Wave3Error(f"value {value} out of range 0-255")

        before = self.read_config()
        if before[offset] == value:
            return before, None
        after = bytearray(before)
        after[offset] = value
        self.write_config(after)
        return before, after


def decode_config(cfg):
    """Human-readable view of the 16-byte config block."""
    dial_mode = cfg[CFG_DIAL_MODE]
    hp_vol = cfg[CFG_HP_VOLUME]
    if hp_vol > 127:
        hp_vol -= 256
    raw_mix = cfg[CFG_MONITOR_MIX]
    return {
        "dial_value": cfg[CFG_DIAL_LO] | (cfg[CFG_DIAL_HI] << 8),
        "dial_mode": dial_mode,
        "dial_mode_name": DIAL_MODES.get(dial_mode, f"unknown ({dial_mode})"),
        "dial_flag": cfg[CFG_DIAL_FLAG],
        "mic_mute": bool(cfg[CFG_MIC_MUTE]),
        "clipguard": bool(cfg[CFG_CLIPGUARD]),
        "hp_volume_db": hp_vol,
        "hp_mute": bool(cfg[CFG_HP_MUTE]),
        "monitor_mix": raw_mix,
        "monitor_pct": mix_to_pct(raw_mix),
        "monitor_indicator": cfg[CFG_MONITOR_IND],
        "brightness": cfg[CFG_BRIGHTNESS],
        # Retained so older scripts that read this key keep working; the RGB
        # interpretation itself is refuted.
        "ring_rgb": (cfg[CFG_RING_R], cfg[CFG_RING_G], cfg[CFG_RING_B]),
    }


def mix_to_pct(raw):
    """Monitor-mix byte -> percent of *your own voice* in the headphones."""
    return int(round(100.0 * min(raw, MIX_MAX) / MIX_MAX))


def pct_to_mix(pct):
    """Percent of your own voice -> monitor-mix byte, clamped to the dial's range."""
    pct = max(0, min(100, pct))
    return int(round(MIX_MIN + (MIX_MAX - MIX_MIN) * pct / 100.0))


def hexdump(cfg):
    return " ".join(f"{b:02x}" for b in cfg)


def annotate(cfg):
    return "\n".join(
        f"  [{i:2d}] 0x{b:02x} {b:3d}  {CFG_NAMES[i]}" for i, b in enumerate(cfg)
    )
