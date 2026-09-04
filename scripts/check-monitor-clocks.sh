#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Shows which driver (clock) each audio sink is scheduled by, and checks that no
# monitor loopback has been linked to itself.
#
# A Monitor fan-out puts every target device in one driver group on one elected
# clock. That is module-loopback working as designed -- separating the ends was
# tried and made it worse (see addPath() in app/src/engine/pwengine.cpp) -- so
# this script is for seeing which device holds the clock, not for expecting
# them to be apart.
#
# A suspended (idle) sink reports "(self)" because it is not scheduled at all.
# Play something to a device before trusting its line.
pw-dump | python3 -c '
import json,sys
d=json.load(sys.stdin)
nodes={x["id"]:x.get("info",{}).get("props",{})
       for x in d if x.get("type")=="PipeWire:Interface:Node"}
name=lambda i: nodes.get(i,{}).get("node.name","?")

print(f"{"NODE":<46} {"STATE":<10} {"HEADROOM":<9} DRIVEN BY")
state={x["id"]:x.get("info",{}).get("state","?") for x in d
       if x.get("type")=="PipeWire:Interface:Node"}
for i,p in nodes.items():
    nm=p.get("node.name","")
    if not (nm.startswith("alsa_output") or nm.startswith("waveline-monitor")):
        continue
    drv=p.get("node.driver-id")
    dn=name(drv) if drv is not None else "(self)"
    # A follower with headroom 0 has no slack to absorb cycle jitter and is the
    # one that crackles under load; the driver never needs any.
    hr=p.get("api.alsa.headroom")
    print(f"{nm[:46]:<46} {state.get(i,"?"):<10} {str(hr if hr is not None else "-"):<9} {dn[:46]}")

# node.link-group is split per end so each device keeps its own clock, which
# means the session manager no longer knows the two ends of a loopback belong
# together. If it ever links one to the other, that is a feedback loop.
loops=[]
for x in d:
    if x.get("type")!="PipeWire:Interface:Link": continue
    p=x.get("info",{}).get("props",{})
    o,i=name(p.get("link.output.node")),name(p.get("link.input.node"))
    if o.startswith("waveline-monitor-out") and i.startswith("waveline-monitor-out"):
        loops.append(f"{o} -> {i}")
print()
print("loopback fed back into itself:", "; ".join(loops) if loops else "none")
'
