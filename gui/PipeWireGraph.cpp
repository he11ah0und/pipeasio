/*
 * PipeWireGraph.cpp - PipeWire adapter over GraphModel (see the .hpp).
 *
 * Owns the pw_thread_loop / context / registry / proxies and translates the
 * pw callbacks into GraphModel calls.  All state and logic live in the
 * model; everything here is plumbing.
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
#include "PipeWireGraph.hpp"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>
#include <pipewire/keys.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/profiler.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/hook.h>

namespace
{

/* One-time pw_init for the process. */
void
ensurePwInit()
{
    static QMutex m;
    static bool   done = false;
    QMutexLocker  lock(&m);
    if (!done)
    {
        pw_init(nullptr, nullptr);
        done = true;
    }
}

QString
nodeStateString(enum pw_node_state st)
{
    switch (st)
    {
    case PW_NODE_STATE_ERROR: return QStringLiteral("error");
    case PW_NODE_STATE_CREATING: return QStringLiteral("creating");
    case PW_NODE_STATE_SUSPENDED: return QStringLiteral("suspended");
    case PW_NODE_STATE_IDLE: return QStringLiteral("idle");
    case PW_NODE_STATE_RUNNING: return QStringLiteral("running");
    }
    return {};
}

QString
dictStr(const spa_dict *props, const char *key)
{
    const char *v = props ? spa_dict_lookup(props, key) : nullptr;
    return v ? QString::fromUtf8(v) : QString();
}

/* Flatten a spa_dict into the model's plain key/value props. */
GraphModel::Props
propsFromDict(const spa_dict *dict)
{
    GraphModel::Props out;
    if (!dict)
        return out;
    const spa_dict_item *item;
    spa_dict_for_each(item, dict)
        out.insert(QString::fromUtf8(item->key), QString::fromUtf8(item->value));
    return out;
}

} // namespace

/* Per-bound-node listener context: lets the pw_node events know which node
 * they fire for and owns the hook/proxy lifetime. */
struct NodeBinding
{
    PipeWireGraphImpl *impl;
    uint32_t             id;
    pw_proxy           *proxy;
    spa_hook             listener{};
};

struct PipeWireGraphImpl
{
    PipeWireGraph   *q;
    pw_thread_loop  *loop     = nullptr;
    pw_context      *context  = nullptr;
    pw_core         *core     = nullptr;
    pw_registry     *registry = nullptr;
    spa_hook         registryListener{};
    spa_hook         coreListener{};
    int              syncSeq  = 0;
    bool             syncDone = false;
    bool             running  = false;

    GraphModel                  model;
    QHash<uint32_t, NodeBinding *> bindings; /* every node is bound (see .hpp) */

    /* Profiler (pw-top's data source): one proxy to the daemon's Profiler
     * object. */
    pw_proxy  *profilerProxy = nullptr;
    uint32_t   profilerId    = SPA_ID_INVALID;
    spa_hook   profilerHook{};

    void noteChanged();
};

/* Queue a coalesced changed() emission on the GUI thread.  Loop thread. */
void
PipeWireGraphImpl::noteChanged()
{
    QPointer<PipeWireGraph> guard(q);
    QMetaObject::invokeMethod(
            q, [guard]() { if (guard) guard->kickCoalesce(); }, Qt::QueuedConnection);
}

/* --- node proxy events (loop thread) ------------------------------------- */

static void
graph_node_info(void *data, const pw_node_info *info)
{
    auto *binding = static_cast<NodeBinding *>(data);
    auto &model   = binding->impl->model;

    model.setNodeState(binding->id, nodeStateString(info->state));
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
        model.updateNodeProps(binding->id, propsFromDict(info->props));

    /* (Re)read the negotiated format whenever the param set changes. */
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS)
    {
        for (uint32_t i = 0; i < info->n_params; i++)
        {
            const spa_param_info &p = info->params[i];
            if (p.id == SPA_PARAM_Format && (p.flags & SPA_PARAM_INFO_READ))
                pw_node_enum_params(reinterpret_cast<pw_node *>(binding->proxy), 0,
                                    SPA_PARAM_Format, 0, 1, nullptr);
        }
    }
    binding->impl->noteChanged();
}

static void
graph_node_param(void *data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/,
                 const spa_pod *param)
{
    if (id != SPA_PARAM_Format || !param)
        return;
    auto *binding = static_cast<NodeBinding *>(data);

    spa_audio_info_raw raw{};
    if (spa_format_audio_raw_parse(param, &raw) < 0)
        return;
    const char *fmt = spa_debug_type_find_short_name(spa_type_audio_format, raw.format);
    binding->impl->model.setNodeFormat(binding->id, (int)raw.rate, (int)raw.channels,
                                       fmt ? QString::fromUtf8(fmt) : QString());
    binding->impl->noteChanged();
}

static const pw_node_events nodeEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .info    = graph_node_info,
        .param   = graph_node_param,
};

/* --- profiler events (loop thread): per-cycle driver measurements -------- */

static void
prof_process_info(const spa_pod *pod, GraphModel::ProfilerClock *clock)
{
    float    load[3];
    int32_t  xruns = 0;
    int64_t  count = 0;
    if (spa_pod_parse_struct(pod, SPA_POD_Long(&count), SPA_POD_Float(&load[0]),
                             SPA_POD_Float(&load[1]), SPA_POD_Float(&load[2]),
                             SPA_POD_Int(&xruns)) >= 0)
        clock->xrunCount = (uint32_t)xruns;
}

static void
prof_process_clock(const spa_pod *pod, GraphModel::ProfilerClock *clockOut)
{
    spa_io_clock clock;
    if (spa_pod_parse_struct(pod, SPA_POD_Int(&clock.flags), SPA_POD_Int(&clock.id),
                             SPA_POD_Stringn(clock.name, sizeof(clock.name)),
                             SPA_POD_Long(&clock.nsec), SPA_POD_Fraction(&clock.rate),
                             SPA_POD_Long(&clock.position), SPA_POD_Long(&clock.duration),
                             SPA_POD_Long(&clock.delay), SPA_POD_Double(&clock.rate_diff),
                             SPA_POD_Long(&clock.next_nsec)) >= 0)
    {
        clockOut->duration  = clock.duration;
        clockOut->rateNum   = clock.rate.num;
        clockOut->rateDenom = clock.rate.denom ? clock.rate.denom : 1;
        clockOut->haveClock = true;
    }
}

static void
prof_process_block(PipeWireGraphImpl *impl, const spa_pod *pod)
{
    GraphModel::ProfilerBlock block;
    char                     *name   = nullptr;
    int64_t                   prev_signal = 0, signal = 0;
    int32_t                   status = 0;
    struct spa_fraction       latency = SPA_FRACTION(0, 1);
    uint32_t                  xruns  = (uint32_t)-1;
    bool                      async  = false;

    if (spa_pod_parse_struct(pod, SPA_POD_Int(&block.id), SPA_POD_String(&name),
                             SPA_POD_Long(&prev_signal), SPA_POD_Long(&signal),
                             SPA_POD_Long(&block.awake), SPA_POD_Long(&block.finish),
                             SPA_POD_Int(&status), SPA_POD_Fraction(&latency),
                             SPA_POD_OPT_Int(&xruns), SPA_POD_OPT_Bool(&async)) < 0)
        return;
    block.hasXruns = xruns != (uint32_t)-1;
    block.xruns    = block.hasXruns ? xruns : 0;

    impl->model.profilerBlock(block);
    impl->noteChanged();
}

static void
graph_profiler_profile(void *data, const spa_pod *pod)
{
    auto               *impl = static_cast<PipeWireGraphImpl *>(data);
    struct spa_pod     *o;
    struct spa_pod_prop *p;

    SPA_POD_STRUCT_FOREACH(pod, o)
    {
        if (!spa_pod_is_object_type(o, SPA_TYPE_OBJECT_Profiler))
            continue;

        GraphModel::ProfilerClock clock;
        SPA_POD_OBJECT_FOREACH((struct spa_pod_object *)o, p)
        {
            switch (p->key)
            {
            case SPA_PROFILER_info:
                prof_process_info(&p->value, &clock);
                break;
            case SPA_PROFILER_clock:
                prof_process_clock(&p->value, &clock);
                impl->model.profilerClock(clock);
                break;
            case SPA_PROFILER_driverBlock:
            case SPA_PROFILER_followerBlock:
                prof_process_block(impl, &p->value);
                break;
            default:
                break;
            }
        }
    }
}

static const pw_profiler_events profilerEvents = {
        PW_VERSION_PROFILER_EVENTS,
        graph_profiler_profile,
};

/* --- registry events (loop thread) --------------------------------------- */

static void
graph_registry_global(void *data, uint32_t id, uint32_t /*permissions*/, const char *type,
                      uint32_t /*version*/, const spa_dict *props)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);

    if (!strcmp(type, PW_TYPE_INTERFACE_Node))
    {
        impl->model.addNode(id, propsFromDict(props));

        /* Bind EVERY node: the registry global carries only a generic prop
         * subset for client nodes (7 generic keys for our flatpak FL64), so
         * media.class and the driver's "pipeasio.node" marker are only known
         * from the node's own info->props.  Binding also brings the state and
         * the negotiated format.  (pw-dump does the same per dump.) */
        auto *proxy = static_cast<pw_proxy *>(
                pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Node,
                                 PW_VERSION_NODE, 0));
        if (proxy)
        {
            auto *binding = new NodeBinding{ impl, id, proxy, {} };
            impl->bindings.insert(id, binding);
            pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &binding->listener,
                                 &nodeEvents, binding);
        }
        impl->noteChanged();
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Link))
    {
        impl->model.addLink(id, dictStr(props, PW_KEY_LINK_OUTPUT_NODE).toUInt(),
                            dictStr(props, PW_KEY_LINK_INPUT_NODE).toUInt());
        impl->noteChanged();
    }
    else if (!strcmp(type, PW_TYPE_INTERFACE_Profiler) && !impl->profilerProxy)
    {
        /* The daemon loads module-profiler by default; bind its Profiler
         * object for per-cycle driver measurements (pw-top's data). */
        impl->profilerProxy = static_cast<pw_proxy *>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_PROFILER, 0));
        if (impl->profilerProxy)
        {
            impl->profilerId = id;
            pw_profiler_add_listener(reinterpret_cast<pw_profiler *>(impl->profilerProxy),
                                     &impl->profilerHook, &profilerEvents, impl);
        }
    }
}

static void
graph_registry_global_remove(void *data, uint32_t id)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);
    impl->model.removeNode(id);
    impl->model.removeLink(id);
    if (id == impl->profilerId)
    {
        pw_proxy_destroy(impl->profilerProxy);
        impl->profilerProxy = nullptr;
        impl->profilerId    = SPA_ID_INVALID;
    }
    if (NodeBinding *binding = impl->bindings.take(id))
    {
        pw_proxy_destroy(binding->proxy);
        delete binding;
    }
    impl->noteChanged();
}

static const pw_registry_events registryEvents = {
        .version        = PW_VERSION_REGISTRY_EVENTS,
        .global         = graph_registry_global,
        .global_remove  = graph_registry_global_remove,
};

/* --- core events: initial registry roundtrip ------------------------------ */

static void
graph_core_done(void *data, uint32_t id, int seq)
{
    auto *impl = static_cast<PipeWireGraphImpl *>(data);
    if (id == PW_ID_CORE && seq == impl->syncSeq)
    {
        impl->syncDone = true;
        pw_thread_loop_signal(impl->loop, false);
    }
}

static const pw_core_events coreEvents = {
        .version = PW_VERSION_CORE_EVENTS,
        .done    = graph_core_done,
};

/* --- public API ------------------------------------------------------------ */

PipeWireGraph::PipeWireGraph(QObject *parent) : QObject(parent), m_impl(new PipeWireGraphImpl)
{
    m_impl->q = this;

    /* Coalesce timer lives on the GUI thread; kicked by Impl::noteChanged. */
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    m_coalesce->setInterval(150);
    connect(m_coalesce, &QTimer::timeout, this, &PipeWireGraph::changed);
}

void
PipeWireGraph::kickCoalesce()
{
    /* Fire-and-cap, NOT restart: profiler events arrive per audio cycle
     * (~200/s) and a restarting timer would never fire.  One kick starts the
     * timer; further kicks while it runs are coalesced away. */
    if (!m_coalesce->isActive())
        m_coalesce->start();
}

PipeWireGraph::~PipeWireGraph()
{
    stop();
    delete m_impl;
}

void
PipeWireGraph::start()
{
    if (m_impl->running)
        return;
    ensurePwInit();

    m_impl->loop = pw_thread_loop_new("pipeasio-panel", nullptr);
    if (!m_impl->loop)
        return;
    m_impl->context = pw_context_new(pw_thread_loop_get_loop(m_impl->loop), nullptr, 0);
    if (!m_impl->context)
    {
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
        return;
    }
    /* Registers the Profiler interface client-side; without it binding the
     * daemon's Profiler object fails (unknown interface type). */
    pw_context_load_module(m_impl->context, PW_EXTENSION_MODULE_PROFILER, NULL, NULL);

    pw_thread_loop_lock(m_impl->loop);
    /* The daemon's Profiler object lives on the manager socket: plain clients
     * can see the global but binding it returns NULL.  Connect with manager
     * intention (what pw-top does); fall back to a plain connection. */
    m_impl->core = pw_context_connect(
            m_impl->context,
            pw_properties_new(PW_KEY_REMOTE_INTENTION, "manager", PW_KEY_REMOTE_NAME,
                              "[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]", NULL),
            0);
    if (!m_impl->core)
        m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (m_impl->core)
    {
        m_impl->registry = pw_core_get_registry(m_impl->core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(m_impl->registry, &m_impl->registryListener,
                                 &registryEvents, m_impl);
        pw_core_add_listener(m_impl->core, &m_impl->coreListener, &coreEvents,
                             m_impl);
        m_impl->running = true;

        /* Wait (bounded) for the initial registry fill so early callers like
         * the Settings tab combos see a complete graph, not an empty one. */
        m_impl->syncDone = false;
        m_impl->syncSeq  = pw_core_sync(m_impl->core, PW_ID_CORE, 0);
        pw_thread_loop_start(m_impl->loop);
        int tries = 4; /* 4 x 2 s worst case, normally milliseconds */
        while (!m_impl->syncDone && tries-- > 0)
            pw_thread_loop_timed_wait(m_impl->loop, 2);
        spa_hook_remove(&m_impl->coreListener);
    }
    pw_thread_loop_unlock(m_impl->loop);

    if (!m_impl->running)
    {
        pw_context_destroy(m_impl->context);
        m_impl->context = nullptr;
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
}

void
PipeWireGraph::stop()
{
    if (!m_impl->loop)
        return;
    if (m_impl->running)
    {
        pw_thread_loop_stop(m_impl->loop);
        m_impl->running = false;
    }

    pw_thread_loop_lock(m_impl->loop);
    spa_hook_remove(&m_impl->registryListener);
    if (m_impl->profilerProxy)
    {
        pw_proxy_destroy(m_impl->profilerProxy);
        m_impl->profilerProxy = nullptr;
        m_impl->profilerId    = SPA_ID_INVALID;
    }
    const auto bindings = m_impl->bindings;
    m_impl->bindings.clear();
    for (NodeBinding *binding : bindings)
    {
        pw_proxy_destroy(binding->proxy);
        delete binding;
    }
    if (m_impl->core)
        pw_core_disconnect(m_impl->core);
    m_impl->core     = nullptr;
    m_impl->registry = nullptr;
    m_impl->model.clear();
    pw_thread_loop_unlock(m_impl->loop);

    pw_context_destroy(m_impl->context);
    m_impl->context = nullptr;
    pw_thread_loop_destroy(m_impl->loop);
    m_impl->loop = nullptr;
}

QList<PipeWireGraph::Device>
PipeWireGraph::audioDevices()
{
    QList<Device> out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    out = m_impl->model.audioDevices();
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}

QString
PipeWireGraph::ownNodeName()
{
    QString out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    out = m_impl->model.ownNodeName();
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}

PipeWireGraph::Connections
PipeWireGraph::ownConnections()
{
    Connections conn;
    if (!m_impl->running)
        return conn;

    pw_thread_loop_lock(m_impl->loop);
    conn = m_impl->model.ownConnections();
    pw_thread_loop_unlock(m_impl->loop);
    return conn;
}

PipeWireGraph::ProfilerStats
PipeWireGraph::profilerStats(const QString &nodeName)
{
    ProfilerStats out;
    if (!m_impl->running)
        return out;

    pw_thread_loop_lock(m_impl->loop);
    out = m_impl->model.profilerStats(nodeName);
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}
