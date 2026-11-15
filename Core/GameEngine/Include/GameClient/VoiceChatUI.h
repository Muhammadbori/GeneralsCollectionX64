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

// VoiceChatUI.h ////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature The screen side of the voice chat (VoiceChat.h):
//   - the talk indicator in the HUD (left side): "Говорят: A, B" / "Speaking: A, B", "Вы говорите" while this player
//     sends, "Микрофон выключен" / "Вы не можете говорить" while the talk key is held but this player may not talk;
//   - in the Tab (Diplomacy) list, next to each team-mate whose voice this computer receives, a button with the
//     volume: 100% -> 50% -> 25% -> Off -> 100% per click; the button turns green while that team-mate speaks.
//     Window names for -uiScript: Diplomacy.wnd:VoiceVolume<row>.

#pragma once

#include "Lib/BaseType.h"

class GameWindow;

namespace VoiceChatUI
{
	/// Every client update (InGameUI::update).
	void update();
	/// One row of the Tab list (PopulateInGameDiplomacyPopup); slot < 0 hides the row's button.
	void decorateDiplomacyRow(Int row, GameWindow *nameText, GameWindow *statusText, Int slot);
	/// GBM_SELECTED in the Tab list: TRUE when it was one of the volume buttons.
	Bool onDiplomacySelected(GameWindow *control);
}
