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

// Packs.h //////////////////////////////////////////////////////////////////////////////////////////
// TheSuperHackers @feature Switchable data packs: interface text, voices, movies, control bar and hotkeys.
//
// A pack is a folder Packs\<Kind>\<Name>\ next to the exe. Its archives are mounted before the archives of the list
// file (first loaded wins), so a pack replaces the files it has and the base data fills the rest. Options.ini chooses
// one pack per kind (TextPack, VoicePack, MoviePack, ControlBarPack, HotkeyPack); "Base" chooses the base data, and a
// missing key takes Default from Packs\<Kind>\default.ini. A pack that is missing or broken is skipped: the base data
// is used and the player is told once. The choice takes effect at the next start.
//
// What a pack folder holds (all optional), searched in this order, first wins:
//   <Game>\Lang<Language>\*.big   only when the interface text is <Language> (English, Russian ...)
//   <Game>\Res<Height>\*.big      the largest Height not above the screen height (else the smallest)
//   <Game>\*.big
//   Lang<Language>\*.big, Res<Height>\*.big, *.big   the same for both games
//   then the Archive lines of pack.ini (paths relative to the exe folder, for archives the folder already has)
// <Game> is ZeroHour or Generals. Movies are loose files: <pack>\<Game>\Data\...\Movies\*.bik, or <pack>\Data\...
// Fonts named by LocalFontFile of Language.ini are taken from <pack>\<Game>\ or <pack>\ when the folder has them.
// pack.ini (UTF-8, Key = Value, ';' or '#' starts a comment):
//   NameEnglish, NameRussian       the names shown in the options
//   Preview                        a .tga picture in the pack folder (default preview.tga), shown under the chooser
//   FullViewport = yes             the 3D view takes the whole screen height (control bars drawn over the view)
//   TextLanguage                   Text pack: its language. Hotkey pack: the text language it fits (empty: any)
//   Language                       Voice and Movies packs: the spoken language (English, Russian ...), for the movies'
//                                  "Auto" choice (O26, row 31)
//   Fallback                       another pack of the same kind, mounted after this one for what this one lacks
//   Archive, ArchiveZeroHour, ArchiveGenerals    archives of the exe folder to mount as part of this pack
//   Exclude, ExcludeZeroHour, ExcludeGenerals    lines of the archive list not to mount while this pack is in use
//   Games                          the games that offer the pack ("ZeroHour", "Generals, ZeroHour"); default: both
//   Require, RequireZeroHour, RequireGenerals    files of the pack folder that must be there (else the pack is broken)
//   Empty = yes                    the pack has no files on purpose. Without it, a pack that has nothing for this game
//                                  (no archive, no Archive or Exclude line, no movie folder) counts as broken.
// Packs\<Kind>\default.ini: Default, ShowBase (yes/no), BaseNameEnglish, BaseNameRussian, and for Text BaseTextLanguage;
// for Voice and Movies BaseLanguage (the spoken language of the base data).
//
// TheSuperHackers @feature O26 (row 31) Movies have one more choice, "Auto" (MoviePack = Auto), the default when
// Options.ini has no MoviePack line: the movie pack whose Language is the Language of the voice pack in use (the base
// voices: BaseLanguage of Packs\Voice\default.ini); the base movies when that language is the base movies' or no pack
// has it. Packs.log says what it became ("MoviePack = Auto -> English").

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include <vector>

class OptionPreferences;

namespace Packs
{
	enum Kind
	{
		KIND_TEXT, KIND_VOICE, KIND_MOVIES, KIND_CONTROLBAR, KIND_HOTKEYS,
		KIND_COUNT
	};

	struct Pack
	{
		AsciiString name;              ///< the folder name
		AsciiString folder;            ///< full path, ends with '\'
		UnicodeString nameEnglish;
		UnicodeString nameRussian;
		AsciiString previewFile;       ///< full path of the preview picture, empty: none
		AsciiString previewTexture;    ///< the bare texture name the preview is known by
		Bool fullViewport;
		AsciiString textLanguage;      ///< lower case, empty: any
		AsciiString language;          ///< Voice, Movies: the spoken language, lower case; empty: unknown
		AsciiString fallback;
		std::vector<AsciiString> archives;   ///< Archive lines of this game, relative to the exe folder
		std::vector<AsciiString> excludes;   ///< Exclude lines of this game, lower case, '\' separators
		std::vector<AsciiString> requiredFiles;   ///< Require lines of this game: files of the pack folder that must be there
		Bool empty;                          ///< "Empty = yes": a pack with no files on purpose (for example the base movies)
	};

	const char *kindKey(Kind kind);       ///< the Options.ini key: "TextPack" ...
	const char *kindFolder(Kind kind);    ///< "Text" ...
	extern const char *const BaseValue;   ///< "Base"
	extern const char *const AutoValue;   ///< "Auto" (movies only)

	/// The movies' "Auto" choice as a pack the options can list (no files of its own): name "Auto", named
	/// "Auto (as voices)" / "Avto (kak ozvuchka)". nullptr for the other kinds.
	const Pack *autoChoice(Kind kind);

	/// Reads the Packs folder (once).
	void scan();
	/// Frees what scan() and the mount made. Call before the memory manager goes.
	void shutdown();

	Int count(Kind kind);
	const Pack *get(Kind kind, Int index);
	const Pack *find(Kind kind, const AsciiString &name);
	Bool baseShown(Kind kind);
	UnicodeString baseName(Kind kind);
	UnicodeString displayName(Kind kind, const Pack *pack);   ///< pack nullptr: the base data
	UnicodeString kindTitle(Kind kind);                      ///< "Interface language", "Voices" ...

	/// The language of the interface text with 'textPack' chosen (nullptr: the base text); empty when unknown.
	AsciiString textLanguageOf(const Pack *textPack);
	/// TRUE when the hotkey pack can go with that text language.
	Bool hotkeysFit(const Pack *hotkeyPack, const AsciiString &textLanguage);

	/// The choice of 'kind' in 'pref' (or the kind's default): a pack name, or empty for the base data.
	AsciiString chosenName(Kind kind, OptionPreferences &pref);
	/// What this run uses: a pack, or nullptr for the base data.
	const Pack *active(Kind kind);

	//--- the archive file system --------------------------------------------------------------------
	struct MountStep
	{
		AsciiString path;   ///< full path of an archive
		Int group;          ///< the archives of one group are mounted all or none
	};
	/// Decides the packs of this run and lists their archives in mount order.
	void prepareMount(std::vector<MountStep> &steps);
	/// FALSE when the group is not mounted after all (its pack fell back to the base data).
	Bool groupWanted(Int group);
	/// A group could not be mounted (a file missing or broken): the pack falls back to the base data.
	void groupFailed(Int group);
	/// TRUE when a line of the archive list must be skipped: excluded by a pack in use, or mounted by a pack already.
	/// 'entry' is the line as written, 'fullPath' the archive it names.
	Bool skipListEntry(const AsciiString &entry, const AsciiString &fullPath);

	/// TheSuperHackers @feature O47 The archives the game would search for a file if 'pack' (nullptr: the base data) were
	/// the chosen pack of 'kind', first one wins, as the mount does: that pack's archives and its fallback's, then the
	/// archive list file without what they (and the packs in use of the other kinds) leave out. Nothing is mounted: the
	/// options' voice preview reads a line from the pack chosen in the list before it is applied.
	void previewArchives(Kind kind, const Pack *pack, std::vector<AsciiString> &out);

	//--- the rest of the game -----------------------------------------------------------------------
	Bool fullViewport();                                     ///< the control bar pack in use wants the full view
	void getMovieRoots(std::vector<AsciiString> &roots);     ///< folders to try first for a movie, each ends with '\'
	AsciiString findFontFile(const AsciiString &fileName);   ///< a font of the packs in use, empty: none
	const char *findPreviewPath(const char *textureName);    ///< the picture of a preview texture name, or nullptr

	/// The splash picture of the chosen text pack (before the engine starts); FALSE when the pack has none.
	Bool findSplash(const char *fileName, char *path, Int pathSize);

	/// Writes Packs.log in the user data folder: the packs of this run, the archives they mounted, the list lines they left
	/// out, the problems, and for some files the archive they are read from ('files' and 'origins' side by side).
	void writeLog(const std::vector<AsciiString> &files, const std::vector<AsciiString> &origins);

	/// Shows the one-time message about packs that could not be used. Call from the shell update.
	void showProblemsOnce();

	/// Ends the game so that it starts again; the new process starts from WinMain after this one has shut down.
	/// O48 (row 33): the quick restart: the same command line plus -nologo (no intro, no logos), -restartWait <this
	/// process> (the new one waits until this one is gone: the instance mutex, the anti-cheat, the files) and
	/// -openOptions <section> (it opens the settings on 'optionsSection' as soon as the main menu is up; -1: not).
	/// A test run (-uiScript) only quits, unless -restartScript <file> names the script of the new process.
	void requestRestart(Int optionsSection = -1);
	/// Call at the very end of WinMain: starts the game again when requestRestart() was called.
	void restartIfRequested();
	/// O48: call first in WinMain: with -restartWait <pid>, waits (at most 20 s) until that process has ended.
	void waitForRestartedParent();
	/// O48: -restartScript <file> was given (a test run may restart; the new process plays that script).
	void setRestartScriptGiven();

	/// O48 (row 33): uses a new choice of 'kind' at once when that is safe; FALSE: it takes effect at the next start.
	/// Only the movies can (their loose files are looked up when a movie starts, Packs::getMovieRoots), and only when
	/// neither the choice in use nor the new one mounts an archive. Every other kind lives in archives mounted at the
	/// start (strings, fonts, sounds, INI data, textures), which the running game holds on to. Packs.log gets a line.
	Bool applyLive(Kind kind, const Pack *pack);
	/// O48: a choice of 'kind' was saved that waits for the next start (the settings show a mark beside its row).
	void markPending(Kind kind);
	Bool isPending(Kind kind);
}
