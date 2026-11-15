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

// TheSuperHackers @feature See PlayerTable.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/PlayerTable.h"

#include "Common/FileSystem.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PlayerTemplate.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Image.h"
#include "GameNetwork/GameInfo.h"

namespace
{
	const char *const kTextures[PlayerTable::PICTURE_COUNT] =
	{
		"PlayerTable_RandomColor.tga",
		"PlayerTable_Color.tga",
		"PlayerTable_NoTeam.tga",
		"PlayerTable_RandomArmy.tga",
		"PlayerTable_AntiCheatOn.tga",
		"PlayerTable_AntiCheatOff.tga",
	};

	//---------------------------------------------------------------------------------------------
	// Small picture windows on top of static texts (loading screen, in-game list). Each one points at an entry of a
	// fixed pool; the pool is reused round robin, more than enough for the rows of one screen.
	struct Icon
	{
		const Image *image;
		Color tint;
	};
	enum { ICON_POOL = 256 };
	Icon s_icons[ICON_POOL];
	Int s_nextIcon = 0;

	void drawIcon(GameWindow *window, WinInstanceData *instData)
	{
		(void)instData;
		const Icon *icon = (const Icon *)window->winGetUserData();
		if (icon == nullptr || icon->image == nullptr || TheDisplay == nullptr) {
			return;
		}
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		TheDisplay->drawImage(icon->image, x, y, x + w, y + h, icon->tint);
	}

	const Int kIconTag = 0x50544943;   ///< "PTIC": the window id of our picture windows

	/// The picture window on 'text' (made on the first call), showing 'image' at the left or, with 'centred', in the
	/// middle. nullptr hides it.
	void setIcon(GameWindow *text, const Image *image, Color tint, Bool centred)
	{
		if (text == nullptr) {
			return;
		}
		GameWindow *iconWin = nullptr;
		for (GameWindow *child = text->winGetChild(); child != nullptr; child = child->winGetNext()) {
			if (child->winGetWindowId() == kIconTag) {
				iconWin = child;
			}
		}
		Int w = 0, h = 0;
		text->winGetSize(&w, &h);
		const Int side = h > 4 ? h - 2 : h;
		TextData *textData = ((text->winGetStyle() & GWS_STATIC_TEXT) != 0) ? (TextData *)text->winGetUserData() : nullptr;
		if (image == nullptr)
		{
			if (iconWin != nullptr) {
				iconWin->winHide(TRUE);
			}
			return;
		}
		if (iconWin == nullptr)
		{
			WinInstanceData inst;
			inst.init();
			inst.m_style = GWS_USER_WINDOW;
			iconWin = TheWindowManager->winCreate(text, WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT, 0, 0, side, side, PassMessagesToParentSystem, &inst);
			if (iconWin == nullptr) {
				return;
			}
			iconWin->winSetWindowId(kIconTag);
			iconWin->winSetDrawFunc(drawIcon);
			iconWin->winSetUserData(&s_icons[s_nextIcon]);
			s_nextIcon = (s_nextIcon + 1) % ICON_POOL;
		}
		Icon *icon = (Icon *)iconWin->winGetUserData();
		icon->image = image;
		icon->tint = tint;
		iconWin->winSetSize(side, side);
		iconWin->winSetPosition(centred ? (w - side) / 2 : 1, (h - side) / 2);
		iconWin->winHide(FALSE);
		if (textData != nullptr && !centred)
		{
			// the text starts after the picture
			textData->centered = FALSE;
			textData->leftMargin = side + 4;
		}
	}

	Bool isObserverSlot(const GameSlot *slot)
	{
		return slot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER;
	}

	/// The colour picture of a colour index: the rainbow for random, the tinted spiral for a colour.
	const Image *colorPicture(Int color, Color *tint)
	{
		*tint = 0xFFFFFFFF;
		if (color < 0 || TheMultiplayerSettings == nullptr || color >= TheMultiplayerSettings->getNumColors()) {
			return PlayerTable::picture(PlayerTable::PICTURE_RANDOM_COLOR);
		}
		const MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(color);
		if (def != nullptr) {
			*tint = def->getColor() | 0xFF000000;
		}
		return PlayerTable::picture(PlayerTable::PICTURE_COLOR);
	}

	/// The army picture of a player template index: the random army emblem, or the template's side icon.
	const Image *armyPicture(Int playerTemplate)
	{
		if (playerTemplate == PLAYERTEMPLATE_OBSERVER) {
			return nullptr;
		}
		if (playerTemplate < 0) {
			return PlayerTable::picture(PlayerTable::PICTURE_RANDOM_ARMY);
		}
		const PlayerTemplate *pt = ThePlayerTemplateStore != nullptr ? ThePlayerTemplateStore->getNthPlayerTemplate(playerTemplate) : nullptr;
		return pt != nullptr ? pt->getSideIconImage() : nullptr;
	}

	Int heightOf(GameWindow *w) { Int x = 0, y = 0; w->winGetSize(&x, &y); return y; }
	Int widthOf(GameWindow *w) { Int x = 0, y = 0; w->winGetSize(&x, &y); return x; }
	ICoord2D posOf(GameWindow *w) { ICoord2D p = { 0, 0 }; w->winGetPosition(&p.x, &p.y); return p; }

	Int dropDownWidth(GameWindow *combo)
	{
		const ComboBoxData *data = (const ComboBoxData *)combo->winGetUserData();
		if (data != nullptr && data->dropDownButton != nullptr) {
			return widthOf(data->dropDownButton);
		}
		return heightOf(combo);
	}

	void centreHeader(GameWindow *header, Int cellX, Int cellW)
	{
		if (header == nullptr) {
			return;
		}
		const ICoord2D p = posOf(header);
		header->winSetPosition(cellX + (cellW - widthOf(header)) / 2, p.y);
	}
}

//-------------------------------------------------------------------------------------------------
const Image *PlayerTable::picture(Picture which)
{
	if (which < 0 || which >= PICTURE_COUNT || TheMappedImageCollection == nullptr) {
		return nullptr;
	}
	static Bool s_checked[PICTURE_COUNT] = { FALSE };
	static Bool s_present[PICTURE_COUNT] = { FALSE };
	const char *name = kTextures[which];
	if (!s_checked[which] && TheFileSystem != nullptr)
	{
		s_checked[which] = TRUE;
		AsciiString path("Art\\Textures\\");
		path.concat(name);
		s_present[which] = TheFileSystem->doesFileExist(path.str());
	}
	if (!s_present[which]) {
		return nullptr;
	}
	const Image *found = TheMappedImageCollection->findImageByName(name);
	if (found != nullptr) {
		return found;
	}
	Image *image = newInstance(Image);
	image->setName(name);
	image->setFilename(name);
	image->setStatus(IMAGE_STATUS_NONE);
	Region2D uv;
	uv.lo.x = 0.0f;
	uv.lo.y = 0.0f;
	uv.hi.x = 1.0f;
	uv.hi.y = 1.0f;
	image->setUV(&uv);
	image->setTextureWidth(64);
	image->setTextureHeight(64);
	ICoord2D size = { 64, 64 };
	image->setImageSize(&size);
	TheMappedImageCollection->addImage(image);
	return image;
}

void PlayerTable::layout(const char *wndPrefix, GameWindow *comboColor[], GameWindow *comboPlayerTemplate[], GameWindow *comboTeam[],
	Int slots)
{
	Int colorX = -1, colorW = 0, armyX = 0, armyW = 0, teamX = 0, teamW = 0;
	for (Int i = 0; i < slots; ++i)
	{
		GameWindow *color = comboColor[i];
		GameWindow *army = comboPlayerTemplate[i];
		GameWindow *team = comboTeam[i];
		if (color == nullptr || army == nullptr || team == nullptr) {
			continue;
		}
		const Int h = heightOf(color);
		const ICoord2D cp = posOf(color), ap = posOf(army), tp = posOf(team);
		const Int oldColorW = widthOf(color), oldArmyW = widthOf(army), oldTeamW = widthOf(team);
		const Int gapColorArmy = ap.x - (cp.x + oldColorW);
		const Int gapArmyTeam = tp.x - (ap.x + oldArmyW);
		const Int teamRight = tp.x + oldTeamW;

		// colour: the picture square plus the arrow; team: one digit square plus the arrow
		const Int newColorW = h + dropDownWidth(color);
		const Int newTeamW = h + dropDownWidth(team);
		const Int newTeamX = teamRight - newTeamW;
		const Int newArmyX = cp.x + newColorW + gapColorArmy;
		const Int newArmyW = newTeamX - gapArmyTeam - newArmyX;
		if (newArmyW <= oldArmyW) {
			continue;   // an unknown layout (columns in another order): keep it
		}

		// the open lists keep the old widths, so the names fit there
		GadgetComboBoxSetListMinWidth(color, oldColorW);
		GadgetComboBoxSetListMinWidth(team, oldTeamW);
		color->winSetSize(newColorW, h);
		army->winSetPosition(newArmyX, ap.y);
		army->winSetSize(newArmyW, heightOf(army));
		team->winSetPosition(newTeamX, tp.y);
		team->winSetSize(newTeamW, heightOf(team));

		if (colorX < 0)
		{
			colorX = cp.x; colorW = newColorW;
			armyX = newArmyX; armyW = newArmyW;
			teamX = newTeamX; teamW = newTeamW;
		}
	}
	if (colorX < 0 || wndPrefix == nullptr) {
		return;
	}

	// the headers over their columns (they share the parent of the first row)
	GameWindow *parent = comboColor[0] != nullptr ? comboColor[0]->winGetParent() : nullptr;
	const char *const headers[3] = { "StaticTextColor", "StaticTextFaction", "StaticTextTeam" };
	const Int xs[3] = { colorX, armyX, teamX };
	const Int ws[3] = { colorW, armyW, teamW };
	for (Int k = 0; k < 3; ++k)
	{
		AsciiString name(wndPrefix);
		name.concat(headers[k]);
		GameWindow *header = TheWindowManager->winGetWindowFromId(parent, (Int)TheNameKeyGenerator->nameToKey(name));
		if (header == nullptr) {
			header = TheWindowManager->winGetWindowFromId(nullptr, (Int)TheNameKeyGenerator->nameToKey(name));
		}
		centreHeader(header, xs[k], ws[k]);
	}
}

void PlayerTable::applyLobbyRow(GameWindow *comboColor, GameWindow *comboPlayerTemplate, GameWindow *comboTeam, const GameSlot *slot)
{
	if (slot == nullptr) {
		return;
	}
	const Bool observer = isObserverSlot(slot);
	if (comboColor != nullptr)
	{
		Color tint = 0xFFFFFFFF;
		const Image *image = observer ? nullptr : colorPicture(slot->getColor(), &tint);
		GadgetComboBoxSetClosedImage(comboColor, image, tint, image != nullptr);
	}
	if (comboPlayerTemplate != nullptr)
	{
		const Image *image = armyPicture(slot->getPlayerTemplate());
		GadgetComboBoxSetClosedImage(comboPlayerTemplate, image, 0xFFFFFFFF, FALSE);
	}
	if (comboTeam != nullptr)
	{
		const Image *pirate = picture(PICTURE_NO_TEAM);
		if (slot->getTeamNumber() < 0 && !observer)
		{
			GadgetComboBoxSetClosedImage(comboTeam, pirate, 0xFFFFFFFF, pirate != nullptr);
		}
		else
		{
			GadgetComboBoxSetClosedImage(comboTeam, nullptr, 0xFFFFFFFF, FALSE);
			if (slot->getTeamNumber() >= 0)
			{
				// the narrow cell shows the digit; the open list keeps the team names
				UnicodeString digit;
				digit.format(L"%d", slot->getTeamNumber() + 1);
				GadgetComboBoxSetText(comboTeam, digit);
			}
		}
	}
}

void PlayerTable::decorateRow(GameWindow *nameText, GameWindow *sideText, GameWindow *teamText, const GameSlot *slot)
{
	if (slot == nullptr) {
		return;
	}
	if (isObserverSlot(slot))
	{
		setIcon(nameText, nullptr, 0xFFFFFFFF, FALSE);
		setIcon(sideText, nullptr, 0xFFFFFFFF, FALSE);
		setIcon(teamText, nullptr, 0xFFFFFFFF, FALSE);
		return;
	}
	// what this client may see: a random choice of a player who is not on its team stays random
	Color tint = 0xFFFFFFFF;
	const Image *colorImage = colorPicture(slot->getApparentColor(), &tint);
	setIcon(nameText, colorImage, tint, FALSE);
	setIcon(sideText, armyPicture(slot->getApparentPlayerTemplate()), 0xFFFFFFFF, FALSE);
	if (teamText != nullptr)
	{
		const Image *pirate = slot->getTeamNumber() < 0 ? picture(PICTURE_NO_TEAM) : nullptr;
		if (pirate != nullptr && (teamText->winGetStyle() & GWS_STATIC_TEXT) != 0)
		{
			GadgetStaticTextSetText(teamText, UnicodeString::TheEmptyString);   // the sign instead of "None"
		}
		setIcon(teamText, pirate, 0xFFFFFFFF, TRUE);
	}
}
