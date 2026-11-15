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

// ChallengeExtras.h ////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The general's challenge ("Generals Challenge") screen without the separate difficulty screen.
//
// The challenge menu gets, at its bottom left: the difficulty (three buttons), the game speed (the skirmish speed
// control with its "Normal" box) and a "Continue" button. The run of the last general played is kept in
// ChallengeRun.ini in the user data folder (ChallengeRun_InstanceNN.ini for later instances):
//   Campaign, Mission     the challenge campaign (CHALLENGE_n) and the battle that comes next
//   General               the index of the general on the challenge screen
//   Difficulty            0 easy, 1 normal, 2 hard
//   RankPoints            the rank points when that battle starts
//   GameSpeed, GameSpeedNormal    the speed of the challenge menu (kept without a run as well)
// A run starts when a general is started from the challenge menu, moves on when a battle is won and another follows
// (the game's own mission save is made as before), and ends when the last battle is won. A battle that is left or lost
// changes nothing, so "Continue" starts the next battle from its beginning, never from the middle. "Continue" locks
// the difficulty (shown in green); only the speed can change.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"

class GameWindow;

namespace ChallengeRun
{
	struct Run
	{
		AsciiString campaign;
		AsciiString mission;
		Int general;
		Int difficulty;
		Int rankPoints;
	};

	/// The stored run; FALSE when there is none (no file, or the last battle was won).
	Bool load(Run &run);
	/// The game speed of the challenge menu (logic steps per second) and its Normal box.
	void loadSpeed(Int &logicFps, Bool &normal);
	void saveSpeed(Int logicFps, Bool normal);
	Int loadDifficulty(Int fallback);   ///< the difficulty chosen last on the challenge menu

	/// A general was started from the challenge menu at the battle the campaign manager is on.
	void onBattleStarted(Int general, Int difficulty);
	/// A challenge battle was won and the campaign manager went on to the next one (empty map: the last was won).
	void onBattleWon();
	/// The logic speed of the run, for the next battle the score screen starts.
	Int getRunLogicFps();
}

namespace ChallengeExtras
{
	/// Called at the end of ChallengeMenuInit. 'labelFontLook' gives the small font (a bio line).
	void init(GameWindow *parentMenu, GameWindow *buttonPlay, GameWindow *buttonBack, GameWindow *labelFontLook);
	/// The menu's shutdown, before its leave transition: destroys the controls made by init.
	void shutdown();
	/// O24: every update of the challenge menu: the controls are made hidden and shown only while 'screenReady' (the
	/// menu's enter transition is over and it is not shutting down); they take no part in the transition.
	void update(Bool screenReady);

	/// GBM_SELECTED of the challenge menu: TRUE when it was a difficulty button, the Normal box or "Continue".
	/// "Continue" selects the run's general through 'selectGeneral' (the menu's own selection code).
	Bool onSelected(GameWindow *control, Bool (*selectGeneral)(Int general));
	/// GSM_SLIDER_TRACK of the challenge menu.
	Bool onSliderTrack(GameWindow *control, Int position);
	/// A general was clicked by the player: "Continue" is off again.
	void onGeneralClicked();

	Bool isContinue();
	Int getDifficulty();
	Int getLogicFps();
	/// Moves the campaign manager to the run's battle and rank points ("Continue"), after the general's campaign was set.
	void applyContinue();
	/// Keeps the speed and the difficulty for the next visit, and starts or moves the run. Call when Play starts a game.
	void onPlay(Int general);
}
