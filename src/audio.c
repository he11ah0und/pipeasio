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

#define _GNU_SOURCE   /* memfd_create, MFD_CLOEXEC, SCHED_FIFO */

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
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

    /* --- M2: pw_filter + memfd buffer pool ---------------------------- */

    struct pw_filter           *filter;
    struct spa_hook             filter_listener;

    int                         memfd;            /* -1 when unallocated */
    audio_sample_t             *memfd_map;        /* mmap'd view; NULL when unallocated */
    size_t                      memfd_bytes;

    /* Registered-port array.  audio_port_register appends; audio_activate
     * walks this to build the filter, audio_deactivate frees it. */
    audio_port_t              **ports;
    uint32_t                    n_ports;
    uint32_t                    cap_ports;

    /* Half index (0 or 1) the host is reading/writing this cycle.
     * Toggled by the process callback after the ASIO bufferSwitch runs. */
    uint32_t                    current_half;

    /* Last spa_io_position.clock.nsec — used by audio_transport_query. */
    uint64_t                    last_clock_nsec;
    uint64_t                    last_clock_position;
};

struct audio_port {
    audio_client_t        *client;
    char                  *name;
    char                  *type;
    uint64_t               flags;
    uint32_t               channel_idx;          /* index into memfd layout */
    audio_latency_range_t  latency[2];           /* [CAPTURE, PLAYBACK] */

    /* --- M2: PipeWire port handle + memfd offsets --------------------- */

    enum pw_direction      direction;
    void                  *pw_filter_port;       /* returned by pw_filter_add_port */
    size_t                 mapoffset[2];         /* byte offsets into memfd for halves 0/1 */
    struct pw_buffer      *pw_buffer[2];         /* set by add_buffer events */
};

/* per-port userdata block stored by pw_filter_add_port — holds a pointer
 * back to our audio_port so the filter events can find it. */
typedef audio_port_t *audio_port_ref_t;

/* --- Forward decls for filter events ------------------------------------ */

static void audio_on_state_changed(void *userdata, enum pw_filter_state old,
                                   enum pw_filter_state state, const char *error);
static void audio_on_io_changed(void *userdata, void *port_data, uint32_t id,
                                void *area, uint32_t size);
static void audio_on_add_buffer(void *userdata, void *port_data,
                                struct pw_buffer *buffer);
static void audio_on_remove_buffer(void *userdata, void *port_data,
                                   struct pw_buffer *buffer);
static void audio_on_process(void *userdata, struct spa_io_position *position);

static const struct pw_filter_events audio_filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = audio_on_state_changed,
    .io_changed    = audio_on_io_changed,
    .add_buffer    = audio_on_add_buffer,
    .remove_buffer = audio_on_remove_buffer,
    .process       = audio_on_process,
};

static void audio_teardown_filter(audio_client_t *c);

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
    c->memfd       = -1;
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

    /* Tear down the filter + memfd first so its destruction sees a live
     * thread loop / core to deliver events on. */
    if (c->active)
        audio_teardown_filter(c);

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

    /* Free port array and any still-registered audio_port_t.  asio.c is
     * supposed to call audio_port_unregister for each port before closing,
     * but defend against leaks. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        free(p->name);
        free(p->type);
        free(p);
    }
    free(c->ports);

    free(c->name);
    free(c);
    return true;
}

/* Helper — tear down the pw_filter, memfd, and per-port resources.
 * Safe to call multiple times; clears all state to "not active". */
static void audio_teardown_filter(audio_client_t *c)
{
    if (c->filter) {
        pw_thread_loop_lock(c->loop);
        pw_filter_destroy(c->filter);
        c->filter = NULL;
        pw_thread_loop_unlock(c->loop);
    }
    if (c->memfd_map && c->memfd_bytes) {
        munmap(c->memfd_map, c->memfd_bytes);
    }
    c->memfd_map   = NULL;
    c->memfd_bytes = 0;
    if (c->memfd >= 0) {
        close(c->memfd);
        c->memfd = -1;
    }
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        p->pw_filter_port = NULL;
        p->pw_buffer[0]   = NULL;
        p->pw_buffer[1]   = NULL;
        p->mapoffset[0]   = 0;
        p->mapoffset[1]   = 0;
    }
    c->current_half = 0;
}

bool audio_activate(audio_client_t *c)
{
    if (!c) return false;
    if (c->active) return true;
    if (!c->n_ports) {
        ERR("audio_activate called with no ports registered\n");
        return false;
    }

    const size_t bsize_samples = c->buffer_size;
    const size_t bsize_bytes   = bsize_samples * sizeof(audio_sample_t);
    const size_t total_bytes   = c->n_ports * 2 * bsize_bytes;

    /* Allocate the memfd that backs every DSP buffer.  Layout:
     *   [ch0 half0][ch0 half1][ch1 half0][ch1 half1]...  */
    c->memfd = memfd_create("pipeasio-buffers", MFD_CLOEXEC);
    if (c->memfd < 0) {
        ERR("memfd_create failed: %s\n", strerror(errno));
        goto fail;
    }
    if (ftruncate(c->memfd, (off_t)total_bytes) < 0) {
        ERR("ftruncate(%zu) failed: %s\n", total_bytes, strerror(errno));
        goto fail;
    }
    void *map = mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                     c->memfd, 0);
    if (map == MAP_FAILED) {
        ERR("mmap(%zu) failed: %s\n", total_bytes, strerror(errno));
        goto fail;
    }
    c->memfd_map   = map;
    c->memfd_bytes = total_bytes;

    /* Pre-compute per-port mapoffsets. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p   = c->ports[i];
        p->channel_idx    = i;
        p->mapoffset[0]   = (size_t)(i * 2 + 0) * bsize_bytes;
        p->mapoffset[1]   = (size_t)(i * 2 + 1) * bsize_bytes;
        p->pw_buffer[0]   = NULL;
        p->pw_buffer[1]   = NULL;
    }

    /* Build the filter's node-level properties.  FORCE_QUANTUM/RATE lock
     * the PipeWire graph to the ASIO host's negotiated buffer size and
     * the configured sample rate. */
    struct pw_properties *filter_props = pw_properties_new(
        PW_KEY_NODE_NAME,            c->name,
        PW_KEY_NODE_DESCRIPTION,     c->name,
        PW_KEY_MEDIA_TYPE,           "Audio",
        PW_KEY_MEDIA_CATEGORY,       "Duplex",
        PW_KEY_MEDIA_ROLE,           "DSP",
        PW_KEY_NODE_ALWAYS_PROCESS,  "true",
        NULL);
    if (!filter_props) {
        ERR("pw_properties_new (filter) failed\n");
        goto fail;
    }
    pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", (unsigned)bsize_samples);
    pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_RATE,    "%u", (unsigned)c->sample_rate);
    pw_properties_setf(filter_props, PW_KEY_NODE_LATENCY,       "%u/%u",
                       (unsigned)bsize_samples, (unsigned)c->sample_rate);

    pw_thread_loop_lock(c->loop);

    c->filter = pw_filter_new_simple(pw_thread_loop_get_loop(c->loop),
                                     c->name, filter_props,
                                     &audio_filter_events, c);
    if (!c->filter) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_new_simple failed\n");
        goto fail;
    }

    /* Add every registered port to the filter.  The format param locks the
     * port to F32 DSP mono; the buffers param locks to 2 memfd-backed
     * buffers sized for one half of the memfd each.  pwasio uses the same
     * shape (src/pwasio.c:1303-1320), this is just the standard recipe. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];

        struct pw_properties *pp = pw_properties_new(NULL, NULL);
        if (!pp) {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_properties_new (port %u) failed\n", i);
            goto fail;
        }
        pw_properties_set(pp, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
        pw_properties_set(pp, PW_KEY_PORT_NAME, p->name);

        uint8_t param_buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(param_buf, sizeof param_buf);
        const struct spa_pod *params[] = {
            spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_buffers,  SPA_POD_Int(2),
                SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)bsize_bytes),
                SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(sizeof(audio_sample_t)),
                SPA_PARAM_BUFFERS_align,    SPA_POD_Int((int)bsize_bytes),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
        };

        p->pw_filter_port = pw_filter_add_port(c->filter, p->direction,
                                               PW_FILTER_PORT_FLAG_ALLOC_BUFFERS,
                                               sizeof(audio_port_ref_t),
                                               pp, params, SPA_N_ELEMENTS(params));
        if (!p->pw_filter_port) {
            pw_thread_loop_unlock(c->loop);
            ERR("pw_filter_add_port failed for port %u (%s)\n", i, p->name);
            goto fail;
        }
        *(audio_port_ref_t *)p->pw_filter_port = p;
    }

    if (pw_filter_connect(c->filter, PW_FILTER_FLAG_NONE, NULL, 0) < 0) {
        pw_thread_loop_unlock(c->loop);
        ERR("pw_filter_connect failed\n");
        goto fail;
    }

    pw_thread_loop_unlock(c->loop);

    c->active = true;
    TRACE("audio_activate: %u ports, %u-sample buffers, %u Hz\n",
          c->n_ports, c->buffer_size, c->sample_rate);
    return true;

fail:
    audio_teardown_filter(c);
    return false;
}

bool audio_deactivate(audio_client_t *c)
{
    if (!c) return false;
    if (!c->active) return true;
    audio_teardown_filter(c);
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
    (void)buffer_size;   /* asio.c passes the channel index here, not a size */
    if (!c || !port_name) return NULL;

    audio_port_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->client      = c;
    p->name        = strdup(port_name);
    p->type        = strdup(port_type ? port_type : AUDIO_DEFAULT_TYPE);
    p->flags       = flags;
    p->channel_idx = c->n_ports;
    p->direction   = (flags & AUDIO_PORT_IS_INPUT) ? PW_DIRECTION_INPUT
                                                   : PW_DIRECTION_OUTPUT;

    if (c->n_ports == c->cap_ports) {
        uint32_t new_cap = c->cap_ports ? c->cap_ports * 2 : 16;
        audio_port_t **grown = realloc(c->ports, new_cap * sizeof(*grown));
        if (!grown) { free(p->name); free(p->type); free(p); return NULL; }
        c->ports     = grown;
        c->cap_ports = new_cap;
    }
    c->ports[c->n_ports++] = p;
    return p;
}

bool audio_port_unregister(audio_client_t *c, audio_port_t *p)
{
    if (!c || !p) return false;
    /* Remove from the client's array (compact in place). */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        if (c->ports[i] == p) {
            memmove(&c->ports[i], &c->ports[i + 1],
                    (c->n_ports - i - 1) * sizeof(*c->ports));
            c->n_ports--;
            break;
        }
    }
    free(p->name);
    free(p->type);
    free(p);
    return true;
}

void *audio_port_get_buffer(audio_port_t *p, audio_nframes_t nframes)
{
    (void)nframes;   /* always == client->buffer_size when the filter is running */
    if (!p) return NULL;
    audio_client_t *c = p->client;
    if (!c || !c->memfd_map) return NULL;

    /* Memfd layout: channel 0 half 0 | channel 0 half 1 | channel 1 half 0 | ...
     * memfd_map is a float pointer, so we index in samples (not bytes). */
    return c->memfd_map + (p->channel_idx * 2 + c->current_half) * c->buffer_size;
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

/* ----------------------------------------------------------------------
 * Filter event callbacks — fire on the PipeWire data thread.  Because the
 * data thread was spawned by our audio_rt_create, it is a real Wine
 * thread; calling back into asio.c's process_cb (which then calls the
 * host's ASIO COM bufferSwitch) is safe.
 * ---------------------------------------------------------------------- */

static void audio_on_state_changed(void *userdata, enum pw_filter_state old,
                                   enum pw_filter_state state, const char *error)
{
    audio_client_t *c = userdata;
    (void)old; (void)c;
    if (state == PW_FILTER_STATE_ERROR && error)
        ERR("pw_filter entered ERROR state: %s\n", error);
}

static void audio_on_io_changed(void *userdata, void *port_data, uint32_t id,
                                void *area, uint32_t size)
{
    audio_client_t *c = userdata;
    (void)port_data;
    if (id == SPA_IO_Position && area && size >= sizeof(struct spa_io_position)) {
        const struct spa_io_position *pos = area;
        /* spa_fraction (num, denom): for an audio graph num=1 and denom is
         * the sample rate in Hz, so we just take denom. */
        audio_nframes_t new_rate = pos->clock.rate.denom
            ? pos->clock.rate.denom : c->sample_rate;
        if (new_rate && new_rate != c->sample_rate) {
            c->sample_rate = new_rate;
            if (c->sample_rate_cb)
                c->sample_rate_cb(new_rate, c->sample_rate_cb_arg);
        }
    }
}

static void audio_on_add_buffer(void *userdata, void *port_data,
                                struct pw_buffer *buffer)
{
    audio_client_t *c = userdata;
    audio_port_t   *p = *(audio_port_ref_t *)port_data;

    struct spa_data *d = &buffer->buffer->datas[0];

    /* Slot in either half 0 or 1 — PipeWire calls add_buffer once per
     * allocated buffer per port (we requested 2 via SPA_PARAM_Buffers). */
    int half = -1;
    if (!p->pw_buffer[0])      half = 0;
    else if (!p->pw_buffer[1]) half = 1;
    else {
        WARN("pw_filter handed us a 3rd buffer for port %u (%s); ignoring\n",
             p->channel_idx, p->name);
        return;
    }
    p->pw_buffer[half] = buffer;

    d->type      = SPA_DATA_MemFd;
    d->flags     = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
    d->fd        = c->memfd;
    d->mapoffset = p->mapoffset[half];
    d->maxsize   = c->buffer_size * sizeof(audio_sample_t);
}

static void audio_on_remove_buffer(void *userdata, void *port_data,
                                   struct pw_buffer *buffer)
{
    (void)userdata;
    audio_port_t *p = *(audio_port_ref_t *)port_data;
    if (buffer == p->pw_buffer[0]) p->pw_buffer[0] = NULL;
    if (buffer == p->pw_buffer[1]) p->pw_buffer[1] = NULL;
}

static void audio_on_process(void *userdata, struct spa_io_position *position)
{
    audio_client_t *c = userdata;

    if (position) {
        c->last_clock_nsec     = position->clock.nsec;
        c->last_clock_position = position->clock.position;
    }

    /* Dequeue the cycle's buffer for every port — this consumes whatever
     * PipeWire had presented to us. */
    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p = c->ports[i];
        if (p->pw_filter_port)
            pw_filter_dequeue_buffer(p->pw_filter_port);
    }

    /* Run the ASIO host's process callback.  audio_port_get_buffer reads
     * c->current_half to find the right memfd slice. */
    if (c->process_cb)
        c->process_cb(c->buffer_size, c->process_cb_arg);

    /* Queue buffer[current_half] for each port.  For output ports, also
     * set the chunk metadata so PipeWire knows the byte count. */
    const uint32_t half = c->current_half;
    const uint32_t bytes_this_cycle =
        (position ? position->clock.duration : c->buffer_size) * sizeof(audio_sample_t);

    for (uint32_t i = 0; i < c->n_ports; i++) {
        audio_port_t *p   = c->ports[i];
        struct pw_buffer *b = p->pw_buffer[half];
        if (!b || !p->pw_filter_port)
            continue;
        if (p->direction == PW_DIRECTION_OUTPUT) {
            struct spa_data *d = &b->buffer->datas[0];
            d->chunk->offset = 0;
            d->chunk->size   = bytes_this_cycle;
            d->chunk->stride = sizeof(audio_sample_t);
            d->chunk->flags  = 0;
        }
        pw_filter_queue_buffer(p->pw_filter_port, b);
    }

    c->current_half ^= 1;
}
