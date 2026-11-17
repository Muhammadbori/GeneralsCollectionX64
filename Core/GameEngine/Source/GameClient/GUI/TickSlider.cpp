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

// TickSlider.cpp ///////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See TickSlider.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/TickSlider.h"

#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/KeyDefs.h"
#include "Common/FrameShots.h"

#include <map>
#include <math.h>
#include <vector>

namespace
{
	//---------------------------------------------------------------------------------------------
	// The colours of the chosen mock (staging\mock-controls\02_slider_ticks.png).
	const Color kTrack = GameMakeColor(42, 46, 72, 255);
	const Color kFill = GameMakeColor(30, 140, 255, 255);
	const Color kFillOff = GameMakeColor(96, 100, 112, 255);
	const Color kTick = GameMakeColor(150, 156, 172, 255);
	const Color kLabel = GameMakeColor(160, 166, 184, 255);
	const Color kLabelChosen = GameMakeColor(255, 255, 255, 255);
	const Color kValue = GameMakeColor(255, 255, 255, 255);
	const Color kHeld = GameMakeColor(140, 255, 50, 255);
	const Color kHeldGlow = GameMakeColor(140, 255, 50, 70);
	const Color kHandle = GameMakeColor(255, 255, 255, 255);
	const Color kHandleOff = GameMakeColor(176, 180, 188, 255);
	const Color kRing = GameMakeColor(30, 140, 255, 255);
	const Color kRingHover = GameMakeColor(110, 190, 255, 255);
	const Color kRingOff = GameMakeColor(96, 100, 112, 255);
	const Color kDrop = GameMakeColor(0, 0, 0, 255);

	struct Info
	{
		GameFont *font;
		Int fontH;
		Bool continuous;
		std::vector<UnicodeString> labels;
		TickSlider::ValueTextFunc func;
		void *context;
		UnicodeString suffix;
		// drawn strings, rebuilt when the range or the labels change
		Int builtMin, builtMax, builtCount;
		Bool built;
		std::vector<DisplayString *> markStrings;
		std::vector<Int> markPositions;
		DisplayString *valueString;
		UnicodeString valueShown;
		// O54: the first label shortened for the ticks ("Станд.") when its whole width hid every middle label
		DisplayString *shortFirst;
		Int shortEvery;     ///< the label step with the short first label; 0: none
		Int shortForW;      ///< the track width it was made for (-1: not tried)
		// dragging
		Bool held;
		Real dragX;      ///< the handle's centre (screen x) while held
		Real grabDX;     ///< the mouse's distance from the handle's centre when it was taken
	};

	typedef std::map<GameWindow *, Info *> InfoMap;
	InfoMap s_infos;

	Info *infoOf(GameWindow *w)
	{
		InfoMap::iterator it = s_infos.find(w);
		return it != s_infos.end() ? it->second : nullptr;
	}

	void freeStrings(Info *in)
	{
		if (TheDisplayStringManager != nullptr)
		{
			for (size_t i = 0; i < in->markStrings.size(); ++i) {
				if (in->markStrings[i] != nullptr) {
					TheDisplayStringManager->freeDisplayString(in->markStrings[i]);
				}
			}
			if (in->valueString != nullptr) {
				TheDisplayStringManager->freeDisplayString(in->valueString);
			}
			if (in->shortFirst != nullptr) {
				TheDisplayStringManager->freeDisplayString(in->shortFirst);
			}
		}
		in->shortFirst = nullptr;
		in->shortEvery = 0;
		in->shortForW = -1;
		in->markStrings.clear();
		in->markPositions.clear();
		in->valueString = nullptr;
		in->valueShown.clear();
		in->built = FALSE;
	}

	Int fontHeightOf(GameFont *font)
	{
		if (font == nullptr || TheWindowManager == nullptr) {
			return 12;
		}
		const Int h = TheWindowManager->winFontHeight(font);
		return h > 0 ? h : 12;
	}

	Int minRadius(Int fontH) { return max(5, fontH * 4 / 10); }
	Int bestRadius(Int fontH) { return max(7, fontH * 7 / 10); }

	Color withAlpha(Color c, Real f)
	{
		if (f >= 1.0f) {
			return c;
		}
		const UnsignedInt a = ((UnsignedInt)c >> 24) & 0xFF;
		const UnsignedInt na = (UnsignedInt)(a * (f > 0.0f ? f : 0.0f));
		return (Color)(((UnsignedInt)c & 0x00FFFFFF) | (na << 24));
	}

	/// A filled circle as one-pixel rows; the two end pixels of each row get the part of them the circle covers.
	void fillCircle(Real cx, Real cy, Real r, Color c)
	{
		if (r <= 0.5f || TheDisplay == nullptr) {
			return;
		}
		const Int r0 = (Int)ceilf(r);
		for (Int dy = -r0; dy <= r0; ++dy)
		{
			const Real ry = (Real)dy;
			const Real hw2 = r * r - ry * ry;
			if (hw2 <= 0.0f) {
				continue;
			}
			const Real hw = sqrtf(hw2);
			const Real l = cx - hw, rr = cx + hw;
			const Int il = (Int)floorf(l), ir = (Int)floorf(rr);
			const Int y = (Int)floorf(cy) + dy;
			if (ir - il >= 2) {
				TheDisplay->drawFillRect(il + 1, y, ir - il - 1, 1, c);
			}
			TheDisplay->drawFillRect(il, y, 1, 1, withAlpha(c, (Real)(il + 1) - l));
			if (ir != il) {
				TheDisplay->drawFillRect(ir, y, 1, 1, withAlpha(c, rr - (Real)ir));
			}
		}
	}

	//---------------------------------------------------------------------------------------------
	struct Geo
	{
		Int x, y, w, h;
		Int r, thick, tickLen;
		Int trackL, trackR, trackY;
		Int valueY, labelY;
		Bool showValue, showLabels;
	};

	UnicodeString numberText(Int v, const UnicodeString &suffix)
	{
		UnicodeString t;
		t.format(L"%d%ls", v, suffix.str());
		return t;
	}

	/// O28 (item 7): a round step between marks: 1, 2, 2.5 (as 25, 250, ...), 5 times a power of ten.
	Bool isRoundStep(Int n)
	{
		if (n <= 0) {
			return FALSE;
		}
		while (n % 10 == 0 && n > 10) {
			n /= 10;
		}
		return n == 1 || n == 2 || n == 5 || n == 10 || n == 25;
	}

	/// The marks: every step of a stepped slider; of a continuous one (or a long range) the 0, 25, 50, 75 and 100 % points
	/// when the quarter is a round number, else the 0, 50 and 100 % points when the half is, else only the two ends
	/// (O28, item 7: never a mark at an odd value).
	void buildMarks(Info *in, const SliderData *sd)
	{
		freeStrings(in);
		const Int range = sd->maxVal - sd->minVal;
		if (in->continuous || range > 40)
		{
			Int parts = 1;
			if (range % 4 == 0 && isRoundStep(range / 4)) {
				parts = 4;
			} else if (range % 2 == 0 && isRoundStep(range / 2)) {
				parts = 2;
			}
			for (Int k = 0; k <= parts; ++k) {
				in->markPositions.push_back(sd->minVal + range * k / parts);
			}
		}
		else
		{
			for (Int p = sd->minVal; p <= sd->maxVal; ++p) {
				in->markPositions.push_back(p);
			}
		}
		for (size_t i = 0; i < in->markPositions.size(); ++i)
		{
			// O27: labels are one per step; with five marks (a long or continuous range) a mark takes the label of its
			// own position when there is one per step, else the i-th label (one per mark), else the number
			size_t li = i;
			if (in->markPositions.size() != in->labels.size()) {
				li = (size_t)(in->markPositions[i] - sd->minVal);
			}
			UnicodeString text = li < in->labels.size() ? in->labels[li] : numberText(in->markPositions[i], UnicodeString::TheEmptyString);
			DisplayString *ds = TheDisplayStringManager != nullptr ? TheDisplayStringManager->newDisplayString() : nullptr;
			if (ds != nullptr)
			{
				ds->setFont(in->font);
				ds->setText(text);
			}
			in->markStrings.push_back(ds);
		}
		in->builtMin = sd->minVal;
		in->builtMax = sd->maxVal;
		in->builtCount = (Int)in->labels.size();
		in->built = TRUE;
	}

	void ensureBuilt(Info *in, const SliderData *sd)
	{
		if (!in->built || in->builtMin != sd->minVal || in->builtMax != sd->maxVal || in->builtCount != (Int)in->labels.size()) {
			buildMarks(in, sd);
		}
	}

	Int stringWidth(DisplayString *ds)
	{
		if (ds == nullptr) {
			return 0;
		}
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		return w;
	}

	Geo geometry(GameWindow *window, Info *in, const SliderData *sd)
	{
		Geo g;
		window->winGetScreenPosition(&g.x, &g.y);
		window->winGetSize(&g.w, &g.h);
		const Int fh = in->fontH;
		const Int needHandle = 2 * minRadius(fh) + 2;
		g.showValue = g.showLabels = FALSE;
		if (g.h >= 2 * fh + 3 + needHandle) {
			g.showValue = g.showLabels = TRUE;
		} else if (g.h >= fh + 1 + needHandle) {
			g.showLabels = TRUE;
		}
		const Int zone = g.h - (g.showValue ? fh + 2 : 0) - (g.showLabels ? fh + 1 : 0);
		g.r = min(bestRadius(fh), max(3, (zone - 2) / 2));
		g.thick = max(3, g.r / 3);
		g.tickLen = max(3, g.r / 3);
		const Int top = g.y + (g.showValue ? fh + 2 : 0);
		g.trackY = top + zone / 2;
		g.valueY = g.y;
		g.labelY = g.trackY + g.r + 1;
		// the ends: room for the handle and for the first and last label, so nothing is drawn outside the window
		Int padL = g.r + 1, padR = g.r + 2;
		if (g.showLabels && !in->markStrings.empty())
		{
			padL = max(padL, stringWidth(in->markStrings.front()) / 2 + 1);
			padR = max(padR, stringWidth(in->markStrings.back()) / 2 + 1);
		}
		g.trackL = g.x + padL;
		g.trackR = g.x + g.w - padR;
		if (g.trackR <= g.trackL) {
			g.trackR = g.trackL + 1;
		}
		(void)sd;
		return g;
	}

	Real positionToX(const Geo &g, const SliderData *sd, Real pos)
	{
		const Int range = sd->maxVal - sd->minVal;
		if (range <= 0) {
			return (Real)g.trackL;
		}
		Real f = (pos - (Real)sd->minVal) / (Real)range;
		f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
		return (Real)g.trackL + f * (Real)(g.trackR - g.trackL);
	}

	Real xToPosition(const Geo &g, const SliderData *sd, Real x)
	{
		const Int range = sd->maxVal - sd->minVal;
		Real f = (x - (Real)g.trackL) / (Real)(g.trackR - g.trackL);
		f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
		return (Real)sd->minVal + f * (Real)range;
	}

	Real handleX(const Geo &g, const Info *in, const SliderData *sd)
	{
		if (in->held)
		{
			const Real x = in->dragX;
			return x < (Real)g.trackL ? (Real)g.trackL : (x > (Real)g.trackR ? (Real)g.trackR : x);
		}
		return positionToX(g, sd, (Real)sd->position);
	}

	UnicodeString valueText(const Info *in, const SliderData *sd)
	{
		if (in->func != nullptr) {
			return in->func(sd->position, in->context);
		}
		if (!in->continuous)
		{
			const Int i = sd->position - sd->minVal;
			if (i >= 0 && i < (Int)in->labels.size()) {
				return in->labels[i];
			}
		}
		return numberText(sd->position, in->suffix);
	}

	/// The position under the handle while it is dragged: the nearest step (a continuous slider: the nearest value too).
	void track(GameWindow *window, Info *in, SliderData *sd, const Geo &g)
	{
		const Real p = xToPosition(g, sd, in->dragX);
		Int pos = (Int)floorf(p + 0.5f);
		pos = pos < sd->minVal ? sd->minVal : (pos > sd->maxVal ? sd->maxVal : pos);
		if (pos != sd->position)
		{
			TheWindowManager->winSendSystemMsg(window, GSM_SET_SLIDER, pos, 0);
			TheWindowManager->winSendSystemMsg(window->winGetOwner(), GSM_SLIDER_TRACK, (WindowMsgData)window, sd->position);
		}
	}

	void release(Info *in)
	{
		in->held = FALSE;   // the handle is drawn at the position again: a stepped slider snaps to its step
	}

	/// O27: the real left button is up (a release the window did not get: focus lost, alt-tab, a dialog took the
	/// mouse-up). Not while a -uiScript plays: its held button is not the real mouse's.
	Bool leftButtonReallyUp()
	{
		if (FrameShots::isUiScriptRunning()) {
			return FALSE;
		}
		const Int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
		return (GetAsyncKeyState(vk) & 0x8000) == 0;
	}

	void step(GameWindow *window, SliderData *sd, Int delta)
	{
		Int pos = sd->position + delta;
		pos = pos < sd->minVal ? sd->minVal : (pos > sd->maxVal ? sd->maxVal : pos);
		if (pos != sd->position)
		{
			TheWindowManager->winSendSystemMsg(window, GSM_SET_SLIDER, pos, 0);
			TheWindowManager->winSendSystemMsg(window->winGetOwner(), GSM_SLIDER_TRACK, (WindowMsgData)window, sd->position);
		}
	}

	//---------------------------------------------------------------------------------------------
	WindowMsgHandledType tickInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		Info *in = infoOf(window);
		SliderData *sd = (SliderData *)window->winGetUserData();
		WinInstanceData *inst = window->winGetInstanceData();
		if (in == nullptr || sd == nullptr) {
			return MSG_IGNORED;
		}
		switch (msg)
		{
			case GWM_MOUSE_ENTERING:
				if (BitIsSet(inst->getStyle(), GWS_MOUSE_TRACK))
				{
					BitSet(inst->m_state, WIN_STATE_HILITED);
					TheWindowManager->winSendSystemMsg(window->winGetOwner(), GBM_MOUSE_ENTERING, (WindowMsgData)window, 0);
				}
				return MSG_HANDLED;

			case GWM_MOUSE_LEAVING:
				if (BitIsSet(inst->getStyle(), GWS_MOUSE_TRACK))
				{
					BitClear(inst->m_state, WIN_STATE_HILITED);
					TheWindowManager->winSendSystemMsg(window->winGetOwner(), GBM_MOUSE_LEAVING, (WindowMsgData)window, 0);
				}
				return MSG_HANDLED;

			case GWM_LEFT_DOWN:
			{
				if (!window->winGetEnabled()) {
					return MSG_HANDLED;
				}
				ensureBuilt(in, sd);
				const Geo g = geometry(window, in, sd);
				const Real mx = (Real)(Int)LOLONGTOSHORT(mData1);
				const Real my = (Real)(Int)HILONGTOSHORT(mData1);
				const Real cx = handleX(g, in, sd);
				// on the handle: it stays under the mouse where it was taken; elsewhere (track, tick label): it jumps there
				const Bool onHandle = fabsf(mx - cx) <= (Real)(g.r + 3) && fabsf(my - (Real)g.trackY) <= (Real)(g.r + 3);
				in->grabDX = onHandle ? mx - cx : 0.0f;
				in->held = TRUE;
				in->dragX = mx - in->grabDX;
				track(window, in, sd, g);
				TheWindowManager->winSetFocus(window);
				return MSG_HANDLED;
			}

			case GWM_LEFT_DRAG:
			{
				if (in->held && leftButtonReallyUp())
				{
					release(in);
				}
				else if (in->held)
				{
					ensureBuilt(in, sd);
					const Geo g = geometry(window, in, sd);
					in->dragX = (Real)(Int)LOLONGTOSHORT(mData1) - in->grabDX;
					track(window, in, sd, g);
				}
				return MSG_HANDLED;
			}

			case GWM_LEFT_UP:
				if (in->held) {
					release(in);
				}
				return MSG_HANDLED;

			case GWM_CHAR:
			{
				if (!BitIsSet(mData2, KEY_STATE_DOWN)) {
					return MSG_HANDLED;
				}
				const Int range = sd->maxVal - sd->minVal;
				const Int one = (in->continuous && range > 20) ? range / 20 : 1;
				switch (mData1)
				{
					case KEY_RIGHT: step(window, sd, one); return MSG_HANDLED;
					case KEY_LEFT: step(window, sd, -one); return MSG_HANDLED;
					case KEY_DOWN:
					case KEY_TAB: window->winNextTab(); return MSG_HANDLED;
					case KEY_UP: window->winPrevTab(); return MSG_HANDLED;
					default: return MSG_IGNORED;
				}
			}
		}
		return MSG_IGNORED;
	}

	WindowMsgHandledType tickSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
	{
		if (msg == GWM_DESTROY)
		{
			InfoMap::iterator it = s_infos.find(window);
			if (it != s_infos.end())
			{
				freeStrings(it->second);
				delete it->second;
				s_infos.erase(it);
			}
		}
		return GadgetHorizontalSliderSystem(window, msg, mData1, mData2);
	}

	/// O28 (item 7): the step k of the shown labels (see tickDraw): 1 when all fit, count - 1 for the two ends alone.
	Int labelEvery(const Geo &g, const Info *in, const SliderData *sd, Int firstWidth = -1)
	{
		const Int count = (Int)in->markPositions.size();
		if (count <= 2) {
			return 1;
		}
		const Int gap = max(4, in->fontH / 2);
		for (Int k = 1; k < count - 1; ++k)
		{
			if ((count - 1) % k != 0) {
				continue;
			}
			Bool fits = TRUE;
			Int lastRight = -100000;
			for (Int i = 0; i < count && fits; i += k)
			{
				const Int mx = (Int)positionToX(g, sd, (Real)in->markPositions[i]);
				const Int w = (i == 0 && firstWidth >= 0) ? firstWidth : stringWidth(in->markStrings[i]);
				fits = mx - w / 2 >= lastRight + gap;
				lastRight = mx + w / 2;
			}
			if (fits) {
				return k;
			}
		}
		return count - 1;
	}

	/// TheSuperHackers @bugfix O54 When only the two end labels fit because the first one is wide (Russian "Стандартная" on
	/// the camera sliders hid "450"), the first label is shortened for the ticks alone (its first letters and a dot, at
	/// least three letters) as far as needed for an even subset to fit; the value above the handle keeps the whole text.
	void makeShortFirst(const Geo &g, Info *in, const SliderData *sd)
	{
		const Int trackW = g.trackR - g.trackL;
		if (in->shortForW == trackW) {
			return;
		}
		if (in->shortFirst != nullptr && TheDisplayStringManager != nullptr) {
			TheDisplayStringManager->freeDisplayString(in->shortFirst);
		}
		in->shortFirst = nullptr;
		in->shortEvery = 0;
		in->shortForW = trackW;
		const Int count = (Int)in->markPositions.size();
		if (count <= 2 || in->labels.empty() || TheDisplayStringManager == nullptr || labelEvery(g, in, sd) != count - 1) {
			return;
		}
		const UnicodeString &full = in->labels[0];
		const Int len = full.getLength();
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return;
		}
		ds->setFont(in->font);
		for (Int n = len - 1; n >= 3; --n)
		{
			UnicodeString part;
			for (Int i = 0; i < n; ++i) {
				part.concat(full.str()[i]);
			}
			part.trim();
			part.concat(L".");
			ds->setText(part);
			const Int every = labelEvery(g, in, sd, stringWidth(ds));
			if (every != count - 1)
			{
				in->shortFirst = ds;
				in->shortEvery = every;
				return;
			}
		}
		TheDisplayStringManager->freeDisplayString(ds);
	}

	void tickDraw(GameWindow *window, WinInstanceData *instData)
	{
		Info *in = infoOf(window);
		SliderData *sd = (SliderData *)window->winGetUserData();
		if (in == nullptr || sd == nullptr || TheDisplay == nullptr) {
			return;
		}
		// a release outside the window does not reach it: it is no longer the window the mouse holds
		if (in->held && (TheWindowManager->winGetGrabWindow() != window || leftButtonReallyUp())) {
			release(in);
		}
		ensureBuilt(in, sd);
		const Geo g = geometry(window, in, sd);
		const Bool enabled = window->winGetEnabled();
		const Bool hover = instData != nullptr && BitIsSet(instData->getState(), WIN_STATE_HILITED);
		const Real cx = handleX(g, in, sd);
		const Int icx = (Int)floorf(cx + 0.5f);

		// track, the filled part, the round ends
		const Int ty = g.trackY - g.thick / 2;
		TheDisplay->drawFillRect(g.trackL, ty, g.trackR - g.trackL, g.thick, kTrack);
		fillCircle((Real)g.trackR, (Real)g.trackY + 0.5f, (Real)g.thick / 2.0f, kTrack);
		const Color fill = enabled ? kFill : kFillOff;
		if (icx > g.trackL) {
			TheDisplay->drawFillRect(g.trackL, ty, icx - g.trackL, g.thick, fill);
		}
		fillCircle((Real)g.trackL, (Real)g.trackY + 0.5f, (Real)g.thick / 2.0f, fill);

		// the ticks and their labels. O28 (item 7): when not all labels fit, an even subset: every k-th label from the first,
		// with the smallest k that divides the gaps evenly (so the last is one of them) and lets no two labels touch; when
		// no such k fits, only the first and the last.
		const Int count = (Int)in->markPositions.size();
		const Int tickTop = g.trackY + g.thick / 2 + 2;
		std::vector<Bool> shown(count, FALSE);
		if (g.showLabels && count > 0)
		{
			makeShortFirst(g, in, sd);   // O54
			const Int every = in->shortFirst != nullptr ? in->shortEvery : labelEvery(g, in, sd);
			for (Int i = 0; i < count; i += every) {
				shown[i] = TRUE;
			}
			shown[count - 1] = TRUE;
		}
		for (Int i = 0; i < count; ++i)
		{
			const Int mx = (Int)positionToX(g, sd, (Real)in->markPositions[i]);
			TheDisplay->drawFillRect(mx, tickTop, 1 + in->fontH / 16, g.tickLen, kTick);
			if (shown[i] && in->markStrings[i] != nullptr)
			{
				DisplayString *ds = (i == 0 && in->shortFirst != nullptr) ? in->shortFirst : in->markStrings[i];   // O54
				const Int w = stringWidth(ds);
				const Bool chosen = !in->continuous && in->markPositions[i] == sd->position;
				ds->draw(mx - w / 2, g.labelY, chosen && enabled ? kLabelChosen : kLabel, kDrop);
			}
		}

		// the handle: a ring in the menu blue round a white disc; held: lit green with a glow
		const Real r = (Real)g.r;
		const Real cy = (Real)g.trackY + 0.5f;
		if (in->held && enabled) {
			fillCircle(cx + 0.5f, cy, r + 3.0f, kHeldGlow);
		}
		fillCircle(cx + 0.5f, cy, r, enabled ? (hover || in->held ? kRingHover : kRing) : kRingOff);
		fillCircle(cx + 0.5f, cy, r - max(2.0f, r / 6.0f), enabled ? (in->held ? kHeld : kHandle) : kHandleOff);

		// the value above the handle
		if (g.showValue && TheDisplayStringManager != nullptr)
		{
			const UnicodeString text = valueText(in, sd);
			if (in->valueString == nullptr)
			{
				in->valueString = TheDisplayStringManager->newDisplayString();
				if (in->valueString != nullptr) {
					in->valueString->setFont(in->font);
				}
			}
			if (in->valueString != nullptr)
			{
				if (text.compare(in->valueShown) != 0)
				{
					in->valueString->setText(text);
					in->valueShown = text;
				}
				const Int w = stringWidth(in->valueString);
				Int x = icx - w / 2;
				x = x + w > g.x + g.w ? g.x + g.w - w : x;
				x = x < g.x ? g.x : x;
				in->valueString->draw(x, g.valueY, enabled ? (in->held ? kHeld : kValue) : kHandleOff, kDrop);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void TickSlider::attach(GameWindow *slider, GameFont *font, Bool continuous)
{
	if (slider == nullptr || slider->winGetUserData() == nullptr || !BitIsSet(slider->winGetStyle(), GWS_HORZ_SLIDER)) {
		return;
	}
	Info *in = infoOf(slider);
	if (in == nullptr)
	{
		in = new Info;
		in->func = nullptr;
		in->context = nullptr;
		in->valueString = nullptr;
		in->shortFirst = nullptr;
		in->shortEvery = 0;
		in->shortForW = -1;
		in->built = FALSE;
		in->builtMin = in->builtMax = in->builtCount = 0;
		in->held = FALSE;
		in->dragX = 0.0f;
		in->grabDX = 0.0f;
		s_infos[slider] = in;
	}
	// O27: every attach starts from a clean setting (a kept entry of a reused window address keeps nothing)
	in->labels.clear();
	in->func = nullptr;
	in->context = nullptr;
	in->suffix.clear();
	in->held = FALSE;
	in->font = font != nullptr ? font : slider->winGetFont();
	in->fontH = fontHeightOf(in->font);
	in->continuous = continuous;
	freeStrings(in);
	slider->winSetDrawFunc(tickDraw);
	slider->winSetInputFunc(tickInput);
	slider->winSetSystemFunc(tickSystem);
	// the old thumb button is not used: hidden, so the whole window takes the clicks
	if (GameWindow *thumb = slider->winGetChild()) {
		thumb->winHide(TRUE);
	}
}

Bool TickSlider::isAttached(GameWindow *slider)
{
	return infoOf(slider) != nullptr;
}

void TickSlider::setLabels(GameWindow *slider, const UnicodeString *labels, Int count)
{
	Info *in = infoOf(slider);
	if (in == nullptr) {
		return;
	}
	in->labels.clear();
	for (Int i = 0; i < count; ++i) {
		in->labels.push_back(labels[i]);
	}
	freeStrings(in);
}

void TickSlider::setValueText(GameWindow *slider, ValueTextFunc func, void *context)
{
	Info *in = infoOf(slider);
	if (in != nullptr)
	{
		in->func = func;
		in->context = context;
	}
}

void TickSlider::setSuffix(GameWindow *slider, const UnicodeString &suffix)
{
	Info *in = infoOf(slider);
	if (in != nullptr) {
		in->suffix = suffix;
	}
}

Int TickSlider::fullHeight(GameFont *font)
{
	const Int fh = fontHeightOf(font);
	return (fh + 2) + (2 * bestRadius(fh) + 2) + (fh + 1);
}

Int TickSlider::handleHeight(GameFont *font)
{
	return 2 * bestRadius(fontHeightOf(font)) + 2;
}

Bool TickSlider::screenPoint(GameWindow *slider, Real pos, Int *x, Int *y)
{
	Info *in = infoOf(slider);
	const SliderData *sd = slider != nullptr ? (const SliderData *)slider->winGetUserData() : nullptr;
	if (in == nullptr || sd == nullptr) {
		return FALSE;
	}
	ensureBuilt(in, sd);
	const Geo g = geometry(slider, in, sd);
	*x = (Int)floorf(positionToX(g, sd, pos) + 0.5f);
	*y = g.trackY;
	return TRUE;
}

Real TickSlider::position(GameWindow *slider)
{
	const SliderData *sd = slider != nullptr ? (const SliderData *)slider->winGetUserData() : nullptr;
	return sd != nullptr ? (Real)sd->position : 0.0f;
}

Int TickSlider::trackCenter(GameWindow *slider)
{
	Info *in = infoOf(slider);
	SliderData *sd = slider != nullptr ? (SliderData *)slider->winGetUserData() : nullptr;
	if (in == nullptr || sd == nullptr)
	{
		Int w = 0, h = 0;
		if (slider != nullptr) {
			slider->winGetSize(&w, &h);
		}
		return h / 2;
	}
	ensureBuilt(in, sd);
	const Geo g = geometry(slider, in, sd);
	return g.trackY - g.y;
}
