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

// OptionsSections.cpp //////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature See OptionsSections.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/OptionsSections.h"

#include "Common/AudioOutputDevices.h"
#include "Common/FramePacer.h"
#include "Common/FrameRateLimit.h"
#include "Common/GameLOD.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/Packs.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Image.h"
#include "GameClient/LocalizedFallback.h"
#include "GameClient/MessageBox.h"
#include "GameLogic/GameLogic.h"

#include <vector>

namespace
{
	//---------------------------------------------------------------------------------------------
	// Sections, in the order of the column.
	enum Section
	{
		SEC_DISPLAY, SEC_GRAPHICS, SEC_SOUND, SEC_LANGUAGE, SEC_CONTROLS, SEC_GAME, SEC_INTERFACE, SEC_INFO, SEC_NETWORK,
		SEC_COUNT
	};

	struct Text
	{
		const char *label;
		const WideChar *english;
		const WideChar *russian;
	};

	UnicodeString textOf(const Text &t)
	{
		return TheGameText->FETCH_OR_SUBSTITUTE(t.label, localizedFallback(t.english, t.russian));
	}

	const Text kSectionNames[SEC_COUNT] =
	{
		{ "GUI:OptionsSectionDisplay", L"Display", L"\x042D\x043A\x0440\x0430\x043D" },
		{ "GUI:OptionsSectionGraphics", L"Graphics", L"\x0413\x0440\x0430\x0444\x0438\x043A\x0430" },
		{ "GUI:OptionsSectionSound", L"Sound", L"\x0417\x0432\x0443\x043A" },
		{ "GUI:OptionsSectionLanguage", L"Language", L"\x042F\x0437\x044B\x043A" },
		{ "GUI:OptionsSectionControls", L"Controls", L"\x0423\x043F\x0440\x0430\x0432\x043B\x0435\x043D\x0438\x0435" },
		{ "GUI:OptionsSectionGame", L"Game", L"\x0418\x0433\x0440\x0430" },
		{ "GUI:OptionsSectionInterface", L"Interface", L"\x0418\x043D\x0442\x0435\x0440\x0444\x0435\x0439\x0441" },
		{ "GUI:OptionsSectionInfo", L"Information", L"\x0418\x043D\x0444\x043E\x0440\x043C\x0430\x0446\x0438\x044F" },
		{ "GUI:OptionsSectionNetwork", L"Network", L"\x0421\x0435\x0442\x044C" },
	};

	//---------------------------------------------------------------------------------------------
	// The settings that were Options.ini keys only.
	enum Kind { KIND_CHECK, KIND_STEPS };

	struct Choice
	{
		const char *value;            ///< as written to Options.ini; "" removes the key (the game's own default)
		Text text;                    ///< label nullptr: the value is shown as it is, followed by 'suffix'
	};

	struct Setting
	{
		Section section;
		Kind kind;
		const char *key;
		Text text;
		const Choice *choices;        ///< KIND_STEPS
		Int choiceCount;
		const char *suffix;           ///< after a plain value: "x", "%", "" ...
		const char *defaultValue;     ///< KIND_CHECK: "yes" or "no"; KIND_STEPS: one of the choice values
		AsciiString (*current)(OptionPreferences &pref);   ///< the value in effect (the game's default when the key is absent)
	};

	#define PLAIN(v) { v, { nullptr, nullptr, nullptr } }

	const Choice kRenderFps[] =
	{
		{ "display", { "GUI:RenderFpsMatchDisplay", L"Match display", L"\x041A\x0430\x043A \x0443 \x044D\x043A\x0440\x0430\x043D\x0430" } },
		PLAIN("60"), PLAIN("75"), PLAIN("90"), PLAIN("100"), PLAIN("120"), PLAIN("144"), PLAIN("165"), PLAIN("180"),
		PLAIN("200"), PLAIN("240"), PLAIN("280"), PLAIN("300"), PLAIN("360"), PLAIN("400"), PLAIN("480"), PLAIN("500"),
		PLAIN("540"), PLAIN("600"), PLAIN("700"), PLAIN("720"), PLAIN("750"), PLAIN("1000"),
		{ "0", { "GUI:RenderFpsNoLimit", L"No limit", L"\x0411\x0435\x0437 \x043E\x0433\x0440\x0430\x043D\x0438\x0447\x0435\x043D\x0438\x044F" } },
	};
	const Choice kTextureFilter[] =
	{
		{ "None", { "GUI:TextureFilterNone", L"None", L"\x041D\x0435\x0442" } },
		{ "Point", { "GUI:TextureFilterPoint", L"Point", L"\x0422\x043E\x0447\x0435\x0447\x043D\x0430\x044F" } },
		{ "Bilinear", { "GUI:TextureFilterBilinear", L"Bilinear", L"\x0411\x0438\x043B\x0438\x043D\x0435\x0439\x043D\x0430\x044F" } },
		{ "Trilinear", { "GUI:TextureFilterTrilinear", L"Trilinear", L"\x0422\x0440\x0438\x043B\x0438\x043D\x0435\x0439\x043D\x0430\x044F" } },
		{ "Anisotropic", { "GUI:TextureFilterAnisotropic", L"Anisotropic", L"\x0410\x043D\x0438\x0437\x043E\x0442\x0440\x043E\x043F\x043D\x0430\x044F" } },
	};
	const Choice kAnisotropy[] = { PLAIN("2"), PLAIN("4"), PLAIN("8"), PLAIN("16") };
	const Choice kSnow[] =
	{
		{ "Default", { "GUI:SnowStyleDefault", L"Standard", L"\x0421\x0442\x0430\x043D\x0434\x0430\x0440\x0442\x043D\x044B\x0439" } },
		{ "Classic", { "GUI:SnowStyleClassic", L"Classic", L"\x041A\x043B\x0430\x0441\x0441\x0438\x0447\x0435\x0441\x043A\x0438\x0439" } },
		{ "Large", { "GUI:SnowStyleLarge", L"Large flakes", L"\x041A\x0440\x0443\x043F\x043D\x044B\x0435 \x0445\x043B\x043E\x043F\x044C\x044F" } },
	};
	const Choice kPercent0To100[] =
	{
		PLAIN("0"), PLAIN("10"), PLAIN("20"), PLAIN("30"), PLAIN("40"), PLAIN("50"), PLAIN("60"), PLAIN("70"), PLAIN("80"),
		PLAIN("90"), PLAIN("100"),
	};
	const Choice kCameraHeight[] =
	{
		{ "", { "GUI:CameraStandard", L"Standard", L"\x0421\x0442\x0430\x043D\x0434\x0430\x0440\x0442\x043D\x0430\x044F" } },
		PLAIN("250"), PLAIN("300"), PLAIN("350"), PLAIN("400"), PLAIN("450"), PLAIN("500"), PLAIN("600"), PLAIN("700"),
		PLAIN("800"), PLAIN("1000"),
	};
	const Choice kCameraPitch[] =
	{
		{ "", { "GUI:CameraStandard", L"Standard", L"\x0421\x0442\x0430\x043D\x0434\x0430\x0440\x0442\x043D\x0430\x044F" } },
		PLAIN("30"), PLAIN("35"), PLAIN("40"), PLAIN("45"), PLAIN("50"), PLAIN("55"), PLAIN("60"), PLAIN("65"), PLAIN("70"),
		PLAIN("75"), PLAIN("80"), PLAIN("85"),
	};
	const Choice kTransitionSpeed[] =
	{
		PLAIN("1"), PLAIN("1.5"), PLAIN("2"), PLAIN("3"), PLAIN("4"), PLAIN("6"), PLAIN("8"),
		{ "1000", { "GUI:TransitionInstant", L"Instant", L"\x041C\x0433\x043D\x043E\x0432\x0435\x043D\x043D\x043E" } },
	};
	const Choice kJpegQuality[] =
	{
		PLAIN("50"), PLAIN("55"), PLAIN("60"), PLAIN("65"), PLAIN("70"), PLAIN("75"), PLAIN("80"), PLAIN("85"), PLAIN("90"),
		PLAIN("95"),
	};
	const Choice kScreenshotFormat[] =
	{
		{ "Jpeg", { "GUI:ScreenshotJpeg", L"JPG", L"JPG" } },
		{ "Png", { "GUI:ScreenshotPng", L"PNG (lossless)", L"PNG (\x0431\x0435\x0437 \x043F\x043E\x0442\x0435\x0440\x044C)" } },
	};
	const Choice kFontAdjust[] =
	{
		{ "-100", { "GUI:FontScaleLanguage", L"Language default", L"\x041A\x0430\x043A \x0432 \x044F\x0437\x044B\x043A\x0435" } },
		PLAIN("0"), PLAIN("25"), PLAIN("50"), PLAIN("75"), PLAIN("100"),
	};
	const Choice kCounterStyle[] =
	{
		{ "Hackers", { "GUI:CounterStyleHackers", L"Hackers", L"Hackers" } },
		{ "GenTool", { "GUI:CounterStyleGenTool", L"GenTool", L"GenTool" } },
	};
	const Choice kWindowed[] =
	{
		{ "no", { "GUI:DisplayModeFullScreen", L"Full screen", L"\x041F\x043E\x043B\x043D\x044B\x0439 \x044D\x043A\x0440\x0430\x043D" } },
		{ "yes", { "GUI:DisplayModeWindow", L"Window", L"\x041E\x043A\x043D\x043E" } },
	};
	const Choice kWindowPosition[] =
	{
		{ "Center", { "GUI:WindowPositionCenter", L"Centre", L"\x041F\x043E \x0446\x0435\x043D\x0442\x0440\x0443" } },
		{ "TopLeft", { "GUI:WindowPositionTopLeft", L"Top left", L"\x0421\x043B\x0435\x0432\x0430 \x0432\x0432\x0435\x0440\x0445\x0443" } },
		{ "TopRight", { "GUI:WindowPositionTopRight", L"Top right", L"\x0421\x043F\x0440\x0430\x0432\x0430 \x0432\x0432\x0435\x0440\x0445\x0443" } },
		{ "BottomLeft", { "GUI:WindowPositionBottomLeft", L"Bottom left", L"\x0421\x043B\x0435\x0432\x0430 \x0432\x043D\x0438\x0437\x0443" } },
		{ "BottomRight", { "GUI:WindowPositionBottomRight", L"Bottom right", L"\x0421\x043F\x0440\x0430\x0432\x0430 \x0432\x043D\x0438\x0437\x0443" } },
	};
	const Choice kRotateSpeed[] = { PLAIN("50"), PLAIN("75"), PLAIN("100"), PLAIN("125"), PLAIN("150"), PLAIN("200"), PLAIN("300") };
	const Choice kFontSize[] =
	{
		{ "0", { "GUI:CounterOff", L"Off", L"\x0412\x044B\x043A\x043B." } },
		PLAIN("6"), PLAIN("7"), PLAIN("8"), PLAIN("9"), PLAIN("10"), PLAIN("11"), PLAIN("12"), PLAIN("14"), PLAIN("16"),
	};

	#define STEPS(list) list, (Int)ARRAY_SIZE(list)

	AsciiString yesNo(Bool b) { return b ? AsciiString("yes") : AsciiString("no"); }
	AsciiString number(Int v) { AsciiString s; s.format("%d", v); return s; }
	AsciiString keyOr(OptionPreferences &p, const char *key, const char *fallback)
	{
		OptionPreferences::const_iterator it = p.find(key);
		return it != p.end() ? it->second : AsciiString(fallback);
	}

	AsciiString curRenderFps(OptionPreferences &p)
	{
		const Int limit = p.getRenderFpsLimit();
		if (limit == RenderFpsPreset::MatchDisplayLimit) return AsciiString("display");
		return number(limit < 0 ? 0 : limit);
	}
	AsciiString curTextureFilter(OptionPreferences &p) { return AsciiString(TextureFilterClass::TextureFilterModeString[p.getTextureFilterMode()]); }
	AsciiString curAnisotropy(OptionPreferences &p) { return number((Int)p.getTextureAnisotropyLevel()); }
	AsciiString curSnow(OptionPreferences &p)
	{
		const SnowStyle s = p.getSnowStyle();
		return AsciiString(s == SNOW_STYLE_CLASSIC ? "Classic" : (s == SNOW_STYLE_LARGE ? "Large" : "Default"));
	}
	AsciiString curMoneyVolume(OptionPreferences &p) { return number(REAL_TO_INT(p.getMoneyTransactionVolume())); }
	AsciiString curRightMouse(OptionPreferences &p) { return yesNo(p.getRightMouseScrollWithAlternateMouseEnabled()); }
	AsciiString curEdgeWindowed(OptionPreferences &p) { return yesNo(p.getScreenEdgeScrollEnabledInWindowedApp()); }
	AsciiString curEdgeFullscreen(OptionPreferences &p) { return yesNo(p.getScreenEdgeScrollEnabledInFullscreenApp()); }
	AsciiString curCaptureWG(OptionPreferences &p) { return yesNo(p.getCursorCaptureEnabledInWindowedGame()); }
	AsciiString curCaptureWM(OptionPreferences &p) { return yesNo(p.getCursorCaptureEnabledInWindowedMenu()); }
	AsciiString curCaptureFG(OptionPreferences &p) { return yesNo(p.getCursorCaptureEnabledInFullscreenGame()); }
	AsciiString curCaptureFM(OptionPreferences &p) { return yesNo(p.getCursorCaptureEnabledInFullscreenMenu()); }
	AsciiString curCameraHeight(OptionPreferences &p) { return keyOr(p, "MaxCameraHeight", ""); }
	AsciiString curCameraPitch(OptionPreferences &p) { return keyOr(p, "CameraPitch", ""); }
	AsciiString curEntireTerrain(OptionPreferences &p) { (void)p; return yesNo(TheGlobalData->m_drawEntireTerrain); }
	AsciiString curArchiveReplays(OptionPreferences &p) { return yesNo(p.getArchiveReplaysEnabled()); }
	AsciiString curPlayerObserver(OptionPreferences &p) { return yesNo(p.getPlayerObserverEnabled()); }
	AsciiString curTransition(OptionPreferences &p) { AsciiString s; s.format("%g", (double)p.getGameWindowTransitionSpeedMultiplier()); return s; }
	AsciiString curJpeg(OptionPreferences &p) { return number(p.getJpegQuality()); }
	AsciiString curScreenshot(OptionPreferences &p) { return keyOr(p, "ScreenshotFormat", "Jpeg"); }
	AsciiString curFontAdjust(OptionPreferences &p)
	{
		const Real v = p.getResolutionFontAdjustment();
		return number(v < 0.0f ? -100 : REAL_TO_INT(v * 100.0f));
	}
	AsciiString curCounterStyle(OptionPreferences &p) { return keyOr(p, "CounterStyle", "Hackers"); }
	AsciiString curFpsSize(OptionPreferences &p) { return number(p.getRenderFpsFontSize()); }
	AsciiString curClockSize(OptionPreferences &p) { return number(p.getSystemTimeFontSize()); }
	AsciiString curGameTimeSize(OptionPreferences &p) { return number(p.getGameTimeFontSize()); }
	AsciiString curLatencySize(OptionPreferences &p) { return number(p.getNetworkLatencyFontSize()); }
	AsciiString curPlayerListSize(OptionPreferences &p) { return number(p.getPlayerInfoListFontSize()); }
	AsciiString curMoneyPerMinute(OptionPreferences &p) { return yesNo(p.getShowMoneyPerMinute()); }
	AsciiString curWindowed(OptionPreferences &p) { const Int w = p.getWindowed(); return yesNo(w >= 0 ? w == 1 : TheGlobalData->m_windowed); }
	AsciiString curWindowPosition(OptionPreferences &p) { return p.getWindowPosition(); }
	AsciiString curRotateSpeed(OptionPreferences &p) { return number(p.getCameraRotateSpeedPercent()); }
	AsciiString curHideIP(OptionPreferences &p) { return yesNo(p.getHideIPAddresses()); }

	/// Keys that take effect at the next start: a change of them brings the restart box.
	Bool needsRestart(const char *key)
	{
		return stricmp(key, "Windowed") == 0 || stricmp(key, "WindowPosition") == 0;
	}

	// The game's own defaults (the getters' values for an absent key): Zero Hour scrolls with the right mouse button
	// with the alternate mouse, the base game does not (GlobalData).
#if RTS_GENERALS
	const char *const kRightMouseDefault = "no";
#else
	const char *const kRightMouseDefault = "yes";
#endif

	const Setting kSettings[] =
	{
		// Graphics
		{ SEC_GRAPHICS, KIND_STEPS, "RenderFpsLimit", { "GUI:RenderFpsLimit", L"Frame rate limit", L"\x041E\x0433\x0440\x0430\x043D\x0438\x0447\x0435\x043D\x0438\x0435 \x043A\x0430\x0434\x0440\x043E\x0432" }, STEPS(kRenderFps), "", "display", curRenderFps },
		{ SEC_GRAPHICS, KIND_STEPS, "TextureFilter", { "GUI:TextureFilter", L"Texture filter", L"\x0424\x0438\x043B\x044C\x0442\x0440\x0430\x0446\x0438\x044F \x0442\x0435\x043A\x0441\x0442\x0443\x0440" }, STEPS(kTextureFilter), "", "Bilinear", curTextureFilter },
		{ SEC_GRAPHICS, KIND_STEPS, "AnisotropyLevel", { "GUI:AnisotropyLevel", L"Anisotropy level", L"\x0423\x0440\x043E\x0432\x0435\x043D\x044C \x0430\x043D\x0438\x0437\x043E\x0442\x0440\x043E\x043F\x0438\x0438" }, STEPS(kAnisotropy), "x", "2", curAnisotropy },
		{ SEC_GRAPHICS, KIND_STEPS, "SnowStyle", { "GUI:SnowStyle", L"Snow", L"\x0421\x043D\x0435\x0433" }, STEPS(kSnow), "", "Default", curSnow },
		// Sound
		{ SEC_SOUND, KIND_STEPS, "MoneyTransactionVolume", { "GUI:MoneyTransactionVolume", L"Money sound volume", L"\x0413\x0440\x043E\x043C\x043A\x043E\x0441\x0442\x044C \x0437\x0432\x0443\x043A\x0430 \x0434\x0435\x043D\x0435\x0433" }, STEPS(kPercent0To100), "%", "50", curMoneyVolume },
		// Controls
		{ SEC_CONTROLS, KIND_CHECK, "UseRightMouseScrollWithAlternateMouse", { "GUI:RightMouseScrollAlternate", L"Right mouse scroll with alternate mouse", L"\x041F\x0440\x043E\x043A\x0440\x0443\x0442\x043A\x0430 \x043F\x0440\x0430\x0432\x043E\x0439 \x043A\x043D\x043E\x043F\x043A\x043E\x0439 (\x0430\x043B\x044C\x0442\x0435\x0440. \x043C\x044B\x0448\x044C)" }, nullptr, 0, "", kRightMouseDefault, curRightMouse },
		{ SEC_CONTROLS, KIND_CHECK, "ScreenEdgeScrollEnabledInWindowedApp", { "GUI:EdgeScrollWindowed", L"Screen edge scroll in a window", L"\x041F\x0440\x043E\x043A\x0440\x0443\x0442\x043A\x0430 \x0443 \x043A\x0440\x0430\x044F \x044D\x043A\x0440\x0430\x043D\x0430 \x0432 \x043E\x043A\x043D\x0435" }, nullptr, 0, "", "no", curEdgeWindowed },
		{ SEC_CONTROLS, KIND_CHECK, "ScreenEdgeScrollEnabledInFullscreenApp", { "GUI:EdgeScrollFullscreen", L"Screen edge scroll in full screen", L"\x041F\x0440\x043E\x043A\x0440\x0443\x0442\x043A\x0430 \x0443 \x043A\x0440\x0430\x044F \x0432 \x043F\x043E\x043B\x043D\x043E\x043C \x044D\x043A\x0440\x0430\x043D\x0435" }, nullptr, 0, "", "yes", curEdgeFullscreen },
		{ SEC_CONTROLS, KIND_CHECK, "CursorCaptureEnabledInWindowedGame", { "GUI:CursorCaptureWindowedGame", L"Keep the cursor in the window: game", L"\x041A\x0443\x0440\x0441\x043E\x0440 \x0432 \x043E\x043A\x043D\x0435: \x0432 \x0438\x0433\x0440\x0435" }, nullptr, 0, "", "yes", curCaptureWG },
		{ SEC_CONTROLS, KIND_CHECK, "CursorCaptureEnabledInWindowedMenu", { "GUI:CursorCaptureWindowedMenu", L"Keep the cursor in the window: menus", L"\x041A\x0443\x0440\x0441\x043E\x0440 \x0432 \x043E\x043A\x043D\x0435: \x0432 \x043C\x0435\x043D\x044E" }, nullptr, 0, "", "no", curCaptureWM },
		{ SEC_CONTROLS, KIND_CHECK, "CursorCaptureEnabledInFullscreenGame", { "GUI:CursorCaptureFullscreenGame", L"Keep the cursor on screen: game", L"\x041A\x0443\x0440\x0441\x043E\x0440 \x043D\x0430 \x044D\x043A\x0440\x0430\x043D\x0435: \x0432 \x0438\x0433\x0440\x0435" }, nullptr, 0, "", "yes", curCaptureFG },
		{ SEC_CONTROLS, KIND_CHECK, "CursorCaptureEnabledInFullscreenMenu", { "GUI:CursorCaptureFullscreenMenu", L"Keep the cursor on screen: menus", L"\x041A\x0443\x0440\x0441\x043E\x0440 \x043D\x0430 \x044D\x043A\x0440\x0430\x043D\x0435: \x0432 \x043C\x0435\x043D\x044E" }, nullptr, 0, "", "yes", curCaptureFM },
		{ SEC_CONTROLS, KIND_CHECK, "DrawEntireTerrain", { "GUI:DrawEntireTerrain", L"Draw the entire terrain", L"\x0420\x0438\x0441\x043E\x0432\x0430\x0442\x044C \x0432\x0441\x044E \x043C\x0435\x0441\x0442\x043D\x043E\x0441\x0442\x044C" }, nullptr, 0, "", "no", curEntireTerrain },
		{ SEC_CONTROLS, KIND_STEPS, "MaxCameraHeight", { "GUI:MaxCameraHeight", L"Highest camera", L"\x041C\x0430\x043A\x0441\x0438\x043C\x0430\x043B\x044C\x043D\x0430\x044F \x0432\x044B\x0441\x043E\x0442\x0430 \x043A\x0430\x043C\x0435\x0440\x044B" }, STEPS(kCameraHeight), "", "", curCameraHeight },
		{ SEC_CONTROLS, KIND_STEPS, "CameraPitch", { "GUI:CameraPitch", L"Camera pitch", L"\x041D\x0430\x043A\x043B\x043E\x043D \x043A\x0430\x043C\x0435\x0440\x044B" }, STEPS(kCameraPitch), "", "", curCameraPitch },
		// Game
		{ SEC_GAME, KIND_CHECK, "ArchiveReplays", { "GUI:ArchiveReplays", L"Keep every replay", L"\x0421\x043E\x0445\x0440\x0430\x043D\x044F\x0442\x044C \x0432\x0441\x0435 \x043F\x043E\x0432\x0442\x043E\x0440\x044B" }, nullptr, 0, "", "no", curArchiveReplays },
		{ SEC_GAME, KIND_CHECK, "PlayerObserverEnabled", { "GUI:PlayerObserverEnabled", L"Observer: see with a player's eyes", L"\x041D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C: \x0433\x043B\x0430\x0437\x0430\x043C\x0438 \x0438\x0433\x0440\x043E\x043A\x0430" }, nullptr, 0, "", "yes", curPlayerObserver },
		// Interface
		{ SEC_INTERFACE, KIND_STEPS, "GameWindowTransitionSpeedMultiplier", { "GUI:MenuTransitionSpeed", L"Menu animation speed", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0430\x043D\x0438\x043C\x0430\x0446\x0438\x0438 \x043C\x0435\x043D\x044E" }, STEPS(kTransitionSpeed), "x", "1", curTransition },
		{ SEC_INTERFACE, KIND_STEPS, "ScreenshotFormat", { "GUI:ScreenshotFormat", L"Screenshot format", L"\x0424\x043E\x0440\x043C\x0430\x0442 \x0441\x043D\x0438\x043C\x043A\x043E\x0432 \x044D\x043A\x0440\x0430\x043D\x0430" }, STEPS(kScreenshotFormat), "", "Jpeg", curScreenshot },
		{ SEC_INTERFACE, KIND_STEPS, "JpegQuality", { "GUI:JpegQuality", L"JPG quality", L"\x041A\x0430\x0447\x0435\x0441\x0442\x0432\x043E JPG" }, STEPS(kJpegQuality), "", "80", curJpeg },
		{ SEC_INTERFACE, KIND_STEPS, "ResolutionFontAdjustment", { "GUI:ResolutionFontAdjustment", L"Text size with resolution", L"\x0420\x0430\x0437\x043C\x0435\x0440 \x0442\x0435\x043A\x0441\x0442\x0430 \x043F\x043E \x0440\x0430\x0437\x0440\x0435\x0448\x0435\x043D\x0438\x044E" }, STEPS(kFontAdjust), "%", "-100", curFontAdjust },
		// Information
		{ SEC_INFO, KIND_STEPS, "CounterStyle", { "GUI:CounterStyle", L"Top counter look", L"\x0412\x0438\x0434 \x0432\x0435\x0440\x0445\x043D\x0435\x0433\x043E \x0441\x0447\x0451\x0442\x0447\x0438\x043A\x0430" }, STEPS(kCounterStyle), "", "Hackers", curCounterStyle },
		{ SEC_INFO, KIND_STEPS, "RenderFpsFontSize", { "GUI:RenderFpsCounter", L"Frame rate (FPS)", L"\x0427\x0430\x0441\x0442\x043E\x0442\x0430 \x043A\x0430\x0434\x0440\x043E\x0432 (FPS)" }, STEPS(kFontSize), "", "8", curFpsSize },
		{ SEC_INFO, KIND_STEPS, "SystemTimeFontSize", { "GUI:SystemTimeCounter", L"Clock", L"\x0427\x0430\x0441\x044B" }, STEPS(kFontSize), "", "8", curClockSize },
		{ SEC_INFO, KIND_STEPS, "GameTimeFontSize", { "GUI:GameTimeCounter", L"Game time", L"\x0412\x0440\x0435\x043C\x044F \x0438\x0433\x0440\x044B" }, STEPS(kFontSize), "", "8", curGameTimeSize },
		{ SEC_INFO, KIND_STEPS, "NetworkLatencyFontSize", { "GUI:NetworkLatencyCounter", L"Network delay (ms)", L"\x0421\x0435\x0442\x0435\x0432\x0430\x044F \x0437\x0430\x0434\x0435\x0440\x0436\x043A\x0430 (\x043C\x0441)" }, STEPS(kFontSize), "", "8", curLatencySize },
		{ SEC_INFO, KIND_STEPS, "PlayerInfoListFontSize", { "GUI:PlayerInfoListCounter", L"Observer money list", L"\x0421\x043F\x0438\x0441\x043E\x043A \x0434\x0435\x043D\x0435\x0433 (\x043D\x0430\x0431\x043B\x044E\x0434\x0430\x0442\x0435\x043B\x044C)" }, STEPS(kFontSize), "", "8", curPlayerListSize },
		// Display, controls, network (O8)
		{ SEC_DISPLAY, KIND_STEPS, "Windowed", { "GUI:DisplayMode", L"Display mode", L"\x0420\x0435\x0436\x0438\x043C \x044D\x043A\x0440\x0430\x043D\x0430" }, STEPS(kWindowed), "", "no", curWindowed },
		{ SEC_DISPLAY, KIND_STEPS, "WindowPosition", { "GUI:WindowPosition", L"Window position", L"\x041F\x043E\x043B\x043E\x0436\x0435\x043D\x0438\x0435 \x043E\x043A\x043D\x0430" }, STEPS(kWindowPosition), "", "Center", curWindowPosition },
		{ SEC_CONTROLS, KIND_STEPS, "CameraRotateSpeed", { "GUI:CameraRotateSpeed", L"Camera rotation speed", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x043F\x043E\x0432\x043E\x0440\x043E\x0442\x0430 \x043A\x0430\x043C\x0435\x0440\x044B" }, STEPS(kRotateSpeed), "%", "100", curRotateSpeed },
		{ SEC_NETWORK, KIND_CHECK, "HideIPAddresses", { "GUI:HideIPAddresses", L"Hide IP addresses", L"\x0421\x043A\x0440\x044B\x0432\x0430\x0442\x044C IP-\x0430\x0434\x0440\x0435\x0441\x0430" }, nullptr, 0, "", "no", curHideIP },
		{ SEC_INFO, KIND_CHECK, "ShowMoneyPerMinute", { "GUI:ShowMoneyPerMinute", L"Money per minute", L"\x0414\x0435\x043D\x044C\x0433\x0438 \x0432 \x043C\x0438\x043D\x0443\x0442\x0443" }, nullptr, 0, "", "no", curMoneyPerMinute },
	};
	enum { SETTING_COUNT = (Int)ARRAY_SIZE(kSettings) };

	//---------------------------------------------------------------------------------------------
	// Existing controls of OptionsMenu.wnd and where they go. The label is the control's own named label window, or a
	// new label with the text of a string file label (the old unnamed labels of the advanced panel).
	struct Existing
	{
		Section section;
		const char *control;
		const char *labelWindow;      ///< nullptr: no label, or 'labelText'
		const char *labelText;        ///< a string file label for a new label; nullptr: none
		Bool customDetail;            ///< one of the "custom detail" controls: enabled only with the Custom level
		Bool show;                    ///< unhide (the .wnd hides it)
	};

	const Existing kExisting[] =
	{
		{ SEC_DISPLAY, "ComboBoxResolution", "ResolutionLabel", nullptr, FALSE, FALSE },
		{ SEC_DISPLAY, "SliderGamma", "GammaLabel", nullptr, FALSE, FALSE },
		{ SEC_GRAPHICS, "ComboBoxDetail", "DetailLabel", nullptr, FALSE, FALSE },
		{ SEC_GRAPHICS, "ComboBoxAntiAliasing", nullptr, "GUI:AntiAliasing", FALSE, TRUE },
		{ SEC_GRAPHICS, "LowResSlider", nullptr, "GUI:LowResSlider", TRUE, FALSE },
		{ SEC_GRAPHICS, "ParticleCapSlider", nullptr, "GUI:ParticleCap", TRUE, FALSE },
		{ SEC_GRAPHICS, "Check3DShadows", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "Check2DShadows", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckCloudShadows", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckGroundLighting", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckSmoothWater", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckShowProps", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckExtraAnimations", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckHeatEffects", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckBehindBuilding", nullptr, nullptr, TRUE, FALSE },
		{ SEC_GRAPHICS, "CheckNoDynamicLOD", nullptr, nullptr, TRUE, FALSE },
		{ SEC_SOUND, "SliderMusicVolume", "MusicVolumeLabel", nullptr, FALSE, FALSE },
		{ SEC_SOUND, "SliderSFXVolume", "SFXVolumeLabel", nullptr, FALSE, FALSE },
		{ SEC_SOUND, "SliderVoiceVolume", "VoiceVolumeLabel", nullptr, FALSE, FALSE },
		{ SEC_CONTROLS, "CheckAlternateMouse", nullptr, nullptr, FALSE, FALSE },
		{ SEC_CONTROLS, "SliderScrollSpeed", "ScrollSpeedLabel", nullptr, FALSE, FALSE },
		{ SEC_CONTROLS, "CheckBoxDrawAnchor", nullptr, nullptr, FALSE, FALSE },
		{ SEC_CONTROLS, "CheckBoxMoveAnchor", nullptr, nullptr, FALSE, FALSE },
		{ SEC_GAME, "Retaliation", nullptr, nullptr, FALSE, FALSE },
		{ SEC_GAME, "CheckDoubleClickAttackMove", nullptr, nullptr, FALSE, FALSE },
		{ SEC_GAME, "CheckLanguageFilter", nullptr, nullptr, FALSE, FALSE },
		{ SEC_GAME, "CheckBoxSaveCamera", nullptr, nullptr, FALSE, TRUE },
		{ SEC_GAME, "CheckBoxUseCamera", nullptr, nullptr, FALSE, TRUE },
		{ SEC_NETWORK, "ComboBoxOnlineIP", "StaticTextOnlineIpAddresses", nullptr, FALSE, FALSE },
		{ SEC_NETWORK, "ComboBoxIP", "StaticTextLANIpAddresses", nullptr, FALSE, FALSE },
		{ SEC_NETWORK, "TextEntryFirewallPortOverride", "StaticTextFirewallPortOverride", nullptr, FALSE, FALSE },
		{ SEC_NETWORK, "ButtonFirewallRefresh", nullptr, nullptr, FALSE, FALSE },
	};
	enum { EXISTING_COUNT = (Int)ARRAY_SIZE(kExisting) };

	/// The old group windows: hidden once their controls are on the pages.
	const char *const kOldGroups[] = { "VideoParent", "AudioParent", "ScrollParent", "NetworkParent", "WinAdvancedDisplayOptions" };

	//---------------------------------------------------------------------------------------------
	// The data pack choosers (Packs.h): one row of steps per kind, some with a preview picture.
	struct PackRowDef
	{
		Packs::Kind kind;
		Section section;
		Bool preview;
	};

	const PackRowDef kPackRows[] =
	{
		{ Packs::KIND_TEXT, SEC_LANGUAGE, FALSE },     // first: the hotkey row depends on it
		{ Packs::KIND_VOICE, SEC_LANGUAGE, FALSE },
		{ Packs::KIND_MOVIES, SEC_LANGUAGE, FALSE },
		{ Packs::KIND_CONTROLBAR, SEC_INTERFACE, TRUE },
		{ Packs::KIND_HOTKEYS, SEC_CONTROLS, TRUE },
	};
	enum { PACK_ROW_COUNT = (Int)ARRAY_SIZE(kPackRows), PACK_ROW_TEXT = 0, MAX_PACK_CHOICES = 32 };

	//---------------------------------------------------------------------------------------------
	struct SettingState
	{
		GameWindow *control;          ///< check box or slider
		GameWindow *label;            ///< KIND_STEPS
		GameWindow *value;            ///< KIND_STEPS
		Int step;
		Bool touched;                 ///< KIND_STEPS: moved by the player or reset by Defaults; else save keeps the stored value
	};

	struct PackRowState
	{
		GameWindow *label;
		GameWindow *value;
		GameWindow *slider;
		GameWindow *preview;          ///< draws 'image' in a frame
		GameWindow *previewText;      ///< "no picture", over the frame
		const Packs::Pack *choices[MAX_PACK_CHOICES];   ///< nullptr: the base data
		Int choiceCount;
		Int step;
		const Packs::Pack *initial;   ///< the choice when the screen opened
		const Image *image;
	};

	enum { MAX_SUB_PAGES = 4 };

	/// The sound output devices of the device row: [0] is the system default (empty name); UTF-8 names.
	std::vector<AsciiString> s_devices;

	struct State
	{
		Bool active;
		OptionPreferences *pref;
		GameWindow *menuParent;
		GameWindow *buttons[SEC_COUNT];
		GameWindow *pages[SEC_COUNT];       ///< the first page of each section
		GameWindow *subPages[SEC_COUNT][MAX_SUB_PAGES];   ///< all pages of a section, [0] = pages[]
		GameWindow *pagers[SEC_COUNT][MAX_SUB_PAGES];     ///< "1/2 >" on each page of a section with more than one
		Int subPageCount[SEC_COUNT];
		Int currentSub[SEC_COUNT];
		Int pageX, pageY, pageW, pageH, gap, titleH;      ///< where the pages are, for the pages made while filling
		Bool restartNeeded;          ///< a key that takes effect at the next start changed in this save
		GameWindow *deviceLabel;     ///< the sound output device row (AudioOutputDevice)
		GameWindow *deviceValue;
		GameWindow *deviceSlider;
		Int deviceStep;
		Int deviceInitial;
		Section current;
		SettingState settings[SETTING_COUNT];
		GameWindow *existing[EXISTING_COUNT];
		GameWindow *existingLabels[EXISTING_COUNT];
		GameWindow *detailCombo;
		PackRowState packRows[PACK_ROW_COUNT];
		// looks
		GameWindow *buttonLook;
		GameWindow *frameLook;
		GameWindow *titleLook;
		GameWindow *labelLook;
		GameWindow *checkLook;
		GameWindow *sliderLook;
		WinDrawData buttonEnabled[MAX_DRAW_DATA];
		WinDrawData buttonHilite[MAX_DRAW_DATA];
	};
	State s;

	GameWindow *findWindow(const char *name)
	{
		AsciiString full("OptionsMenu.wnd:");
		full.concat(name);
		return TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(full));
	}

	Int windowId(const char *name)
	{
		return (Int)TheNameKeyGenerator->nameToKey(AsciiString(name));
	}

	ICoord2D posOf(GameWindow *w) { ICoord2D p = { 0, 0 }; w->winGetPosition(&p.x, &p.y); return p; }
	ICoord2D sizeOf(GameWindow *w) { ICoord2D p = { 0, 0 }; w->winGetSize(&p.x, &p.y); return p; }

	/// The look of 'look' (draw data, text colors, font) on a new instance data of 'style'.
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

	/// The drawn size of 'text' in 'font', wrapped at 'wrapWidth' pixels (0: one line).
	ICoord2D textSize(GameFont *font, const UnicodeString &text, Int wrapWidth)
	{
		ICoord2D size = { 0, 0 };
		if (font == nullptr || TheDisplayStringManager == nullptr || text.isEmpty()) {
			return size;
		}
		DisplayString *ds = TheDisplayStringManager->newDisplayString();
		if (ds == nullptr) {
			return size;
		}
		ds->setFont(font);
		ds->setWordWrap(wrapWidth > 0 ? wrapWidth : 0);
		ds->setText(text);
		ds->getSize(&size.x, &size.y);
		TheDisplayStringManager->freeDisplayString(ds);
		return size;
	}

	/// A static text window of 'width' wraps its text at width - 10 (W3DStaticText): the height its text needs.
	Int labelHeightFor(GameFont *font, const UnicodeString &text, Int width, Int minHeight)
	{
		const ICoord2D size = textSize(font, text, width - 10);
		return max(minHeight, size.y + 4);
	}

	/// A check box draws its text right of the box (which is as high as the window), centred on the window's middle, with
	/// no clip. The window gets 'width'. When the text is wider than the room right of the box it is wrapped there (the
	/// display string keeps the wrap; the draw does not reset it) and the lines spread above and below the window's middle.
	/// Returns the height to reserve; the window goes in the middle of it, so the box keeps its size.
	Int fitCheckBox(GameWindow *box, Int width)
	{
		if (box == nullptr) {
			return 0;
		}
		const ICoord2D size = sizeOf(box);
		box->winSetSize(width, size.y);
		WinInstanceData *inst = box->winGetInstanceData();
		DisplayString *ds = inst != nullptr ? inst->getTextDisplayString() : nullptr;
		if (ds == nullptr || ds->getTextLength() == 0) {
			return size.y;
		}
		ds->setFont(box->winGetFont());
		ds->setWordWrap(0);
		Int w = 0, h = 0;
		ds->getSize(&w, &h);
		const Int room = width - size.y - 4;   // the text starts one window height right of the left edge
		if (w <= room || room <= 40) {
			return max(size.y, h);
		}
		ds->setWordWrap(room);
		ds->getSize(&w, &h);
		return max(size.y, h + 2);
	}

	UnsignedInt imageStatusOf(GameWindow *look)
	{
		return look != nullptr ? (look->winGetStatus() & WIN_STATUS_IMAGE) : 0;
	}

	GameWindow *makeLabel(GameWindow *parent, GameWindow *look, Int x, Int y, Int w, Int h, const UnicodeString &text, Bool right)
	{
		WinInstanceData inst;
		copyLook(inst, look, GWS_STATIC_TEXT);
		TextData textData;
		memset(&textData, 0, sizeof(textData));
		textData.centered = FALSE;
		textData.centeredVertically = TRUE;
		GameWindow *win = TheWindowManager->gogoGadgetStaticText(parent, WIN_STATUS_ENABLED | imageStatusOf(look), x, y, w, h,
			&inst, &textData, inst.m_font, look == nullptr);
		if (win != nullptr)
		{
			GadgetStaticTextSetText(win, text);
			if (right)
			{
				// right aligned: the window is moved so that the text ends at x + w
				Int tw = 0, th = 0;
				TextData *data = (TextData *)win->winGetUserData();
				if (data != nullptr && data->text != nullptr) {
					data->text->getSize(&tw, &th);
					if (tw > 0 && tw < w) {
						win->winSetPosition(x + w - tw - 2, y);
						win->winSetSize(tw + 2, h);
					}
				}
			}
		}
		return win;
	}

	GameWindow *makeCheckBox(GameWindow *parent, Int x, Int y, Int w, Int h, const UnicodeString &text, Int id)
	{
		WinInstanceData inst;
		copyLook(inst, s.checkLook, GWS_CHECK_BOX | GWS_MOUSE_TRACK);
		inst.m_id = id;
		GameWindow *win = TheWindowManager->gogoGadgetCheckbox(parent, WIN_STATUS_ENABLED | imageStatusOf(s.checkLook), x, y, w, h,
			&inst, inst.m_font, s.checkLook == nullptr);
		if (win != nullptr)
		{
			win->winSetWindowId(id);
			win->winSetOwner(s.menuParent);
			GadgetCheckBoxSetText(win, text);
		}
		return win;
	}

	GameWindow *makeSlider(GameWindow *parent, Int x, Int y, Int w, Int h, Int maxValue, Int id)
	{
		WinInstanceData inst;
		copyLook(inst, s.sliderLook, GWS_HORZ_SLIDER | GWS_MOUSE_TRACK);
		inst.m_id = id;
		SliderData data;
		memset(&data, 0, sizeof(data));
		data.minVal = 0;
		data.maxVal = maxValue > 0 ? maxValue : 1;
		GameWindow *win = TheWindowManager->gogoGadgetSlider(parent, WIN_STATUS_ENABLED | imageStatusOf(s.sliderLook), x, y, w, h,
			&inst, &data, inst.m_font, s.sliderLook == nullptr);
		if (win != nullptr)
		{
			win->winSetWindowId(id);
			win->winSetOwner(s.menuParent);
			// the thumb looks like the menu's slider thumbs
			GameWindow *thumb = win->winGetChild();
			GameWindow *lookThumb = s.sliderLook != nullptr ? s.sliderLook->winGetChild() : nullptr;
			if (thumb != nullptr && lookThumb != nullptr)
			{
				WinInstanceData *to = thumb->winGetInstanceData();
				const WinInstanceData *from = lookThumb->winGetInstanceData();
				for (Int i = 0; i < MAX_DRAW_DATA; ++i)
				{
					to->m_enabledDrawData[i] = from->m_enabledDrawData[i];
					to->m_disabledDrawData[i] = from->m_disabledDrawData[i];
					to->m_hiliteDrawData[i] = from->m_hiliteDrawData[i];
				}
				thumb->winSetDrawFunc(lookThumb->winGetDrawFunc());
				thumb->winSetSize(sizeOf(lookThumb).x, sizeOf(lookThumb).y);
			}
		}
		return win;
	}

	//---------------------------------------------------------------------------------------------
	Int findChoice(const Setting &set, const AsciiString &value)
	{
		for (Int i = 0; i < set.choiceCount; ++i) {
			if (stricmp(set.choices[i].value, value.str()) == 0) {
				return i;
			}
		}
		// the nearest number
		char *end = nullptr;
		const double v = strtod(value.str(), &end);
		if (end != value.str() && value.getLength() > 0)
		{
			Int best = -1;
			double bestDist = 0.0;
			for (Int i = 0; i < set.choiceCount; ++i)
			{
				char *e = nullptr;
				const double c = strtod(set.choices[i].value, &e);
				if (e == set.choices[i].value || set.choices[i].value[0] == '\0') {
					continue;
				}
				const double d = c > v ? c - v : v - c;
				if (best < 0 || d < bestDist) {
					best = i;
					bestDist = d;
				}
			}
			if (best >= 0) {
				return best;
			}
		}
		return -1;
	}

	Int defaultStep(const Setting &set)
	{
		const Int i = findChoice(set, AsciiString(set.defaultValue));
		return i >= 0 ? i : 0;
	}

	UnicodeString choiceText(const Setting &set, Int step)
	{
		const Choice &c = set.choices[step];
		if (c.text.label != nullptr)
		{
			UnicodeString text = textOf(c.text);
			if (strcmp(c.value, "display") == 0)
			{
				UnicodeString hz;
				hz.format(L" (%d)", RenderFpsPreset::getDisplayRefreshRate());
				text.concat(hz);
			}
			return text;
		}
		UnicodeString text;
		UnicodeString value;
		value.translate(AsciiString(c.value));
		UnicodeString suffix;
		suffix.translate(AsciiString(set.suffix));
		text.format(L"%ls%ls", value.str(), suffix.str());
		return text;
	}

	void showStep(Int index)
	{
		SettingState &st = s.settings[index];
		const Setting &set = kSettings[index];
		if (st.control != nullptr) {
			GadgetSliderSetPosition(st.control, st.step);
		}
		if (st.value != nullptr) {
			GadgetStaticTextSetText(st.value, choiceText(set, st.step));
		}
	}

	void setChecked(Int index, Bool checked)
	{
		if (s.settings[index].control != nullptr) {
			GadgetCheckBoxSetChecked(s.settings[index].control, checked);
		}
	}

	void showSection(Section section)
	{
		s.current = section;
		for (Int i = 0; i < SEC_COUNT; ++i)
		{
			for (Int k = 0; k < s.subPageCount[i]; ++k) {
				if (s.subPages[i][k] != nullptr) {
					s.subPages[i][k]->winHide(i != section || k != s.currentSub[i]);
				}
			}
			if (s.buttons[i] != nullptr)
			{
				// the chosen section's button is drawn with the menu buttons' highlight look
				WinInstanceData *inst = s.buttons[i]->winGetInstanceData();
				for (Int d = 0; d < MAX_DRAW_DATA; ++d) {
					inst->m_enabledDrawData[d] = (i == section) ? s.buttonHilite[d] : s.buttonEnabled[d];
				}
			}
		}
	}

	Bool customDetailSelected()
	{
		if (s.detailCombo == nullptr) {
			return TRUE;
		}
		Int index = -1;
		GadgetComboBoxGetSelectedPos(s.detailCombo, &index);
		return index == STATIC_GAME_LOD_CUSTOM;
	}

	/// The custom detail controls follow the detail level: usable only with "Custom", as the old panel was.
	void updateCustomDetail()
	{
		const Bool enable = customDetailSelected();
		for (Int i = 0; i < EXISTING_COUNT; ++i)
		{
			if (kExisting[i].customDetail && s.existing[i] != nullptr) {
				s.existing[i]->winEnable(enable);
			}
		}
	}

	//---------------------------------------------------------------------------------------------
	GameWindow *makePage(Int sec);

	/// Places items top to bottom in two columns of a page; a new column starts when the next item does not fit, and a
	/// new page of the section when the second column is full too (the section then gets a "1/2 >" button on each page).
	/// So nothing leaves a page at any resolution; only an item taller than a whole column could, and that is logged.
	struct Flow
	{
		GameWindow *page;
		Int sec;
		Int x0, y0, colW, gap, bottom;
		Int col;
		Int y;
		Int valueW;      ///< the width of the value texts of this section's step rows (one column of values)
		Bool overflow;   ///< an item taller than a column

		void begin(Int section, GameWindow *p, Int left, Int top, Int columnWidth, Int g, Int b)
		{
			sec = section; page = p; x0 = left; y0 = top; colW = columnWidth; gap = g; bottom = b; col = 0; y = top;
			valueW = 0; overflow = FALSE;
		}
		/// The position of an item of 'height'.
		ICoord2D next(Int height)
		{
			if (y + height > bottom && y > y0 && col == 0) {
				col = 1;
				y = y0;
			}
			else if (y + height > bottom && y > y0 && col == 1)
			{
				GameWindow *more = makePage(sec);
				if (more != nullptr)
				{
					page = more;
					col = 0;
					y = y0;
				}
			}
			if (y + height > bottom) {
				overflow = TRUE;
			}
			ICoord2D p = { x0 + col * (colW + gap * 2), y };
			y += height + gap;
			return p;
		}
	};

	GameFont *labelFont()
	{
		return s.labelLook != nullptr ? s.labelLook->winGetFont() : nullptr;
	}

	/// "2/3 >" on every page of a section with more than one page: the next page of the section.
	void updatePagers(Int sec)
	{
		const Int count = s.subPageCount[sec];
		if (count < 2) {
			return;
		}
		for (Int k = 0; k < count; ++k)
		{
			GameWindow *page = s.subPages[sec][k];
			if (page == nullptr) {
				continue;
			}
			UnicodeString text;
			text.format(L"%d/%d  >", k + 1, count);
			if (s.pagers[sec][k] == nullptr)
			{
				WinInstanceData inst;
				copyLook(inst, s.buttonLook, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
				if (labelFont() != nullptr) {
					inst.m_font = labelFont();
				}
				AsciiString name;
				name.format("OptionsMenu.wnd:Sections.Pager%d_%d", sec, k);
				inst.m_id = windowId(name.str());
				const Int w = textSize(inst.m_font, UnicodeString(L"4/4  >"), 0).x + s.gap * 6;
				GameWindow *button = TheWindowManager->gogoGadgetPushButton(page, WIN_STATUS_ENABLED | imageStatusOf(s.buttonLook),
					s.pageW - s.gap * 3 - w, s.gap, w, s.titleH, &inst, inst.m_font, FALSE);
				if (button != nullptr)
				{
					button->winSetWindowId(inst.m_id);
					button->winSetOwner(s.menuParent);
				}
				s.pagers[sec][k] = button;
			}
			if (s.pagers[sec][k] != nullptr) {
				GadgetButtonSetText(s.pagers[sec][k], text);
			}
		}
	}

	/// A (further) page of a section, framed like the old groups, with the section's title; hidden until shown.
	GameWindow *makePage(Int sec)
	{
		if (s.subPageCount[sec] >= MAX_SUB_PAGES) {
			return nullptr;
		}
		WinInstanceData inst;
		copyLook(inst, s.frameLook, GWS_USER_WINDOW);
		GameWindow *page = TheWindowManager->winCreate(s.menuParent, WIN_STATUS_ENABLED | imageStatusOf(s.frameLook), s.pageX, s.pageY,
			s.pageW, s.pageH, PassMessagesToParentSystem, &inst);
		if (page == nullptr) {
			return nullptr;
		}
		if (s.frameLook != nullptr) {
			page->winSetDrawFunc(s.frameLook->winGetDrawFunc());
		}
		makeLabel(page, s.titleLook, s.gap * 3, s.gap, s.pageW - s.gap * 6, s.titleH, textOf(kSectionNames[sec]), FALSE);
		page->winHide(TRUE);
		s.subPages[sec][s.subPageCount[sec]++] = page;
		updatePagers(sec);
		return page;
	}

	/// A row of steps: the title over the whole column on as many lines as it needs, then the slider with the value text
	/// right of it. 'valueTextW' is the width of the widest value text; 'extraBelow' is room kept under the row.
	/// Returns the y below the row (in the page) through 'below'.
	void placeStepRow(Flow &flow, const UnicodeString &title, Int valueTextW, Int sliderMax, Int id, Int labelH, Int sliderH,
		Int extraBelow, GameWindow **label, GameWindow **value, GameWindow **slider, ICoord2D *below)
	{
		// TheSuperHackers @bugfix (H1-H4) One rule for every row of a section: the value column is as wide as the section's
		// widest value in the label font (the static text wraps 10 pixels inside its window), with no cap, so a value is
		// never cut or wrapped beside the slider. When that would leave the slider less than two fifths of the column, the
		// section's values go on their own line under the slider instead, as wide as the column.
		const Int titleH = labelHeightFor(labelFont(), title, flow.colW, labelH);
		const Int widest = max(valueTextW, flow.valueW);
		const Int sideValueW = widest + 14;
		const Bool stacked = flow.colW - sideValueW - flow.gap < flow.colW * 2 / 5;
		if (!stacked)
		{
			const Int lineH = max(labelH, sliderH);
			const Int sliderW = flow.colW - sideValueW - flow.gap;
			const ICoord2D at = flow.next(titleH + lineH + extraBelow);
			*label = makeLabel(flow.page, s.labelLook, at.x, at.y, flow.colW, titleH, title, FALSE);
			*slider = makeSlider(flow.page, at.x, at.y + titleH + (lineH - sliderH) / 2, sliderW, sliderH, sliderMax, id);
			*value = makeLabel(flow.page, s.labelLook, at.x + sliderW + flow.gap, at.y + titleH, sideValueW, lineH,
				UnicodeString::TheEmptyString, FALSE);
			if (below != nullptr) {
				below->x = at.x;
				below->y = at.y + titleH + lineH;
			}
			return;
		}
		// a value wider than the column wraps there: room for its lines
		const Int wrapW = max(1, flow.colW - 10);
		const Int valueLines = max(1, (valueTextW + wrapW - 1) / wrapW + ((valueTextW > wrapW) ? 1 : 0));
		const Int valueH = labelH * valueLines;
		const ICoord2D at = flow.next(titleH + sliderH + valueH + extraBelow);
		*label = makeLabel(flow.page, s.labelLook, at.x, at.y, flow.colW, titleH, title, FALSE);
		*slider = makeSlider(flow.page, at.x, at.y + titleH, flow.colW, sliderH, sliderMax, id);
		*value = makeLabel(flow.page, s.labelLook, at.x, at.y + titleH + sliderH, flow.colW, valueH,
			UnicodeString::TheEmptyString, FALSE);
		if (below != nullptr) {
			below->x = at.x;
			below->y = at.y + titleH + sliderH + valueH;
		}
	}

	/// A check box: the column's width, its text wrapped when it is wider (fitCheckBox), in the middle of its room.
	void placeCheckBox(Flow &flow, GameWindow *box)
	{
		const Int room = fitCheckBox(box, flow.colW);
		const Int h = sizeOf(box).y;
		const ICoord2D at = flow.next(room);
		box->winSetParent(flow.page);
		box->winSetPosition(at.x, at.y + (room - h) / 2);
	}

	void placeExisting(Flow &flow, Int index)
	{
		GameWindow *control = s.existing[index];
		if (control == nullptr) {
			return;
		}
		const Existing &ex = kExisting[index];
		GameWindow *label = s.existingLabels[index];
		if (!ex.show && control->winIsHidden())
		{
			// hidden by the .wnd (the scroll anchor boxes): no room on the page
			control->winSetParent(flow.page);
			if (label != nullptr) {
				label->winSetParent(flow.page);
				label->winHide(TRUE);
			}
			return;
		}
		ICoord2D cs = sizeOf(control);
		const UnsignedInt style = control->winGetStyle();
		const Bool isSlider = (style & GWS_HORZ_SLIDER) != 0;
		const Bool isCheck = (style & GWS_CHECK_BOX) != 0;
		if (isCheck)
		{
			// one font and height for all check boxes of the screen (some old ones were smaller)
			if (s.checkLook != nullptr && control != s.checkLook)
			{
				control->winSetFont(s.checkLook->winGetFont());
				control->winSetSize(cs.x, sizeOf(s.checkLook).y);
			}
			if (label != nullptr) {
				label->winHide(TRUE);
			}
			placeCheckBox(flow, control);
			if (ex.show) {
				control->winHide(FALSE);
			}
			return;
		}
		if (isSlider)
		{
			// sliders take the column's width
			Int minVal = 0, maxVal = 0;
			GadgetSliderGetMinMax(control, &minVal, &maxVal);
			const Int pos = GadgetSliderGetPosition(control);
			control->winSetSize(flow.colW, cs.y);
			cs.x = flow.colW;
			TheWindowManager->winSendSystemMsg(control, GSM_SET_MIN_MAX, minVal, maxVal);
			GadgetSliderSetPosition(control, pos);
		}
		Int labelH = 0;
		if (label != nullptr)
		{
			// the title over the whole column, as high as its text needs
			const UnicodeString text = GadgetStaticTextGetText(label);
			labelH = labelHeightFor(label->winGetFont(), text, flow.colW, sizeOf(label).y);
			label->winSetSize(flow.colW, labelH);
		}
		const ICoord2D at = flow.next(labelH + cs.y);
		if (label != nullptr)
		{
			label->winSetParent(flow.page);
			label->winSetPosition(at.x, at.y);
			label->winHide(FALSE);
		}
		control->winSetParent(flow.page);
		control->winSetPosition(at.x, at.y + labelH);
		if (ex.show) {
			control->winHide(FALSE);
		}
	}

	void placeSetting(Flow &flow, Int index, Int labelH, Int checkH, Int sliderH)
	{
		const Setting &set = kSettings[index];
		SettingState &st = s.settings[index];
		AsciiString name("OptionsMenu.wnd:Sections.");
		name.concat(set.key);
		const Int id = windowId(name.str());
		if (set.kind == KIND_CHECK)
		{
			st.control = makeCheckBox(flow.page, 0, 0, flow.colW, checkH, textOf(set.text), id);
			if (st.control != nullptr) {
				placeCheckBox(flow, st.control);
			}
			setChecked(index, stricmp(set.current(*s.pref).str(), "yes") == 0);
			return;
		}
		Int valueTextW = 0;
		for (Int c = 0; c < set.choiceCount; ++c) {
			valueTextW = max(valueTextW, textSize(labelFont(), choiceText(set, c), 0).x);
		}
		placeStepRow(flow, textOf(set.text), valueTextW, set.choiceCount - 1, id, labelH, sliderH, 0,
			&st.label, &st.value, &st.control, nullptr);
		const Int found = findChoice(set, set.current(*s.pref));
		st.step = found >= 0 ? found : defaultStep(set);
		showStep(index);
	}

	//---------------------------------------------------------------------------------------------
	// Data pack rows
	const Packs::Pack *selectedPack(Int row)
	{
		const PackRowState &st = s.packRows[row];
		return (st.choiceCount > 0 && st.step >= 0 && st.step < st.choiceCount) ? st.choices[st.step] : nullptr;
	}

	/// The choices of a row; the hotkey row offers only the sets that fit the chosen interface language.
	void buildPackChoices(Int row, const Packs::Pack *keep)
	{
		PackRowState &st = s.packRows[row];
		const Packs::Kind kind = kPackRows[row].kind;
		const AsciiString textLanguage = (kind == Packs::KIND_HOTKEYS) ? Packs::textLanguageOf(selectedPack(PACK_ROW_TEXT)) : AsciiString::TheEmptyString;
		st.choiceCount = 0;
		if (Packs::baseShown(kind)) {
			st.choices[st.choiceCount++] = nullptr;
		}
		const Int count = Packs::count(kind);
		for (Int i = 0; i < count && st.choiceCount < MAX_PACK_CHOICES; ++i)
		{
			const Packs::Pack *pack = Packs::get(kind, i);
			if (kind == Packs::KIND_HOTKEYS && !Packs::hotkeysFit(pack, textLanguage)) {
				continue;
			}
			st.choices[st.choiceCount++] = pack;
		}
		if (st.choiceCount == 0) {
			st.choices[st.choiceCount++] = nullptr;   // nothing fits: the base data, as the game would use
		}
		st.step = -1;
		for (Int i = 0; i < st.choiceCount; ++i) {
			if (st.choices[i] == keep) {
				st.step = i;
			}
		}
		if (st.step < 0)
		{
			// a choice that is gone (another interface language): the base data if offered, else the first
			st.step = 0;
			for (Int i = 0; i < st.choiceCount; ++i) {
				if (st.choices[i] == nullptr) {
					st.step = i;
					break;
				}
			}
		}
	}

	/// The image of a pack's preview picture, made once and kept by the mapped image collection.
	const Image *previewImageOf(const Packs::Pack *pack)
	{
		if (pack == nullptr || pack->previewTexture.isEmpty() || TheMappedImageCollection == nullptr) {
			return nullptr;
		}
		const Image *found = TheMappedImageCollection->findImageByName(pack->previewTexture);
		if (found != nullptr) {
			return found;
		}
		Image *image = newInstance(Image);
		image->setName(pack->previewTexture);
		image->setFilename(pack->previewTexture);
		image->setStatus(IMAGE_STATUS_NONE);
		Region2D uv;
		uv.lo.x = 0.0f;
		uv.lo.y = 0.0f;
		uv.hi.x = 1.0f;
		uv.hi.y = 1.0f;
		image->setUV(&uv);
		image->setTextureWidth(512);
		image->setTextureHeight(256);
		ICoord2D size = { 512, 256 };
		image->setImageSize(&size);
		TheMappedImageCollection->addImage(image);
		return image;
	}

	/// A chooser's title with its position, "Control bar   2/4", so the number of choices can be seen (H5: a slider at its
	/// first step looks the same as a disabled one).
	UnicodeString countedTitle(const UnicodeString &title, Int step, Int count)
	{
		if (count < 2) {
			return title;
		}
		UnicodeString text;
		text.format(L"%ls   %d/%d", title.str(), step + 1, count);
		return text;
	}

	void showPackRow(Int row, Bool moveSlider = TRUE)
	{
		PackRowState &st = s.packRows[row];
		const Packs::Kind kind = kPackRows[row].kind;
		if (st.label != nullptr) {
			GadgetStaticTextSetText(st.label, countedTitle(Packs::kindTitle(kind), st.step, st.choiceCount));
		}
		if (st.slider != nullptr && moveSlider)
		{
			TheWindowManager->winSendSystemMsg(st.slider, GSM_SET_MIN_MAX, 0, st.choiceCount > 1 ? st.choiceCount - 1 : 1);
			GadgetSliderSetPosition(st.slider, st.step);
			st.slider->winEnable(st.choiceCount > 1);
		}
		if (st.value != nullptr) {
			GadgetStaticTextSetText(st.value, Packs::displayName(kind, selectedPack(row)));
		}
		if (st.preview != nullptr)
		{
			st.image = previewImageOf(selectedPack(row));
			// TheSuperHackers @bugfix (H6) The base data has no preview: no frame and no "No picture" for it.
			const Bool base = selectedPack(row) == nullptr;
			st.preview->winHide(base);
			if (st.previewText != nullptr) {
				st.previewText->winHide(st.image != nullptr || base);
			}
		}
	}

	/// The preview frame: the picture stretched over the window, and a thin border.
	void drawPackPreview(GameWindow *window, WinInstanceData *instData)
	{
		(void)instData;
		Int x = 0, y = 0, w = 0, h = 0;
		window->winGetScreenPosition(&x, &y);
		window->winGetSize(&w, &h);
		const Int row = (Int)(intptr_t)window->winGetUserData();
		const Image *image = (row >= 0 && row < PACK_ROW_COUNT) ? s.packRows[row].image : nullptr;
		if (image != nullptr) {
			TheDisplay->drawImage(image, x, y, x + w, y + h);
		} else {
			TheDisplay->drawFillRect(x, y, w, h, GameMakeColor(0, 0, 0, 128));
		}
		TheDisplay->drawOpenRect(x, y, w, h, 1.0f, GameMakeColor(255, 255, 255, 96));
	}

	void placePackRow(Flow &flow, Int row, Int labelH, Int sliderH)
	{
		const PackRowDef &def = kPackRows[row];
		PackRowState &st = s.packRows[row];
		const Int previewH = def.preview ? flow.colW / 2 : 0;   // preview pictures are 2:1 (512 x 256)
		AsciiString name("OptionsMenu.wnd:Sections.");
		name.concat(Packs::kindKey(def.kind));
		const Int id = windowId(name.str());
		// the widest name of any choice of this kind (the hotkey row's list changes with the language)
		Int valueTextW = textSize(labelFont(), Packs::displayName(def.kind, nullptr), 0).x;
		for (Int i = 0; i < Packs::count(def.kind); ++i) {
			valueTextW = max(valueTextW, textSize(labelFont(), Packs::displayName(def.kind, Packs::get(def.kind, i)), 0).x);
		}
		ICoord2D below = { 0, 0 };
		placeStepRow(flow, countedTitle(Packs::kindTitle(def.kind), st.choiceCount - 1, st.choiceCount), valueTextW, st.choiceCount > 1 ? st.choiceCount - 1 : 1, id, labelH, sliderH,
			def.preview ? flow.gap + previewH : 0, &st.label, &st.value, &st.slider, &below);
		if (def.preview)
		{
			const ICoord2D at = below;
			const Int py = below.y + flow.gap;
			WinInstanceData inst;
			inst.init();
			inst.m_style = GWS_USER_WINDOW;
			st.preview = TheWindowManager->winCreate(flow.page, WIN_STATUS_ENABLED, at.x, py, flow.colW, previewH,
				PassMessagesToParentSystem, &inst);
			if (st.preview != nullptr)
			{
				st.preview->winSetUserData((void *)(intptr_t)row);
				st.preview->winSetDrawFunc(drawPackPreview);
			}
			st.previewText = makeLabel(flow.page, s.labelLook, at.x + flow.gap * 2, py + (previewH - labelH) / 2, flow.colW - flow.gap * 4, labelH,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackNoPreview", localizedFallback(L"No picture", L"\x041D\x0435\x0442 \x0438\x0437\x043E\x0431\x0440\x0430\x0436\x0435\x043D\x0438\x044F")), FALSE);
		}
		showPackRow(row);
	}

	UnicodeString deviceName(Int index)
	{
		if (index <= 0 || index >= (Int)s_devices.size()) {
			return TheGameText->FETCH_OR_SUBSTITUTE("GUI:AudioDeviceDefault", localizedFallback(L"System default", L"\x0421\x0438\x0441\x0442\x0435\x043C\x043D\x043E\x0435 \x043F\x043E \x0443\x043C\x043E\x043B\x0447\x0430\x043D\x0438\x044E"));
		}
		WideChar buffer[256];
		const int n = MultiByteToWideChar(CP_UTF8, 0, s_devices[index].str(), -1, buffer, ARRAY_SIZE(buffer));
		if (n <= 0) {
			UnicodeString plain;
			plain.translate(s_devices[index]);
			return plain;
		}
		return UnicodeString(buffer);
	}

	/// The sound output device row: the devices the audio device lists, the system default first. Takes effect at the
	/// next start (the restart box).
	/// The title of the sound output device row.
	UnicodeString deviceTitle()
	{
		return TheGameText->FETCH_OR_SUBSTITUTE("GUI:AudioOutputDevice", localizedFallback(L"Sound output device", L"\x0423\x0441\x0442\x0440\x043E\x0439\x0441\x0442\x0432\x043E \x0432\x044B\x0432\x043E\x0434\x0430 \x0437\x0432\x0443\x043A\x0430"));
	}

	/// The device list: the system default first, then what the audio device lists.
	void loadDevices()
	{
		s_devices.clear();
		s_devices.push_back(AsciiString::TheEmptyString);
		std::vector<AsciiString> names;
		AudioOutputDevices::list(names);
		s_devices.insert(s_devices.end(), names.begin(), names.end());
	}

	void placeDeviceRow(Flow &flow, Int labelH, Int sliderH)
	{
		if (s_devices.empty()) {
			loadDevices();
		}
		const AsciiString chosen = s.pref->getAudioOutputDevice();
		s.deviceStep = 0;
		for (size_t i = 1; i < s_devices.size(); ++i) {
			if (s_devices[i] == chosen) {
				s.deviceStep = (Int)i;
			}
		}
		s.deviceInitial = s.deviceStep;
		Int valueTextW = 0;
		for (size_t i = 0; i < s_devices.size(); ++i) {
			valueTextW = max(valueTextW, textSize(labelFont(), deviceName((Int)i), 0).x);
		}
		const Int count = (Int)s_devices.size();
		placeStepRow(flow, countedTitle(deviceTitle(), count - 1, count),
			valueTextW, count > 1 ? count - 1 : 1, windowId("OptionsMenu.wnd:Sections.AudioOutputDevice"), labelH, sliderH, 0,
			&s.deviceLabel, &s.deviceValue, &s.deviceSlider, nullptr);
		if (s.deviceLabel != nullptr) {
			GadgetStaticTextSetText(s.deviceLabel, countedTitle(deviceTitle(), s.deviceStep, count));
		}
		if (s.deviceSlider != nullptr)
		{
			GadgetSliderSetPosition(s.deviceSlider, s.deviceStep);
			s.deviceSlider->winEnable(count > 1);
		}
		if (s.deviceValue != nullptr) {
			GadgetStaticTextSetText(s.deviceValue, deviceName(s.deviceStep));
		}
	}

	void onRestartNow()
	{
		Packs::requestRestart();
	}

	/// After a save that changed a pack: the box that offers the restart (in a match: a note only).
	void offerRestart()
	{
		const UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartTitle",
			localizedFallback(L"Restart to apply", L"\x041D\x0443\x0436\x0435\x043D \x043F\x0435\x0440\x0435\x0437\x0430\x043F\x0443\x0441\x043A"));
		const Bool inMatch = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
		if (inMatch)
		{
			MessageBoxOk(title, TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartLater",
				localizedFallback(L"These changes take effect the next time the game starts.",
					L"\x042D\x0442\x0438 \x0438\x0437\x043C\x0435\x043D\x0435\x043D\x0438\x044F \x0432\x0441\x0442\x0443\x043F\x044F\x0442 \x0432 \x0441\x0438\x043B\x0443 \x043F\x0440\x0438 \x0441\x043B\x0435\x0434\x0443\x044E\x0449\x0435\x043C \x0437\x0430\x043F\x0443\x0441\x043A\x0435 \x0438\x0433\x0440\x044B.")),
				nullptr);
			return;
		}
		GameWindow *box = MessageBoxYesNo(title, TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartBody",
			localizedFallback(L"These changes take effect after a restart of the game.",
				L"\x042D\x0442\x0438 \x0438\x0437\x043C\x0435\x043D\x0435\x043D\x0438\x044F \x0432\x0441\x0442\x0443\x043F\x044F\x0442 \x0432 \x0441\x0438\x043B\x0443 \x043F\x043E\x0441\x043B\x0435 \x043F\x0435\x0440\x0435\x0437\x0430\x043F\x0443\x0441\x043A\x0430 \x0438\x0433\x0440\x044B.")),
			onRestartNow, nullptr);
		if (box == nullptr) {
			return;
		}
		// the buttons say what they do
		GameWindow *yes = TheWindowManager->winGetWindowFromId(box, windowId("MessageBox.wnd:ButtonYes"));
		GameWindow *no = TheWindowManager->winGetWindowFromId(box, windowId("MessageBox.wnd:ButtonNo"));
		if (yes != nullptr)
		{
			// The text must fit the box's button: the long text when it does, else the short one ("Restart").
			UnicodeString text = TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartNow",
				localizedFallback(L"Restart now", L"\x041F\x0435\x0440\x0435\x0437\x0430\x043F\x0443\x0441\x0442\x0438\x0442\x044C"));
			if (textSize(yes->winGetFont(), text, 0).x > sizeOf(yes).x - 12)
			{
				text = TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartShort",
					localizedFallback(L"Restart", L"\x041F\x0435\x0440\x0435\x0437\x0430\x043F\x0443\x0441\x043A"));
			}
			GadgetButtonSetText(yes, text);
		}
		if (no != nullptr) {
			GadgetButtonSetText(no, TheGameText->FETCH_OR_SUBSTITUTE("GUI:PackRestartLater2",
				localizedFallback(L"Later", L"\x041F\x043E\x0437\x0436\x0435")));
		}
	}
}

//-------------------------------------------------------------------------------------------------
Bool OptionsSections::isActive()
{
	return s.active;
}

void OptionsSections::init(OptionPreferences *pref)
{
	memset(&s, 0, sizeof(s));
	s.pref = pref;
	// The visible frame of the screen: OptionsMenuParentOld holds the buttons and the groups (OptionsMenuParent holds it).
	s.menuParent = findWindow("OptionsMenuParentOld");
	if (s.menuParent == nullptr) {
		s.menuParent = findWindow("OptionsMenuParent");
	}
	GameWindow *defaults = findWindow("ButtonDefaults");
	if (s.menuParent == nullptr || defaults == nullptr || pref == nullptr) {
		return;
	}

	// the looks to copy
	s.buttonLook = defaults;
	s.frameLook = findWindow("AudioParent");
	s.labelLook = findWindow("MusicVolumeLabel");
	s.checkLook = findWindow("CheckAlternateMouse");
	s.sliderLook = findWindow("SliderMusicVolume");
	s.detailCombo = findWindow("ComboBoxDetail");
	if (s.frameLook != nullptr)
	{
		// the group title: the topmost static text of the sound group
		Int bestY = 0;
		for (GameWindow *child = s.frameLook->winGetChild(); child != nullptr; child = child->winGetNext())
		{
			if ((child->winGetStyle() & GWS_STATIC_TEXT) != 0 && child != s.labelLook)
			{
				const ICoord2D p = posOf(child);
				if (s.titleLook == nullptr || p.y < bestY) {
					s.titleLook = child;
					bestY = p.y;
				}
			}
		}
	}
	{
		const WinInstanceData *look = s.buttonLook->winGetInstanceData();
		for (Int d = 0; d < MAX_DRAW_DATA; ++d) {
			s.buttonEnabled[d] = look->m_enabledDrawData[d];
			s.buttonHilite[d] = look->m_hiliteDrawData[d];
		}
	}

	// the area of the old groups, in the parent's coordinates
	Int left = 0, top = 0, right = 0, bottom = 0;
	Bool any = FALSE;
	for (Int g = 0; g < 4; ++g)
	{
		GameWindow *group = findWindow(kOldGroups[g]);
		if (group == nullptr || group->winGetParent() != s.menuParent) {
			continue;
		}
		const ICoord2D p = posOf(group), z = sizeOf(group);
		if (!any) { left = p.x; top = p.y; right = p.x + z.x; bottom = p.y + z.y; any = TRUE; }
		left = min(left, p.x); top = min(top, p.y); right = max(right, p.x + z.x); bottom = max(bottom, p.y + z.y);
	}
	const ICoord2D defaultsPos = posOf(defaults);
	const ICoord2D defaultsSize = sizeOf(defaults);
	if (!any || right - left < 200 || bottom - top < 150) {
		return;   // an unknown layout: keep the old screen
	}
	bottom = min(bottom, defaultsPos.y - 4);

	const Int width = right - left;
	const Int height = bottom - top;
	const Int gap = max(3, width / 150);
	const Int columnW = width * 26 / 100;
	const Int buttonH = min(defaultsSize.y, (height - (SEC_COUNT - 1) * gap) / SEC_COUNT);
	const Int pageX = left + columnW + gap * 2;
	const Int pageW = right - pageX;

	// the section buttons, in the menu buttons' look with the labels' font
	GameFont *buttonFont = s.labelLook != nullptr ? s.labelLook->winGetFont() : nullptr;
	for (Int i = 0; i < SEC_COUNT; ++i)
	{
		WinInstanceData inst;
		copyLook(inst, s.buttonLook, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);
		if (buttonFont != nullptr) {
			inst.m_font = buttonFont;
		}
		AsciiString name;
		name.format("OptionsMenu.wnd:Sections.Button%d", i);
		inst.m_id = windowId(name.str());
		GameWindow *button = TheWindowManager->gogoGadgetPushButton(s.menuParent, WIN_STATUS_ENABLED | imageStatusOf(s.buttonLook),
			left, top + i * (buttonH + gap), columnW, buttonH, &inst, inst.m_font, FALSE);
		if (button != nullptr)
		{
			button->winSetWindowId(inst.m_id);
			GadgetButtonSetText(button, textOf(kSectionNames[i]));
		}
		s.buttons[i] = button;
	}

	// the pages, framed like the old groups (a section that needs more gets further pages while it is filled)
	const Int titleH = s.titleLook != nullptr ? sizeOf(s.titleLook).y : (buttonH * 2 / 3);
	s.pageX = pageX;
	s.pageY = top;
	s.pageW = pageW;
	s.pageH = height;
	s.gap = gap;
	s.titleH = titleH;
	for (Int i = 0; i < SEC_COUNT; ++i) {
		s.pages[i] = makePage(i);
	}

	// the existing controls, found before the old groups go
	for (Int i = 0; i < EXISTING_COUNT; ++i)
	{
		s.existing[i] = findWindow(kExisting[i].control);
		s.existingLabels[i] = kExisting[i].labelWindow != nullptr ? findWindow(kExisting[i].labelWindow) : nullptr;
		if (s.existing[i] != nullptr && s.existingLabels[i] == nullptr && kExisting[i].labelText != nullptr && s.labelLook != nullptr)
		{
			const Int h = sizeOf(s.labelLook).y;
			s.existingLabels[i] = makeLabel(s.menuParent, s.labelLook, 0, 0, pageW / 2 - gap * 3, h,
				TheGameText->fetch(kExisting[i].labelText), FALSE);
		}
	}

	// fill the pages
	const Int labelH = s.labelLook != nullptr ? sizeOf(s.labelLook).y : 20;
	const Int checkH = s.checkLook != nullptr ? sizeOf(s.checkLook).y : 18;
	const Int sliderH = s.sliderLook != nullptr ? sizeOf(s.sliderLook).y : 16;
	const Int contentTop = gap * 2 + titleH + gap;
	const Int colW = (pageW - gap * 8) / 2;

	// the pack rows start at the saved choice (it may wait for a restart), else at what this run uses
	for (Int r = 0; r < PACK_ROW_COUNT; ++r)
	{
		const Packs::Kind kind = kPackRows[r].kind;
		const AsciiString saved = Packs::chosenName(kind, *pref);
		const Packs::Pack *keep = saved.isEmpty() ? nullptr : Packs::find(kind, saved);
		if (saved.isNotEmpty() && keep == nullptr) {
			keep = Packs::active(kind);
		}
		buildPackChoices(r, keep);
		s.packRows[r].initial = selectedPack(r);
	}

	// "Show Props" has no Russian text in the Russian string file. TheSuperHackers @bugfix (H2) Set before the layout, so
	// the check box text is measured and wrapped in its column.
	if (GameWindow *props = findWindow("CheckShowProps"))
	{
		WinInstanceData *inst = props->winGetInstanceData();
		if (isRussianText() && inst != nullptr && inst->getText().compare(L"Show Props") == 0)
		{
			GadgetCheckBoxSetText(props, TheGameText->FETCH_OR_SUBSTITUTE("GUI:ShowPropsFallback",
				L"\x0414\x0435\x043A\x043E\x0440\x0430\x0442\x0438\x0432\x043D\x044B\x0435 \x043E\x0431\x044A\x0435\x043A\x0442\x044B"));
		}
	}

	for (Int sec = 0; sec < SEC_COUNT; ++sec)
	{
		if (s.pages[sec] == nullptr) {
			continue;
		}
		Flow flow;
		flow.begin(sec, s.pages[sec], gap * 3, contentTop, colW, gap, height - gap * 2);
		// one value column for the section: the widest value of its step rows
		for (Int i = 0; i < SETTING_COUNT; ++i)
		{
			if (kSettings[i].section != sec || kSettings[i].kind != KIND_STEPS) {
				continue;
			}
			for (Int c = 0; c < kSettings[i].choiceCount; ++c) {
				flow.valueW = max(flow.valueW, textSize(labelFont(), choiceText(kSettings[i], c), 0).x);
			}
		}
		if (sec == SEC_SOUND)
		{
			loadDevices();
			for (size_t d = 0; d < s_devices.size(); ++d) {
				flow.valueW = max(flow.valueW, textSize(labelFont(), deviceName((Int)d), 0).x);
			}
		}
		for (Int r = 0; r < PACK_ROW_COUNT; ++r)
		{
			if (kPackRows[r].section != sec) {
				continue;
			}
			const Packs::Kind kind = kPackRows[r].kind;
			flow.valueW = max(flow.valueW, textSize(labelFont(), Packs::displayName(kind, nullptr), 0).x);
			for (Int i = 0; i < Packs::count(kind); ++i) {
				flow.valueW = max(flow.valueW, textSize(labelFont(), Packs::displayName(kind, Packs::get(kind, i)), 0).x);
			}
		}
		for (Int i = 0; i < EXISTING_COUNT; ++i) {
			if (kExisting[i].section == sec) {
				placeExisting(flow, i);
			}
		}
		for (Int i = 0; i < SETTING_COUNT; ++i) {
			if (kSettings[i].section == sec) {
				placeSetting(flow, i, labelH, checkH, sliderH);
			}
		}
		for (Int r = 0; r < PACK_ROW_COUNT; ++r) {
			if (kPackRows[r].section == sec) {
				placePackRow(flow, r, labelH, sliderH);
			}
		}
		if (sec == SEC_SOUND) {
			placeDeviceRow(flow, labelH, sliderH);
		}
		if (flow.overflow) {
			DEBUG_LOG(("OptionsSections: an item of section %d is taller than a column of the page.", sec));
		}
	}

	// An open drop-down list is drawn with its combo box: the combo boxes go on top of their pages, so their lists are
	// not covered by the controls placed after them.
	// The lists open downwards, so a combo box higher on a page must lie over the ones below it: the last brought to
	// the top is drawn last, so they go from the bottom of the table up.
	for (Int i = EXISTING_COUNT - 1; i >= 0; --i)
	{
		if (s.existing[i] != nullptr && (s.existing[i]->winGetStyle() & GWS_COMBO_BOX) != 0) {
			s.existing[i]->winBringToTop();
		}
	}

	// TheSuperHackers @tweak Small drop-down lists show all their entries without a scroll bar (H7: the detail list no
	// longer ends in the middle of the title under it).
	for (Int i = 0; i < EXISTING_COUNT; ++i)
	{
		if (s.existing[i] != nullptr && (s.existing[i]->winGetStyle() & GWS_COMBO_BOX) != 0)
		{
			const ComboBoxData *data = (const ComboBoxData *)s.existing[i]->winGetUserData();
			if (data != nullptr && data->maxDisplay < 8) {
				GadgetComboBoxSetMaxDisplay(s.existing[i], 8);
			}
		}
	}

	// the old groups and what is left on them; the old "unlock fps" box (now the frame rate limit) and the keyboard button
	for (Int g = 0; g < (Int)ARRAY_SIZE(kOldGroups); ++g)
	{
		GameWindow *group = findWindow(kOldGroups[g]);
		if (group != nullptr) {
			group->winHide(TRUE);
		}
	}
	if (GameWindow *unlock = findWindow("CheckUnlockFPS")) {
		unlock->winHide(TRUE);
	}
	// whatever else of the old screen lies in the area (group titles, separator lines)
	{
		GameWindow *keep[4] = { defaults, findWindow("ButtonAccept"), findWindow("ButtonBack"), findWindow("LabelVersion") };
		for (GameWindow *child = s.menuParent->winGetChild(); child != nullptr; child = child->winGetNext())
		{
			Bool ours = FALSE;
			for (Int i = 0; i < SEC_COUNT; ++i) {
				ours = ours || child == s.buttons[i];
				for (Int k = 0; k < s.subPageCount[i]; ++k) {
					ours = ours || child == s.subPages[i][k];
				}
			}
			for (Int i = 0; i < 4; ++i) {
				ours = ours || child == keep[i];
			}
			if (ours) {
				continue;
			}
			const ICoord2D p = posOf(child), z = sizeOf(child);
			if (p.x < right && p.x + z.x > left && p.y < bottom && p.y + z.y > top) {
				child->winHide(TRUE);
			}
		}
	}

	s.active = TRUE;
	updateCustomDetail();
	showSection(SEC_DISPLAY);
}

void OptionsSections::shutdown()
{
	memset(&s, 0, sizeof(s));
	s_devices.clear();
}

Bool OptionsSections::onSystem(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
{
	if (!s.active) {
		return FALSE;
	}
	GameWindow *control = (GameWindow *)mData1;
	if (control == nullptr) {
		return FALSE;
	}
	switch (msg)
	{
		case GBM_SELECTED:
		{
			for (Int i = 0; i < SEC_COUNT; ++i)
			{
				if (control == s.buttons[i]) {
					showSection((Section)i);
					return TRUE;
				}
				for (Int k = 0; k < s.subPageCount[i]; ++k)
				{
					if (control == s.pagers[i][k])
					{
						s.currentSub[i] = (s.currentSub[i] + 1) % s.subPageCount[i];
						showSection((Section)i);
						return TRUE;
					}
				}
			}
			for (Int i = 0; i < SETTING_COUNT; ++i) {
				if (control == s.settings[i].control) {
					return TRUE;   // a check box: read on save
				}
			}
			return FALSE;
		}
		case GSM_SLIDER_TRACK:
		{
			for (Int i = 0; i < SETTING_COUNT; ++i)
			{
				if (control == s.settings[i].control && kSettings[i].kind == KIND_STEPS)
				{
					Int step = (Int)mData2;
					step = step < 0 ? 0 : (step >= kSettings[i].choiceCount ? kSettings[i].choiceCount - 1 : step);
					s.settings[i].step = step;
					s.settings[i].touched = TRUE;
					if (s.settings[i].value != nullptr) {
						GadgetStaticTextSetText(s.settings[i].value, choiceText(kSettings[i], step));
					}
					return TRUE;
				}
			}
			if (control == s.deviceSlider && s.deviceSlider != nullptr)
			{
				const Int count = (Int)s_devices.size();
				Int step = (Int)mData2;
				step = step < 0 ? 0 : (step >= count ? count - 1 : step);
				s.deviceStep = step;
				if (s.deviceValue != nullptr) {
					GadgetStaticTextSetText(s.deviceValue, deviceName(step));
				}
				if (s.deviceLabel != nullptr) {
					GadgetStaticTextSetText(s.deviceLabel, countedTitle(deviceTitle(), step, count));
				}
				return TRUE;
			}
			for (Int r = 0; r < PACK_ROW_COUNT; ++r)
			{
				PackRowState &st = s.packRows[r];
				if (st.slider == nullptr || control != st.slider) {
					continue;
				}
				Int step = (Int)mData2;
				step = step < 0 ? 0 : (step >= st.choiceCount ? st.choiceCount - 1 : step);
				if (step != st.step)
				{
					st.step = step;
					showPackRow(r, FALSE);   // the slider is where the player put it
					if (r == PACK_ROW_TEXT)
					{
						// another interface language: the hotkey sets that fit it
						for (Int h = 0; h < PACK_ROW_COUNT; ++h)
						{
							if (kPackRows[h].kind == Packs::KIND_HOTKEYS)
							{
								buildPackChoices(h, selectedPack(h));
								showPackRow(h);
							}
						}
					}
				}
				return TRUE;
			}
			return FALSE;
		}
		case GCM_SELECTED:
		{
			if (control == s.detailCombo) {
				updateCustomDetail();
			}
			return FALSE;   // the menu handles the detail level too
		}
	}
	return FALSE;
}

void OptionsSections::save(OptionPreferences *pref)
{
	if (!s.active || pref == nullptr) {
		return;
	}
	for (Int i = 0; i < SETTING_COUNT; ++i)
	{
		const Setting &set = kSettings[i];
		const SettingState &st = s.settings[i];
		if (st.control == nullptr) {
			continue;
		}
		const AsciiString before = set.current(*pref);
		if (set.kind == KIND_CHECK) {
			(*pref)[set.key] = GadgetCheckBoxIsChecked(st.control) ? "yes" : "no";
		} else if (!st.touched) {
			continue;   // a hand-written value between two choices stays as it is (the slider shows the nearest)
		} else if (set.choices[st.step].value[0] == '\0') {
			pref->erase(set.key);
		} else {
			(*pref)[set.key] = set.choices[st.step].value;
		}
		if (needsRestart(set.key) && stricmp(before.str(), set.current(*pref).str()) != 0) {
			s.restartNeeded = TRUE;
		}
	}

	// the sound output device (AudioOutputDevice), opened at the start
	if (s.deviceSlider != nullptr && s.deviceStep != s.deviceInitial && s.deviceStep >= 0 && s.deviceStep < (Int)s_devices.size())
	{
		if (s.deviceStep == 0) {
			pref->erase("AudioOutputDevice");
		} else {
			(*pref)["AudioOutputDevice"] = s_devices[s.deviceStep];
		}
		s.deviceInitial = s.deviceStep;
		s.restartNeeded = TRUE;
	}

	// The pack choices: written when changed here, so a missing key keeps following Packs\<Kind>\default.ini.
	// They take effect at the next start; the box offers the restart.
	Bool packChanged = FALSE;
	for (Int r = 0; r < PACK_ROW_COUNT; ++r)
	{
		const PackRowState &st = s.packRows[r];
		if (st.slider == nullptr) {
			continue;
		}
		const Packs::Pack *pack = selectedPack(r);
		if (pack == st.initial) {
			continue;
		}
		(*pref)[Packs::kindKey(kPackRows[r].kind)] = (pack != nullptr) ? pack->name : AsciiString(Packs::BaseValue);
		s.packRows[r].initial = pack;
		packChanged = TRUE;
	}

	// The keys the menu's save does not apply.
	TheWritableGlobalData->m_renderFpsLimit = pref->getRenderFpsLimit();
	TheWritableGlobalData->applyRenderFpsLimitOption();
	if (TheFramePacer != nullptr) {
		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->getDefaultRenderFpsLimit());
	}
	TheWritableGlobalData->m_snowStyle = pref->getSnowStyle();
	pref->applyCameraOverrides(TRUE);   // "Standard" (no key) goes back to the GameData values
	TheWritableGlobalData->m_counterStyle = pref->getCounterStyle();
	TheWritableGlobalData->m_screenshotFormat = pref->getScreenshotFormat();

	if (packChanged || s.restartNeeded)
	{
		// the box comes after the menu has written Options.ini (it does so right after this call)
		s.restartNeeded = FALSE;
		offerRestart();
	}
}

void OptionsSections::setDefaults()
{
	if (!s.active) {
		return;
	}
	for (Int i = 0; i < SETTING_COUNT; ++i)
	{
		const Setting &set = kSettings[i];
		if (set.kind == KIND_CHECK) {
			setChecked(i, stricmp(set.defaultValue, "yes") == 0);
		} else {
			s.settings[i].step = defaultStep(set);
			s.settings[i].touched = TRUE;
			showStep(i);
		}
	}
	updateCustomDetail();
}
