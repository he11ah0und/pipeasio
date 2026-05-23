/*
 * audio.c — native libpipewire-0.3 backend for PipeASIO.
 *
 * Phase 3 milestone M1: the PipeWire context and thread loop are stood
 * up here; spa_thread_utils is overridden so PipeWire's RT thread is
 * allocated via Win32 CreateThread, giving it a proper Wine TEB.
 * No pw_filter is created yet — ports and audio flow land in M2, and
 * registry-driven port discovery / autoconnect land in M3.
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 */

#include "audio.h"

#define WIN32_LEAN_AND_MEAN
#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

/* Force-print logging matching the convention asio.c uses, so messages
 * land in the user's stderr/stdout regardless of WINEDEBUG channel
 * configuration. */
#undef TRACE
#undef WARN
#undef ERR
#define TRACE(...)         do { } while (0)
#define WARN(fmt, ...)     do { fprintf(stdout, "[pipeasio] " fmt, ##__VA_ARGS__); } while (0)
#define ERR(fmt, ...)      do { fprintf(stderr, "[pipeasio] " fmt, ##__VA_ARGS__); } while (0)

#include <pipewire/pipewire.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(asio);

/* M1 placeholder defaults; M2 reads from registry / PipeWire metadata. */
#define AUDIO_M1_DEFAULT_SAMPLE_RATE  48000u
#define AUDIO_M1_DEFAULT_BUFFER_SIZE   1024u

/* SCHED_FIFO range used in M1.  M3 replaces this with values read from
 * libpipewire-module-rt's "rt.prio" / "rt.time.soft" properties so we
 * honor the user's rlimit_rtprio. */
#define AUDIO_M1_RT_PRIO_MIN  1
#define AUDIO_M1_RT_PRIO_MAX 80

/* ----------------------------------------------------------------------
 * Wine RT thread bridge — install custom spa_thread_utils on the data
 * loop so the audio thread is a CreateThread'd Wine thread, capable of
 * calling back into the ASIO host's COM methods.  PipeWire's pw_data_loop
 * spawns exactly one RT thread, so a single state slot is sufficient.
 * ---------------------------------------------------------------------- */

struct audio_rt_state {
    HANDLE       win_handle;                /* Win32 handle for join() */
    DWORD        win_tid;
    pthread_t    ptid;                      /* captured inside the spawned thread */
    int          rt_priority;               /* current SCHED_FIFO priority, 0 = none */
    atomic_bool  ready;                     /* released once ptid is captured */
    void      *(*user_entry)(void *);       /* PipeWire-provided entry */
    void        *user_arg;
    void        *user_ret;
};

static DWORD WINAPI audio_rt_trampoline(LPVOID raw)
{
    struct audio_rt_state *s = raw;
    s->ptid = pthread_self();
    atomic_store_explicit(&s->ready, true, memory_order_release);
    s->user_ret = s->user_entry(s->user_arg);
    return 0;
}

static struct spa_thread *
audio_rt_create(void *data, const struct spa_dict *props,
                void *(*entry)(void *), void *arg)
{
    struct audio_rt_state *s = data;
    (void)props;

    s->user_entry = entry;
    s->user_arg   = arg;
    atomic_store_explicit(&s->ready, false, memory_order_relaxed);

    s->win_handle = CreateThread(NULL, 0, audio_rt_trampoline, s, 0, &s->win_tid);
    if (!s->win_handle) {
        ERR("CreateThread failed for PipeWire RT thread\n");
        return NULL;
    }

    while (!atomic_load_explicit(&s->ready, memory_order_acquire))
        sched_yield();

    return (struct spa_thread *)(uintptr_t)s->ptid;
}

static int audio_rt_join(void *data, struct spa_thread *thread, void **retval)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (!s->win_handle)
        return -1;

    DWORD wait = WaitForSingleObject(s->win_handle, INFINITE);
    if (retval)
        *retval = s->user_ret;

    CloseHandle(s->win_handle);
    s->win_handle = NULL;
    return (wait == WAIT_OBJECT_0) ? 0 : -1;
}

static int audio_rt_get_range(void *data, const struct spa_dict *props,
                              int *min, int *max)
{
    (void)data; (void)props;
    *min = AUDIO_M1_RT_PRIO_MIN;
    *max = AUDIO_M1_RT_PRIO_MAX;
    return 0;
}

static int audio_rt_acquire(void *data, struct spa_thread *thread, int priority)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (priority <= 0)
        return 0;

    int err = pthread_setschedparam(s->ptid, SCHED_FIFO,
                                    &(struct sched_param){.sched_priority = priority});
    if (err) {
        WARN("pthread_setschedparam(SCHED_FIFO, %d) failed: %s\n",
             priority, strerror(err));
        return -1;
    }
    s->rt_priority = priority;
    return 0;
}

static int audio_rt_drop(void *data, struct spa_thread *thread)
{
    struct audio_rt_state *s = data;
    (void)thread;

    if (!s->rt_priority)
        return 0;

    int err = pthread_setschedparam(s->ptid, SCHED_OTHER,
                                    &(struct sched_param){.sched_priority = 0});
    if (err) {
        WARN("pthread_setschedparam(SCHED_OTHER) failed: %s\n", strerror(err));
        return -1;
    }
    s->rt_priority = 0;
    return 0;
}

static const struct spa_thread_utils_methods audio_rt_methods = {
    SPA_VERSION_THREAD_UTILS_METHODS,
    .create       = audio_rt_create,
    .join         = audio_rt_join,
    .get_rt_range = audio_rt_get_range,
    .acquire_rt   = audio_rt_acquire,
    .drop_rt      = audio_rt_drop,
};

/* ----------------------------------------------------------------------
 * Opaque types backing audio.h
 * ---------------------------------------------------------------------- */

struct audio_client {
    char                       *name;
    audio_nframes_t             sample_rate;
    audio_nframes_t             buffer_size;

    struct pw_thread_loop      *loop;
    struct pw_context          *ctx;
    struct pw_core             *core;

    struct audio_rt_state       rt;
    struct spa_thread_utils     rt_iface;

    audio_process_cb            process_cb;
    void                       *process_cb_arg;
    audio_buffer_size_cb        buffer_size_cb;
    void                       *buffer_size_cb_arg;
    audio_sample_rate_cb        sample_rate_cb;
    void                       *sample_rate_cb_arg;
    audio_latency_cb            latency_cb;
    void                       *latency_cb_arg;

    bool                        active;
};

struct audio_port {
    audio_client_t        *client;
    char                  *name;
    char                  *type;
    uint64_t               flags;
    uint32_t               channel_idx;
    audio_latency_range_t  latency[2];   /* [CAPTURE, PLAYBACK] */
};

/* ----------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

audio_client_t *audio_open(const char *client_name, uint32_t options, uint32_t *status)
{
    (void)options;   /* JACK-era flags do not map onto PipeWire */
    if (status) *status = 0;

    audio_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        ERR("out of memory allocating audio_client\n");
        if (status) *status = 1;
        return NULL;
    }

    c->name        = strdup(client_name ? client_name : "PipeASIO");
    c->sample_rate = AUDIO_M1_DEFAULT_SAMPLE_RATE;
    c->buffer_size = AUDIO_M1_DEFAULT_BUFFER_SIZE;
    atomic_init(&c->rt.ready, false);

    pw_init(NULL, NULL);

    c->loop = pw_thread_loop_new(c->name, NULL);
    if (!c->loop) {
        ERR("pw_thread_loop_new(%s) failed\n", c->name);
        goto fail_alloc;
    }

    c->ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->ctx) {
        ERR("pw_context_new failed\n");
        goto fail_loop;
    }

    /* Wire our Wine-thread spa_thread_utils into the data loop before
     * the RT thread is spawned (pw_thread_loop_start triggers it). */
    c->rt_iface.iface = SPA_INTERFACE_INIT(
        SPA_TYPE_INTERFACE_ThreadUtils, SPA_VERSION_THREAD_UTILS,
        &audio_rt_methods, &c->rt);
    pw_data_loop_set_thread_utils(pw_context_get_data_loop(c->ctx), &c->rt_iface);

    if (pw_thread_loop_start(c->loop) < 0) {
        ERR("pw_thread_loop_start failed\n");
        goto fail_ctx;
    }

    pw_thread_loop_lock(c->loop);
    c->core = pw_context_connect(c->ctx, NULL, 0);
    pw_thread_loop_unlock(c->loop);

    if (!c->core) {
        ERR("pw_context_connect failed (is the PipeWire daemon running?)\n");
        goto fail_started;
    }

    TRACE("audio_open(%s) -> %p\n", c->name, c);
    return c;

fail_started:
    pw_thread_loop_stop(c->loop);
fail_ctx:
    pw_context_destroy(c->ctx);
fail_loop:
    pw_thread_loop_destroy(c->loop);
fail_alloc:
    free(c->name);
    free(c);
    if (status) *status = 1;
    return NULL;
}

bool audio_close(audio_client_t *c)
{
    if (!c) return false;

    if (c->loop) {
        pw_thread_loop_lock(c->loop);
        if (c->core) {
            pw_core_disconnect(c->core);
            c->core = NULL;
        }
        pw_thread_loop_unlock(c->loop);
        pw_thread_loop_stop(c->loop);
    }
    if (c->ctx)  pw_context_destroy(c->ctx);
    if (c->loop) pw_thread_loop_destroy(c->loop);

    free(c->name);
    free(c);
    return true;
}

bool audio_activate(audio_client_t *c)
{
    if (!c) return false;
    /* M1 stub.  M2 builds a pw_filter with N inputs + M outputs, memfd-
     * backed DSP buffers, FORCE_QUANTUM / FORCE_RATE properties, and
     * connects it. */
    c->active = true;
    return true;
}

bool audio_deactivate(audio_client_t *c)
{
    if (!c) return false;
    c->active = false;
    return true;
}

const char *audio_get_client_name(audio_client_t *c)
{
    return c ? c->name : NULL;
}

/* ----------------------------------------------------------------------
 * Properties
 * ---------------------------------------------------------------------- */

audio_nframes_t audio_get_sample_rate(audio_client_t *c)
{
    return c ? c->sample_rate : 0;
}

audio_nframes_t audio_get_buffer_size(audio_client_t *c)
{
    return c ? c->buffer_size : 0;
}

bool audio_set_buffer_size(audio_client_t *c, audio_nframes_t nframes)
{
    if (!c || !nframes) return false;
    c->buffer_size = nframes;
    if (c->buffer_size_cb)
        c->buffer_size_cb(nframes, c->buffer_size_cb_arg);
    /* M2 will re-issue PW_KEY_NODE_FORCE_QUANTUM and force a reconnect. */
    return true;
}

/* ----------------------------------------------------------------------
 * Ports — M1 stubs.  audio_port_register returns a heap-allocated
 * audio_port so the asio.c IOChannel layer has a non-null handle to
 * carry around; audio_port_get_buffer returns NULL, so the process
 * callback (which doesn't fire in M1 — no filter is connected) cannot
 * accidentally write into garbage.
 * ---------------------------------------------------------------------- */

audio_port_t *audio_port_register(audio_client_t *c,
                                  const char *port_name, const char *port_type,
                                  uint64_t flags, uint64_t buffer_size)
{
    (void)buffer_size;
    if (!c || !port_name) return NULL;

    audio_port_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->client = c;
    p->name   = strdup(port_name);
    p->type   = strdup(port_type ? port_type : AUDIO_DEFAULT_TYPE);
    p->flags  = flags;
    return p;
}

bool audio_port_unregister(audio_client_t *c, audio_port_t *p)
{
    (void)c;
    if (!p) return false;
    free(p->name);
    free(p->type);
    free(p);
    return true;
}

void *audio_port_get_buffer(audio_port_t *p, audio_nframes_t nframes)
{
    (void)p; (void)nframes;
    /* M2 returns a pointer into the memfd-mapped region for this port's
     * current buffer half. */
    return NULL;
}

const char *audio_port_name(const audio_port_t *p) { return p ? p->name : NULL; }
const char *audio_port_type(const audio_port_t *p) { return p ? p->type : NULL; }

audio_port_t *audio_port_by_name(audio_client_t *c, const char *port_name)
{
    (void)c; (void)port_name;
    /* M3 looks this up in the registry-walker cache. */
    return NULL;
}

const char **audio_get_ports(audio_client_t *c,
                             const char *port_name_pattern,
                             const char *type_name_pattern,
                             uint64_t flags)
{
    (void)c; (void)port_name_pattern; (void)type_name_pattern; (void)flags;
    /* M3 walks PipeWire's registry, filtering by media class / direction. */
    return NULL;
}

void audio_port_get_latency_range(audio_port_t *p, uint32_t mode,
                                  audio_latency_range_t *range)
{
    if (!range) return;
    if (!p) { range->min = range->max = 0; return; }
    *range = p->latency[mode == AUDIO_PLAYBACK_LATENCY ? 1 : 0];
}

/* ----------------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------------- */

bool audio_set_process_callback(audio_client_t *c, audio_process_cb cb, void *arg)
{
    if (!c) return false;
    c->process_cb = cb;
    c->process_cb_arg = arg;
    return true;
}

bool audio_set_buffer_size_callback(audio_client_t *c, audio_buffer_size_cb cb, void *arg)
{
    if (!c) return false;
    c->buffer_size_cb = cb;
    c->buffer_size_cb_arg = arg;
    return true;
}

bool audio_set_sample_rate_callback(audio_client_t *c, audio_sample_rate_cb cb, void *arg)
{
    if (!c) return false;
    c->sample_rate_cb = cb;
    c->sample_rate_cb_arg = arg;
    return true;
}

bool audio_set_latency_callback(audio_client_t *c, audio_latency_cb cb, void *arg)
{
    if (!c) return false;
    c->latency_cb = cb;
    c->latency_cb_arg = arg;
    return true;
}

void audio_set_thread_creator(audio_thread_creator creator)
{
    /* Superseded by the spa_thread_utils override installed in audio_open.
     * The PipeWire data loop's RT thread is always a CreateThread'd Wine
     * thread regardless of what creator (if any) the caller registers
     * here. */
    (void)creator;
}

/* ----------------------------------------------------------------------
 * Connections / transport / memory
 * ---------------------------------------------------------------------- */

bool audio_connect(audio_client_t *c, const char *src, const char *dst)
{
    (void)c; (void)src; (void)dst;
    /* M3 issues pw_core_create_object(... PW_TYPE_INTERFACE_Link ...). */
    return false;
}

uint32_t audio_transport_query(const audio_client_t *c, audio_position_t *pos)
{
    if (pos) {
        pos->frame      = 0;
        pos->usecs      = 0;
        pos->frame_rate = c ? c->sample_rate : 0;
    }
    return AUDIO_TRANSPORT_STOPPED;
}

void audio_free(void *ptr)
{
    free(ptr);
}
