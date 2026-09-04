# SPDX-License-Identifier: GPL-2.0-or-later
# Waveline device profiles

This directory is the **only place** new hardware support belongs. Profiles are
discovered automatically — no installer edits for a normal new mic / mixer /
capture card.

Intermittent robotic, metallic, pitch-shifted, or rubber-band capture audio has
a separate evidence-driven guide: **[Device audio troubleshooting](TROUBLESHOOTING.md)**.
It covers baseline collection, per-device ALSA headroom tuning, clean A/B
batches, and the checks required before calling a workaround fixed.

Before publishing or posting device support, a human must complete
**[Verify before posting](VERIFY_BEFORE_POSTING.md)**: 20 daemon startups,
20 physical hotplugs per device, and 20 Rebuild actions per device, with at
least 99% success in every category.

---

## For users (and agents helping them)

Paste this into Claude / Cursor (or similar) with the device plugged in:

> Read `devices/README.md`, `devices/TROUBLESHOOTING.md`, and
> `devices/VERIFY_BEFORE_POSTING.md` in this repo end-to-end. Add Waveline
> support for my hardware: **&lt;full product name&gt;** (microphone / mixer /
> capture card). Follow the layout, fill `device.conf` correctly, add
> WirePlumber / udev / kernel bits only if evidence shows they are needed, run
> the agent testing checklist yourself, then walk me through the required human
> verification and wait for my results before calling it done.

The agent must **not** invent USB/PCI IDs, ALSA node names, or quirks. Gather
them from the machine with the device plugged in, or ask the user for command
output.

---

## Layout

```
devices/
  generic/                              # fallback — not under a brand
    device.conf
  <brand>/                              # e.g. elgato, logitech, fifine
    README.md
    microphones/<id>/                   # USB / analogue / webcam mics
      device.conf
      wireplumber/…                     # optional
      pipewire/…                        # optional
      udev/…                            # optional
      kernel/…                          # optional
    capture-cards/<id>/                 # HDMI / SDI capture (PCIe or USB)
      device.conf
    mixers/<id>/                        # hardware mixers / interfaces
      device.conf
```

| Category | Use for |
|----------|---------|
| `microphones/` | USB / analogue mics, headset mics, webcam mics |
| `capture-cards/` | HDMI / SDI capture (PCIe or USB) |
| `mixers/` | Hardware mixers / audio interfaces as their own profile |

Rules:

- `PROFILE_ID` **must** equal the directory name (`wave3`, `c922`, `4k60mk2`).
- Installer flags use that id: `WAVELINE_PROFILES=wave3`.
- Only create a category folder when you add a device into it.
- Empty brand directories (README only) are fine — they reserve the vendor name.
- Never put `device.conf` directly under the brand folder.
- Prefer an existing brand dir. New brands need `devices/<brand>/README.md`
  matching the others (point back here for the full format).

Examples already in-tree:

- Mic + quirks: `elgato/microphones/wave3/`
- Simple USB mic: `logitech/microphones/c922/`
- PCIe capture: `elgato/capture-cards/4k60mk2/`
- Fallback: `generic/`

---

## `device.conf` keys

Copy `devices/generic/device.conf` as a starting point. Keys are loaded by
`scripts/lib/profiles.sh` (`WAVELINE_PROFILE_KEYS`). Paths are **relative to
the profile directory**.

| Key | Required | Meaning |
|-----|----------|---------|
| `PROFILE_ID` | yes | Same as directory basename |
| `PROFILE_LABEL` | yes | Human name (“Elgato Wave:3”) |
| `BRAND` | yes | Short brand prefix on published nodes (“Wave:3 Stream Mix”) |
| `USB_IDS` | if USB | Space-separated `vid:pid` (lowercase hex), from `lsusb` |
| `PCI_IDS` | if PCIe | Space-separated `vid:pid` (e.g. `12ab:0710`) |
| `ALSA_NODE_MATCH` | usually for USB | PipeWire input node name **prefix** (no wildcards here) |
| `HARDWARE_CONTROLS` | yes | `1` only if Waveline talks a vendor USB protocol; else `0` |
| `HARDWARE_VENDOR_ID` / `HARDWARE_PRODUCT_ID` | if HC=1 | `0xVID` / `0xPID` for the daemon device layer |
| `KERNEL_PATCH` | optional | e.g. `kernel/apply.py` — leave empty unless proven needed |
| `WIREPLUMBER_CONF` | optional | e.g. `wireplumber/51-waveline-<id>.conf` |
| `PIPEWIRE_CONF` | **do not use** | A PipeWire drop-in is global. See the note below — no device profile should set this |
| `UDEV_RULES` | optional | e.g. `udev/60-waveline-<id>.rules`. Independent of `HARDWARE_CONTROLS`: a device with no vendor protocol may still need one, e.g. to set an ALSA control that comes up at an unusable value. Number it above `90` if it writes a mixer element, or `90-alsa-restore.rules` will overwrite it |
| `SYNC_SERVICE` | optional | `1` only if a userspace sync helper is required |

**Do not** install another device’s WirePlumber / udev / kernel workarounds onto
machines that do not have that device. Leave unused keys empty (`""` / `0`).

**Never set graph-wide policy from a device profile.** `PIPEWIRE_CONF` drops a
file into `pipewire.conf.d/`, where `context.properties` applies to *every*
device on the machine — so a setting written for one microphone silently
retunes all of them, and its effect depends on what else is plugged in. The
Wave:3 profile used to ship a quantum lock this way; it cost ~85 ms of latency
on every device and could only be removed by re-running the installer.

Graph clock policy has exactly one owner:
[`data/pipewire/50-waveline-clock.conf`](../data/pipewire/50-waveline-clock.conf),
installed on every machine, with the quantum reachable live from the mixer's
Latency control. Driver election has one owner too:
[`data/wireplumber/50-waveline-driver-policy.conf`](../data/wireplumber/50-waveline-driver-policy.conf).
If your device needs something from either, that is a change to those files and
a conversation, not a drop-in in your profile directory.

Specifically, do **not** put these in a device profile: `clock.rate`,
`clock.quantum` and its min/max, `clock.allowed-rates`, or `priority.driver`.
`node.latency` is also pointless in one — it is a quantum request, quantum is
negotiated per driver group rather than per node, and `node.lock-quantum` (which
every keep-alive profile sets) masks it anyway. Two profiles here set it for
months and neither device was any faster for it.

---

## Agent playbook — adding a device

### 1. Identify the hardware (device plugged in)

Run and record:

```bash
lsusb                    # USB: note vid:pid and product string
lspci -nn                # PCIe capture: note [vid:pid]
pw-cli ls Node | grep -i alsa_input
wpctl status
pactl list sources short
cat /proc/asound/cards
# For a USB card, also:
#   cat /proc/asound/card*/usbid
#   cat /proc/asound/card*/usbmixer   # if present
```

Ask the user for paste output if you cannot run commands on their machine.

Pick:

- **brand** directory (create + README if missing)
- **category**: `microphones` / `capture-cards` / `mixers`
- **id**: short lowercase, no spaces (`fifine_k669`, `wave3`, `4kpro`)

### 2. Create the profile tree

```bash
mkdir -p devices/<brand>/<category>/<id>
cp devices/generic/device.conf devices/<brand>/<category>/<id>/device.conf
```

Edit `device.conf`:

1. Set `PROFILE_ID`, `PROFILE_LABEL`, `BRAND`.
2. Set `USB_IDS` and/or `PCI_IDS` from step 1 (never guess).
3. Set `ALSA_NODE_MATCH` to the PipeWire **input** node prefix for USB devices
   (copy from `pw-cli` / `pactl`, strip any trailing instance suffix).
4. For PCIe cards whose PipeWire name is BDF-based, leave `ALSA_NODE_MATCH=""`
   and rely on `PCI_IDS` (see `elgato/capture-cards/4k60mk2`).
5. Keep `HARDWARE_CONTROLS="0"` unless you are implementing a real vendor
   protocol in the daemon (rare — Wave:3 is the reference).
6. Add the capture node prefix and display brand to
   `app/src/engine/masterbus.h` (`k*CapturePrefix` + `masterCaptureBrand()`).
   Without this, strips auto-name as **Input #N** even when `device.conf` has
   the right `BRAND` — the mixer matches PipeWire node names in C++, not from
   the profile file at runtime.

### 3. Add optional drop-ins only with evidence

| Symptom | Typical fix | Reference |
|---------|-------------|-----------|
| Mic goes silent after headphones / second stream | WirePlumber keep-alive on **input only** | `elgato/microphones/wave3/wireplumber/` |
| Intermittent robotic / pitch-shifted startup capture | Establish a baseline, then test per-device ALSA headroom | [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) |
| Rubber-band audio after suspend or quantum change | Keep-alive props (`node.lock-quantum`) on the input | `c922`, `wave3` |
| Mic "sounds good" out of the box and lags its own video | `waveline.hidden-latency = true` on the input — see below | `meet4k` |
| Userspace needs raw USB access | `udev` rule for the vid:pid | `wave3/udev/` |
| Kernel usb-audio bug unique to this device | `KERNEL_PATCH` + `kernel/apply.py` | `wave3/kernel/` |

#### Devices that hide their own latency

If your device processes audio before handing it over — a conference camera, a
headset with onboard AEC or noise suppression, anything whose microphone sounds
polished with no software help — add this to its input rule:

```
waveline.hidden-latency = true
```

The mixer then shows **N/A** for that input instead of a latency figure.

This is not modesty, it is accuracy. Waveline measures capture latency from
ALSA, which covers everything from the moment the device hands audio to your
machine and *nothing it did before that*. On a device doing its own DSP the
hidden part dominates: the OBSBOT Meet 4K measures among the fastest inputs on
its machine while being, by ear, clearly the slowest. Showing that figure does
not merely understate it — it sorts a slow microphone above a fast one, which
is worse than showing nothing.

The test for whether it belongs is not "is this device slow". It is **"is the
delay decided somewhere this machine cannot see"**. Use
`scripts/latency-offset-test.sh` to find out: it records your device and a
direct microphone straight from ALSA and cross-correlates a clap, which
measures the in-device delay that nothing else can reach.

Do not record that measurement as a number in the profile. It is a difference
against whichever reference you used, on one firmware, in one processing mode,
and a device that adapts its processing to the room has no constant to record.
A precise-looking number that is wrong is the failure this whole subsystem was
rebuilt to end.

WirePlumber pattern (input node only — do not touch the sink unless proven
necessary):

```
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_input.<exact-prefix>.*" }
    ]
    actions = {
      update-props = {
        session.suspend-timeout-seconds = 0
        node.pause-on-idle              = false
        node.always-process             = true
        node.lock-quantum               = true
      }
    }
  }
]
```

Name files `51-waveline-<id>.conf` / `60-waveline-<id>.rules` so they sort with
existing drop-ins.

### 4. Smoke-test discovery (agent)

From the repo root, with the device plugged in:

```bash
export WAVELINE_ROOT="$PWD"
source scripts/lib/profiles.sh

profile_resolve <id>          # must print the new directory
profile_load "$(profile_resolve <id>)"
echo "id=$PROFILE_ID label=$PROFILE_LABEL usb=$USB_IDS pci=$PCI_IDS"
profile_present && echo PRESENT || echo ABSENT   # must be PRESENT
profile_detect | grep -x <id>                    # must list this id
```

If `profile_present` fails, fix IDs before anything else.

### 5. Install / runtime checks (agent, when possible)

```bash
# After install or with an existing install pointed at this tree:
./install.sh                  # or re-run profile install path the project uses
systemctl --user restart wavelined
systemctl --user status wavelined --no-pager

# Profile the daemon thinks it is using:
grep -E '^(PROFILE_ID|PROFILE_LABEL|BRAND|ALSA_NODE_MATCH)=' \
  ~/.config/waveline/profile.conf

# Nodes / capture:
wpctl status
pactl list sources short | grep -iE 'waveline|stream|monitor|<id-or-brand>'
# Short capture smoke (replace SOURCE with the device or Waveline input):
#   pw-record --target SOURCE /tmp/waveline-hw-test.wav   # speak 3s, Ctrl+C
#   aplay /tmp/waveline-hw-test.wav
```

Confirm:

- Device is detected (not silently falling back to `generic` when it should match).
- Input Device strip / published names use `BRAND`.
- Capture produces non-silent audio.
- No unrelated devices picked up this profile’s rules.

If anything fails, fix the profile and re-test. Do **not** declare support complete
on “config looks right” alone.

### 6. User verification (required — agent must ask)

After agent checks pass, **ask the user** to confirm each item that applies.
Wait for their answers; iterate if something fails.

**All devices**

1. Unplug / replug (or reboot). Does Waveline still detect the device?
2. Speak / play into the device. Do Stream and Monitor meters move? Is audio clean?
3. Route the device into an app (OBS, browser, Audacity). Is the correct Waveline
   input / mix selected and audible?
4. Open `waveline-mixer`. Is the strip named with your `BRAND` / label, not Generic?

**Microphones**

5. Mute / unmute and gain on the device (if it has hardware controls). Expected?
6. If `HARDWARE_CONTROLS=1`: Clipguard / direct monitor / headphone jack behave?

**Capture cards**

5. Feed a known HDMI/SDI source. Is audio present and in sync enough for use?
6. Stop/start the video app once. Does audio recover without a reboot?

**Mixers / interfaces**

5. Each input you care about appears and can be routed independently (or as designed).
6. Hotplug of the interface does not brick PipeWire; recovery steps documented if needed.

Only after the user confirms the relevant items (or you document known gaps they
accepted) is the work done.

Before the profile is posted or published, complete the stricter
[`VERIFY_BEFORE_POSTING.md`](VERIFY_BEFORE_POSTING.md) human test matrix.

---

## What not to do

- Do not create empty `microphones/` / `capture-cards/` / `mixers/` folders “for later”.
- Do not copy Wave:3 kernel / udev / sync pieces onto a mic that does not need them.
- Do not set `HARDWARE_CONTROLS=1` without a working vendor protocol in the daemon.
- Do not change `install.sh` for a normal new profile — discovery is automatic.
- Do not commit secrets or machine-specific paths inside `device.conf`.

---

## Reference profiles

| Kind | Path |
|------|------|
| Full USB mic (quirks + HC) | `elgato/microphones/wave3/` |
| Simple USB mic | `logitech/microphones/c922/`, `obsbot/microphones/meet4k/` |
| Low-latency instrument input | `hercules/microphones/rocksmith-tone-cable/` |
| PCIe capture | `elgato/capture-cards/4k60mk2/`, `elgato/capture-cards/4kpro/` |
| Fallback | `generic/` |

Shared loader: `scripts/lib/profiles.sh` (`profile_dirs`, `profile_resolve`,
`profile_load`, `profile_detect`, `profile_present`).
