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

// GameSpeedControl.cpp /////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See GameSpeedControl.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GameSpeedControl.h"

#include "Common/NameKeyGenerator.h"
#include "Common/Registry.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/DisplayString.h"
#include "GameClient/Gadget.h"
#include "GameClient/DisplayStringManager.h"

namespace
{
	Int measureText(GameFont *font, const UnicodeString &text)
	{
		if (font == nullptr || TheDisplayStringManager == nullptr) {
			return (Int)text.getLength() * 7;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return (Int)text.getLength() * 7;
		}
		ds->setFont(font);
		ds->setText(text);
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		TheDisplayStringManager->freeDisplayString(ds);
		return w;
	}

	/// In 'parent' coordinates: where the visible text in front of 'neighbour' starts: the static text that ends at the
	/// neighbour's left edge in the same row (its text centred in it, or after its margin), else the neighbour's edge.
	Int textStartLeftOf(GameWindow *parent, GameWindow *neighbour)
	{
		Int px = 0, py = 0, nx = 0, ny = 0, nw = 0, nh = 0;
		parent->winGetScreenPosition(&px, &py);
		neighbour->winGetScreenPosition(&nx, &ny);
		neighbour->winGetSize(&nw, &nh);
		Int limit = nx - px;
		GameWindow *row = neighbour->winGetParent();
		for (GameWindow *child = row != nullptr ? row->winGetChild() : nullptr; child != nullptr; child = child->winGetNext())
		{
			if ((child->winGetStyle() & GWS_STATIC_TEXT) == 0 || child->winIsHidden()) {
				continue;
			}
			Int cx = 0, cy = 0, cw = 0, ch = 0;
			child->winGetScreenPosition(&cx, &cy);
			child->winGetSize(&cw, &ch);
			const Bool endsAtNeighbour = cx + cw >= nx - 8 && cx + cw <= nx + 8;
			const Bool sameRow = cy < ny + nh && cy + ch > ny;
			if (!endsAtNeighbour || !sameRow) {
				continue;
			}
			const TextData *data = (const TextData *)child->winGetUserData();
			Int textW = cw;
			if (data != nullptr && data->text != nullptr)
			{
				Int tw = 0, th = 0;
				data->text->getSize(&tw, &th);
				textW = tw;
			}
			const Bool centred = data != nullptr && data->centered;
			const Int start = centred ? cx + (cw - textW) / 2 : cx + (data != nullptr ? data->leftMargin : 0);
			limit = min(limit, start - px);
		}
		return limit;
	}
}

namespace
{
	Int clampStep(Int step)
	{
		return step < GameSpeed::MinStep ? GameSpeed::MinStep : (step > GameSpeed::MaxStep ? GameSpeed::MaxStep : step);
	}
}

//-------------------------------------------------------------------------------------------------
const WideChar *GameSpeed::localized(const WideChar *english, const WideChar *russian)
{
	return localizedFallback(english, russian);
}

Int GameSpeed::stepToLogicFps(Int step)
{
	return clampStep(step) * StepLogicFps;
}

Int GameSpeed::logicFpsToStep(Int logicFps)
{
	// 61 is the old top slider position "UNCAP" as Skirmish.ini stored it; new values are multiples of StepLogicFps.
	if (logicFps >= 1000 || logicFps <= 0 || logicFps == 61) {
		return NormalStep;
	}
	return clampStep((logicFps + StepLogicFps / 2) / StepLogicFps);
}

UnicodeString GameSpeed::multiplierText(Int logicFps)
{
	const Int step = logicFpsToStep(logicFps);
	UnicodeString text;
	if (step % 2 == 0) {
		text.format(L"%dx", step / 2);
	} else {
		text.format(L"%d.5x", step / 2);
	}
	return text;
}

//-------------------------------------------------------------------------------------------------
GameSpeedControl::GameSpeedControl()
	: m_slider(nullptr)
	, m_text(nullptr)
	, m_checkBox(nullptr)
	, m_sliderStep(GameSpeed::NormalStep)
	, m_normal(TRUE)
{
}

void GameSpeedControl::init(GameWindow *parent, GameWindow *slider, GameWindow *text, GameWindow *checkBoxTemplate,
	const char *checkBoxName, Int logicFps, Bool normal, Int checkBoxWidth, GameWindow *rightNeighbour)
{
	m_slider = slider;
	m_text = text;
	m_checkBox = nullptr;
	m_sliderStep = GameSpeed::logicFpsToStep(logicFps);
	m_normal = normal;

	if (m_slider != nullptr) {
		TheWindowManager->winSendSystemMsg(m_slider, GSM_SET_MIN_MAX, GameSpeed::MinStep, GameSpeed::MaxStep);
	}

	// The check box goes right of the speed text box, in the same row and as high as it. The text box may sit in a child
	// window of 'parent', so the place is taken from the screen positions.
	if (parent != nullptr && m_text != nullptr)
	{
		Int x = 0, y = 0, width = 0, height = 0;
		Int parentX = 0, parentY = 0;
		m_text->winGetScreenPosition(&x, &y);
		parent->winGetScreenPosition(&parentX, &parentY);
		x -= parentX;
		y -= parentY;
		m_text->winGetSize(&width, &height);
		const Int boxHeight = height > 12 ? height : 12;

		WinInstanceData instData;
		instData.init();
		instData.m_style = GWS_CHECK_BOX | GWS_MOUSE_TRACK;
		UnsignedInt status = WIN_STATUS_ENABLED;
		GameFont *font = nullptr;
		if (checkBoxTemplate != nullptr)
		{
			const WinInstanceData *look = checkBoxTemplate->winGetInstanceData();
			for (Int i = 0; i < MAX_DRAW_DATA; ++i) {
				instData.m_enabledDrawData[i] = look->m_enabledDrawData[i];
				instData.m_disabledDrawData[i] = look->m_disabledDrawData[i];
				instData.m_hiliteDrawData[i] = look->m_hiliteDrawData[i];
			}
			instData.m_enabledText = look->m_enabledText;
			instData.m_disabledText = look->m_disabledText;
			instData.m_hiliteText = look->m_hiliteText;
			instData.m_imageOffset = look->m_imageOffset;
			instData.m_font = look->m_font;
			font = look->m_font;
			status |= checkBoxTemplate->winGetStatus() & WIN_STATUS_IMAGE;
		}
		else
		{
			// TheSuperHackers @bugfix No check box to copy (base Generals): not the engine's red default look but no
			// background, and the box, the cross and the text in the colour of the speed text (G2).
			const WinInstanceData *textLook = m_text->winGetInstanceData();
			const Color textColor = textLook->m_enabledText.color;
			const Color clear = GameMakeColor(0, 0, 0, 0);
			const Color grey = GameMakeColor(128, 128, 128, 255);
			for (Int i = 0; i < MAX_DRAW_DATA; ++i)
			{
				instData.m_enabledDrawData[i].image = nullptr;
				instData.m_enabledDrawData[i].color = clear;
				instData.m_enabledDrawData[i].borderColor = clear;
				instData.m_disabledDrawData[i] = instData.m_enabledDrawData[i];
				instData.m_hiliteDrawData[i] = instData.m_enabledDrawData[i];
			}
			instData.m_enabledDrawData[1].color = WIN_COLOR_UNDEFINED;   // unchecked: the empty box
			instData.m_enabledDrawData[1].borderColor = textColor;
			instData.m_enabledDrawData[2].color = textColor;             // checked: the box with the cross
			instData.m_enabledDrawData[2].borderColor = textColor;
			instData.m_hiliteDrawData[1] = instData.m_enabledDrawData[1];
			instData.m_hiliteDrawData[2] = instData.m_enabledDrawData[2];
			instData.m_disabledDrawData[1].color = WIN_COLOR_UNDEFINED;
			instData.m_disabledDrawData[1].borderColor = grey;
			instData.m_disabledDrawData[2].color = grey;
			instData.m_disabledDrawData[2].borderColor = grey;
			instData.m_enabledText = textLook->m_enabledText;
			instData.m_hiliteText = textLook->m_enabledText;
			instData.m_disabledText = textLook->m_disabledText;
			font = m_text->winGetFont();
			instData.m_font = font;
		}
		instData.m_id = TheNameKeyGenerator->nameToKey(checkBoxName);

		const UnicodeString boxText = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GameSpeedNormal",
			GameSpeed::localized(L"Normal", L"\x041E\x0431\x044B\x0447\x043D\x0430\x044F"));   // ru: Obychnaya
		Int boxX = x + width + 10;
		Int boxW = checkBoxWidth > 20 ? checkBoxWidth : 160;
		if (checkBoxTemplate == nullptr)
		{
			// the width the box and its text need, so it does not reach into the next control (G2)
			boxW = boxHeight + 6 + measureText(font, boxText);
		}
		if (rightNeighbour != nullptr)
		{
			// TheSuperHackers @tweak The box takes the width it needs and must end before the text of the next control of
			// the row (in skirmish: the "starting money" label, centred in its window left of the money box). If the room
			// is short, the slider gets shorter at its right end (its left end stays clear of its title) and the speed text
			// moves left, by at most a third of the slider.
			const Int need = boxHeight + 6 + measureText(font, boxText);
			const Int limit = textStartLeftOf(parent, rightNeighbour) - 6;
			boxW = need;
			if (boxX + need > limit && m_slider != nullptr)
			{
				Int sliderW = 0, sliderH = 0;
				m_slider->winGetSize(&sliderW, &sliderH);
				const Int shift = min(boxX + need - limit, sliderW / 3);
				Int minVal = 0, maxVal = 0;
				GadgetSliderGetMinMax(m_slider, &minVal, &maxVal);
				const Int pos = GadgetSliderGetPosition(m_slider);
				m_slider->winSetSize(sliderW - shift, sliderH);
				TheWindowManager->winSendSystemMsg(m_slider, GSM_SET_MIN_MAX, minVal, maxVal);
				GadgetSliderSetPosition(m_slider, pos);
				Int tx = 0, ty = 0;
				m_text->winGetPosition(&tx, &ty);
				m_text->winSetPosition(tx - shift, ty);
				boxX -= shift;
			}
		}
		m_checkBox = TheWindowManager->gogoGadgetCheckbox(parent, status, boxX, y, boxW, boxHeight, &instData, font, FALSE);
		if (m_checkBox != nullptr)
		{
			m_checkBox->winSetWindowId(instData.m_id);
			GadgetCheckBoxSetText(m_checkBox, boxText);
			GadgetCheckBoxSetChecked(m_checkBox, m_normal);
		}
	}
	apply();
}

void GameSpeedControl::reset()
{
	m_slider = nullptr;
	m_text = nullptr;
	m_checkBox = nullptr;
}

Bool GameSpeedControl::onSliderTrack(GameWindow *control, Int position)
{
	if (control == nullptr || control != m_slider) {
		return FALSE;
	}
	if (!m_normal) {
		m_sliderStep = clampStep(position);
	}
	apply();
	return TRUE;
}

Bool GameSpeedControl::onSelected(GameWindow *control)
{
	if (control == nullptr || control != m_checkBox) {
		return FALSE;
	}
	m_normal = GadgetCheckBoxIsChecked(m_checkBox);
	apply();
	return TRUE;
}

Int GameSpeedControl::getLogicFps() const
{
	return m_normal ? LOGICFRAMES_PER_SECOND : GameSpeed::stepToLogicFps(m_sliderStep);
}

Int GameSpeedControl::getSliderLogicFps() const
{
	return GameSpeed::stepToLogicFps(m_sliderStep);
}

/// Normal: the slider shows 1x and cannot be moved. Otherwise the slider and its own step. The text shows the speed
/// played, greyed at 1x as the menu always did for the normal speed.
void GameSpeedControl::apply()
{
	const Int step = m_normal ? GameSpeed::NormalStep : m_sliderStep;
	if (m_slider != nullptr)
	{
		GadgetSliderSetPosition(m_slider, step);
		m_slider->winEnable(!m_normal);
	}
	if (m_text != nullptr)
	{
		GadgetStaticTextSetText(m_text, GameSpeed::multiplierText(GameSpeed::stepToLogicFps(step)));
		m_text->winEnable(step != GameSpeed::NormalStep);
	}
}
