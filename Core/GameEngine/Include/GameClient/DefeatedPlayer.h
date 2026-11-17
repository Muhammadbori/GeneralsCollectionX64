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

// DefeatedPlayer.h ////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O59 (row 47) A defeated player in a network match of several people (GameInfo::
// hasSeveralHumanPlayers) stays with the team, by the user's rule:
//   - the match goes on for him with the "game over" look; nothing moves him anywhere by itself;
//   - the map does not open for him (VictoryConditions no longer reveals it in such a match, the same on every machine):
//     he sees what his team sees; no enemy money list, no looking through an enemy, no stealthed or disguised enemy;
//   - his push to talk and his text go to his team-mates only, and he hears and reads them; enemies and observers get
//     nothing from him;
//   - by his own choice ("Join observers") he becomes an observer: the whole map (the display as the "ReplayObserver"
//     player; nothing in the logic changes), his voice and text go to the room's observers only (LanRoom's observer
//     channel, which takes him in because he is a defeated player of this match), the players neither hear nor read him.
// A skirmish, a single player game and a network match with one human are as before.

#pragma once

#include "Lib/BaseType.h"

class Player;

namespace DefeatedPlayer
{
	Bool teamMatch();                       ///< TheGameInfo: several human players (the same on every machine)
	Bool isDefeatedInTeamMatch(const Player *player);   ///< defeated (not an Observer army) in such a match
	Bool localStaysWithTeam();              ///< this player, defeated in such a live match, not joined the observers
	Bool localJoinedObservers();
	/// The screen's rules for 'player' (the shown player): an active player, or this defeated player staying with the
	/// team (sees no stealthed or disguised enemy, as an active player).
	Bool viewsAsActive(const Player *player);
	/// TRUE when 'a' and 'b' are allies both ways.
	Bool allies(const Player *a, const Player *b);
	void joinObservers();
	void update();                          ///< every frame (InGameUI): the button, and the reset after the match
}
