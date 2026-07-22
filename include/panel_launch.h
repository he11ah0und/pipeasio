/*
 * panel_launch.h - spawn the native settings panel for the ControlPanel
 * handoff, straight from the unix side.
 *
 * Why not ShellExecute: bare names do not resolve (current Wine does not
 * import the Unix PATH into the session's %PATH%), and mapping Unix paths
 * to DOS paths through dosdevices fails entirely when the prefix has no
 * drive for '/' (Bottles bottles ship without a Z: drive).  The unixlib is
 * native code inside the Wine process, so it can simply spawn the binary
 * itself - no drive letters, no PATH import, no DOS translation.
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

#include <stdbool.h>

/* Spawns the panel from the first location that has it and returns true
 * when it stayed alive long enough to count as started:
 *   1. <driver .so dir>/../../../bin/pipeasio-settings  (installed layout)
 *   2. /usr/local/bin, /usr/bin, ~/.local/bin /pipeasio-settings
 * Returns false when nothing launchable was found (built-in dialog then).
 * Inside bwrap sandboxes (e.g. Flatpak Bottles) none of these exist, so
 * the built-in dialog is the UI there. */
bool panel_launch_try(void);

/* Existence-only probe (nothing is spawned) for the About tab's "native
 * panel available" indicator. */
bool panel_launch_available(void);
