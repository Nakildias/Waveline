# SPDX-License-Identifier: GPL-2.0-or-later
# Verify before posting device support

This is the required human test before publishing, posting, or declaring a
Waveline device profile fixed. An agent cannot certify audio quality by reading
node state or logs; a human must listen to every device.

Run this checklist for each affected device after installing the exact profile
files that will be posted.

## Acceptance rule

The minimum accepted success rate is **99% per device and per test category**.
Do not combine multiple devices into one percentage because clean devices can
hide a broken one.

Each category below has 20 trials. With only 20 trials:

```text
20/20 = 100% -> accepted
19/20 =  95% -> rejected
```

Therefore every category must pass 20/20. If a larger test contains 100 or more
trials, 99.0% or higher is accepted. Do not round a value below 99% upward.

One failure means the tested configuration is not accepted. Fix it, install the
new candidate, and restart that category from trial 1.

## What counts as a failure

Mark the trial failed if any of these occur:

- robotic, metallic, pitch-shifted, rubber-band, crackling, or distorted input;
- silence or a frozen meter;
- software monitoring is enabled but cannot be heard;
- monitoring requires toggling off/on to recover;
- the wrong capture device is selected;
- Rebuild or hotplug silences unrelated outputs;
- recovery requires another Rebuild, daemon restart, or manual routing change.

Do not discard a failure as a fluke. Do not perform an unrecorded retry.

## Before testing

1. Install the candidate profile with `sudo ./install.sh`.
2. Confirm every affected device is connected and detected.
3. Verify the effective headroom and graph state:

   ```bash
   ./scripts/capture-startup-diag.sh snapshot |
     jq '.mics[] | {name, headroom: .device.headroom}'
   ```

4. Close applications that automatically reconnect or reroute devices unless
   they are intentionally part of the test.
5. Prepare a result sheet with one row per trial and one column per device.
6. Test Stream output and software Monitor output. Keep unrelated system
   playback running when practical so output loss is immediately obvious.

## Test A: 20 daemon startups

Use a fresh diagnostic directory for this exact candidate:

```bash
WAVELINE_DIAG_DIR="$HOME/.local/share/waveline/verify-<candidate>-startup" \
  ./scripts/capture-startup-diag.sh batch 20
```

For every restart:

1. Listen to every listed input.
2. Label every device honestly as `good` or `robotic`.
3. Confirm Stream and software Monitor audio are present.
4. Confirm unrelated playback still works.

Required result: **20/20 for every device**.

## Test B: 20 physical hotplugs per device

Test one physical device at a time. Do not restart Waveline or PipeWire between
trials.

For each of 20 trials:

1. Start with the device detected and sounding clean.
2. Physically unplug the device.
3. Confirm Waveline notices its removal without killing unrelated output.
4. Reconnect it and wait until Waveline detects it again.
5. Listen to that device through Stream and software Monitor paths.
6. Confirm monitoring recovered without toggling it off/on.
7. Confirm every other connected input and unrelated output still works.
8. Record pass or failure before continuing.

Required result: **20/20 for each physical device**.

## Test C: 20 Rebuild actions per device

Test one device strip at a time while all normal devices remain connected.

For each of 20 trials:

1. Start with the device sounding clean.
2. Press that device's **Rebuild** action once.
3. Wait for the rebuild to finish; do not press it repeatedly.
4. Listen through Stream and software Monitor paths.
5. Confirm monitoring recovered without toggling it off/on.
6. Confirm all other inputs and unrelated outputs still work.
7. Record pass or failure before continuing.

Required result: **20/20 for each device**.

## Human sign-off record

Post or preserve the following for every device:

```text
Device:
Profile ID:
PipeWire input node:
Configured headroom:
Effective headroom:
Graph rate / quantum / driver:

Daemon startup:  __ / 20  (__%)
Physical hotplug: __ / 20  (__%)
Rebuild action:   __ / 20  (__%)

Stream audio clean: yes/no
Software monitoring clean: yes/no
Monitoring latency acceptable: yes/no
Unrelated outputs survived: yes/no

Human tester:
Date:
Diagnostic/log locations:
```

Device support may be posted only when every applicable category is at least
99%, all qualitative checks are `yes`, and the exact tested configuration is
the one being published.

