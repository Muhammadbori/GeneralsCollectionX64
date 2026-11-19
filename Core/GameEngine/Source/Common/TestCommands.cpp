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

// TheSuperHackers @feature O96 -testCommands <file> (tests, *uitest*.exe only). See TestCommands.h.
//
// File format: one action per line, "<logic frame> <action> [arguments]"; '#' starts a comment. Lines run in file order, each
// on the first client update at which the logic frame has reached its stamp (the frame it really ran at is logged).
// Coordinates are world units (a map cell is 10), or h / e / c with an optional +n or -n: the local player's start
// position (Player_1_Start), the first computer player's (Player_2_Start) or the map centre, e.g. "h+200 h-50". <player> is "local" or an index in the player list (the log lists them
// at the start). An object reference <obj> is an object id, "gN" (group N: every live object of it) or "gN.K" (its K-th
// object, from 1). Where one object is needed, "gN" means the first live object of group N. Groups are numbered 1, 2, ...
// in the order "spawn" and "pick" lines create them.
//
//   spawn <ThingTemplate> <player> <x> <y> [count]   create complete objects at full health (a grid, 25 apart); new group
//   pick <ThingTemplate> <player> [built|unbuilt]    new group: that player's live objects of that template; unbuilt: only
//                                                    scaffolds (under construction), built: only finished ones
//   money <player> <amount>                          give a player money
//   select <obj> [<obj> ...]                         replace the selection (InGameUI + MSG_CREATE_SELECTED_GROUP)
//   clear                                            clear the selection (MSG_DESTROY_SELECTED_GROUP)
//   move <x> <y>                                     the selection: MSG_DO_MOVETO (a right click on the ground)
//   forceattack <x> <y>                              the selection: MSG_DO_FORCE_ATTACK_GROUND (Ctrl + click)
//   enter <obj>                                      the selection: MSG_ENTER into that object (tunnel, transport, ...)
//   evacuate <obj>                                   select it, MSG_EVACUATE (its Evacuate button)
//   build <dozer> <BuildingTemplate> <x> <y> [angle] select the dozer, MSG_DOZER_CONSTRUCT (its build button + placement)
//   queue <building> <UnitTemplate|UpgradeName>      select the building, MSG_QUEUE_UNIT_CREATE / MSG_QUEUE_UPGRADE (a button)
//   sell <building>                                  select it, MSG_SELL (its Sell button)
//   specialpower <building> <x> <y> [SpecialPower]   MSG_DO_SPECIAL_POWER_AT_LOCATION from that building with the options of
//                                                    its command button (as the button or the power shortcut does)
//   beacon <x> <y>                                   MSG_PLACE_BEACON
//   log <text>                                       write the text
//   dump [all]                                       per object: id, template, owner, position, health, flags, production
//                                                    queue, contained; the local player's and the groups' objects, or all
//   count <ThingTemplate>                            the number of live objects of a template (e.g. a missile in flight)
//   sp <building> <SpecialPower>                     that special power's ready frame on the building
//   spready <building> <SpecialPower>                set it ready now (scene setup, so a test needs no 5-minute wait)
//   quit                                             end the program
//
// With -skirmish <map>, -testCommands makes the local player a real player in slot 0 (start position 1, side from
// -testCommandsSide, default FactionGLA) instead of the observer; the -skirmishAI computer players take the next positions.
// The orders are posted to TheMessageStream as the control bar and CommandXlat post them for a human player, so they pass the
// same translators, command list and GameLogic dispatch checks. Only spawn, pick, money and spready set the scene directly (between
// logic frames; a test run is a local skirmish, so nothing else has to agree).

#include "PreRTS.h"

#include "Common/TestCommands.h"

#include "Common/BuildAssistant.h"
#include "Common/CommandLine.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ProductionPrerequisite.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/CreateModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

namespace
{
	struct Line
	{
		UnsignedInt frame;
		Int number;                       // line number in the file
		std::vector<std::string> words;   // words[0] is the action
	};

	std::string s_path;
	std::string s_side = "FactionGLA";
	std::vector<Line> s_lines;
	size_t s_next = 0;
	Bool s_loaded = FALSE;
	Bool s_started = FALSE;
	FILE *s_log = nullptr;
	std::vector< std::vector<ObjectID> > s_groups;
	// O99: "spawn" lines wait for the next logic update (TestCommands::logicUpdate); the lines after them wait too.
	std::vector<Line> s_deferred;
	Bool s_hold = FALSE;

	Real coordinate(const std::string &word, Bool isX);

	void tcLog(const char *format, ...)
	{
		if (s_log == nullptr) {
			return;
		}
		const UnsignedInt frame = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
		fprintf(s_log, "[%u] ", frame);
		va_list args;
		va_start(args, format);
		vfprintf(s_log, format, args);
		va_end(args);
		fputc('\n', s_log);
		fflush(s_log);
	}

	void openLog()
	{
		if (s_log != nullptr) {
			return;
		}
		AsciiString path = TheGlobalData->getPath_UserData();
		path.concat("TestCommands.log");
		s_log = fopen(path.str(), "w");
	}

	void loadScript()
	{
		s_loaded = TRUE;
		openLog();
		FILE *file = fopen(s_path.c_str(), "r");
		if (file == nullptr) {
			tcLog("ERROR cannot open %s", s_path.c_str());
			return;
		}
		char buffer[1024];
		Int number = 0;
		while (fgets(buffer, sizeof(buffer), file) != nullptr) {
			++number;
			char *hash = strchr(buffer, '#');
			// "log" keeps its whole text, '#' included
			std::vector<std::string> words;
			Line line;
			line.number = number;
			char *cursor = buffer;
			while (*cursor != '\0') {
				while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
					++cursor;
				}
				if (*cursor == '\0') {
					break;
				}
				if (words.size() == 2 && words[1] == "log") {
					std::string text(cursor);
					while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
						text.pop_back();
					}
					words.push_back(text);
					break;
				}
				if (cursor == hash) {
					break;
				}
				char *start = cursor;
				while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n' && cursor != hash) {
					++cursor;
				}
				words.push_back(std::string(start, cursor - start));
			}
			if (words.empty()) {
				continue;
			}
			if (words.size() < 2) {
				tcLog("ERROR line %d: no action", number);
				continue;
			}
			line.frame = (UnsignedInt)strtoul(words[0].c_str(), nullptr, 10);
			line.words.assign(words.begin() + 1, words.end());
			s_lines.push_back(line);
		}
		fclose(file);
		tcLog("script %s: %d actions", s_path.c_str(), (Int)s_lines.size());
	}

	void logPlayers()
	{
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
			Player *player = ThePlayerList->getNthPlayer(i);
			if (player == nullptr) {
				continue;
			}
			tcLog("player %d side=%s%s money=%u", i, player->getSide().str(),
				player == ThePlayerList->getLocalPlayer() ? " (local)" : "", player->getMoney()->countMoney());
		}
		tcLog("positions h=%.0f,%.0f e=%.0f,%.0f c=%.0f,%.0f", coordinate("h", TRUE), coordinate("h", FALSE),
			coordinate("e", TRUE), coordinate("e", FALSE), coordinate("c", TRUE), coordinate("c", FALSE));
	}

	Player *findPlayer(const std::string &word)
	{
		if (word == "local") {
			return ThePlayerList->getLocalPlayer();
		}
		return ThePlayerList->getNthPlayer(atoi(word.c_str()));
	}

	Object *liveObject(ObjectID id)
	{
		Object *obj = TheGameLogic->findObjectByID(id);
		return (obj != nullptr && !obj->isEffectivelyDead()) ? obj : nullptr;
	}

	/// the objects a reference names (see the top comment); FALSE for a bad reference
	Bool resolve(const std::string &word, std::vector<Object *> &out)
	{
		if (!word.empty() && (word[0] == 'g' || word[0] == 'G')) {
			const Int group = atoi(word.c_str() + 1);
			if (group < 1 || group > (Int)s_groups.size()) {
				return FALSE;
			}
			const size_t dot = word.find('.');
			const std::vector<ObjectID> &ids = s_groups[group - 1];
			if (dot != std::string::npos) {
				const Int k = atoi(word.c_str() + dot + 1);
				if (k < 1 || k > (Int)ids.size()) {
					return FALSE;
				}
				Object *obj = liveObject(ids[k - 1]);
				if (obj != nullptr) {
					out.push_back(obj);
				}
				return obj != nullptr;
			}
			for (size_t i = 0; i < ids.size(); ++i) {
				Object *obj = liveObject(ids[i]);
				if (obj != nullptr) {
					out.push_back(obj);
				}
			}
			return TRUE;
		}
		Object *obj = liveObject((ObjectID)atoi(word.c_str()));
		if (obj != nullptr) {
			out.push_back(obj);
		}
		return obj != nullptr;
	}

	Object *resolveOne(const std::string &word)
	{
		std::vector<Object *> objects;
		if (!resolve(word, objects) || objects.empty()) {
			return nullptr;
		}
		return objects[0];
	}

	/// a coordinate word: a number, or "h", "e" or "c" with an optional +n/-n: the local player's start (Player_1_Start),
	/// the first computer player's start (Player_2_Start) or the map centre, plus n
	Real coordinate(const std::string &word, Bool isX)
	{
		if (word.empty() || (word[0] != 'h' && word[0] != 'e' && word[0] != 'c')) {
			return (Real)atof(word.c_str());
		}
		Coord3D base;
		base.zero();
		if (word[0] == 'c') {
			Region3D extent;
			TheTerrainLogic->getExtent(&extent);
			base.x = (extent.lo.x + extent.hi.x) * 0.5f;
			base.y = (extent.lo.y + extent.hi.y) * 0.5f;
		}
		else {
			Waypoint *start = TheTerrainLogic->getWaypointByName(AsciiString(word[0] == 'h' ? "Player_1_Start" : "Player_2_Start"));
			if (start != nullptr) {
				base = *start->getLocation();
			}
		}
		return (isX ? base.x : base.y) + (word.size() > 1 ? (Real)atof(word.c_str() + 1) : 0.0f);
	}

	Coord3D groundAt(const std::string &x, const std::string &y)
	{
		Coord3D pos;
		pos.x = coordinate(x, TRUE);
		pos.y = coordinate(y, FALSE);
		pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
		return pos;
	}

	const char *templateName(const Object *obj)
	{
		return obj->getTemplate() != nullptr ? obj->getTemplate()->getName().str() : "?";
	}

	/// replaces the selection the way SelectionXlat does: the drawables in InGameUI, then the logic's selected group
	Int selectObjects(const std::vector<Object *> &objects)
	{
		TheInGameUI->deselectAllDrawables();
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
		msg->appendBooleanArgument(TRUE);
		Int count = 0;
		for (size_t i = 0; i < objects.size(); ++i) {
			Object *obj = objects[i];
			if (obj->getDrawable() != nullptr) {
				TheInGameUI->selectDrawable(obj->getDrawable());
			}
			msg->appendObjectIDArgument(obj->getID());
			++count;
		}
		return count;
	}

	Bool selectOne(Object *obj)
	{
		std::vector<Object *> objects(1, obj);
		return selectObjects(objects) == 1;
	}

	void dumpObject(const Object *obj)
	{
		const Coord3D *pos = obj->getPosition();
		const BodyModuleInterface *body = obj->getBodyModule();
		const Player *owner = obj->getControllingPlayer();
		Int queue = -1;
		ProductionUpdateInterface *pu = const_cast<Object *>(obj)->getProductionUpdateInterface();
		if (pu != nullptr) {
			queue = (Int)pu->getProductionCount();
		}
		const ContainModuleInterface *contain = obj->getContain();
		tcLog("  obj %u %s owner=%d pos=%.0f,%.0f,%.0f health=%.0f/%.0f%s%s%s queue=%d contained=%d", (UnsignedInt)obj->getID(),
			templateName(obj), owner != nullptr ? (Int)owner->getPlayerIndex() : -1, pos->x, pos->y, pos->z,
			body != nullptr ? body->getHealth() : 0.0f, body != nullptr ? body->getMaxHealth() : 0.0f,
			obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ? " UNDER_CONSTRUCTION" : "",
			obj->testStatus(OBJECT_STATUS_SOLD) ? " SOLD" : "", obj->isEffectivelyDead() ? " DEAD" : "",
			queue, contain != nullptr ? (Int)contain->getContainCount() : -1);
	}

	Bool inGroups(ObjectID id)
	{
		for (size_t g = 0; g < s_groups.size(); ++g) {
			for (size_t i = 0; i < s_groups[g].size(); ++i) {
				if (s_groups[g][i] == id) {
					return TRUE;
				}
			}
		}
		return FALSE;
	}

	/// the command button of the building's command set that fires this special power (nullptr: none); with no name, the
	/// first special power button that needs a target position
	const CommandButton *findSpecialPowerButton(const Object *obj, const SpecialPowerTemplate *power)
	{
		const CommandSet *set = TheControlBar->findCommandSet(obj->getCommandSetString());
		if (set == nullptr) {
			return nullptr;
		}
		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i) {
			const CommandButton *button = set->getCommandButton(i);
			if (button == nullptr || button->getSpecialPowerTemplate() == nullptr) {
				continue;
			}
			if (power != nullptr ? button->getSpecialPowerTemplate() == power : BitIsSet(button->getOptions(), NEED_TARGET_POS)) {
				return button;
			}
		}
		return nullptr;
	}

	void run(const Line &line)
	{
		const std::vector<std::string> &w = line.words;
		const std::string &action = w[0];
		const size_t argc = w.size() - 1;
		std::string joined;
		for (size_t i = 0; i < w.size(); ++i) {
			if (i != 0) {
				joined += ' ';
			}
			joined += w[i];
		}
		const char *text = joined.c_str();
#define TC_FAIL(why) do { tcLog("line %d (stamp %u) %s: FAILED %s", line.number, line.frame, text, why); return; } while (0)
#define TC_OK(...) do { char tc_r[512]; snprintf(tc_r, sizeof(tc_r), __VA_ARGS__); \
	tcLog("line %d (stamp %u) %s: %s", line.number, line.frame, text, tc_r); return; } while (0)

		if (action == "log") {
			TC_OK("%s", argc >= 1 ? w[1].c_str() : "");
		}
		else if (action == "spawn") {
			if (argc < 4) TC_FAIL("needs <template> <player> <x> <y> [count]");
			// O99: objects are made inside a logic update, like produced or script-made ones. Made from the client update,
			// their drawables took the model state while the shared barrel/bone data could not be computed, and a
			// Scorpion's first shot crashed in W3DModelDraw::handleWeaponFireFX (S46).
			if (!TheGameLogic->isInGameLogicUpdate()) {
				s_deferred.push_back(line);
				s_hold = TRUE;
				return;
			}
			const ThingTemplate *tt = TheThingFactory->findTemplate(AsciiString(w[1].c_str()));
			if (tt == nullptr) TC_FAIL("no such template");
			Player *player = findPlayer(w[2]);
			if (player == nullptr || player->getDefaultTeam() == nullptr) TC_FAIL("no such player");
			Int count = argc >= 5 ? atoi(w[5].c_str()) : 1;
			if (count < 1) {
				count = 1;
			}
			const Coord3D base = groundAt(w[3], w[4]);
			const Int columns = 8;
			std::vector<ObjectID> ids;
			for (Int i = 0; i < count; ++i) {
				Object *obj = TheThingFactory->newObject(tt, player->getDefaultTeam());
				if (obj == nullptr) {
					continue;
				}
				Coord3D pos = base;
				pos.x += (Real)(i % columns) * 25.0f;
				pos.y += (Real)(i / columns) * 25.0f;
				pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
				obj->setOrientation(0.0f);
				obj->setPosition(&pos);
				// O100: an object made complete is "built" for its create modules, as the map's objects are
				// (GameLogic's map load calls onBuildComplete). Without it a Generals Tunnel Network never joined the
				// network, nor did a Scud Storm or supply center start their build-complete work.
				if (!obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
					for (BehaviorModule **m = obj->getBehaviorModules(); *m; ++m) {
						CreateModuleInterface *create = (*m)->getCreate();
						if (create != nullptr) {
							create->onBuildComplete();
						}
					}
				}
				if (obj->isKindOf(KINDOF_STRUCTURE)) {
					TheAI->pathfinder()->addObjectToPathfindMap(obj);
				}
				ids.push_back(obj->getID());
			}
			s_groups.push_back(ids);
			TC_OK("group g%d = %d objects, ids %u..%u", (Int)s_groups.size(), (Int)ids.size(),
				ids.empty() ? 0u : (UnsignedInt)ids.front(), ids.empty() ? 0u : (UnsignedInt)ids.back());
		}
		else if (action == "pick") {
			if (argc < 2) TC_FAIL("needs <template> <player> [built|unbuilt]");
			Player *player = findPlayer(w[2]);
			if (player == nullptr) TC_FAIL("no such player");
			const Int state = argc < 3 ? 0 : w[3] == "built" ? 1 : w[3] == "unbuilt" ? 2 : -1;
			if (state < 0) TC_FAIL("the filter is built or unbuilt");
			std::vector<ObjectID> ids;
			for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject()) {
				const Bool unbuilt = obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
				if (!obj->isEffectivelyDead() && obj->getControllingPlayer() == player && obj->getTemplate() != nullptr
					&& (state == 0 || (state == 2) == unbuilt)
					&& stricmp(obj->getTemplate()->getName().str(), w[1].c_str()) == 0) {
					ids.push_back(obj->getID());
				}
			}
			s_groups.push_back(ids);
			TC_OK("group g%d = %d objects, first id %u", (Int)s_groups.size(), (Int)ids.size(),
				ids.empty() ? 0u : (UnsignedInt)ids.front());
		}
		else if (action == "money") {
			if (argc < 2) TC_FAIL("needs <player> <amount>");
			Player *player = findPlayer(w[1]);
			if (player == nullptr) TC_FAIL("no such player");
			player->getMoney()->deposit((UnsignedInt)atoi(w[2].c_str()), FALSE, FALSE);
			TC_OK("money now %u", player->getMoney()->countMoney());
		}
		else if (action == "select") {
			if (argc < 1) TC_FAIL("needs objects");
			std::vector<Object *> objects;
			for (size_t i = 1; i < w.size(); ++i) {
				if (!resolve(w[i], objects)) {
					tcLog("line %d: reference %s names no live object", line.number, w[i].c_str());
				}
			}
			if (objects.empty()) TC_FAIL("no live objects");
			TC_OK("selected %d objects", selectObjects(objects));
		}
		else if (action == "clear") {
			TheInGameUI->deselectAllDrawables();
			TC_OK("selection cleared");
		}
		else if (action == "move" || action == "forceattack" || action == "beacon") {
			if (argc < 2) TC_FAIL("needs <x> <y>");
			const Coord3D pos = groundAt(w[1], w[2]);
			const GameMessage::Type type = action == "move" ? GameMessage::MSG_DO_MOVETO
				: action == "forceattack" ? GameMessage::MSG_DO_FORCE_ATTACK_GROUND : GameMessage::MSG_PLACE_BEACON;
			GameMessage *msg = TheMessageStream->appendMessage(type);
			msg->appendLocationArgument(pos);
			TC_OK("sent to %.0f,%.0f,%.0f (%d drawables selected)", pos.x, pos.y, pos.z,
				(Int)TheInGameUI->getAllSelectedDrawables()->size());
		}
		else if (action == "enter") {
			if (argc < 1) TC_FAIL("needs <obj>");
			Object *target = resolveOne(w[1]);
			if (target == nullptr) TC_FAIL("no live target");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_ENTER);
			msg->appendObjectIDArgument(INVALID_ID);
			msg->appendObjectIDArgument(target->getID());
			TC_OK("sent, target %u %s", (UnsignedInt)target->getID(), templateName(target));
		}
		else if (action == "evacuate" || action == "sell") {
			if (argc < 1) TC_FAIL("needs <obj>");
			Object *obj = resolveOne(w[1]);
			if (obj == nullptr) TC_FAIL("no live object");
			selectOne(obj);
			TheMessageStream->appendMessage(action == "sell" ? GameMessage::MSG_SELL : GameMessage::MSG_EVACUATE);
			TC_OK("sent for %u %s", (UnsignedInt)obj->getID(), templateName(obj));
		}
		else if (action == "build") {
			if (argc < 4) TC_FAIL("needs <dozer> <template> <x> <y> [angle]");
			Object *dozer = resolveOne(w[1]);
			if (dozer == nullptr) TC_FAIL("no live dozer");
			const ThingTemplate *tt = TheThingFactory->findTemplate(AsciiString(w[2].c_str()));
			if (tt == nullptr) TC_FAIL("no such template");
			const Coord3D pos = groundAt(w[3], w[4]);
			const Real angle = argc >= 5 ? (Real)atof(w[5].c_str()) : 0.0f;
			// O99: the same checks as the build button and the placement (BuildAssistant::buildObjectNow drops the order
			// without a word: S46's Tunnel Network and Generals Barracks never started), so the log says why.
			Player *owner = dozer->getControllingPlayer();
			if (!TheBuildAssistant->isPossibleToMakeUnit(dozer, tt)) {
				std::string why = "the dozer cannot build it now";
				if (owner != nullptr && !owner->canBuild(tt)) {
					why = "prerequisites missing:";
					for (Int i = 0; i < tt->getPrereqCount(); ++i) {
						const ProductionPrerequisite *pre = tt->getNthPrereq(i);
						if (!pre->isSatisfied(owner)) {
							AsciiString names;
							names.translate(pre->getRequiresList(owner));
							why += " [";
							why += names.str();
							why += "]";
						}
					}
				}
				TC_FAIL(why.c_str());
			}
			const LegalBuildCode lbc = TheBuildAssistant->isLocationLegalToBuild(&pos, tt, angle,
				BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH
				| BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED | BuildAssistant::IGNORE_STEALTHED,
				dozer, nullptr);
			if (lbc != LBC_OK) {
				static const char *const kLbc[] = { "ok", "restricted terrain", "not flat enough", "objects in the way",
					"no clear path", "shroud", "too close to supplies", "generic failure" };
				char why[96];
				snprintf(why, sizeof(why), "illegal spot (%s, code %d)",
					(Int)lbc >= 0 && (Int)lbc < (Int)(sizeof(kLbc) / sizeof(kLbc[0])) ? kLbc[lbc] : "?", (Int)lbc);
				TC_FAIL(why);
			}
			selectOne(dozer);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
			msg->appendIntegerArgument(tt->getTemplateID());
			msg->appendLocationArgument(pos);
			msg->appendRealArgument(angle);
			TC_OK("sent, dozer %u at %.0f,%.0f (pick the scaffold later with: pick %s <player>)", (UnsignedInt)dozer->getID(),
				pos.x, pos.y, w[2].c_str());
		}
		else if (action == "queue") {
			if (argc < 2) TC_FAIL("needs <building> <unit or upgrade>");
			Object *building = resolveOne(w[1]);
			if (building == nullptr) TC_FAIL("no live building");
			ProductionUpdateInterface *pu = building->getProductionUpdateInterface();
			const Int before = pu != nullptr ? (Int)pu->getProductionCount() : -1;
			const ThingTemplate *tt = TheThingFactory->findTemplate(AsciiString(w[2].c_str()));
			const UpgradeTemplate *upgrade = tt == nullptr ? TheUpgradeCenter->findUpgrade(AsciiString(w[2].c_str())) : nullptr;
			if (tt == nullptr && upgrade == nullptr) TC_FAIL("no such unit template or upgrade");
			if (tt != nullptr && pu == nullptr) TC_FAIL("the building has no production");
			selectOne(building);
			if (tt != nullptr) {
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
				msg->appendIntegerArgument(tt->getTemplateID());
				msg->appendIntegerArgument(pu->requestUniqueUnitID());
			}
			else {
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
				msg->appendObjectIDArgument(building->getID());
				msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
			}
			TC_OK("sent %s to %u %s, queue length before %d (dump later to see whether the logic took it)",
				tt != nullptr ? "unit" : "upgrade", (UnsignedInt)building->getID(), templateName(building), before);
		}
		else if (action == "specialpower") {
			if (argc < 3) TC_FAIL("needs <building> <x> <y> [special power]");
			Object *building = resolveOne(w[1]);
			if (building == nullptr) TC_FAIL("no live building");
			const SpecialPowerTemplate *power = nullptr;
			if (argc >= 4) {
				power = TheSpecialPowerStore->findSpecialPowerTemplate(AsciiString(w[4].c_str()));
				if (power == nullptr) TC_FAIL("no such special power");
			}
			const CommandButton *button = findSpecialPowerButton(building, power);
			if (button == nullptr) TC_FAIL("the building's command set has no button for it");
			power = button->getSpecialPowerTemplate();
			const Coord3D pos = groundAt(w[2], w[3]);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
			msg->appendIntegerArgument(power->getID());
			msg->appendLocationArgument(pos);
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_NETWORKING)
			msg->appendRealArgument(INVALID_ANGLE);
#endif
			msg->appendObjectIDArgument(INVALID_ID);
			msg->appendIntegerArgument(button->getOptions());
			msg->appendObjectIDArgument(building->getID());
			TC_OK("sent %s from %u at %.0f,%.0f", power->getName().str(), (UnsignedInt)building->getID(), pos.x, pos.y);
		}
		else if (action == "sp" || action == "spready") {
			if (argc < 2) TC_FAIL("needs <building> <special power>");
			Object *building = resolveOne(w[1]);
			if (building == nullptr) TC_FAIL("no live building");
			const SpecialPowerTemplate *power = TheSpecialPowerStore->findSpecialPowerTemplate(AsciiString(w[2].c_str()));
			if (power == nullptr) TC_FAIL("no such special power");
			SpecialPowerModuleInterface *module = building->getSpecialPowerModule(power);
			if (module == nullptr) TC_FAIL("the building has no such special power module");
			if (action == "spready") {
				module->setReadyFrame(TheGameLogic->getFrame());
			}
			TC_OK("ready frame %u, ready %d", module->getReadyFrame(), (Int)module->isReady());
		}
		else if (action == "count") {
			if (argc < 1) TC_FAIL("needs <template>");
			Int count = 0;
			for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject()) {
				if (!obj->isEffectivelyDead() && obj->getTemplate() != nullptr
					&& stricmp(obj->getTemplate()->getName().str(), w[1].c_str()) == 0) {
					++count;
				}
			}
			TC_OK("%d live", count);
		}
		else if (action == "dump") {
			const Bool all = argc >= 1 && w[1] == "all";
			tcLog("line %d (stamp %u) %s:", line.number, line.frame, text);
			const Player *local = ThePlayerList->getLocalPlayer();
			Int count = 0;
			for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject()) {
				if (all || obj->getControllingPlayer() == local || inGroups(obj->getID())) {
					dumpObject(obj);
					++count;
				}
			}
			TC_OK("%d objects", count);
		}
		else if (action == "quit") {
			tcLog("line %d (stamp %u) quit", line.number, line.frame);
			TheGameEngine->setQuitting(TRUE);
			return;
		}
		else {
			TC_FAIL("unknown action");
		}
#undef TC_FAIL
#undef TC_OK
	}
}

void TestCommands::setScript(const char *path)
{
	if (!CommandLine::isTestExe()) {
		return;
	}
	s_path = path != nullptr ? path : "";
}

Bool TestCommands::isActive()
{
	return !s_path.empty();
}

void TestCommands::setSkirmishSide(const char *side)
{
	if (side != nullptr && side[0] != 0) {
		s_side = side;
	}
}

const char *TestCommands::getSkirmishSide()
{
	return s_path.empty() ? nullptr : s_side.c_str();
}

void TestCommands::update()
{
	if (s_path.empty() || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
		|| TheGameLogic->isLoadingMap() || ThePlayerList == nullptr || ThePlayerList->getLocalPlayer() == nullptr) {
		return;
	}
	if (!s_loaded) {
		loadScript();
	}
	if (!s_started) {
		s_started = TRUE;
		logPlayers();
	}
	const UnsignedInt frame = TheGameLogic->getFrame();
	while (!s_hold && s_next < s_lines.size() && s_lines[s_next].frame <= frame) {
		run(s_lines[s_next++]);
	}
	if (!s_hold && s_next == s_lines.size() && s_log != nullptr) {
		tcLog("script finished");
		fclose(s_log);
		s_log = nullptr;
		++s_next;
	}
}

void TestCommands::logicUpdate()
{
	if (!s_hold) {
		return;
	}
	std::vector<Line> lines;
	lines.swap(s_deferred);
	s_hold = FALSE;
	for (size_t i = 0; i < lines.size(); ++i) {
		run(lines[i]);
	}
}
