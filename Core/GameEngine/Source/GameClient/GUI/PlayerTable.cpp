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
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameText.h"
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
	/// middle. nullptr hides it. 'growInto': the next window of the row; when it starts right after 'text' and is wide,
	/// 'text' grows by the picture's room into it (once, when the picture is made), so the text keeps its full width.
	void setIcon(GameWindow *text, const Image *image, Color tint, Bool centred, GameWindow *growInto = nullptr)
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
		const Int side = h;   // TheSuperHackers @tweak the whole row height (G5)
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

			// TheSuperHackers @tweak The picture in front of a name does not cut the name's end: the name window grows
			// by the picture's room into the next window of the row (the in-game list: the army column, which has room).
			const Int room = side + 4;
			if (!centred && growInto != nullptr && growInto->winGetParent() == text->winGetParent())
			{
				Int tx = 0, ty = 0, gx = 0, gy = 0, gw = 0, gh = 0;
				text->winGetPosition(&tx, &ty);
				growInto->winGetPosition(&gx, &gy);
				growInto->winGetSize(&gw, &gh);
				const Bool sameRow = gy < ty + h && gy + gh > ty;
				const Bool adjacent = gx >= tx + w - 4 && gx <= tx + w + 16;
				if (sameRow && adjacent && gw > 4 * room)
				{
					text->winSetSize(w + room, h);
					growInto->winSetPosition(gx + room, gy);
					growInto->winSetSize(gw - room, gh);
					w += room;
				}
			}
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

	/// The header's visible text centred over its cell. With 'keepInsideRight' a text wider than the cell ends at the
	/// cell's right edge instead (the team column: its header must not run into the next column's header, G3).
	void centreHeader(GameWindow *header, Int cellX, Int cellW, Bool keepInsideRight)
	{
		if (header == nullptr) {
			return;
		}
		const ICoord2D p = posOf(header);
		const Int headerW = widthOf(header);
		Int textW = headerW;
		Int textOffset = 0;   // the visible text runs from x + textOffset for textW
		if ((header->winGetStyle() & GWS_STATIC_TEXT) != 0)
		{
			const TextData *data = (const TextData *)header->winGetUserData();
			if (data != nullptr && data->text != nullptr)
			{
				Int tw = 0, th = 0;
				data->text->getSize(&tw, &th);
				if (tw > 0 && tw <= headerW)
				{
					textW = tw;
					textOffset = data->centered ? (headerW - tw) / 2 : data->leftMargin;
				}
			}
		}
		Int textX = cellX + (cellW - textW) / 2;
		if (keepInsideRight && textX + textW > cellX + cellW) {
			textX = cellX + cellW - textW;
		}
		header->winSetPosition(textX - textOffset, p.y);
	}

	Int textWidth(GameFont *font, const UnicodeString &str)
	{
		if (font == nullptr || TheDisplayStringManager == nullptr) {
			return (Int)str.getLength() * 8;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return (Int)str.getLength() * 8;
		}
		ds->setFont(font);
		ds->setText(str);
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		TheDisplayStringManager->freeDisplayString(ds);
		return w;
	}

	/// The width the open colour list needs for its longest name ("Random" and the colour names), with room for the
	/// list's margins and its scroll bar. G4: the list is no wider than that, so it covers less of the army column.
	Int colourListWidth(GameWindow *combo, Int scrollW)
	{
		GameFont *font = combo->winGetFont();
		Int widest = textWidth(font, TheGameText->fetch("GUI:Random"));
		if (TheMultiplayerSettings != nullptr)
		{
			for (Int c = 0; c < TheMultiplayerSettings->getNumColors(); ++c)
			{
				const MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(c);
				if (def != nullptr) {
					widest = max(widest, textWidth(font, TheGameText->fetch(def->getTooltipName().str())));
				}
			}
		}
		return widest + 12 + scrollW;
	}

	/// The narrow cells need their pictures: without them the old texts and widths stay (G6).
	Bool colourPicturesThere()
	{
		return PlayerTable::picture(PlayerTable::PICTURE_RANDOM_COLOR) != nullptr && PlayerTable::picture(PlayerTable::PICTURE_COLOR) != nullptr;
	}

	Bool teamPictureThere()
	{
		return PlayerTable::picture(PlayerTable::PICTURE_NO_TEAM) != nullptr;
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
	// TheSuperHackers @bugfix Without the picture files the cells keep their old widths, so the old texts fit (G6).
	const Bool narrowColor = colourPicturesThere();
	const Bool narrowTeam = teamPictureThere();
	if (!narrowColor && !narrowTeam) {
		return;
	}
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
		const Int newColorW = narrowColor ? h + dropDownWidth(color) : oldColorW;
		const Int newTeamW = narrowTeam ? h + dropDownWidth(team) : oldTeamW;
		const Int newTeamX = teamRight - newTeamW;
		const Int newArmyX = cp.x + newColorW + gapColorArmy;
		const Int newArmyW = newTeamX - gapArmyTeam - newArmyX;
		if (newArmyW <= oldArmyW) {
			continue;   // an unknown layout (columns in another order): keep it
		}

		// the open lists are wide enough for the names: the team list keeps the old width, the colour list takes the
		// width of its longest name (at most the old width), so it hides less of the army column under it (G4)
		if (narrowColor) {
			GadgetComboBoxSetListMinWidth(color, max(newColorW, min(oldColorW, colourListWidth(color, dropDownWidth(color)))));
		}
		if (narrowTeam) {
			GadgetComboBoxSetListMinWidth(team, oldTeamW);
		}
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
		centreHeader(header, xs[k], ws[k], k == 2);
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
			if (slot->getTeamNumber() >= 0 && pirate != nullptr)
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
	setIcon(nameText, colorImage, tint, FALSE, sideText);
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

const Image *PlayerTable::colorPictureOf(Int color, Color *tint)
{
	return colorPicture(color, tint);
}

const Image *PlayerTable::armyPictureOf(Int playerTemplate)
{
	return armyPicture(playerTemplate);
}
