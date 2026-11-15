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

// VoiceChatUI.cpp //////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See VoiceChatUI.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/VoiceChatUI.h"

#include "Common/NameKeyGenerator.h"
#include "Common/VoiceChat.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/LocalizedFallback.h"

namespace
{
	const char *const kHudName = "VoiceChat.wnd:TextSpeaking";
	GameWindow *s_rowButton[VoiceChat::MAX_PEERS] = { nullptr };
	Int s_rowSlot[VoiceChat::MAX_PEERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	Int s_rowShownVolume[VoiceChat::MAX_PEERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	Int s_rowShownSpeaking[VoiceChat::MAX_PEERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	UnsignedInt s_hudShown = 0;   ///< a hash of the shown line (no game string kept in a static: they must be gone before the memory manager)

	UnsignedInt hashOf(const UnicodeString &str)
	{
		UnsignedInt h = 2166136261u;
		for (const WideChar *p = str.str(); *p != 0; ++p) { h ^= (UnsignedInt)*p; h *= 16777619u; }
		return h + 1;
	}

	UnicodeString text(const char *label, const WideChar *english, const WideChar *russian)
	{
		return TheGameText->FETCH_OR_SUBSTITUTE(label, localizedFallback(english, russian));
	}

	NameKeyType idOf(const char *name)
	{
		return TheNameKeyGenerator->nameToKey(AsciiString(name));
	}

	GameFont *uiFont(Int size)
	{
		return TheFontLibrary != nullptr
			? TheFontLibrary->getFont(AsciiString("Arial"), TheGlobalLanguageData != nullptr ? TheGlobalLanguageData->adjustFontSize(size) : size, TRUE)
			: nullptr;
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

	void plainLook(WinInstanceData &inst, UnsignedInt style, GameFont *font, Color fill, Color border)
	{
		inst.init();
		inst.m_style = style;
		inst.m_font = font;
		inst.m_enabledText.color = GameMakeColor(255, 255, 255, 255);
		inst.m_enabledText.borderColor = GameMakeColor(0, 0, 0, 255);
		inst.m_disabledText = inst.m_enabledText;
		inst.m_hiliteText = inst.m_enabledText;
		for (Int i = 0; i < MAX_DRAW_DATA; ++i)
		{
			inst.m_enabledDrawData[i].image = nullptr;
			inst.m_enabledDrawData[i].color = fill;
			inst.m_enabledDrawData[i].borderColor = border;
			inst.m_disabledDrawData[i] = inst.m_enabledDrawData[i];
			inst.m_hiliteDrawData[i] = inst.m_enabledDrawData[i];
		}
	}

	UnicodeString volumeText(Int volume)
	{
		if (volume <= 0) {
			return text("GUI:VoiceOff", L"Off", L"\x0412\x044B\x043A\x043B");
		}
		UnicodeString s;
		s.format(L"%d%%", volume);
		return s;
	}

	//--- the HUD line -----------------------------------------------------------------------------
	void updateHud()
	{
		GameWindow *hud = TheWindowManager != nullptr ? TheWindowManager->winGetWindowFromId(nullptr, idOf(kHudName)) : nullptr;
		UnicodeString line;
		if (VoiceChat::isActive())
		{
			if (VoiceChat::isLocalTalking()) {
				line = text("GUI:VoiceYouTalk", L"You are talking", L"\x0412\x044B \x0433\x043E\x0432\x043E\x0440\x0438\x0442\x0435");
			} else if (VoiceChat::isLocalBlocked()) {
				line = text("GUI:VoiceCannotTalk", L"You cannot talk (microphone off or not in play)", L"\x0412\x044B \x043D\x0435 \x043C\x043E\x0436\x0435\x0442\x0435 \x0433\x043E\x0432\x043E\x0440\x0438\x0442\x044C (\x043C\x0438\x043A\x0440\x043E\x0444\x043E\x043D \x0432\x044B\x043A\x043B\x044E\x0447\x0435\x043D \x0438\x043B\x0438 \x0432\x044B \x043D\x0435 \x0432 \x0438\x0433\x0440\x0435)");
			}
			UnicodeString names;
			for (Int i = 0; i < VoiceChat::MAX_PEERS; ++i)
			{
				if (!VoiceChat::isSpeaking(i)) continue;
				if (!names.isEmpty()) names.concat(L", ");
				names.concat(VoiceChat::nameOf(i));
			}
			if (!names.isEmpty())
			{
				if (!line.isEmpty()) line.concat(L"\n");
				line.concat(text("GUI:VoiceSpeaking", L"Speaking: ", L"\x0413\x043E\x0432\x043E\x0440\x044F\x0442: "));
				line.concat(names);
			}
		}
		if (line.isEmpty())
		{
			if (hud != nullptr) hud->winHide(TRUE);
			s_hudShown = 0;
			return;
		}
		if (hud == nullptr && TheDisplay != nullptr)
		{
			const Int w = TheDisplay->getWidth(), h = TheDisplay->getHeight();
			const Real scale = min((Real)w / 800.0f, (Real)h / 600.0f);
			GameFont *font = uiFont(11);
			WinInstanceData inst;
			plainLook(inst, GWS_STATIC_TEXT, font, GameMakeColor(0, 0, 0, 140), GameMakeColor(0, 0, 0, 0));
			inst.m_id = idOf(kHudName);
			TextData textData;
			memset(&textData, 0, sizeof(textData));
			textData.centeredVertically = TRUE;
			hud = TheWindowManager->gogoGadgetStaticText(nullptr, WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT, REAL_TO_INT(6 * scale), h * 2 / 5,
				REAL_TO_INT(240 * scale), REAL_TO_INT(34 * scale), &inst, &textData, font, FALSE);
			if (hud != nullptr) hud->winSetWindowId(inst.m_id);
		}
		if (hud == nullptr) return;
		hud->winHide(FALSE);
		const UnsignedInt h = hashOf(line);
		if (h != s_hudShown)
		{
			GadgetStaticTextSetText(hud, line);
			s_hudShown = h;
		}
	}

	//--- the Tab list buttons -----------------------------------------------------------------------
	void showButton(Int row)
	{
		GameWindow *button = s_rowButton[row];
		const Int slot = s_rowSlot[row];
		if (button == nullptr) return;
		if (slot < 0 || !VoiceChat::isVoiceMate(slot))
		{
			button->winHide(TRUE);
			return;
		}
		button->winHide(FALSE);
		const Int volume = VoiceChat::volumeOf(slot);
		const Int speaking = VoiceChat::isSpeaking(slot) ? 1 : 0;
		if (volume != s_rowShownVolume[row])
		{
			GadgetButtonSetText(button, volumeText(volume));
			s_rowShownVolume[row] = volume;
		}
		if (speaking != s_rowShownSpeaking[row])
		{
			const Color fill = speaking ? GameMakeColor(30, 140, 40, 230) : GameMakeColor(40, 44, 56, 220);
			for (Int k = 0; k < MAX_DRAW_DATA; ++k)
			{
				button->winSetEnabledColor(k, fill);
				button->winSetHiliteColor(k, fill);
			}
			s_rowShownSpeaking[row] = speaking;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void VoiceChatUI::update()
{
	updateHud();
	for (Int row = 0; row < VoiceChat::MAX_PEERS; ++row)
	{
		if (s_rowButton[row] != nullptr && TheWindowManager->winGetWindowFromId(nullptr, s_rowButton[row]->winGetWindowId()) != s_rowButton[row])
		{
			s_rowButton[row] = nullptr;   // the Tab list went away with its windows
			s_rowShownVolume[row] = s_rowShownSpeaking[row] = -1;
		}
		if (s_rowButton[row] != nullptr && !s_rowButton[row]->winIsHidden()) {
			showButton(row);
		}
	}
}

void VoiceChatUI::decorateDiplomacyRow(Int row, GameWindow *nameText, GameWindow *statusText, Int slot)
{
	if (row < 0 || row >= VoiceChat::MAX_PEERS) return;
	s_rowSlot[row] = slot;
	if (slot >= 0 && s_rowButton[row] == nullptr && nameText != nullptr && statusText != nullptr && VoiceChat::isVoiceMate(slot))
	{
		// right of the status column, as high as the row
		GameWindow *parent = statusText->winGetParent();
		Int sx = 0, sy = 0, sw = 0, sh = 0;
		statusText->winGetPosition(&sx, &sy);
		statusText->winGetSize(&sw, &sh);
		GameFont *font = statusText->winGetFont();
		const Int w = textWidth(font, UnicodeString(L"100%")) + 16;
		const Int w2 = textWidth(font, volumeText(0)) + 16;
		WinInstanceData inst;
		plainLook(inst, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK, font, GameMakeColor(40, 44, 56, 220), GameMakeColor(140, 150, 170, 255));
		AsciiString name;
		name.format("Diplomacy.wnd:VoiceVolume%d", row);
		inst.m_id = idOf(name.str());
		GameWindow *button = TheWindowManager->gogoGadgetPushButton(parent, WIN_STATUS_ENABLED, sx + sw + 4, sy, max(w, w2), sh, &inst, font, FALSE);
		if (button != nullptr)
		{
			button->winSetWindowId(inst.m_id);
			button->winSetOwner(statusText->winGetOwner() != nullptr ? statusText->winGetOwner() : parent);
			button->winSetTooltip(text("GUI:VoiceVolumeTip", L"Volume of this team-mate's voice (click: 100% / 50% / 25% / off)",
				L"\x0413\x0440\x043E\x043C\x043A\x043E\x0441\x0442\x044C \x0433\x043E\x043B\x043E\x0441\x0430 \x0441\x043E\x044E\x0437\x043D\x0438\x043A\x0430 (\x043D\x0430\x0436\x0430\x0442\x0438\x0435: 100% / 50% / 25% / \x0432\x044B\x043A\x043B)"));
		}
		s_rowButton[row] = button;
		s_rowShownVolume[row] = s_rowShownSpeaking[row] = -1;
	}
	if (s_rowButton[row] != nullptr)
	{
		if (slot < 0) s_rowButton[row]->winHide(TRUE);
		else showButton(row);
	}
}

Bool VoiceChatUI::onDiplomacySelected(GameWindow *control)
{
	for (Int row = 0; row < VoiceChat::MAX_PEERS; ++row)
	{
		if (control == nullptr || control != s_rowButton[row] || s_rowSlot[row] < 0) continue;
		const Int v = VoiceChat::volumeOf(s_rowSlot[row]);
		const Int next = v > 50 ? 50 : (v > 25 ? 25 : (v > 0 ? 0 : 100));
		VoiceChat::setVolumeOf(s_rowSlot[row], next);
		showButton(row);
		return TRUE;
	}
	return FALSE;
}
