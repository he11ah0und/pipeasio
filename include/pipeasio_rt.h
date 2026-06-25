/*
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

/* Host-callback entry points exported by src/asio.c for the WoW64 PE pump. */
#pragma once

#include <stdint.h>

#include "audio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Does not gather/scatter ports, toggle host_buffer_index, or check Running. */
    void pipeasio_host_buffer_switch(void *This, int32_t buffer_index, audio_nframes_t add_samples,
                                     uint64_t time_nsec);

    /* Nonzero when host callbacks are valid for process-time buffer switches. */
    int pipeasio_host_is_running(void *This);

#ifdef __cplusplus
}
#endif
