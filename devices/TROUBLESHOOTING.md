# SPDX-License-Identifier: GPL-2.0-or-later
# Device audio troubleshooting

This guide is for users and agents diagnosing a device that sometimes starts
with robotic, metallic, pitch-shifted, rubber-band, or crackling capture audio.
Do not add a workaround from another device without reproducing the problem and
testing one controlled change at a time.

## What this failure usually means

When one USB capture device drives the PipeWire graph, other capture devices
follow its clock through adaptive resampling. Some devices do not provide enough
capture data during startup for the default ALSA safety margin. The graph can
look completely correct while a follower starts in a bad resampler state:

- the correct capture device and links are present;
- every node reports `running`;
- rate, quantum, graph driver, and visible properties match a good startup;
- restarting or rebuilding the capture path may clear the sound.

This is not proof that every robotic-audio problem is a headroom problem. First
collect a baseline. User listening is the ground truth; logs alone must not be
used to label a trial good or robotic.

## 1. Record a baseline

From the repository root, run at least 20 startup trials:

```bash
WAVELINE_DIAG_DIR="$HOME/.local/share/waveline/capture-startup-diag-baseline" \
  ./scripts/capture-startup-diag.sh batch 20
```

Each restart is one trial. Listen to every listed input and label each one
`good` or `robotic`. Never reuse a diagnostic directory for a different
configuration because that mixes the populations.

Inspect the result again with:

```bash
WAVELINE_DIAG_DIR="$HOME/.local/share/waveline/capture-startup-diag-baseline" \
  ./scripts/capture-startup-diag.sh summary
```

Record:

- failures per device, not only failures per restart;
- which node was graph driver;
- graph rate and quantum;
- each device's effective `api.alsa.headroom`;
- whether multiple follower devices fail together.

The diagnostic's journal excerpt may include unrelated or cumulative PipeWire
resync messages. A resync count is supporting evidence, not a pass/fail signal.

## 2. Decide whether headroom is the right experiment

ALSA headroom is a device-specific amount of extra ring-buffer safety. It is a
good first experiment when:

- the failure is intermittent at daemon or capture-path startup;
- rebuilding or reopening the affected capture path usually fixes it;
- the graph is otherwise correctly wired;
- a stable graph driver remains clean while one or more followers fail.

Do not use headroom to cover up a wrong sample rate, wrong source, broken link,
USB disconnect, kernel error, or a device that is silent in raw ALSA.

## 3. Add an input-only WirePlumber rule

Create `wireplumber/51-waveline-<id>.conf` inside the affected device profile
and point `WIREPLUMBER_CONF` in its `device.conf` to that file.

Start with 512 configured samples:

```ini
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_input.<exact-device-prefix>.*" }
    ]
    actions = {
      update-props = {
        api.alsa.headroom = 512
      }
    }
  }
]
```

Rules must match the exact **input** prefix gathered from the machine. Do not
guess a node name, match every ALSA node, or modify the output node unless
separate evidence requires it.

Increase configured headroom in 512-sample steps only when the previous value
still fails:

```text
512 -> 1024 -> 1536 -> 2048
```

Headroom is per device and per environment. USB controller behavior, graph
quantum, sample rate, device firmware, and driver/follower role can all affect
the minimum. A value that fixes one microphone is not a global default.

PipeWire may report a negotiated effective value larger than the configured
value. On the test system, devices with a 512-frame period reported:

```text
configured default/0 -> effective 512
configured 512       -> effective 1024
configured 2048      -> effective 2560
```

Treat the runtime value as authoritative; do not assume every device negotiates
the same offset.

## 4. Install and verify the candidate

Install normally with the device plugged in, or force its profile:

```bash
sudo ./install.sh
# or:
sudo WAVELINE_PROFILES="<id>" ./install.sh
```

Verify the effective value before testing:

```bash
./scripts/capture-startup-diag.sh snapshot |
  jq '.mics[] | {name, headroom: .device.headroom}'
```

If the affected device still reports its old value, stop. The candidate is not
installed, so a test batch would be invalid.

## 5. Test one candidate in a fresh directory

Use a directory name that states the configured value:

```bash
WAVELINE_DIAG_DIR="$HOME/.local/share/waveline/capture-startup-diag-<id>-headroom-512" \
  ./scripts/capture-startup-diag.sh batch 20
```

Rules for a valid comparison:

1. Change only one variable between batches.
2. Keep the same devices connected and use the same graph rate and quantum.
3. Use a fresh `WAVELINE_DIAG_DIR`.
4. Label every device in every trial.
5. Preserve the baseline and candidate logs.
6. If a device fails once, that candidate has not achieved zero observed
   failures; increase only that device's headroom and repeat.

Twenty clean runs are a useful screen, not proof of a zero failure rate. With
zero failures, the approximate upper 95% bound is still 13.9% after 20 trials,
5.8% after 50, and 3.0% after 100.

## 6. Acceptance checks

Before calling the workaround fixed, complete the human test matrix in
[`VERIFY_BEFORE_POSTING.md`](VERIFY_BEFORE_POSTING.md). At minimum:

- establish a baseline that reproduced the failure;
- achieve at least 20/20 clean startups at the candidate value;
- for release confidence, achieve 50/50 clean startups (100/100 is better for
  an intermittent production bug);
- unplug/replug the device at least 10 times;
- use Waveline's Rebuild action at least 10 times;
- confirm software monitoring remains audible after startup, replug, and
  rebuild;
- confirm monitoring latency still feels acceptable;
- confirm unrelated inputs and all outputs remain working.

The publishing threshold is at least 99% per device and per category. Because
each required category contains 20 trials, that means 20/20 daemon startups,
20/20 physical hotplugs, and 20/20 Rebuild actions for every tested device.

Use the lowest value that passes the chosen confidence target **only if**
minimizing latency is important. If a larger tested value has no observable
downside, prefer its reliability margin over an unproven minimum.

## 7. What agents must report

An agent helping with this issue must include:

- device and exact PipeWire input-node match;
- baseline failures/trials per device;
- configured and effective headroom for each candidate;
- candidate failures/trials per device;
- graph driver, rate, and quantum;
- startup, replug, rebuild, monitoring, and latency results;
- the exact profile files changed.

Do not claim “0% failure rate” from a finite batch. Say “zero observed failures
in N trials.” Do not apply the winning value globally; keep it in the affected
device profile.

