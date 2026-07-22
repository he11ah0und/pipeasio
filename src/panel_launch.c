/*
 * panel_launch.c - see panel_launch.h for the module description.
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
#define _GNU_SOURCE /* dladdr, Dl_info */
#include "panel_launch.h"

#include <dlfcn.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Spawn file+argv and give it ~800 ms: still running then counts as a
 * successful launch (a launcher that dies instantly - missing portal
 * permission, missing host binary - falls to the next candidate). */
static bool
spawn_and_check(const char *file, char *const argv[])
{
    pid_t pid;
    if (posix_spawn(&pid, file, NULL, NULL, argv, environ) != 0)
        return false;

    const struct timespec slice = { 0, 100 * 1000 * 1000 }; /* 100 ms x 8 */
    for (int i = 0; i < 8; i++)
    {
        nanosleep(&slice, NULL);
        int   status = 0;
        pid_t r      = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            /* Exited fast: only a clean exit counts as a launched panel. */
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r < 0)
            return true; /* already reaped / reparented: it ran */
    }
    /* Alive after 800 ms: success.  No one reaps it later - one zombie per
     * panel open until the host exits, same trade-off ShellExecute had. */
    return true;
}

/* The .so lives at <prefix>/lib/wine/x86_64-unix; the panel installs to
 * <prefix>/bin. */
static bool
panel_next_to_driver(char *out, size_t out_sz)
{
    Dl_info info;
    if (!dladdr((void *)panel_launch_try, &info) || !info.dli_fname)
        return false;
    snprintf(out, out_sz, "%s", info.dli_fname);
    for (int i = 0; i < 3; i++)
    {
        char *slash = strrchr(out, '/');
        if (!slash)
            return false;
        *slash = '\0';
    }
    const size_t len = strlen(out);
    snprintf(out + len, out_sz - len, "/bin/pipeasio-settings");
    return access(out, X_OK) == 0;
}

/* Finds the panel binary among the host locations (installed layout first,
 * then the PATH staples); out may be NULL for a pure existence check. */
static bool
find_panel_path(char *out, size_t out_sz)
{
    char path[PATH_MAX];

    if (panel_next_to_driver(path, sizeof path))
    {
        if (out)
            snprintf(out, out_sz, "%s", path);
        return true;
    }
    static const char *dirs[] = { "/usr/local/bin", "/usr/bin" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    {
        snprintf(path, sizeof path, "%s/pipeasio-settings", dirs[i]);
        if (access(path, X_OK) == 0)
        {
            if (out)
                snprintf(out, out_sz, "%s", path);
            return true;
        }
    }
    const char *home = getenv("HOME");
    if (home)
    {
        snprintf(path, sizeof path, "%s/.local/bin/pipeasio-settings", home);
        if (access(path, X_OK) == 0)
        {
            if (out)
                snprintf(out, out_sz, "%s", path);
            return true;
        }
    }
    return false;
}

/* Existence-only probe for the About tab's "native panel" indicator. */
bool
panel_launch_available(void)
{
    return find_panel_path(NULL, 0);
}

bool
panel_launch_try(void)
{
    char path[PATH_MAX];

    if (find_panel_path(path, sizeof path))
    {
        char *const argv[] = { path, NULL };
        if (spawn_and_check(path, argv))
            return true;
    }

    return false;
}
