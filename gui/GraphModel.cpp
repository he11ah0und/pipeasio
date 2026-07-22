/*
 * GraphModel.cpp - see GraphModel.hpp for the module description.
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

#include <algorithm>

namespace
{

QString
prettyBtCodec(const QString &codec)
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("sbc"), QStringLiteral("SBC") },
        { QStringLiteral("sbc_xq"), QStringLiteral("SBC-XQ") },
        { QStringLiteral("aac"), QStringLiteral("AAC") },
        { QStringLiteral("aptx"), QStringLiteral("aptX") },
        { QStringLiteral("aptx_hd"), QStringLiteral("aptX HD") },
        { QStringLiteral("aptx_ll"), QStringLiteral("aptX LL") },
        { QStringLiteral("aptx_ll_duplex"), QStringLiteral("aptX LL") },
        { QStringLiteral("ldac"), QStringLiteral("LDAC") },
        { QStringLiteral("lc3"), QStringLiteral("LC3") },
        { QStringLiteral("faststream"), QStringLiteral("FastStream") },
        { QStringLiteral("opus_05"), QStringLiteral("Opus") },
    };
    return names.value(codec.toLower(), codec.toUpper());
}

/* pw-top's S column letter for a node state string. */
QString
stateLetter(const QString &state)
{
    if (state == QLatin1String("running"))
        return QStringLiteral("R");
    if (state == QLatin1String("idle"))
        return QStringLiteral("I");
    if (state == QLatin1String("suspended"))
        return QStringLiteral("S");
    if (state == QLatin1String("error"))
        return QStringLiteral("E");
    if (state == QLatin1String("creating"))
        return QStringLiteral("C");
    return state;
}

} // namespace

void
GraphModel::applyProps(NodeRec &rec, const Props &props) const
{
    rec.name        = props.value(QStringLiteral("node.name"));
    rec.description = props.value(QStringLiteral("node.description"));
    if (rec.description.isEmpty())
        rec.description = props.value(QStringLiteral("node.nick"));
    if (rec.description.isEmpty())
        rec.description = rec.name;
    rec.mediaClass   = props.value(QStringLiteral("media.class"));
    rec.codec        = props.value(QStringLiteral("api.bluez5.codec"));
    rec.deviceApi    = props.value(QStringLiteral("device.api"));
    const QString ch = props.value(QStringLiteral("audio.channels"));
    if (!ch.isEmpty())
        rec.channels = ch.toInt();
    rec.isPipeAsio = props.value(QStringLiteral("pipeasio.node")) == QLatin1String("1");
}

void
GraphModel::addNode(uint32_t id, const Props &props)
{
    NodeRec rec;
    applyProps(rec, props);
    m_nodes.insert(id, rec);
}

void
GraphModel::updateNodeProps(uint32_t id, const Props &props)
{
    applyProps(m_nodes[id], props);
}

void
GraphModel::setNodeState(uint32_t id, const QString &state)
{
    m_nodes[id].state = state;
}

void
GraphModel::setNodeFormat(uint32_t id, int rate, int channels, const QString &sampleFmt)
{
    NodeRec &rec  = m_nodes[id];
    rec.rate      = rate;
    rec.channels  = channels;
    rec.sampleFmt = sampleFmt;
}

void
GraphModel::removeNode(uint32_t id)
{
    m_nodes.remove(id);
    if (id == m_profId)
    {
        m_prof   = {};
        m_profId = ~0u;
    }
}

void
GraphModel::addLink(uint32_t id, uint32_t outNode, uint32_t inNode)
{
    m_links.insert(id, LinkRec{ outNode, inNode });
}

void
GraphModel::removeLink(uint32_t id)
{
    m_links.remove(id);
}

void
GraphModel::clear()
{
    m_nodes.clear();
    m_links.clear();
    m_clock  = {};
    m_prof   = {};
    m_profId = ~0u;
    m_profName.clear();
}

QList<GraphModel::Device>
GraphModel::audioDevices() const
{
    QList<Device> out;
    for (const NodeRec &rec : m_nodes)
    {
        Device d;
        if (rec.mediaClass.startsWith(QLatin1String("Audio/Sink")))
            d.isSink = true;
        else if (rec.mediaClass.startsWith(QLatin1String("Audio/Source")))
            d.isSink = false;
        else
            continue;
        d.name        = rec.name;
        d.description = rec.description;
        out.append(d);
    }
    std::sort(out.begin(), out.end(),
              [](const Device &a, const Device &b) { return a.name < b.name; });
    return out;
}

QString
GraphModel::ownNodeName() const
{
    for (const NodeRec &rec : m_nodes)
        if (rec.isPipeAsio)
            return rec.name;
    return QString();
}

GraphModel::Connections
GraphModel::ownConnections() const
{
    Connections conn;

    uint32_t ownId = ~0u;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it)
        if (it->isPipeAsio)
        {
            ownId = it.key();
            break;
        }
    if (ownId == ~0u)
        return conn;

    /* A link FROM our node lands on a sink we play to; a link TO our node
     * comes from a source we capture from. */
    QList<uint32_t> outIds, inIds;
    for (const LinkRec &link : m_links)
    {
        if (link.outNode == ownId && m_nodes.contains(link.inNode) && !outIds.contains(link.inNode))
            outIds.append(link.inNode);
        if (link.inNode == ownId && m_nodes.contains(link.outNode) && !inIds.contains(link.outNode))
            inIds.append(link.outNode);
    }

    const auto describe = [](const NodeRec &rec, QString *name, QString *detail)
    {
        *name = rec.description;
        QStringList attrs;
        if (!rec.codec.isEmpty())
            attrs << prettyBtCodec(rec.codec);
        else if (rec.deviceApi == QLatin1String("bluez5"))
            attrs << QStringLiteral("Bluetooth");
        if (rec.rate > 0)
            attrs << QStringLiteral("%1 Hz").arg(rec.rate);
        if (rec.channels > 0)
        {
            QString ch = QStringLiteral("%1 ch").arg(rec.channels);
            if (!rec.sampleFmt.isEmpty())
                ch += QLatin1Char(' ') + rec.sampleFmt;
            attrs << ch;
        }
        if (!rec.state.isEmpty())
            attrs << rec.state;
        *detail = attrs.join(QStringLiteral(" · "));
    };

    const auto fill = [&](const QList<uint32_t> &ids, QString *name, QString *detail)
    {
        if (ids.isEmpty())
            return;
        if (ids.size() == 1)
        {
            describe(m_nodes.value(ids.first()), name, detail);
            return;
        }
        QStringList names;
        for (uint32_t id : ids)
        {
            QString n, d;
            describe(m_nodes.value(id), &n, &d);
            names << n;
        }
        *name = names.join(QStringLiteral(", "));
    };
    fill(outIds, &conn.output, &conn.outputDetail);
    fill(inIds, &conn.input, &conn.inputDetail);
    return conn;
}

void
GraphModel::profilerClock(const ProfilerClock &clock)
{
    m_clock = clock;
}

void
GraphModel::profilerBlock(const ProfilerBlock &block)
{
    const NodeRec rec      = m_nodes.value(block.id);
    const bool    isTarget = rec.isPipeAsio || (!m_profName.isEmpty() && rec.name == m_profName);
    if (!isTarget)
        return;

    m_profId       = block.id;
    m_prof.found   = true;
    m_prof.quantum = m_clock.haveClock ? (int)m_clock.duration : 0;
    /* The clock rate is the PERIOD fraction (1/48000), so the Hz rate is its
     * denominator; pw-top prints QUANT/RATE = duration/denom. */
    m_prof.rate = m_clock.haveClock ? (int)m_clock.rateDenom : 0;
    /* B/Q of pw-top: busy (finish - awake) over the cycle period. */
    const double busyNs = block.finish >= block.awake ? (double)(block.finish - block.awake) : 0.0;
    const double periodNs
            = m_clock.haveClock && m_clock.rateDenom
                      ? 1e9 * (double)m_clock.duration * m_clock.rateNum / m_clock.rateDenom
                      : 0.0;
    m_prof.dspLoad = periodNs > 0 ? busyNs / periodNs : 0.0;
    m_prof.xruns   = block.hasXruns ? (long)block.xruns : (long)m_clock.xrunCount;
}

GraphModel::ProfilerStats
GraphModel::profilerStats(const QString &nodeName)
{
    m_profName = nodeName; /* manual target for future blocks */

    uint32_t target = ~0u;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it)
        if (nodeName.isEmpty() ? it->isPipeAsio : it->name == nodeName)
        {
            target = it.key();
            break;
        }
    if (target != ~0u && target == m_profId && m_prof.found)
    {
        ProfilerStats out = m_prof;
        out.state         = stateLetter(m_nodes.value(target).state);
        return out;
    }
    return {};
}
