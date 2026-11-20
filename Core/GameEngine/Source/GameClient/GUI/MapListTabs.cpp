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

// MapListTabs.cpp //////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature O112 The map packs' tabs in the map selection windows (MapListTabs.h).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/MapListTabs.h"

#include "Common/NameKeyGenerator.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MapUtil.h"

namespace
{
	struct TabInfo
	{
		Int tab;
		const char *id;
		const WideChar *english;
		const WideChar *russian;
	};

	const TabInfo kTabs[2] =
	{
		{ MAP_TAB_GENTOOL, "MapListTabs:GenTool", L"GenTool maps", L"\x041A\x0430\x0440\x0442\x044B GenTool" },
		{ MAP_TAB_GENPATCHER, "MapListTabs:GenPatcher", L"GenPatcher maps", L"\x041A\x0430\x0440\x0442\x044B GenPatcher" },
	};

	NameKeyType idOf(const TabInfo &t)
	{
		return TheNameKeyGenerator->nameToKey(AsciiString(t.id));
	}

	void copyLook(WinInstanceData &inst, GameWindow *look)
	{
		inst.init();
		inst.m_style = GWS_RADIO_BUTTON | GWS_MOUSE_TRACK;
		const WinInstanceData *from = look->winGetInstanceData();
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			inst.m_enabledDrawData[i] = from->m_enabledDrawData[i];
			inst.m_disabledDrawData[i] = from->m_disabledDrawData[i];
			inst.m_hiliteDrawData[i] = from->m_hiliteDrawData[i];
		}
		inst.m_enabledText = from->m_enabledText;
		inst.m_disabledText = from->m_disabledText;
		inst.m_hiliteText = from->m_hiliteText;
		inst.m_imageOffset = from->m_imageOffset;
		inst.m_font = from->m_font;
	}
}

void MapListTabs::attach(GameWindow *systemButton, GameWindow *userButton)
{
	if (systemButton == nullptr || userButton == nullptr || TheMapCache == nullptr || TheWindowManager == nullptr) {
		return;
	}
	GameWindow *parent = systemButton->winGetParent();
	if (parent == nullptr || userButton->winGetParent() != parent) {
		return;
	}
	if (TheWindowManager->winGetWindowFromId(parent, idOf(kTabs[0])) != nullptr
		|| TheWindowManager->winGetWindowFromId(parent, idOf(kTabs[1])) != nullptr) {
		return;   // already there
	}

	TheMapCache->updateCache();
	Bool want[2];
	Int count = 0;
	for (Int i = 0; i < 2; ++i)
	{
		want[i] = TheMapCache->hasMapsInTab(kTabs[i].tab);
		count += want[i] ? 1 : 0;
	}
	if (count == 0) {
		return;
	}

	Int sx = 0, sy = 0, sw = 0, sh = 0, ux = 0, uy = 0, uw = 0, uh = 0, pw = 0, ph = 0;
	systemButton->winGetPosition(&sx, &sy);
	systemButton->winGetSize(&sw, &sh);
	userButton->winGetPosition(&ux, &uy);
	userButton->winGetSize(&uw, &uh);
	parent->winGetSize(&pw, &ph);

	// the tabs go on in the direction the game's two go: side by side, or one under the other
	const Bool across = (uy - sy) * (uy - sy) < (ux - sx) * (ux - sx);
	Int x[4], y[4], w = sw, h = sh;
	if (across)
	{
		const Int first = min(sx, ux);
		Int pitch = abs(ux - sx);
		Int gap = max(2, pitch - sw);
		// O115 (S61): the right limit is the window's edge or the first window to the right of the tab row (the map
		// preview frame), with a gap
		Int rightLimit = pw - 4;
		const Int rowRight = max(sx + sw, ux + uw);
		const Int margin = max(6, pw / 100);
		for (GameWindow *other = parent->winGetChild(); other != nullptr; other = other->winGetNext())
		{
			if (other == systemButton || other == userButton) {
				continue;
			}
			Int ox = 0, oy = 0, ow = 0, oh = 0;
			other->winGetPosition(&ox, &oy);
			other->winGetSize(&ow, &oh);
			const Bool overlapsRow = oy < sy + sh && oy + oh > sy;
			if (overlapsRow && ox >= rowRight && ox - margin < rightLimit) {
				rightLimit = ox - margin;
			}
		}
		if (first + (count + 1) * pitch + sw > rightLimit)
		{
			// not enough room: all tabs share the width from the first one to the right limit
			const Int span = rightLimit - first;
			w = (span - (count + 1) * gap) / (count + 2);
			pitch = w + gap;
			systemButton->winSetSize(w, sh);
			userButton->winSetSize(w, uh);
			systemButton->winSetPosition(sx <= ux ? first : first + pitch, sy);
			userButton->winSetPosition(sx <= ux ? first + pitch : first, uy);
		}
		for (Int i = 0; i < 4; ++i)
		{
			x[i] = first + (2 + i) * pitch;
			y[i] = sy;
		}
	}
	else
	{
		const Int pitch = abs(uy - sy);
		const Int last = max(sy, uy);
		for (Int i = 0; i < 4; ++i)
		{
			x[i] = sx;
			y[i] = last + (1 + i) * pitch;
		}
	}

	RadioButtonData group;
	const RadioButtonData *from = (const RadioButtonData *)systemButton->winGetUserData();
	group.screen = from != nullptr ? from->screen : 0;
	group.group = from != nullptr ? from->group : 0;

	Int slot = 0;
	for (Int i = 0; i < 2; ++i)
	{
		if (!want[i]) {
			continue;
		}
		WinInstanceData inst;
		copyLook(inst, systemButton);
		inst.m_id = (Int)idOf(kTabs[i]);
		GameWindow *button = TheWindowManager->gogoGadgetRadioButton(parent,
			WIN_STATUS_ENABLED | (systemButton->winGetStatus() & WIN_STATUS_IMAGE), x[slot], y[slot], w, h,
			&inst, &group, inst.m_font, FALSE);
		++slot;
		if (button == nullptr) {
			continue;
		}
		button->winSetWindowId(inst.m_id);
		button->winSetOwner(systemButton->winGetOwner());
		GadgetRadioSetText(button, UnicodeString(localizedFallback(kTabs[i].english, kTabs[i].russian)));
	}
}

Int MapListTabs::startTab(const AsciiString &mapName, Bool usesSystemMapDir)
{
	if (TheMapCache != nullptr && !mapName.isEmpty() && TheMapCache->findMap(mapName) != nullptr)
	{
		const Int tab = TheMapCache->getTabOfMap(mapName);
		if (tab == MAP_TAB_OFFICIAL || TheMapCache->isMapInTab(mapName, tab)) {
			return tab;
		}
	}
	return usesSystemMapDir ? MAP_TAB_OFFICIAL : MAP_TAB_USER;
}

void MapListTabs::selectTab(Int tab, GameWindow *systemButton, GameWindow *userButton)
{
	GameWindow *button = nullptr;
	for (Int i = 0; i < 2; ++i)
	{
		if (kTabs[i].tab == tab && systemButton != nullptr && systemButton->winGetParent() != nullptr) {
			button = TheWindowManager->winGetWindowFromId(systemButton->winGetParent(), idOf(kTabs[i]));
		}
	}
	if (button == nullptr) {
		button = (tab == MAP_TAB_OFFICIAL) ? systemButton : userButton;
	}
	if (button != nullptr) {
		GadgetRadioSetSelection(button, FALSE);
	}
}

Bool MapListTabs::isTabButton(GameWindow *control, Int *tab)
{
	if (control == nullptr) {
		return FALSE;
	}
	const Int id = control->winGetWindowId();
	for (Int i = 0; i < 2; ++i)
	{
		if (id == (Int)idOf(kTabs[i]))
		{
			if (tab != nullptr) {
				*tab = kTabs[i].tab;
			}
			return TRUE;
		}
	}
	return FALSE;
}
