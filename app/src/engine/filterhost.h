// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The one PipeWire connection every DSP filter node in this process shares.
//
// Each filter used to open its own: pw_filter_new_simple() on a fresh
// pw_thread_loop builds a private pw_context behind it, so a filter cost a
// context, a socket, a connection handshake, a real-time data thread and -- the
// part that turned out to matter -- a Client global in the PipeWire registry.
// A seven-channel graph is about a hundred filters, and measured on a Steam
// Deck that was 83 of the 98 PipeWire clients on the whole machine, against one
// for every other application put together.
//
// That is what a session manager pays for. WirePlumber walks its Lua hooks over
// clients as well as nodes, and the work is per client *and* per object, so a
// hundred clients multiply the entire graph rather than adding to it. Measured
// on the Deck: 80 extra clients carrying no nodes at all took wireplumber from
// 1.1% of a core to 15%, and a full graph pinned it outright.
//
// So: one connection, one client, and the filters are nodes on it. The engine
// keeps its own separate connection for registry and link work -- two clients
// in total -- because that loop is busy with graph events and should not be the
// lock a hundred filter startups queue behind.
//
// The scheduling is not lost with the threads. The context asks PipeWire for
// several data loops and it spreads nodes across them, so the DSP still runs on
// more than one core; what goes away is a hundred separate threads each woken
// by their own eventfd every quantum, which on the development machine was 8262
// context switches a second with the mixer sitting idle.

#pragma once

#include <string>

struct pw_core;
struct pw_thread_loop;

namespace waveline {

// Brings the shared connection up if it is not already, and hands back the loop
// and core to build filters on. Safe to call from several threads at once --
// channel chains are built in parallel -- and idempotent: the second caller
// gets the connection the first one made.
//
// Every filter must hold loop() locked around pw_filter_new(), pw_filter_add_
// port(), pw_filter_connect() and pw_filter_destroy(), exactly as it did with
// its own loop. The only thing that changes is that the loop is not its own to
// start or stop.
class FilterHost {
public:
    static bool start(std::string &error);
    // Tears the shared connection down. Only at shutdown, and only once every
    // filter has been stopped: destroying the core takes every filter node on
    // it with it.
    static void stop();

    // Null until start() has succeeded.
    static pw_thread_loop *loop();
    static pw_core *core();
};

}  // namespace waveline
