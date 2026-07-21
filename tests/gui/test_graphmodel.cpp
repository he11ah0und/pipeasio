/*
 * test_graphmodel.cpp - headless GraphModel tests: recorded event fixtures.
 *
 * Replaces the coverage the pw-dump / pw-top parser fixtures gave before the
 * panel went event-driven: registry globals (incl. the virtual-device and
 * own-node cases), links -> connections, and profiler samples -> pw-top
 * stats.  Events are fed straight to the model; no PipeWire daemon, no Qt
 * event loop.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "GraphModel.hpp"

#include <cmath>
#include <cstdio>

static int g_checks, g_failed;

#define CHECK(cond)                                                                              \
    do                                                                                           \
    {                                                                                            \
        g_checks++;                                                                              \
        if (!(cond))                                                                             \
        {                                                                                        \
            g_failed++;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                        \
    } while (0)

#define GROUP(name) fprintf(stderr, "[group] %s\n", name);

static GraphModel::Props
props(std::initializer_list<std::pair<const char *, const char *>> kv)
{
    GraphModel::Props p;
    for (const auto &e : kv)
        p.insert(QString::fromUtf8(e.first), QString::fromUtf8(e.second));
    return p;
}

/* A recorded desktop session: hardware sink+source, the EasyEffects virtual
 * pair, the driver's own node (FL64) and its wiring.  Mirrors what the
 * registry/node events deliver on a real easyeffects+FL Studio setup. */
static void
feedDesktopSession(GraphModel &m)
{
    m.addNode(40, props({{ "node.name", "alsa_output.pci-analog" },
                         { "node.description", "Built-in Audio Analog Stereo" },
                         { "media.class", "Audio/Sink" },
                         { "device.api", "alsa" },
                         { "audio.channels", "2" }}));
    m.addNode(41, props({{ "node.name", "alsa_input.pci-mic" },
                         { "node.description", "Built-in Mic" },
                         { "media.class", "Audio/Source" },
                         { "device.api", "alsa" },
                         { "audio.channels", "2" }}));
    m.addNode(50, props({{ "node.name", "easyeffects_sink" },
                         { "node.description", "Easy Effects Sink" },
                         { "media.class", "Audio/Sink/Virtual" },
                         { "device.api", "pipewire" },
                         { "audio.channels", "2" }}));
    m.addNode(51, props({{ "node.name", "easyeffects_source" },
                         { "node.nick", "Easy Effects Source" },
                         { "media.class", "Audio/Source/Virtual" },
                         { "device.api", "pipewire" },
                         { "audio.channels", "2" }}));
    m.addNode(70, props({{ "node.name", "FL64" },
                         { "node.description", "FL Studio" },
                         { "media.class", "Stream/Output/Audio" },
                         { "pipeasio.node", "1" }}));

    /* Bound-node events: state + negotiated format. */
    m.setNodeState(50, "running");
    m.setNodeFormat(50, 48000, 2, "F32P");
    m.setNodeState(51, "running");
    m.setNodeFormat(51, 48000, 2, "F32P");
    m.setNodeState(70, "running");
    m.setNodeFormat(70, 48000, 2, "F32P");

    /* FL64 -> easyeffects_sink (playback), easyeffects_source -> FL64 (mic). */
    m.addLink(90, 70, 50);
    m.addLink(91, 70, 50);
    m.addLink(92, 51, 70);
    m.addLink(93, 51, 70);
}

int
main(void)
{
    GROUP("audioDevices: classes, virtual devices, sorting")
    {
        GraphModel m;
        feedDesktopSession(m);
        const auto devs = m.audioDevices();
        CHECK(devs.size() == 4);
        /* Sorted by node.name; Stream/Output/Audio node is not a device. */
        CHECK(devs[0].name == "alsa_input.pci-mic" && !devs[0].isSink);
        CHECK(devs[1].name == "alsa_output.pci-analog" && devs[1].isSink);
        CHECK(devs[2].name == "easyeffects_sink" && devs[2].isSink);
        CHECK(devs[3].name == "easyeffects_source" && !devs[3].isSink);
        /* Virtual classes must not be filtered out (the easyeffects bug). */
        bool foundVirtualSource = false;
        for (const auto &d : devs)
            if (d.name == "easyeffects_source")
                foundVirtualSource = true;
        CHECK(foundVirtualSource);
        /* description fallback: node.nick when node.description is absent. */
        CHECK(devs[3].description == "Easy Effects Source");
    }

    GROUP("own node: marker detection, rename via prop update")
    {
        GraphModel m;
        CHECK(m.ownNodeName().isEmpty()); /* no nodes yet */
        feedDesktopSession(m);
        CHECK(m.ownNodeName() == "FL64");
        /* The node got renamed (config node_name): marker stays authoritative. */
        m.updateNodeProps(70, props({{ "node.name", "PipeASIO" },
                                     { "node.description", "PipeASIO" },
                                     { "media.class", "Stream/Output/Audio" },
                                     { "pipeasio.node", "1" }}));
        CHECK(m.ownNodeName() == "PipeASIO");
        /* Removing the node drops it from devices and own-node lookups. */
        m.removeNode(70);
        CHECK(m.ownNodeName().isEmpty());
    }

    GROUP("own connections: sink/source sides, detail line, multi-peer")
    {
        GraphModel m;
        feedDesktopSession(m);
        const auto conn = m.ownConnections();
        CHECK(conn.output == "Easy Effects Sink");
        CHECK(conn.input == "Easy Effects Source");
        /* Single peer: detail carries rate/channels+format/state. */
        CHECK(conn.outputDetail.contains("48000 Hz"));
        CHECK(conn.outputDetail.contains("2 ch F32P"));
        CHECK(conn.outputDetail.contains("running"));

        /* Second sink wired to the same side: names join, detail goes away. */
        m.addLink(94, 70, 40);
        const auto conn2 = m.ownConnections();
        CHECK(conn2.output.contains("Easy Effects Sink"));
        CHECK(conn2.output.contains("Built-in Audio Analog Stereo"));
        CHECK(conn2.outputDetail.isEmpty());

        /* Links to unknown nodes are ignored; removing links empties the side. */
        m.addLink(95, 70, 999);
        m.removeLink(90);
        m.removeLink(91);
        m.removeLink(94);
        m.removeLink(95);
        const auto conn3 = m.ownConnections();
        CHECK(conn3.output.isEmpty());
        CHECK(conn3.input == "Easy Effects Source");
    }

    GROUP("bluetooth peer detail: codec pretty name")
    {
        GraphModel m;
        m.addNode(70, props({{ "node.name", "FL64" }, { "pipeasio.node", "1" }}));
        m.addNode(60, props({{ "node.name", "bluez_output.aa_bb" },
                             { "node.description", "BT Headphones" },
                             { "media.class", "Audio/Sink" },
                             { "device.api", "bluez5" },
                             { "api.bluez5.codec", "aptx_hd" }}));
        m.setNodeState(60, "idle");
        m.addLink(90, 70, 60);
        const auto conn = m.ownConnections();
        CHECK(conn.output == "BT Headphones");
        CHECK(conn.outputDetail.contains("aptX HD"));
        CHECK(conn.outputDetail.contains("idle"));
    }

    GROUP("profiler: clock + block -> pw-top stats, state letter, targeting")
    {
        GraphModel m;
        feedDesktopSession(m);

        /* 256 frames @ 48 kHz: period = 1e9 * 256 * 1/48 ns. */
        GraphModel::ProfilerClock clock;
        clock.haveClock = true;
        clock.duration  = 256;
        clock.rateNum   = 1;
        clock.rateDenom = 48000;
        clock.xrunCount = 7;
        m.profilerClock(clock);

        /* A block for an unrelated node is ignored. */
        GraphModel::ProfilerBlock other;
        other.id       = 40;
        other.awake    = 1000;
        other.finish   = 2000;
        other.hasXruns = true;
        other.xruns    = 99;
        m.profilerBlock(other);
        CHECK(!m.profilerStats().found);

        /* The own node: busy = 1/4 of the period -> dspLoad 0.25. */
        const double periodNs  = 1e9 * 256.0 / 48000.0;
        const int64_t baseNs   = 1000000;
        GraphModel::ProfilerBlock own;
        own.id       = 70;
        own.awake    = baseNs;
        own.finish   = baseNs + (int64_t)(periodNs / 4);
        own.hasXruns = false; /* fall back to the driver clock counter */
        m.profilerBlock(own);

        const auto st = m.profilerStats();
        CHECK(st.found);
        CHECK(st.quantum == 256);
        CHECK(st.rate == 48000);
        CHECK(std::fabs(st.dspLoad - 0.25) < 1e-6);
        CHECK(st.xruns == 7);
        CHECK(st.state == "R"); /* node 70 is "running" */

        /* Per-block xruns win over the clock counter. */
        own.hasXruns = true;
        own.xruns    = 42;
        m.profilerBlock(own);
        CHECK(m.profilerStats().xruns == 42);

        /* Manual target by name (the Monitor tab's device picker). */
        m.setNodeState(50, "suspended");
        GraphModel::ProfilerBlock sink;
        sink.id     = 50;
        sink.awake  = baseNs;
        sink.finish = baseNs + (int64_t)(periodNs / 2);
        m.profilerStats("easyeffects_sink"); /* re-target */
        m.profilerBlock(sink);
        const auto st2 = m.profilerStats("easyeffects_sink");
        CHECK(st2.found);
        CHECK(std::fabs(st2.dspLoad - 0.5) < 1e-6);
        CHECK(st2.state == "S");

        /* Target node removed: stats reset, found clears. */
        m.removeNode(50);
        CHECK(!m.profilerStats("easyeffects_sink").found);
    }

    GROUP("clear() resets everything")
    {
        GraphModel m;
        feedDesktopSession(m);
        m.clear();
        CHECK(m.audioDevices().isEmpty());
        CHECK(m.ownNodeName().isEmpty());
        CHECK(m.ownConnections().output.isEmpty());
        CHECK(!m.profilerStats().found);
    }

    fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
