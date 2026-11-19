/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// TheSuperHackers @feature O96 (test switch, *uitest*.exe only) -testCommands <file>: plays frame-stamped player actions
// (spawn, select, move, force attack, dozer build, production, sell, special power, ...) in a running game, so a script
// can check the logic fixes without a person. The orders are the same GameMessages the control bar and the command
// translator send for a human player; only "spawn", "pick", "money" and "spready" set the scene directly. Every executed line and its
// result goes to <user data>\TestCommands.log. The line format is in TestCommands.cpp (top comment).

#pragma once

class TestCommands
{
public:
	static void setScript(const char *path); ///< -testCommands <file>; ignored outside a *uitest*.exe
	static Bool isActive();
	/// -testCommandsSide <PlayerTemplate>: with -testCommands the command-line skirmish (-skirmish) gives the local player
	/// slot 0 and start position 1 with this side (default FactionGLA) instead of the observer, so its orders count.
	static void setSkirmishSide(const char *side);
	static const char *getSkirmishSide(); ///< nullptr without -testCommands (the -skirmish observer stays)
	/// Call once per client update after the device messages were appended (beside FrameShots::injectTestInput).
	static void update();
	/// O99: call at the start of GameLogic::update (inside its m_isInUpdate latch). Runs the "spawn" lines update() held
	/// back, so spawned objects are made in logic time like produced ones; the script goes on after them.
	static void logicUpdate();
};
