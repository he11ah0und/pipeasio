/*
 * PipeWireGraph.hpp - live PipeWire graph model for the settings panel.
 *
 * Replaces the pw-dump subprocess polling: a pw_thread_loop with a registry
 * listener tracks nodes and links as they appear/change/disappear, so the
 * Monitor connections and the device combos update event-driven with zero
 * JSON parsing.  EVERY node proxy is bound: the registry global carries only
 * a generic prop subset for client nodes, so media.class and the driver's
 * "pipeasio.node" marker are only known from the node's own info->props;
 * binding also yields the state and the negotiated format (rate/channels/
 * sample format) via pw_node info/param events.
 *
 * All classification / connection / profiler logic lives in GraphModel (no
 * libpipewire dependency, unit-tested headless); this class is the adapter
 * that owns the pw objects and translates their callbacks into model calls.
 *
 * Threading: all PipeWire callbacks run on the thread-loop thread.  The
 * public snapshot API locks the loop, copies out, and is therefore safe to
 * call from the GUI thread.  changed() is emitted coalesced on the GUI
 * thread (queued), at most every ~150 ms.
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

#include "GraphModel.hpp"

#include <QList>
#include <QObject>
#include <QString>

class QTimer;
struct PipeWireGraphImpl;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_proxy;

class PipeWireGraph : public QObject
{
    Q_OBJECT
  public:
    /* Snapshot shapes are defined on the model so tests see the same types
     * the panel renders. */
    using Device        = GraphModel::Device;
    using Connections   = GraphModel::Connections;
    using ProfilerStats = GraphModel::ProfilerStats;

    explicit PipeWireGraph(QObject *parent = nullptr);
    ~PipeWireGraph() override;

    /* Connect to the default PipeWire daemon and start the thread loop.
     * No-op when already running or when the daemon is unreachable (all
     * snapshots are simply empty then). */
    void start();
    void stop();

    /* Snapshot: every node whose media.class starts with Audio/Sink or
     * Audio/Source (for the Settings tab combos). */
    QList<Device> audioDevices();

    /* node.name of the driver's own node ("pipeasio.node"="1"), "" if absent. */
    QString ownNodeName();

    /* Connections of the driver's own node (empty names when not wired). */
    Connections ownConnections();

    /* DSP stats of a node: the driver's own node when nodeName is empty,
     * else the node whose node.name equals nodeName. */
    ProfilerStats profilerStats(const QString &nodeName = QString());

  signals:
    /* Coalesced "something in the graph changed" hint for UI refresh. */
    void changed();

  private:
    void kickCoalesce(); /* restart the changed() coalesce timer (GUI thread) */

    friend struct PipeWireGraphImpl;
    PipeWireGraphImpl *m_impl;
    QTimer            *m_coalesce = nullptr;
};
