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
#include "GameClient/LocalizedFallback.h"
#include "GameNetwork/GameInfo.h"
#include "GameClient/Mouse.h"
#include "GameClient/LookPalette.h"   // O94
#include "Common/FrameShots.h"

#include <map>
#include <vector>

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

	//---------------------------------------------------------------------------------------------
	// O60 (row 46, S25): the admin's star in front of a name (a child window of the name text, after the colour picture)
	const Int kStarTag = 0x50545354;   ///< "PTST"

	void drawStarWindow(GameWindow *window, WinInstanceData *)
	{
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		PlayerTable::drawAdminStar(x + w / 2, y + h / 2, max(3, min(w, h) / 2 - 1));
	}

	void setStar(GameWindow *text, Bool show)
	{
		if (text == nullptr) {
			return;
		}
		GameWindow *starWin = nullptr;
		GameWindow *iconWin = nullptr;
		for (GameWindow *child = text->winGetChild(); child != nullptr; child = child->winGetNext())
		{
			if (child->winGetWindowId() == kStarTag) starWin = child;
			if (child->winGetWindowId() == kIconTag) iconWin = child;
		}
		Int w = 0, h = 0;
		text->winGetSize(&w, &h);
		const Int side = h;
		const Bool iconShown = iconWin != nullptr && !iconWin->winIsHidden();
		TextData *textData = ((text->winGetStyle() & GWS_STATIC_TEXT) != 0) ? (TextData *)text->winGetUserData() : nullptr;
		if (!show)
		{
			if (starWin != nullptr && !starWin->winIsHidden())
			{
				starWin->winHide(TRUE);
				if (textData != nullptr) {
					textData->leftMargin = iconShown ? side + 4 : 0;
				}
			}
			return;
		}
		if (starWin == nullptr)
		{
			WinInstanceData inst;
			inst.init();
			inst.m_style = GWS_USER_WINDOW | GWS_MOUSE_TRACK;
			starWin = TheWindowManager->winCreate(text, WIN_STATUS_ENABLED, 0, 0, side, side, PassMessagesToParentSystem, &inst);
			if (starWin == nullptr) {
				return;
			}
			starWin->winSetWindowId(kStarTag);
			starWin->winSetDrawFunc(drawStarWindow);
			starWin->winSetTooltip(PlayerTable::adminTip());
		}
		const Int starX = iconShown ? side + 3 : 1;
		starWin->winSetSize(side, side);
		starWin->winSetPosition(starX, (h - side) / 2);
		starWin->winHide(FALSE);
		if (textData != nullptr)
		{
			textData->centered = FALSE;
			textData->leftMargin = starX + side + 3;
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

	/// The army picture of a player template index: the random army emblem, or the template's own emblem.
	/// TheSuperHackers @tweak O91 (row 53) A general shows its own emblem (PlayerTemplate GeneralImage, the one of its
	/// command centre: Zero Hour's twelve armies), not only its side's icon (three for all twelve); a template without one
	/// (the base game, a FactionBossGeneral "None") keeps its side icon.
	const Image *armyPicture(Int playerTemplate)
	{
		if (playerTemplate == PLAYERTEMPLATE_OBSERVER) {
			return nullptr;
		}
		if (playerTemplate < 0) {
			return PlayerTable::picture(PlayerTable::PICTURE_RANDOM_ARMY);
		}
		const PlayerTemplate *pt = ThePlayerTemplateStore != nullptr ? ThePlayerTemplateStore->getNthPlayerTemplate(playerTemplate) : nullptr;
		if (pt == nullptr) {
			return nullptr;
		}
		const Image *emblem = pt->getGeneralImage();
		return emblem != nullptr ? emblem : pt->getSideIconImage();
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
		if (observer) {
			GadgetComboBoxSetText(comboColor, UnicodeString(L"-"));   // O56: an observer has no colour (was "Ran", cut)
		}
	}
	if (comboPlayerTemplate != nullptr)
	{
		const Image *image = armyPicture(slot->getPlayerTemplate());
		GadgetComboBoxSetClosedImage(comboPlayerTemplate, image, 0xFFFFFFFF, FALSE);
	}
	if (comboTeam != nullptr)
	{
		const Image *pirate = picture(PICTURE_NO_TEAM);
		if (observer)
		{
			// O56: an observer has no team (was "Non", cut)
			GadgetComboBoxSetClosedImage(comboTeam, nullptr, 0xFFFFFFFF, FALSE);
			GadgetComboBoxSetText(comboTeam, UnicodeString(L"-"));
		}
		else if (slot->getTeamNumber() < 0)
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

void PlayerTable::drawAdminStar(Int cx, Int cy, Int r)
{
	if (TheDisplay == nullptr || r < 2) {
		return;
	}
	// ten corners: the points (radius r) and the inner corners (0.45 r), the first point straight up
	Real px[10], py[10];
	for (Int i = 0; i < 10; ++i)
	{
		const Real a = -1.5707963f + i * 0.62831853f;
		const Real rad = (i % 2) == 0 ? (Real)r : 0.45f * r;
		px[i] = cx + rad * cosf(a);
		py[i] = cy + rad * sinf(a);
	}
	const Color gold = GameMakeColor(255, 200, 40, 255);
	const Color rim = GameMakeColor(70, 45, 0, 255);
	// filled by rows: the star's edges crossed at each row's middle, the spans between pairs of crossings
	for (Int y = cy - r; y <= cy + r; ++y)
	{
		const Real sy = y + 0.5f;
		Real xs[10];
		Int n = 0;
		for (Int i = 0; i < 10; ++i)
		{
			const Int j = (i + 1) % 10;
			if ((py[i] <= sy && py[j] > sy) || (py[j] <= sy && py[i] > sy)) {
				xs[n++] = px[i] + (sy - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
			}
		}
		for (Int a = 1; a < n; ++a)
		{
			const Real v = xs[a];
			Int b = a;
			for (; b > 0 && xs[b - 1] > v; --b) xs[b] = xs[b - 1];
			xs[b] = v;
		}
		for (Int k = 0; k + 1 < n; k += 2)
		{
			const Int x0 = REAL_TO_INT_FLOOR(xs[k] + 0.5f);
			const Int x1 = REAL_TO_INT_FLOOR(xs[k + 1] + 0.5f);
			if (x1 > x0) {
				TheDisplay->drawFillRect(x0, y, x1 - x0, 1, gold);
			}
		}
	}
	for (Int i = 0; i < 10; ++i)
	{
		const Int j = (i + 1) % 10;
		TheDisplay->drawLine((Int)px[i], (Int)py[i], (Int)px[j], (Int)py[j], 1.0f, rim);
	}
}

UnicodeString PlayerTable::adminTip()
{
	return TheGameText->FETCH_OR_SUBSTITUTE("GUI:AdminMarkTip", localizedFallback(L"Host (admin)", L"\x0425\x043E\x0441\x0442 (\x0430\x0434\x043C\x0438\x043D)"));
}

void PlayerTable::decorateRow(GameWindow *nameText, GameWindow *sideText, GameWindow *teamText, const GameSlot *slot, Bool admin)
{
	if (slot == nullptr) {
		return;
	}
	if (isObserverSlot(slot))
	{
		setIcon(nameText, nullptr, 0xFFFFFFFF, FALSE);
		setIcon(sideText, nullptr, 0xFFFFFFFF, FALSE);
		setIcon(teamText, nullptr, 0xFFFFFFFF, FALSE);
		setStar(nameText, admin);
		return;
	}
	// what this client may see: a random choice of a player who is not on its team stays random
	Color tint = 0xFFFFFFFF;
	const Image *colorImage = colorPicture(slot->getApparentColor(), &tint);
	setIcon(nameText, colorImage, tint, FALSE, sideText);
	setStar(nameText, admin);   // O60
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

//-------------------------------------------------------------------------------------------------
// O56 (row 44) team groups
namespace
{
	struct GroupLines
	{
		std::vector<Int> ys;
		Int x0, x1;
	};
	std::map<const GameWindow *, GroupLines> s_groupLines;
	const Color kGroupLine = LookPalette::kGroupLine;   // O94: gold in the base game

	void drawGroupLines(GameWindow *window, WinInstanceData *)
	{
		std::map<const GameWindow *, GroupLines>::const_iterator it = s_groupLines.find(window);
		if (it == s_groupLines.end() || TheDisplay == nullptr) {
			return;
		}
		for (size_t i = 0; i < it->second.ys.size(); ++i) {
			TheDisplay->drawFillRect(it->second.x0, it->second.ys[i] - 1, it->second.x1 - it->second.x0, 2, kGroupLine);
		}
	}

	WindowMsgHandledType groupLinesSystem(GameWindow *window, UnsignedInt msg, WindowMsgData, WindowMsgData)
	{
		if (msg == GWM_DESTROY) {
			s_groupLines.erase(window);
		}
		return MSG_IGNORED;
	}

	Bool slotOccupied(const GameSlot *slot)
	{
		return slot != nullptr && slot->isOccupied();
	}

	/// The lobby rows' state of the screen whose first window is 'key'.
	struct LobbyGroups
	{
		const GameWindow *key;
		enum { MAX_COLUMNS = 8 };
		Int baseY[MAX_COLUMNS][MAX_SLOTS];
		Int applied[MAX_SLOTS];
		Int pending[MAX_SLOTS];
		Bool pendingStarts[MAX_SLOTS];
		Bool appliedStarts[MAX_SLOTS];
		UnsignedInt pendingSince;
		UnsignedInt lastInput;
		ICoord2D lastMouse;
	};
	LobbyGroups s_lobby = { nullptr };
}

Bool PlayerTable::teamOrder(const GameInfo *game, Int order[], Bool groupStart[])
{
	Int rank[MAX_SLOTS];
	Bool anyTeam = FALSE;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game != nullptr ? game->getConstSlot(i) : nullptr;
		const Int team = slot != nullptr ? slot->getTeamNumber() : -1;
		if (slotOccupied(slot) && team >= 0) {
			rank[i] = team;                  // teams 1, 2, 3, 4 ...
			anyTeam = TRUE;
		} else if (slotOccupied(slot)) {
			rank[i] = 1000 + i;              // each player without a team alone
		} else {
			rank[i] = 100000;                // open and closed slots last, one group
		}
		order[i] = i;
	}
	if (!anyTeam)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			groupStart[i] = FALSE;
		}
		return FALSE;
	}
	// stable insertion sort by rank
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		const Int s = order[i];
		Int k = i;
		while (k > 0 && rank[order[k - 1]] > rank[s]) {
			order[k] = order[k - 1];
			--k;
		}
		order[k] = s;
	}
	for (Int k = 0; k < MAX_SLOTS; ++k) {
		groupStart[k] = k > 0 && rank[order[k]] != rank[order[k - 1]];
	}
	return TRUE;
}

void PlayerTable::showGroupLines(GameWindow *parent, const char *idName, const Int *ys, Int count, Int x0, Int x1)
{
	if (parent == nullptr || TheWindowManager == nullptr) {
		return;
	}
	const Int id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(idName));
	GameWindow *lines = TheWindowManager->winGetWindowFromId(parent, id);
	if (lines == nullptr)
	{
		if (count <= 0) {
			return;
		}
		Int w = 0, h = 0;
		parent->winGetSize(&w, &h);
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_USER_WINDOW;
		inst.m_id = id;
		// not enabled: a hit test passes it by (GameWindow::winPointInChild), so it takes no click. O67: and only 1 x 1 pixel
		// at the parent's corner: the tooltip search (winPointInAnyChild) ignores the enabled flag, and a window the size of
		// the parent took every tooltip of the screen. Its draw function draws at the lines' screen points anyway.
		(void)w;
		(void)h;
		lines = TheWindowManager->winCreate(parent, 0, 0, 0, 1, 1, groupLinesSystem, &inst);
		if (lines == nullptr) {
			return;
		}
		lines->winSetWindowId(id);
		lines->winSetDrawFunc(drawGroupLines);
	}
	GroupLines &g = s_groupLines[lines];
	g.ys.assign(ys, ys + (count > 0 ? count : 0));
	g.x0 = x0;
	g.x1 = x1;
}

void PlayerTable::groupLobbyRows(const GameInfo *game, GameWindow *parent, GameWindow **columns[], Int columnCount)
{
	if (game == nullptr || parent == nullptr || columns == nullptr || columnCount <= 0) {
		return;
	}
	columnCount = min(columnCount, (Int)LobbyGroups::MAX_COLUMNS);
	const GameWindow *key = nullptr;
	for (Int c = 0; c < columnCount && key == nullptr; ++c) {
		key = columns[c] != nullptr ? columns[c][0] : nullptr;
	}
	if (key == nullptr) {
		return;
	}
	LobbyGroups &g = s_lobby;
	const UnsignedInt now = timeGetTime();
	// a screen made anew (possibly at the same addresses) shows the slot order: its rows are not where this state put them
	Bool fresh = g.key != key;
	for (Int c = 0; c < columnCount && !fresh; ++c) {
		for (Int k = 0; k < MAX_SLOTS && !fresh; ++k)
		{
			GameWindow *w = columns[c] != nullptr ? columns[c][g.applied[k]] : nullptr;
			if (w != nullptr)
			{
				Int x = 0, y = 0;
				w->winGetPosition(&x, &y);
				fresh = y != g.baseY[c][k];
			}
		}
	}
	if (fresh)
	{
		// a new screen: its rows' places as the .wnd made them
		g.key = key;
		for (Int c = 0; c < columnCount; ++c) {
			for (Int s = 0; s < MAX_SLOTS; ++s)
			{
				Int x = 0, y = 0;
				GameWindow *w = columns[c] != nullptr ? columns[c][s] : nullptr;
				if (w != nullptr) {
					w->winGetPosition(&x, &y);
				}
				g.baseY[c][s] = y;
			}
		}
		for (Int s = 0; s < MAX_SLOTS; ++s) {
			g.applied[s] = g.pending[s] = s;
			g.appliedStarts[s] = g.pendingStarts[s] = FALSE;
		}
		g.pendingSince = g.lastInput = now;
		g.lastMouse.x = g.lastMouse.y = -1;
	}

	// mouse input: the pointer moved or a button is held
	// The real mouse (TheMouse: the device's state, every frame) or, while a -uiScript plays, the script's pointer and
	// button (O67: it sends raw mouse messages, which TheMouse never sees, so its motion did not count)
	{
		ICoord2D pos = { -1, -1 };
		Bool held = FALSE;
		Bool known = FrameShots::uiScriptMouse(&pos, &held);
		if (!known && TheMouse != nullptr)
		{
			const MouseIO *m = TheMouse->getMouseStatus();
			if (m != nullptr)
			{
				pos = m->pos;
				held = m->leftState == MBS_Down || m->rightState == MBS_Down;
				known = TRUE;
			}
		}
		if (known && (pos.x != g.lastMouse.x || pos.y != g.lastMouse.y || held))
		{
			g.lastMouse = pos;
			g.lastInput = now;
		}
	}
	// a list of the table that is open
	Bool listOpen = FALSE;
	for (Int c = 0; c < columnCount && !listOpen; ++c) {
		for (Int s = 0; s < MAX_SLOTS && !listOpen; ++s)
		{
			GameWindow *w = columns[c] != nullptr ? columns[c][s] : nullptr;
			if (w != nullptr && (w->winGetStyle() & GWS_COMBO_BOX) != 0)
			{
				GameWindow *list = GadgetComboBoxGetListBox(w);
				listOpen = list != nullptr && !list->winIsHidden();
			}
		}
	}

	Int order[MAX_SLOTS];
	Bool starts[MAX_SLOTS];
	teamOrder(game, order, starts);
	Bool changed = FALSE;
	for (Int k = 0; k < MAX_SLOTS; ++k) {
		changed = changed || order[k] != g.pending[k] || starts[k] != g.pendingStarts[k];
	}
	if (changed)
	{
		for (Int k = 0; k < MAX_SLOTS; ++k) {
			g.pending[k] = order[k];
			g.pendingStarts[k] = starts[k];
		}
		g.pendingSince = now;
	}
	Bool differs = FALSE;
	for (Int k = 0; k < MAX_SLOTS; ++k) {
		differs = differs || g.pending[k] != g.applied[k] || g.pendingStarts[k] != g.appliedStarts[k];
	}
	if (differs && !listOpen && now - g.pendingSince >= 1000 && now - g.lastInput >= 1000)
	{
		for (Int k = 0; k < MAX_SLOTS; ++k)
		{
			const Int s = g.pending[k];
			for (Int c = 0; c < columnCount; ++c)
			{
				GameWindow *w = columns[c] != nullptr ? columns[c][s] : nullptr;
				if (w != nullptr)
				{
					Int x = 0, y = 0;
					w->winGetPosition(&x, &y);
					w->winSetPosition(x, g.baseY[c][k]);
				}
			}
			g.applied[k] = s;
			g.appliedStarts[k] = g.pendingStarts[k];
		}
	}

	// the lines between the applied groups, over the row's whole width
	Int ys[MAX_SLOTS];
	Int count = 0;
	Int x0 = 100000, x1 = -100000;
	for (Int k = 0; k < MAX_SLOTS; ++k)
	{
		const Int s = g.applied[k];
		Int top = 100000;
		for (Int c = 0; c < columnCount; ++c)
		{
			GameWindow *w = columns[c] != nullptr ? columns[c][s] : nullptr;
			if (w == nullptr || w->winIsHidden()) {
				continue;
			}
			Int sx = 0, sy = 0, sw = 0, sh = 0;
			w->winGetScreenPosition(&sx, &sy);
			w->winGetSize(&sw, &sh);
			x0 = min(x0, sx);
			x1 = max(x1, sx + sw);
			top = min(top, sy);
		}
		if (g.appliedStarts[k] && top < 100000) {
			ys[count++] = top;
		}
	}
	showGroupLines(parent, "PlayerTable.wnd:GroupLines", ys, x1 > x0 ? count : 0, x0, x1);
}

const Image *PlayerTable::colorPictureOf(Int color, Color *tint)
{
	return colorPicture(color, tint);
}

const Image *PlayerTable::armyPictureOf(Int playerTemplate)
{
	return armyPicture(playerTemplate);
}
