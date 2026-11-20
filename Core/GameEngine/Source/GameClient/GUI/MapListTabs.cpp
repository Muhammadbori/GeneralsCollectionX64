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

#include <map>

#include "Common/NameKeyGenerator.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameFont.h"
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
		// O120: short labels, so four tabs fit one row
		{ MAP_TAB_GENTOOL, "MapListTabs:GenTool", L"GenTool", L"GenTool" },
		{ MAP_TAB_GENPATCHER, "MapListTabs:GenPatcher", L"GenPatcher", L"GenPatcher" },
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

namespace
{
	Int labelWidth(GameWindow *button, const UnicodeString &label)
	{
		if (button == nullptr || TheDisplayStringManager == nullptr) {
			return 0;
		}
		DisplayString *probe = TheDisplayStringManager->newDisplayString();
		if (probe == nullptr) {
			return 0;
		}
		probe->setFont(button->winGetFont());
		probe->setText(label);
		const Int w = probe->getWidth();
		TheDisplayStringManager->freeDisplayString(probe);
		return w;
	}
}

// TheSuperHackers @tweak O125 (the user's picture, with his correction) Each map tab is one plain cell: a frame and its
// label over the whole width, without the radio button's two lit end squares. The selected tab keeps its green label,
// and the squares' lit look moves onto the whole cell, dimmer than the squares were (their own picture stretched over
// the cell at about 40 % strength, or their colour as a faint fill). The others are plain with the normal label. The
// label shrinks to fit the cell (down to 50 %).
namespace
{
	GameFont *fitTabFont(DisplayString *text, GameFont *own, Int width)
	{
		struct Fit
		{
			GameFont *own;
			Int width;
			UnicodeString str;
			GameFont *use;
		};
		static std::map<DisplayString *, Fit> s_fit;
		if (own == nullptr || width <= 8 || TheFontLibrary == nullptr || TheDisplayStringManager == nullptr) {
			return own;
		}
		const UnicodeString str = text->getText();
		std::map<DisplayString *, Fit>::iterator it = s_fit.find(text);
		if (it != s_fit.end() && it->second.own == own && it->second.width == width && it->second.str.compare(str) == 0) {
			return it->second.use;
		}
		GameFont *use = own;
		DisplayString *probe = TheDisplayStringManager->newDisplayString();
		if (probe != nullptr)
		{
			probe->setText(str);
			const Int minSize = max(6, own->pointSize / 2);
			for (Int size = own->pointSize; size >= minSize; --size)
			{
				GameFont *f = (size == own->pointSize) ? own : TheFontLibrary->getFont(own->nameString, size, own->bold);
				if (f == nullptr) {
					continue;
				}
				probe->setFont(f);
				use = f;
				if (probe->getWidth() <= width - 10) {
					break;
				}
			}
			TheDisplayStringManager->freeDisplayString(probe);
		}
		Fit fit;
		fit.own = own;
		fit.width = width;
		fit.str = str;
		fit.use = use;
		s_fit[text] = fit;
		return use;
	}

	void drawPlainTab(GameWindow *window, WinInstanceData *instData)
	{
		if (window == nullptr || instData == nullptr || TheDisplay == nullptr) {
			return;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
		const Bool selected = BitIsSet(instData->getState(), WIN_STATE_SELECTED);
		const Bool hilited = BitIsSet(instData->getState(), WIN_STATE_HILITED);

		TheDisplay->drawFillRect(x, y, w, h, GameMakeColor(0, 0, 0, 150));
		if (selected)
		{
			// the lit squares' own look (the "checked box" picture or colour of the radio button), over the whole cell,
			// dimmed
			// O126 (S69): a plain colour fill; the squares' picture stretched over the cell showed darker bands at both ends
			UnsignedByte r = 0, g = 0, b = 0, a = 0;
			GameGetColorComponents(window->winGetEnabledColor(2), &r, &g, &b, &a);
			if (a == 0 || (r < 40 && g < 40 && b < 40)) {
				r = 60; g = 200; b = 60;
			}
			TheDisplay->drawFillRect(x, y, w, h, GameMakeColor(r, g, b, 70));
		}
		const Color frame = selected ? GameMakeColor(110, 170, 110, 255)
			: (hilited ? GameMakeColor(135, 145, 158, 255) : GameMakeColor(88, 96, 108, 255));
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, frame);

		DisplayString *text = instData->getTextDisplayString();
		if (text == nullptr || text->getTextLength() == 0) {
			return;
		}
		GameFont *font = fitTabFont(text, window->winGetFont(), w);
		if (text->getFont() != font) {
			text->setFont(font);
		}
		IRegion2D clip;
		clip.lo.x = x;
		clip.lo.y = y;
		clip.hi.x = x + w;
		clip.hi.y = y + h;
		text->setClipRegion(&clip);
		Int tw = 0, th = 0;
		text->getSize(&tw, &th);
		// the selected tab keeps the green label (the radio's lit text colour when it is green, else a game green)
		Color textColor = enabled ? window->winGetEnabledTextColor() : window->winGetDisabledTextColor();
		if (enabled && selected)
		{
			UnsignedByte r = 0, g = 0, b = 0, a = 0;
			GameGetColorComponents(window->winGetHiliteTextColor(), &r, &g, &b, &a);
			textColor = (a > 0 && g > r && g > b) ? window->winGetHiliteTextColor() : GameMakeColor(80, 230, 80, 255);
		}
		const Color dropColor = GameMakeColor(0, 0, 0, 255);
		text->draw(x + (w - tw) / 2, y + (h - th) / 2, textColor, dropColor);
	}

	void plainTab(GameWindow *button)
	{
		if (button != nullptr) {
			button->winSetDrawFunc(drawPlainTab);
		}
	}
}

// TheSuperHackers @tweak O120 (the user's picture) The four tabs overlapped one another and the frames. Now: short
// labels ("Official" / "Unofficial" / "GenTool" / "GenPatcher", Russian "Официальные" / "Неофициальные"), equal tab
// widths with gaps in one row from the first tab to the window's edge or the first window right of the row (the map
// preview frame); when the widest label would need less than 75 % of its font to fit, two rows of two (2x2) and the list
// below moves down by one row. Each label shrinks to fit its own frame (W3DRadioButton.cpp). Never overlapping.
void MapListTabs::attach(GameWindow *systemButton, GameWindow *userButton)
{
	if (systemButton == nullptr || userButton == nullptr || TheMapCache == nullptr || TheWindowManager == nullptr) {
		return;
	}
	GameWindow *parent = systemButton->winGetParent();
	if (parent == nullptr || userButton->winGetParent() != parent) {
		return;
	}

	// the game's two tabs get short labels in both games (Generals keeps only these two)
	const UnicodeString officialLabel(localizedFallback(L"Official", L"\x041E\x0444\x0438\x0446\x0438\x0430\x043B\x044C\x043D\x044B\x0435"));
	const UnicodeString userLabel(localizedFallback(L"Unofficial", L"\x041D\x0435\x043E\x0444\x0438\x0446\x0438\x0430\x043B\x044C\x043D\x044B\x0435"));
	GadgetRadioSetText(systemButton, officialLabel);
	GadgetRadioSetText(userButton, userLabel);
	plainTab(systemButton);   // O125
	plainTab(userButton);

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

	// the buttons in tab order and their labels
	GameWindow *buttons[4] = { systemButton, userButton, nullptr, nullptr };
	UnicodeString labels[4] = { officialLabel, userLabel, UnicodeString::TheEmptyString, UnicodeString::TheEmptyString };
	Int n = 2;

	RadioButtonData group;
	const RadioButtonData *from = (const RadioButtonData *)systemButton->winGetUserData();
	group.screen = from != nullptr ? from->screen : 0;
	group.group = from != nullptr ? from->group : 0;

	for (Int i = 0; i < 2; ++i)
	{
		if (!want[i]) {
			continue;
		}
		WinInstanceData inst;
		copyLook(inst, systemButton);
		inst.m_id = (Int)idOf(kTabs[i]);
		GameWindow *button = TheWindowManager->gogoGadgetRadioButton(parent,
			WIN_STATUS_ENABLED | (systemButton->winGetStatus() & WIN_STATUS_IMAGE), sx, sy, sw, sh,
			&inst, &group, inst.m_font, FALSE);
		if (button == nullptr) {
			continue;
		}
		button->winSetWindowId(inst.m_id);
		button->winSetOwner(systemButton->winGetOwner());
		labels[n] = UnicodeString(localizedFallback(kTabs[i].english, kTabs[i].russian));
		GadgetRadioSetText(button, labels[n]);
		plainTab(button);   // O125
		buttons[n++] = button;
	}

	const Bool across = (uy - sy) * (uy - sy) < (ux - sx) * (ux - sx);
	const Int h = sh;
	const Int gap = max(4, pw / 100);
	if (!across)
	{
		// the game's two are one under the other: the new ones go on below them
		const Int pitch = abs(uy - sy);
		const Int last = max(sy, uy);
		for (Int k = 2; k < n; ++k) {
			buttons[k]->winSetPosition(sx, last + (k - 1) * pitch);
		}
		return;
	}

	// the row: from the first tab to the window's edge or the first window right of the row (the preview frame)
	const Int first = min(sx, ux);
	Int rightLimit = pw - 4;
	const Int rowRight = max(sx + sw, ux + uw);
	for (GameWindow *other = parent->winGetChild(); other != nullptr; other = other->winGetNext())
	{
		Bool mine = FALSE;
		for (Int k = 0; k < n; ++k) {
			mine |= (other == buttons[k]);
		}
		if (mine) {
			continue;
		}
		Int ox = 0, oy = 0, ow = 0, oh = 0;
		other->winGetPosition(&ox, &oy);
		other->winGetSize(&ow, &oh);
		if (oy < sy + sh && oy + oh > sy && ox >= rowRight && ox - gap < rightLimit) {
			rightLimit = ox - gap;
		}
	}
	const Int span = rightLimit - first;

	// one row when the widest label fits a tab at 75 % of its font or more
	Int widest = 0;
	for (Int k = 0; k < n; ++k) {
		widest = max(widest, labelWidth(buttons[k], labels[k]));
	}
	const Int oneRowW = (span - (n - 1) * gap) / n;
	if (oneRowW >= widest * 3 / 4 + 8 || n <= 2)
	{
		for (Int k = 0; k < n; ++k)
		{
			buttons[k]->winSetSize(oneRowW, h);
			buttons[k]->winSetPosition(first + k * (oneRowW + gap), sy);
		}
		return;
	}

	// two rows of two; the list (and anything else under the row, across it) moves down by one row
	const Int twoW = (span - gap) / 2;
	for (Int k = 0; k < n; ++k)
	{
		buttons[k]->winSetSize(twoW, h);
		buttons[k]->winSetPosition(first + (k % 2) * (twoW + gap), sy + (k / 2) * (h + gap));
	}
	const Int shift = h + gap;
	for (GameWindow *other = parent->winGetChild(); other != nullptr; other = other->winGetNext())
	{
		Bool mine = FALSE;
		for (Int k = 0; k < n; ++k) {
			mine |= (other == buttons[k]);
		}
		if (mine) {
			continue;
		}
		Int ox = 0, oy = 0, ow = 0, oh = 0;
		other->winGetPosition(&ox, &oy);
		other->winGetSize(&ow, &oh);
		const Bool below = oy >= sy + h - 1 && oy < sy + h + shift + gap;
		const Bool acrossRow = ox < rightLimit && ox + ow > first;
		if (below && acrossRow && oh > shift + 20)
		{
			other->winSetPosition(ox, oy + shift);
			other->winSetSize(ow, oh - shift);
		}
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
