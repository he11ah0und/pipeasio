/*
 * DeviceEnumerator.cpp - implementation.
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
#include "DeviceEnumerator.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QHash>
#include <QStringList>

namespace DeviceEnumerator
{

QList<Device>
parsePwDump(const QByteArray &json)
{
    QList<Device> devices;

    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return devices;

    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;

        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();

        const QString mediaClass = props.value(QStringLiteral("media.class")).toString();
        const bool    isSink     = (mediaClass == QLatin1String("Audio/Sink"));
        const bool    isSource   = (mediaClass == QLatin1String("Audio/Source"));
        if (!isSink && !isSource)
            continue;

        const QString name = props.value(QStringLiteral("node.name")).toString();
        if (name.isEmpty())
            continue;

        QString description = props.value(QStringLiteral("node.description")).toString();
        if (description.isEmpty())
            description = props.value(QStringLiteral("node.nick")).toString();
        if (description.isEmpty())
            description = name;

        Device d;
        d.name        = name;
        d.description = description;
        d.isSink      = isSink;
        devices.append(d);
    }

    return devices;
}

QString
findOwnNode(const QByteArray &json)
{
    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return {};

    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;
        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();
        /* The driver sets pipeasio.node="1" (a string), but pw-dump serialises
         * that numeric-looking value as a JSON number, so accept both forms. */
        const QJsonValue marker = props.value(QStringLiteral("pipeasio.node"));
        if (marker.toString() == QLatin1String("1") || marker.toInt() == 1)
            return props.value(QStringLiteral("node.name")).toString();
    }
    return {};
}

Connections
resolveConnections(const QByteArray &json)
{
    Connections conn;

    QJsonParseError     err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return conn;

    const QJsonArray arr = doc.array();

    /* Pass 1: map every node id -> display name, and find our own node id via
     * the driver's "pipeasio.node" marker (set regardless of node_name). */
    QHash<int, QString> nodeDesc;
    int                 ownId = -1;
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Node"))
            continue;
        const int id = obj.value(QStringLiteral("id")).toInt(-1);
        if (id < 0)
            continue;
        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();
        QString d = props.value(QStringLiteral("node.description")).toString();
        if (d.isEmpty())
            d = props.value(QStringLiteral("node.nick")).toString();
        if (d.isEmpty())
            d = props.value(QStringLiteral("node.name")).toString();
        nodeDesc.insert(id, d);

        const QJsonValue marker = props.value(QStringLiteral("pipeasio.node"));
        if (marker.toString() == QLatin1String("1") || marker.toInt() == 1)
            ownId = id;
    }
    if (ownId < 0)
        return conn;

    /* Pass 2: links touching our node. A link FROM our node (output.node ==
     * ours) lands on a sink we play to; a link TO our node (input.node == ours)
     * comes from a source we capture from. Distinct peers, link order kept. */
    QStringList outs, ins;
    for (const QJsonValue &v : arr)
    {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        if (obj.value(QStringLiteral("type")).toString()
            != QLatin1String("PipeWire:Interface:Link"))
            continue;
        const QJsonObject props = obj.value(QStringLiteral("info"))
                                          .toObject()
                                          .value(QStringLiteral("props"))
                                          .toObject();
        const int outNode = props.value(QStringLiteral("link.output.node")).toInt(-1);
        const int inNode  = props.value(QStringLiteral("link.input.node")).toInt(-1);

        if (outNode == ownId && nodeDesc.contains(inNode))
        {
            const QString d = nodeDesc.value(inNode);
            if (!d.isEmpty() && !outs.contains(d))
                outs.append(d);
        }
        if (inNode == ownId && nodeDesc.contains(outNode))
        {
            const QString d = nodeDesc.value(outNode);
            if (!d.isEmpty() && !ins.contains(d))
                ins.append(d);
        }
    }

    conn.output = outs.join(QStringLiteral(", "));
    conn.input  = ins.join(QStringLiteral(", "));
    return conn;
}

QByteArray
runPwDump()
{
    QProcess proc;
    proc.start(QStringLiteral("pw-dump"), QStringList());
    if (!proc.waitForStarted(3000))
        return {};
    if (!proc.waitForFinished(5000))
    {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    return proc.readAllStandardOutput();
}

QList<Device>
enumerate()
{
    return parsePwDump(runPwDump());
}

} // namespace DeviceEnumerator
