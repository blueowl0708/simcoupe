// Part of SimCoupe - A SAM Coupe emulator
//
// Remote.h: ZRCP-style remote control over a local TCP socket
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#pragma once

namespace Remote
{
bool Init();
void Exit();

// Accept connections and run any complete commands (called between chunks)
void Poll();

bool IsActive();
bool IsStopped();

// Say on the status line that remote control has stopped the emulation, and redraw the
// frame so it shows (for the window's GUI and keys, which can't do anything until resumed)
void ShowStopped();

// Per-instruction hook, only called while trace_instructions is set.
// Returns true if execution should stop before the next instruction.
bool OnInstruction();
void OnFrameEnd();

// Merge keys pressed by remote commands into the SAM keyboard matrix
void ApplyKeys(std::array<uint8_t, 9>& key_matrix);

// Add a line to the log read by get-breakpoint-log
void Log(const std::string& str);

extern bool trace_instructions;
}
