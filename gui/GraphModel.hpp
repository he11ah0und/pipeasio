/*
 * GraphModel.hpp - pure PipeWire graph model for the settings panel.
 *
 * Holds the node/link records and all the classification / connection /
 * profiler logic of PipeWireGraph with zero libpipewire dependency, so the
 * logic is unit-testable headless: tests replay recorded registry / node /
 * profiler event sequences and assert on the snapshots.  PipeWireGraph is a
 * thin adapter that translates pw callbacks into these calls.
 *
 * Not thread-safe by itself: PipeWireGraph calls it under the thread-loop
 * lock, tests call it directly.
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
#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <stdint.h>

class GraphModel
{
  public:
    using Props = QHash<QString, QString>;

    struct Device
    {
        QString name;           /* node.name */
        QString description;    /* node.description (fallback node.nick / node.name) */
        bool    isSink = false; /* true: Audio/Sink* (output); false: Audio/Source* */
    };

    /* What our own filter node (tagged "pipeasio.node"="1" by the driver) is
     * wired to: the sink our outputs feed and the source feeding our inputs.
     * Each side's *Detail carries the peer's codec/format/state second line
     * (empty when unknown, or when several peers share a side). */
    struct Connections
    {
        QString output;       /* sink name(s) our outputs feed */
        QString outputDetail; /* codec / rate / channels / state of a single sink */
        QString input;        /* source name(s) feeding our inputs */
        QString inputDetail;
    };

    /* Live DSP stats for one node, from the daemon's Profiler interface
     * (the data pw-top renders).  found == false while the node is not
     * being measured (suspended / not driven). */
    struct ProfilerStats
    {
        bool    found   = false;
        int     quantum = 0; /* driver clock duration, frames per cycle */
        int     rate    = 0; /* driver clock rate, Hz */
        double  dspLoad = 0; /* busy time / cycle period, [0..1+] */
        long    xruns   = 0; /* cumulative xrun counter */
        QString state;       /* "R" running, "I" idle, "S" suspended, "E" error */
    };

    /* Parsed driver-level Profiler block: the cycle clock + error counters. */
    struct ProfilerClock
    {
        bool     haveClock = false;
        int64_t  duration  = 0; /* frames per cycle (the QUANT column) */
        uint32_t rateNum   = 0; /* period fraction numerator (1/rate) */
        uint32_t rateDenom = 1; /* period fraction denominator -> Hz */
        uint32_t xrunCount = 0;
    };

    /* Parsed per-node Profiler block (only the fields the stats need). */
    struct ProfilerBlock
    {
        uint32_t id       = 0;
        int64_t  awake    = 0;
        int64_t  finish   = 0;
        bool     hasXruns = false;
        uint32_t xruns    = 0;
    };

    /* Registry-style events; props are plain key/value pairs (the adapter
     * flattens spa_dict).  Unknown ids are created on update. */
    void addNode(uint32_t id, const Props &props);
    void updateNodeProps(uint32_t id, const Props &props);
    void setNodeState(uint32_t id, const QString &state); /* "running"/"idle"/... */
    void setNodeFormat(uint32_t id, int rate, int channels, const QString &sampleFmt);
    void removeNode(uint32_t id);
    void addLink(uint32_t id, uint32_t outNode, uint32_t inNode);
    void removeLink(uint32_t id);
    void clear();

    /* Profiler feed; latest measurement per matching target node wins. */
    void profilerClock(const ProfilerClock &clock);
    void profilerBlock(const ProfilerBlock &block);

    /* Snapshots (same shapes the panel renders). */
    QList<Device> audioDevices() const;
    QString       ownNodeName() const;
    Connections   ownConnections() const;

    /* Stats of the driver's own node (nodeName empty) or of the node whose
     * node.name equals nodeName; the name also re-targets future blocks. */
    ProfilerStats profilerStats(const QString &nodeName = QString());

  private:
    struct NodeRec
    {
        QString name;           /* node.name */
        QString description;    /* node.description / node.nick fallback */
        QString mediaClass;     /* media.class */
        QString codec;          /* api.bluez5.codec */
        QString deviceApi;      /* device.api */
        QString state;          /* "running" / "idle" / ... (bound nodes only) */
        QString sampleFmt;      /* negotiated sample format, e.g. "F32P" */
        int     rate       = 0; /* negotiated rate (bound nodes only) */
        int     channels   = 0; /* negotiated channels, else audio.channels prop */
        bool    isPipeAsio = false;
    };

    struct LinkRec
    {
        uint32_t outNode = 0;
        uint32_t inNode  = 0;
    };

    void applyProps(NodeRec &rec, const Props &props) const;

    QHash<uint32_t, NodeRec> m_nodes;
    QHash<uint32_t, LinkRec> m_links;

    ProfilerClock m_clock;
    ProfilerStats m_prof;
    uint32_t      m_profId = ~0u; /* node id the stored stats belong to */
    QString       m_profName;     /* manual node_name target (else marker) */
};
