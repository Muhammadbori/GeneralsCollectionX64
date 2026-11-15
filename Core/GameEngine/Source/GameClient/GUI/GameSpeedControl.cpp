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
	if (logicFps >= 1000 || logicFps <= 0) {
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
	const char *checkBoxName, Int logicFps, Bool normal)
{
	m_slider = slider;
	m_text = text;
	m_checkBox = nullptr;
	m_sliderStep = GameSpeed::logicFpsToStep(logicFps);
	m_normal = normal;

	if (m_slider != nullptr) {
		TheWindowManager->winSendSystemMsg(m_slider, GSM_SET_MIN_MAX, GameSpeed::MinStep, GameSpeed::MaxStep);
	}

	// The check box goes right of the speed text, as high as the text, in the parent's coordinates.
	if (parent != nullptr && m_text != nullptr)
	{
		Int x = 0, y = 0, width = 0, height = 0;
		m_text->winGetPosition(&x, &y);
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
		instData.m_id = TheNameKeyGenerator->nameToKey(checkBoxName);

		m_checkBox = TheWindowManager->gogoGadgetCheckbox(parent, status, x + width + 6, y, 140, boxHeight, &instData, font,
			checkBoxTemplate == nullptr);
		if (m_checkBox != nullptr)
		{
			m_checkBox->winSetWindowId(instData.m_id);
			GadgetCheckBoxSetText(m_checkBox, TheGameText->FETCH_OR_SUBSTITUTE("GUI:GameSpeedNormal",
				GameSpeed::localized(L"Normal", L"\x041E\x0431\x044B\x0447\x043D\x0430\x044F")));   // ru: Obychnaya
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
