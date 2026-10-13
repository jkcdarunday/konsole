#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Konsole contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Animate terminal content at a chosen input rate for visual rendering checks."""

import argparse
import shutil
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fps", type=float, default=144, help="terminal input frames per second")
parser.add_argument("--duration", type=float, default=10, help="animation duration in seconds")
parser.add_argument("--speed", type=float, default=48, help="bar movement in terminal cells per second")
args = parser.parse_args()
if args.fps <= 0 or args.duration <= 0 or args.speed <= 0:
    parser.error("fps, duration and speed must be positive")

start = time.perf_counter()
deadline = start
frame = 0
sys.stdout.write("\033[?1049h\033[?25l")
try:
    while time.perf_counter() - start < args.duration:
        columns, rows = shutil.get_terminal_size()
        width = max(1, columns - 2)
        elapsed = time.perf_counter() - start
        lines = [
            "GPU rendering preview",
            f"Input: {args.fps:g} frames/sec | Frame: {frame} | Ends in {max(0, args.duration - elapsed):.1f}s",
            "Moving bars should travel smoothly. Press Ctrl+C to stop.",
            "",
        ]
        for row in range(min(24, max(0, rows - 5))):
            span = max(1, width - 10)
            travel = (elapsed * args.speed + row * 3) % (2 * span)
            position = min(travel, 2 * span - travel)
            cell = int(position)
            fraction = int((position - cell) * 8)
            color = 16 + (row * 9 + frame // 4) % 216
            partial = " ▏▎▍▌▋▊▉"[fraction]
            glyphs = "████████" if fraction == 0 else f"\033[7m{partial}\033[27m███████{partial}"
            bar = " " * cell + f"\033[38;5;{color}m{glyphs}\033[0m"
            lines.append(bar)
        sys.stdout.write("\033[H" + "\r\n".join((line[:columns] if "\033" not in line else line) + "\033[K" for line in lines) + "\033[J")
        sys.stdout.flush()
        frame += 1
        # Do not flood the PTY with catch-up frames after a missed deadline.
        deadline += 1 / args.fps
        now = time.perf_counter()
        if deadline < now:
            deadline = now
        delay = deadline - now
        if delay > 0:
            time.sleep(delay)
except KeyboardInterrupt:
    pass
finally:
    sys.stdout.write("\033[?25h\033[?1049l")
    sys.stdout.flush()
