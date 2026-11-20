/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: MapUtil.h /////////////////////////////////////////////////////////
// Author: Matt Campbell, December 2001
// Description: Map utility/convenience routines
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include "Common/STLTypedefs.h"

class GameWindow;
class AsciiString;
struct Coord3D;
struct FileInfo;
class Image;
class DataChunkInput;
struct DataChunkInfo;
// This matches the windows timestamp.
enum { SUPPLY_TECH_SIZE = 15};
typedef std::list <ICoord2D> ICoord2DList;

class TechAndSupplyImages
{
public:
	ICoord2DList m_techPosList;
	ICoord2DList m_supplyPosList;
};

struct WinTimeStamp
{
	UnsignedInt m_lowTimeStamp;
	UnsignedInt m_highTimeStamp;
};


class WaypointMap : public std::map<AsciiString, Coord3D>
{
public:
	void update();	///< returns the number of multiplayer start spots found
	Int m_numStartSpots;
};

typedef std::list <Coord3D> Coord3DList;

class MapMetaData
{
public:
	UnicodeString m_displayName;
	AsciiString m_nameLookupTag;
	Region3D m_extent;
	Int m_numPlayers;

	Bool m_isMultiplayer;
	Bool m_isOfficial;
	Bool m_doesExist; ///< Flag to indicate whether the map physically exists. Should be true.
	UnsignedInt m_filesize;
	UnsignedInt m_CRC;

	WinTimeStamp m_timestamp;

	WaypointMap m_waypoints;
	Coord3DList m_supplyPositions;
	Coord3DList m_techPositions;
	AsciiString m_fileName;
};

// TheSuperHackers @performance xezon 02/11/2025 Simplifies and improves the implementation of MapCache
// to prevent expensive reoccurring redundant map cache reads.

// TheSuperHackers @feature O112 The map list's tabs: the game's own maps, the player's own maps, and the collection's
// map packs read in place (Packs\Maps\<Pack>\<Game>\Maps, nothing copied): GenTool's ranked maps and GenPatcher's
// packs (every other pack).
enum MapListTab
{
	MAP_TAB_OFFICIAL = 0,
	MAP_TAB_USER,
	MAP_TAB_GENTOOL,
	MAP_TAB_GENPATCHER,
	MAP_TAB_COUNT
};

class MapCache : public std::map<AsciiString, MapMetaData>
{
	typedef std::set<AsciiString> MapNameSet;

public:
	struct PackDir
	{
		AsciiString pack;   ///< lowercase folder name of the pack
		AsciiString dir;    ///< packs\maps\<pack>\<game>\maps, lowercase, no trailing separator
		Int tab;            ///< MapListTab
	};

	MapCache()
		: m_doCreateStandardMapCacheINI(TRUE)
		, m_doLoadStandardMapCacheINI(TRUE)
		, m_doLoadUserMapCacheINI(TRUE)
		, m_packDirsFound(FALSE)
		, m_doLoadPackCacheINI(TRUE)
	{}

	void updateCache();

	AsciiString getMapDir() const;
	AsciiString getUserMapDir() const;
	AsciiString getMapExtension() const;

	const MapMetaData *findMap(AsciiString mapName);

	// allow us to create a set of shippable maps to be in mapcache.ini.  For use with -buildMapCache.
	void addShippingMap(AsciiString mapName) { mapName.toLower(); m_allowedMaps.insert(mapName); }

	// O112: the map packs of this game
	const std::vector<PackDir> &getPackDirs();
	AsciiString getPackMapDir(const AsciiString &pack) const;            ///< packs\maps\<pack>\<game>\maps
	Bool getPackOfMap(const AsciiString &mapName, AsciiString *pack) const; ///< TRUE (and the pack) for a map in a pack folder
	static Bool isSafePackName(const AsciiString &pack);                 ///< letters, digits, '_', '-', ' ' only
	Int getTabOfMap(const AsciiString &mapName);                         ///< MapListTab
	Bool isMapInTab(const AsciiString &mapName, Int tab);
	Bool hasMapsInTab(Int tab);

private:
	void findPackDirs();
	void prepareUnseenMaps(const AsciiString &mapDir);
	Bool clearUnseenMaps(const AsciiString &mapDir);
	void loadMapsFromMapCacheINI(const AsciiString &mapDir);
	Bool loadMapsFromDisk(const AsciiString &mapDir, Bool isOfficial, Bool filterByAllowedMaps = FALSE); // returns true if we needed to (re)parse a map
	Bool addMap(const AsciiString &mapDir, const AsciiString &fname, const AsciiString &lowerFname, FileInfo &fileInfo, Bool isOfficial); ///< returns true if it had to (re)parse the map
	void writeCacheINI(const AsciiString &mapDir);

	static const char *const m_mapCacheName;

	MapNameSet m_allowedMaps;
	Bool m_doCreateStandardMapCacheINI;
	Bool m_doLoadStandardMapCacheINI;
	Bool m_doLoadUserMapCacheINI;
	std::vector<PackDir> m_packDirs;
	MapNameSet m_packFolderNames;   ///< lowercase map folder names in the packs: the player's copies are not listed again
	Bool m_packDirsFound;
	Bool m_doLoadPackCacheINI;
};

extern MapCache *TheMapCache;
extern TechAndSupplyImages TheSupplyAndTechImageLocations;

// TheSuperHackers @refactor xezon 28/11/2025 Refactors the map list population implementation
// by breaking it into smaller pieces to make it more maintainable.

/// O112: fill the listbox with the maps of one MapListTab
Int populateMapListboxTab( GameWindow *listbox, Int tab, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString );
Int populateMapListboxTabNoReset( GameWindow *listbox, Int tab, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString );
Int populateMapListbox( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString );		/// Read a list of maps from the run directory and fill in the listbox.  Return the selected index
Int populateMapListboxNoReset( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString );		/// Read a list of maps from the run directory and fill in the listbox.  Return the selected index
Bool isValidMap( AsciiString mapName, Bool isMultiplayer );						/// Validate a map
Image *getMapPreviewImage( AsciiString mapName );
AsciiString getDefaultMap( Bool isMultiplayer );											/// Find a valid map
AsciiString getDefaultOfficialMap();
Bool isOfficialMap( AsciiString mapName );
Bool parseMapPreviewChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
void findDrawPositions( Int startX, Int startY, Int width, Int height, Region3D extent,
															 ICoord2D *ul, ICoord2D *lr );
Bool WouldMapTransfer( const AsciiString& mapName );
