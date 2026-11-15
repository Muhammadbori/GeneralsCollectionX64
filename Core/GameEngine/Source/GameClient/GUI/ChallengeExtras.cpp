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

// TheSuperHackers @feature See ChallengeExtras.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/ChallengeExtras.h"

#include "Common/GameCommon.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/UserPreferences.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/Color.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameSpeedControl.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Image.h"
#include "GameClient/LocalizedFallback.h"

//=================================================================================================
// The stored run
//=================================================================================================
namespace
{
	class ChallengeRunPreferences : public UserPreferences
	{
	public:
		ChallengeRunPreferences()
		{
			if (rts::ClientInstance::getInstanceId() > 1u)
			{
				AsciiString fname;
				fname.format("ChallengeRun_Instance%.2u.ini", rts::ClientInstance::getInstanceId());
				load(fname);
			}
			else
			{
				load("ChallengeRun.ini");
			}
		}
	};

	Int clampDifficulty(Int d)
	{
		return d < DIFFICULTY_EASY ? DIFFICULTY_EASY : (d > DIFFICULTY_HARD ? DIFFICULTY_HARD : d);
	}

	/// The index of the challenge general whose campaign is 'campaign', or -1.
	Int generalOfCampaign(const AsciiString &campaign)
	{
		if (TheChallengeGenerals == nullptr || campaign.isEmpty()) {
			return -1;
		}
		const GeneralPersona *generals = TheChallengeGenerals->getChallengeGenerals();
		for (Int i = 0; i < NUM_GENERALS; ++i) {
			if (generals[i].getCampaign().compareNoCase(campaign) == 0) {
				return i;
			}
		}
		return -1;
	}

	/// Writes the battle the campaign manager is on as the run's next battle.
	void storeCurrentBattle(Int general, Int difficulty)
	{
		Campaign *campaign = TheCampaignManager != nullptr ? TheCampaignManager->getCurrentCampaign() : nullptr;
		Mission *mission = TheCampaignManager != nullptr ? TheCampaignManager->getCurrentMission() : nullptr;
		ChallengeRunPreferences prefs;
		if (campaign == nullptr || mission == nullptr || general < 0)
		{
			prefs.erase("Campaign");
			prefs.erase("Mission");
		}
		else
		{
			prefs.setAsciiString("Campaign", campaign->m_name);
			prefs.setAsciiString("Mission", mission->m_name);
			prefs.setInt("General", general);
			prefs.setInt("Difficulty", clampDifficulty(difficulty));
			prefs.setInt("RankPoints", TheCampaignManager->getRankPoints());
		}
		prefs.write();
	}
}

Bool ChallengeRun::load(Run &run)
{
	ChallengeRunPreferences prefs;
	run.campaign = prefs.getAsciiString("Campaign", AsciiString::TheEmptyString);
	run.mission = prefs.getAsciiString("Mission", AsciiString::TheEmptyString);
	run.general = prefs.getInt("General", -1);
	run.difficulty = clampDifficulty(prefs.getInt("Difficulty", DIFFICULTY_NORMAL));
	run.rankPoints = prefs.getInt("RankPoints", 0);
	if (run.campaign.isEmpty() || run.mission.isEmpty() || run.general < 0 || run.general >= NUM_GENERALS) {
		return FALSE;
	}
	// the general must still be there with the same campaign (the challenge INI may differ between installs)
	return generalOfCampaign(run.campaign) == run.general;
}

void ChallengeRun::loadSpeed(Int &logicFps, Bool &normal)
{
	ChallengeRunPreferences prefs;
	logicFps = prefs.getInt("GameSpeed", LOGICFRAMES_PER_SECOND);
	normal = prefs.getBool("GameSpeedNormal", GameSpeed::logicFpsToStep(logicFps) == GameSpeed::NormalStep);
}

void ChallengeRun::saveSpeed(Int logicFps, Bool normal)
{
	ChallengeRunPreferences prefs;
	prefs.setInt("GameSpeed", logicFps);
	prefs.setBool("GameSpeedNormal", normal);
	prefs.write();
}

Int ChallengeRun::loadDifficulty(Int fallback)
{
	ChallengeRunPreferences prefs;
	return clampDifficulty(prefs.getInt("MenuDifficulty", fallback));
}

void ChallengeRun::onBattleStarted(Int general, Int difficulty)
{
	storeCurrentBattle(general, difficulty);
}

void ChallengeRun::onBattleWon()
{
#if RTS_ZEROHOUR
	Campaign *campaign = TheCampaignManager != nullptr ? TheCampaignManager->getCurrentCampaign() : nullptr;
	if (campaign == nullptr || !campaign->isChallengeCampaign()) {
		return;
	}
	if (TheCampaignManager->getCurrentMap().isEmpty())
	{
		// the last battle of this general was won: the run is over
		ChallengeRunPreferences prefs;
		if (prefs.getAsciiString("Campaign", AsciiString::TheEmptyString).compareNoCase(campaign->m_name) == 0)
		{
			prefs.erase("Campaign");
			prefs.erase("Mission");
			prefs.write();
		}
		return;
	}
	// The run follows the general just played, also when the battle came from a loaded save.
	storeCurrentBattle(generalOfCampaign(campaign->m_name), TheCampaignManager->getGameDifficulty());
#endif
}

Int ChallengeRun::getRunLogicFps()
{
	Int fps = LOGICFRAMES_PER_SECOND;
	Bool normal = TRUE;
	loadSpeed(fps, normal);
	return normal ? (Int)LOGICFRAMES_PER_SECOND : GameSpeed::stepToLogicFps(GameSpeed::logicFpsToStep(fps));
}

//=================================================================================================
// The controls of the challenge menu
//=================================================================================================
namespace
{
	enum { DIFFICULTY_BUTTONS = 3 };

	struct State
	{
		GameWindow *parent;
		GameWindow *difficultyLabel;
		GameWindow *difficulty[DIFFICULTY_BUTTONS];
		GameWindow *speedLabel;
		GameWindow *speedSlider;
		GameWindow *speedText;
		GameWindow *checkTemplate;
		GameWindow *buttonContinue;
		WinDrawData buttonEnabled[MAX_DRAW_DATA];
		WinDrawData buttonHilite[MAX_DRAW_DATA];
		Color difficultyTextColor;
		Color difficultyTextBorder;
		Color enabledTextColor;      ///< the button's own text colours, and its highlight ones for the chosen difficulty
		Color enabledTextBorder;
		Color hiliteTextColor;
		Color hiliteTextBorder;
		Int chosenDifficulty;
		Bool continueMode;
		Bool hasRun;
		Bool shown;   ///< O24: the controls are shown (only after the menu's enter transition)
	};
	State s;
	ChallengeRun::Run s_run;   ///< kept apart from the plain state; emptied on shutdown
	GameSpeedControl s_speed;

	// Everything is placed in the 800 x 600 frame of ChallengeMenu.wnd and scaled to the parent's size.
	Int sx(Int x) { Int w = 800, h = 600; if (s.parent) s.parent->winGetSize(&w, &h); return x * w / 800; }
	Int sy(Int y) { Int w = 800, h = 600; if (s.parent) s.parent->winGetSize(&w, &h); return y * h / 600; }

	Int textWidth(GameFont *font, const UnicodeString &text)
	{
		if (font == nullptr || TheDisplayStringManager == nullptr || text.isEmpty()) {
			return 0;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return 0;
		}
		ds->setFont(font);
		ds->setText(text);
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		TheDisplayStringManager->freeDisplayString(ds);
		return w;
	}

	void copyLook(WinInstanceData &inst, GameWindow *look, UnsignedInt style)
	{
		inst.init();
		inst.m_style = style;
		if (look == nullptr) {
			return;
		}
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

	const Image *imageNamed(const char *name)
	{
		return TheMappedImageCollection != nullptr ? TheMappedImageCollection->findImageByName(name) : nullptr;
	}

	void setDraw(WinDrawData &d, const char *image, UnsignedByte r, UnsignedByte g, UnsignedByte b, UnsignedByte a,
		UnsignedByte br, UnsignedByte bg, UnsignedByte bb, UnsignedByte ba)
	{
		d.image = image != nullptr ? imageNamed(image) : nullptr;
		d.color = GameMakeColor(r, g, b, a);
		d.borderColor = GameMakeColor(br, bg, bb, ba);
	}

	UnicodeString text(const char *label, const WideChar *english, const WideChar *russian)
	{
		return TheGameText->FETCH_OR_SUBSTITUTE(label, localizedFallback(english, russian));
	}

	GameWindow *makeLabel(GameFont *font, Int x, Int y, Int w, Int h, const UnicodeString &str)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_STATIC_TEXT;
		inst.m_font = font;
		inst.m_enabledText.color = GameMakeColor(255, 254, 252, 255);
		inst.m_enabledText.borderColor = GameMakeColor(0, 0, 0, 255);
		inst.m_disabledText = inst.m_enabledText;
		inst.m_hiliteText = inst.m_enabledText;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			setDraw(inst.m_enabledDrawData[i], nullptr, 255, 255, 255, 0, 255, 255, 255, 0);
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
		}
		TextData textData;
		memset(&textData, 0, sizeof(textData));
		textData.centeredVertically = TRUE;
		GameWindow *win = TheWindowManager->gogoGadgetStaticText(s.parent, WIN_STATUS_ENABLED, x, y, w, h, &inst, &textData, font, FALSE);
		if (win != nullptr) {
			GadgetStaticTextSetText(win, str);
		}
		return win;
	}

	GameWindow *makeButton(GameWindow *look, GameFont *font, Int x, Int y, Int w, Int h, const UnicodeString &str, const char *name)
	{
		WinInstanceData inst;
		copyLook(inst, look, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
		if (font != nullptr) {
			inst.m_font = font;
		}
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));
		const UnsignedInt status = WIN_STATUS_ENABLED | (look != nullptr ? (look->winGetStatus() & WIN_STATUS_IMAGE) : 0);
		GameWindow *win = TheWindowManager->gogoGadgetPushButton(s.parent, status, x, y, w, h, &inst, inst.m_font, look == nullptr);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			GadgetButtonSetText(win, str);
		}
		return win;
	}

	/// The speed slider in the look of the skirmish menu's SliderGameSpeed (its images by name).
	GameWindow *makeSpeedSlider(Int x, Int y, Int w, Int h)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_HORZ_SLIDER | GWS_MOUSE_TRACK;
		inst.m_id = (Int)TheNameKeyGenerator->nameToKey("ChallengeMenu.wnd:SliderGameSpeed");
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			setDraw(inst.m_enabledDrawData[i], nullptr, 255, 255, 255, 255, 255, 255, 255, 255);
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
		}
		setDraw(inst.m_enabledDrawData[0], nullptr, 255, 0, 0, 255, 255, 128, 128, 255);
		setDraw(inst.m_disabledDrawData[0], "hilightedbox", 128, 128, 128, 255, 64, 64, 64, 255);
		setDraw(inst.m_disabledDrawData[1], "dehilightedbox", 255, 255, 255, 255, 255, 255, 255, 255);
		setDraw(inst.m_hiliteDrawData[0], "linebox", 0, 255, 0, 255, 0, 128, 0, 255);
		setDraw(inst.m_hiliteDrawData[1], "arrow", 255, 255, 255, 255, 255, 255, 255, 255);
		SliderData data;
		memset(&data, 0, sizeof(data));
		data.minVal = GameSpeed::MinStep;
		data.maxVal = GameSpeed::MaxStep;
		GameWindow *win = TheWindowManager->gogoGadgetSlider(s.parent, WIN_STATUS_ENABLED | WIN_STATUS_IMAGE, x, y, w, h, &inst, &data,
			nullptr, FALSE);
		if (win != nullptr)
		{
			win->winSetWindowId(inst.m_id);
			if (GameWindow *thumb = win->winGetChild())
			{
				WinInstanceData *t = thumb->winGetInstanceData();
				setDraw(t->m_enabledDrawData[0], nullptr, 255, 128, 128, 255, 255, 0, 0, 255);
				setDraw(t->m_enabledDrawData[1], nullptr, 128, 128, 128, 255, 192, 192, 192, 255);
				setDraw(t->m_disabledDrawData[0], nullptr, 64, 64, 64, 255, 128, 128, 128, 255);
				setDraw(t->m_disabledDrawData[1], nullptr, 0, 0, 0, 255, 64, 64, 64, 255);
				setDraw(t->m_hiliteDrawData[0], "arrow", 0, 255, 0, 255, 128, 255, 128, 255);
				setDraw(t->m_hiliteDrawData[1], "arrow", 0, 0, 255, 255, 128, 128, 255, 255);
			}
		}
		return win;
	}

	/// A hidden check box in the look of the skirmish menu's check boxes; the Normal box copies it.
	GameWindow *makeCheckTemplate(GameFont *font)
	{
		WinInstanceData inst;
		inst.init();
		inst.m_style = GWS_CHECK_BOX | GWS_MOUSE_TRACK;
		inst.m_font = font;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			setDraw(inst.m_enabledDrawData[i], nullptr, 255, 255, 255, 0, 255, 255, 255, 0);
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
		}
		setDraw(inst.m_enabledDrawData[0], nullptr, 255, 0, 0, 255, 255, 128, 128, 255);
		setDraw(inst.m_enabledDrawData[1], "Active-Unchecked", 255, 255, 255, 0, 128, 128, 255, 255);
		setDraw(inst.m_enabledDrawData[2], "Active-Checked", 0, 0, 255, 255, 128, 128, 255, 255);
		setDraw(inst.m_disabledDrawData[0], nullptr, 128, 128, 128, 255, 192, 192, 192, 255);
		setDraw(inst.m_disabledDrawData[1], "Disabled-Unchecked", 255, 255, 255, 0, 192, 192, 192, 255);
		setDraw(inst.m_disabledDrawData[2], "Disabled-Checked", 64, 64, 64, 255, 254, 254, 254, 255);
		setDraw(inst.m_hiliteDrawData[0], nullptr, 0, 255, 0, 255, 128, 255, 128, 255);
		setDraw(inst.m_hiliteDrawData[1], "Active-HiLighted", 255, 255, 255, 0, 128, 128, 255, 255);
		setDraw(inst.m_hiliteDrawData[2], "Active-Checked", 255, 255, 0, 255, 254, 254, 254, 255);
		inst.m_enabledText.color = GameMakeColor(254, 254, 254, 255);
		inst.m_enabledText.borderColor = GameMakeColor(0, 0, 0, 255);
		inst.m_disabledText.color = GameMakeColor(192, 192, 192, 255);
		inst.m_disabledText.borderColor = GameMakeColor(64, 64, 64, 255);
		inst.m_hiliteText.color = GameMakeColor(128, 128, 255, 255);
		inst.m_hiliteText.borderColor = GameMakeColor(0, 0, 128, 255);
		GameWindow *win = TheWindowManager->gogoGadgetCheckbox(s.parent, WIN_STATUS_ENABLED | WIN_STATUS_IMAGE, 0, 0, 20, 20, &inst, font, FALSE);
		if (win != nullptr) {
			win->winHide(TRUE);
		}
		return win;
	}

	/// The chosen difficulty button is drawn with the button's highlight look; with "Continue" all three are disabled
	/// and the run's difficulty is written in green.
	void showDifficulty()
	{
		const Int shown = s.continueMode ? s_run.difficulty : s.chosenDifficulty;
		for (Int i = 0; i < DIFFICULTY_BUTTONS; ++i)
		{
			GameWindow *b = s.difficulty[i];
			if (b == nullptr) {
				continue;
			}
			// the chosen one: the button's highlight look, pictures and text (the game's look of a selected button)
			WinInstanceData *inst = b->winGetInstanceData();
			const Bool chosen = (i == shown && !s.continueMode);
			for (Int d = 0; d < MAX_DRAW_DATA; ++d) {
				inst->m_enabledDrawData[d] = chosen ? s.buttonHilite[d] : s.buttonEnabled[d];
			}
			b->winSetEnabledTextColors(chosen ? s.hiliteTextColor : s.enabledTextColor, chosen ? s.hiliteTextBorder : s.enabledTextBorder);
			b->winEnable(!s.continueMode);
			if (s.continueMode && i == shown)
			{
				b->winSetDisabledTextColors(GameMakeColor(0, 255, 0, 255), GameMakeColor(0, 64, 0, 255));
			}
			else
			{
				b->winSetDisabledTextColors(s.difficultyTextColor, s.difficultyTextBorder);
			}
		}
	}
}

/// O24: shows or hides every control made here (not the hidden check box template). They take no part in the menu's
/// transition (ChallengeMenuFade, defined by window names in the game's data), so they are shown only when it is over.
static void setControlsShown(Bool shown)
{
	if (shown == s.shown) {
		return;
	}
	GameWindow *const mine[] = { s.difficultyLabel, s.difficulty[0], s.difficulty[1], s.difficulty[2], s.speedLabel,
		s.speedSlider, s.speedText, s.buttonContinue };
	for (Int i = 0; i < (Int)ARRAY_SIZE(mine); ++i) {
		if (mine[i] != nullptr) {
			mine[i]->winHide(!shown);
		}
	}
	s_speed.setShown(shown);
	s.shown = shown;
}

void ChallengeExtras::init(GameWindow *parentMenu, GameWindow *buttonPlay, GameWindow *buttonBack, GameWindow *labelFontLook)
{
	shutdown();
	s.parent = parentMenu;
	if (s.parent == nullptr || buttonBack == nullptr) {
		return;
	}
	GameFont *smallFont = labelFontLook != nullptr ? labelFontLook->winGetFont() : nullptr;
	{
		const WinInstanceData *look = buttonBack->winGetInstanceData();
		for (Int d = 0; d < MAX_DRAW_DATA; ++d)
		{
			s.buttonEnabled[d] = look->m_enabledDrawData[d];
			s.buttonHilite[d] = look->m_hiliteDrawData[d];
		}
		s.difficultyTextColor = look->m_disabledText.color;
		s.difficultyTextBorder = look->m_disabledText.borderColor;
		s.enabledTextColor = look->m_enabledText.color;
		s.enabledTextBorder = look->m_enabledText.borderColor;
		s.hiliteTextColor = look->m_hiliteText.color;
		s.hiliteTextBorder = look->m_hiliteText.borderColor;
	}

	// difficulty: what was chosen here last, else the campaign difficulty of the options
	OptionPreferences pref;
	s.chosenDifficulty = ChallengeRun::loadDifficulty(pref.getCampaignDifficulty());
	s.hasRun = ChallengeRun::load(s_run);
	s.continueMode = FALSE;

	// Placement in the frame of ChallengeMenu.wnd (800 x 600, scaled): the frame is 41..760 x 40..561, the bio box ends
	// at y 493, Play and Back are at y 505..541 from x 382. "Continue" joins Play and Back in that row (three buttons of
	// the same size), so it does not lie on the map art at the bottom left. Left of them, below the map art, the rows for
	// the difficulty and the speed; all widths come from the real text widths of the small (bio) font.
	const Int left = 46, right = 376;
	const UnicodeString diffTitle = text("GUI:ChallengeDifficulty", L"Difficulty", L"\x0421\x043B\x043E\x0436\x043D\x043E\x0441\x0442\x044C");
	const UnicodeString speedTitle = text("GUI:GameSpeed", L"Game speed", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0438\x0433\x0440\x044B");
	// a static text wraps at its width - 10, so the titles get their text width + 14
	const Int diffTitleW = textWidth(smallFont, diffTitle) + 14;
	const Int speedTitleW = textWidth(smallFont, speedTitle) + 14;

	// the bottom row: Continue, Play, Back
	{
		const Int rowX = 382, rowRight = 748, gap = 12;
		const Int w = (rowRight - rowX - 2 * gap) / 3;
		GameWindow *row[3] = { nullptr, buttonPlay, buttonBack };
		s.buttonContinue = makeButton(buttonBack, nullptr, sx(rowX), sy(505), sx(w), sy(36),
			text("GUI:ChallengeContinue", L"Continue", L"\x041F\x0440\x043E\x0434\x043E\x043B\x0436\x0438\x0442\x044C"),
			"ChallengeMenu.wnd:ButtonContinue");
		row[0] = s.buttonContinue;
		for (Int k = 0; k < 3; ++k)
		{
			GameWindow *b = row[k];
			if (b == nullptr) {
				continue;
			}
			// in the button's parent coordinates (the menu's windows may sit in a child of the menu)
			Int px = 0, py = 0, mx = 0, my = 0;
			if (b->winGetParent() != nullptr) {
				b->winGetParent()->winGetScreenPosition(&px, &py);
			}
			s.parent->winGetScreenPosition(&mx, &my);
			b->winSetPosition(mx + sx(rowX + k * (w + gap)) - px, my + sy(505) - py);
			b->winSetSize(sx(w), sy(36));
		}
		if (s.buttonContinue != nullptr)
		{
			// a smaller font when the text does not fit the button
			const UnicodeString t = text("GUI:ChallengeContinue", L"Continue", L"\x041F\x0440\x043E\x0434\x043E\x043B\x0436\x0438\x0442\x044C");
			if (textWidth(s.buttonContinue->winGetFont(), t) > sx(w) - 8 && smallFont != nullptr) {
				s.buttonContinue->winSetFont(smallFont);
			}
			s.buttonContinue->winEnable(s.hasRun);
		}
	}

	// difficulty: the buttons as wide as their texts; one row with the title when they fit, else the title on a row of
	// its own above them
	const char *const labels[DIFFICULTY_BUTTONS] = { "GUI:EasyCaps", "GUI:MediumDifficultyCaps", "GUI:HardCaps" };
	const WideChar *const english[DIFFICULTY_BUTTONS] = { L"EASY", L"NORMAL", L"HARD" };
	const WideChar *const russian[DIFFICULTY_BUTTONS] = {
		L"\x041B\x0415\x0413\x041A\x041E", L"\x041D\x041E\x0420\x041C\x0410\x041B\x042C\x041D\x041E", L"\x0421\x041B\x041E\x0416\x041D\x041E" };
	UnicodeString names[DIFFICULTY_BUTTONS];
	Int widths[DIFFICULTY_BUTTONS];
	const Int buttonGap = sx(4);
	Int buttonsTotal = 0;
	for (Int i = 0; i < DIFFICULTY_BUTTONS; ++i)
	{
		names[i] = text(labels[i], english[i], russian[i]);
		widths[i] = textWidth(smallFont, names[i]) + sx(14);
		buttonsTotal += widths[i] + (i > 0 ? buttonGap : 0);
	}
	const Int room = sx(right) - sx(left);
	const Bool oneRow = diffTitleW + buttonsTotal <= room;
	Int y = oneRow ? 499 : 497;
	const Int rowH = oneRow ? 20 : 18;
	s.difficultyLabel = makeLabel(smallFont, sx(left), sy(y), diffTitleW, sy(rowH), diffTitle);
	Int buttonsX = oneRow ? sx(left) + diffTitleW : sx(left);
	if (!oneRow) {
		y += rowH;
	}
	if (buttonsTotal > room)
	{
		// even a row of its own is short: the buttons share it equally
		const Int w = (room - buttonGap * (DIFFICULTY_BUTTONS - 1)) / DIFFICULTY_BUTTONS;
		for (Int i = 0; i < DIFFICULTY_BUTTONS; ++i) {
			widths[i] = w;
		}
	}
	for (Int i = 0; i < DIFFICULTY_BUTTONS; ++i)
	{
		AsciiString name;
		name.format("ChallengeMenu.wnd:ButtonDifficulty%d", i);
		s.difficulty[i] = makeButton(buttonBack, smallFont, buttonsX, sy(y), widths[i], sy(rowH), names[i], name.str());
		buttonsX += widths[i] + buttonGap;
	}
	y += rowH + (oneRow ? 4 : 2);

	// speed row: the skirmish control (slider, "1x" text, Normal box)
	s.speedLabel = makeLabel(smallFont, sx(left), sy(y), speedTitleW, sy(rowH), speedTitle);
	const Int sliderX = sx(left) + speedTitleW;
	s.speedSlider = makeSpeedSlider(sliderX, sy(y), sx(84), sy(rowH));
	s.speedText = makeLabel(smallFont, sliderX + sx(88), sy(y), sx(32), sy(rowH), UnicodeString::TheEmptyString);
	s.checkTemplate = makeCheckTemplate(smallFont);
	Int fps = LOGICFRAMES_PER_SECOND;
	Bool normal = TRUE;
	ChallengeRun::loadSpeed(fps, normal);
	const Int boxRight = sx(right);
	const Int boxLeft = sliderX + sx(88) + sx(32) + sx(10);
	s_speed.init(s.parent, s.speedSlider, s.speedText, s.checkTemplate, "ChallengeMenu.wnd:CheckBoxGameSpeedNormal", fps, normal,
		boxRight - boxLeft);

	showDifficulty();

	// O24: all hidden until the menu's enter transition is over (update)
	s.shown = TRUE;
	setControlsShown(FALSE);
}

void ChallengeExtras::update(Bool screenReady)
{
	if (s.parent != nullptr) {
		setControlsShown(screenReady);
	}
}

void ChallengeExtras::shutdown()
{
	// TheSuperHackers @bugfix O24 (row 27) The controls made here are destroyed here, before the menu's leave transition
	// starts. Forgetting them was not enough: the shell keeps the menu's layout when it is hidden for a battle and runs the
	// menu's init again afterwards, so a second set was made and the first, outside the transition, stayed on the shell.
	if (TheWindowManager != nullptr)
	{
		GameWindow *const mine[] = { s.difficultyLabel, s.difficulty[0], s.difficulty[1], s.difficulty[2], s.speedLabel,
			s.speedSlider, s.speedText, s.checkTemplate, s.buttonContinue };
		for (Int i = 0; i < (Int)ARRAY_SIZE(mine); ++i) {
			if (mine[i] != nullptr) {
				TheWindowManager->winDestroy(mine[i]);
			}
		}
	}
	s_speed.reset();
	memset(&s, 0, sizeof(s));
	s_run.campaign.clear();
	s_run.mission.clear();
	s_run.general = -1;
}

Bool ChallengeExtras::onSelected(GameWindow *control, Bool (*selectGeneral)(Int general))
{
	if (control == nullptr || s.parent == nullptr) {
		return FALSE;
	}
	if (s_speed.onSelected(control)) {
		return TRUE;
	}
	for (Int i = 0; i < DIFFICULTY_BUTTONS; ++i)
	{
		if (control == s.difficulty[i])
		{
			if (!s.continueMode)
			{
				s.chosenDifficulty = DIFFICULTY_EASY + i;
				showDifficulty();
			}
			return TRUE;
		}
	}
	if (control == s.buttonContinue && s.hasRun)
	{
		// the menu's own selection (medal, bio, Play button); a general that is not on the screen cannot continue
		if (selectGeneral == nullptr || !selectGeneral(s_run.general)) {
			return TRUE;
		}
		s.continueMode = TRUE;   // after the selection, which ends "Continue" through onGeneralClicked
		showDifficulty();
		return TRUE;
	}
	return FALSE;
}

Bool ChallengeExtras::onSliderTrack(GameWindow *control, Int position)
{
	return s.parent != nullptr && s_speed.onSliderTrack(control, position);
}

void ChallengeExtras::onGeneralClicked()
{
	if (s.continueMode)
	{
		s.continueMode = FALSE;
		showDifficulty();
	}
}

Bool ChallengeExtras::isContinue()
{
	return s.continueMode && s.hasRun;
}

Int ChallengeExtras::getDifficulty()
{
	return isContinue() ? s_run.difficulty : s.chosenDifficulty;
}

Int ChallengeExtras::getLogicFps()
{
	return s.parent != nullptr ? s_speed.getLogicFps() : (Int)LOGICFRAMES_PER_SECOND;
}

void ChallengeExtras::applyContinue()
{
	if (!isContinue() || TheCampaignManager == nullptr) {
		return;
	}
	TheCampaignManager->setCampaignAndMission(s_run.campaign, s_run.mission);
	TheCampaignManager->setRankPoints(s_run.rankPoints);
}

void ChallengeExtras::onPlay(Int general)
{
	if (s.parent == nullptr) {
		return;
	}
	ChallengeRun::saveSpeed(s_speed.getSliderLogicFps(), s_speed.isNormal());
	{
		ChallengeRunPreferences prefs;
		prefs.setInt("MenuDifficulty", s.chosenDifficulty);
		prefs.write();
	}
	// A new run starts with this general at the battle the campaign manager is on (the first, or the run's one).
	ChallengeRun::onBattleStarted(general, getDifficulty());
}
