/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#include "PreRTS.h"

#include "Common/GameUtility.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/Radar.h"

#include "GameClient/ControlBar.h"
#include "GameClient/Display.h"
#include "GameClient/GameClient.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ParticleSys.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"
#include "GameLogic/PartitionManager.h"

namespace rts
{

namespace detail
{
static void changePlayerCommon(Player* player)
{
	TheParticleSystemManager->setLocalPlayerIndex(player->getPlayerIndex());
	ThePartitionManager->refreshShroudForLocalPlayer();
	TheGhostObjectManager->setLocalPlayerIndex(player->getPlayerIndex());
	TheGameClient->updateFakeDrawables();
	TheRadar->refreshObjects();
	TheInGameUI->deselectAllDrawables();
}

} // namespace detail

bool localPlayerHasRadar()
{
	// Using "local" instead of "observed or local" player because as an observer we prefer
	// the radar to be turned on when observing a player that has no radar.
	const Player* player = ThePlayerList->getLocalPlayer();
	const PlayerIndex index = player->getPlayerIndex();

	if (TheRadar->isRadarForced(index))
		return true;

	if (!TheRadar->isRadarHidden(index) && player->hasRadar())
		return true;

	return false;
}

// O59 (row 47): the display as the observer player (setDisplayAsObserver)
static Bool s_displayAsObserver = FALSE;
static Player *s_displayObserverPlayer = nullptr;

Player* getObservedOrLocalPlayer()
{
	DEBUG_ASSERTCRASH(TheControlBar != nullptr, ("TheControlBar is null"));
	Player* player = TheControlBar->getObservedPlayer();
	if (player == nullptr && s_displayAsObserver) {
		player = s_displayObserverPlayer;
	}
	if (player == nullptr)
	{
		DEBUG_ASSERTCRASH(ThePlayerList != nullptr, ("ThePlayerList is null"));
		player = ThePlayerList->getLocalPlayer();
	}
	return player;
}

Player* getObservedOrLocalPlayer_Safe()
{
	Player* player = nullptr;

	if (TheControlBar != nullptr)
		player = TheControlBar->getObservedPlayer();

	if (player == nullptr && s_displayAsObserver)
		player = s_displayObserverPlayer;

	if (player == nullptr)
		if (ThePlayerList != nullptr)
			player = ThePlayerList->getLocalPlayer();

	return player;
}

PlayerIndex getObservedOrLocalPlayerIndex_Safe()
{
	if (Player* player = getObservedOrLocalPlayer_Safe())
		return player->getPlayerIndex();

	return 0;
}

void changeLocalPlayer(Player* player)
{
	DEBUG_ASSERTCRASH(player != nullptr, ("Player is null"));

	ThePlayerList->setLocalPlayer(player);
	TheControlBar->setObserverLookAtPlayer(nullptr);
	TheControlBar->setObservedPlayer(nullptr);
	TheControlBar->setControlBarSchemeByPlayer(player);
	TheControlBar->initSpecialPowershortcutBar(player);

	detail::changePlayerCommon(player);
}

void changeObservedPlayer(Player* player)
{
	TheControlBar->setObserverLookAtPlayer(player);

	const Bool canBeginObservePlayer = TheGlobalData->m_enablePlayerObserver && TheGhostObjectManager->trackAllPlayers();
	const Bool canEndObservePlayer = TheControlBar->getObservedPlayer() != nullptr && TheControlBar->getObserverLookAtPlayer() == nullptr;

	if (canBeginObservePlayer || canEndObservePlayer)
	{
		TheControlBar->setObservedPlayer(player);

		Player *becomePlayer = player;
		if (becomePlayer == nullptr)
			becomePlayer = ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey("ReplayObserver"));
		detail::changePlayerCommon(becomePlayer);
	}
}

void setDisplayAsObserver(Bool on)
{
	Player *observer = (on && ThePlayerList != nullptr)
		? ThePlayerList->findPlayerWithNameKey(TheNameKeyGenerator->nameToKey("ReplayObserver")) : nullptr;
	const Bool want = on && observer != nullptr;
	if (want == s_displayAsObserver) {
		return;
	}
	s_displayAsObserver = want;
	s_displayObserverPlayer = observer;
	// shroud, ghosts, particles, radar and selection follow the shown player, as when a replay changes its player
	if (TheGameLogic != nullptr && TheGameLogic->isInGame() && ThePlayerList != nullptr && TheControlBar != nullptr) {
		detail::changePlayerCommon(getObservedOrLocalPlayer());
	}
}

Bool isDisplayAsObserver()
{
	return s_displayAsObserver;
}

void enableLetterBox(Bool enable)
{
	if (enable)
	{
		HideControlBar(TRUE);
		TheDisplay->enableLetterBox(TRUE);
	}
	else
	{
		ShowControlBar(FALSE);
		TheDisplay->enableLetterBox(FALSE);
	}
}

} // namespace rts
