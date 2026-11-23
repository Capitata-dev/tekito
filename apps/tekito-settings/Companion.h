#pragma once

#include <windows.h>

#include <optional>

// The desktop companion (Settings > General > Desktop companion, off at
// first): Home's companion in a corner of the desktop, run as
// "TEKITO.exe --companion" and started with Windows while it is on. It is
// colored as the mode of the keyboard in front (a click goes to the next
// mode), says when Caps Lock turns on, tells what a word dropped on it (or
// copied) reads and means (CompanionLookup), and has a menu of the modes,
// secret mode, adding a word, reporting a wrong conversion and Settings.
// It watches the pointer, blinks, hops when the mode changes, closes its
// eyes in secret mode, can be dragged anywhere, and steps aside for
// full-screen apps. It leaves when the setting is turned off.

// Runs the companion when the command line asks for it; its exit code then.
[[nodiscard]] std::optional<int> RunCompanionFromCommandLine();
// Starts the companion when it is not running.
void StartCompanion();
// Starts the companion with Windows, or no longer.
void SetCompanionAtStartup(bool on);
