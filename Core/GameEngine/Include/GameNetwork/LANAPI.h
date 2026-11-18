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

// LANAPI.h ///////////////////////////////////////////////////////////////
// LANAPI singleton class - defines interface to LAN broadcast communications
// Author: Matthew D. Campbell, October 2001

#pragma once

#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/LANPlayer.h"
#include "GameNetwork/LANGameInfo.h"

//static const Int g_lanPlayerNameLength = 20;
static const Int g_lanPlayerNameLength = 12; // reduced length because of game option length
//static const Int g_lanLoginNameLength = 16;
//static const Int g_lanHostNameLength = 16;
static const Int g_lanLoginNameLength = 1;
static const Int g_lanHostNameLength = 1;
//static const Int g_lanGameNameLength = 32;
static const Int g_lanGameNameLength = 16; // reduced length because of game option length
static const Int g_lanGameNameReservedLength = 16; // save N wchars for ID info
static const Int g_lanMaxChatLength = 100;
static const Int m_lanMaxOptionsLength = MAX_LANAPI_PACKET_SIZE - ( 8 + (g_lanGameNameLength+1)*2 + 4 + (g_lanPlayerNameLength+1)*2
																														+ (g_lanLoginNameLength+1) + (g_lanHostNameLength+1) );
static const Int g_maxSerialLength = 23; // including the trailing '\0'

struct LANMessage;

/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPIInterface : public SubsystemInterface
{
public:

	virtual ~LANAPIInterface() override { };

	virtual void setIsActive(Bool isActive ) = 0;								///< Tell TheLAN whether or not the app is active.

	// Possible types of chat messages
	enum ChatType
	{
		LANCHAT_NORMAL = 0,
		LANCHAT_EMOTE,
		LANCHAT_SYSTEM,
	};

	// Request functions generate network traffic
	virtual void RequestLocations() = 0;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) = 0;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) = 0;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() = 0;																				///< Tell everyone we're leaving
	virtual void RequestAccept() = 0;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() = 0;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) = 0;						///< Send a chat message
	virtual void RequestPlayerChat( UnicodeString message ) = 0;
	virtual void RequestGameStart() = 0;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) = 0;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) = 0;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) = 0;	///< Try to host a game
	virtual void RequestGameAnnounce() = 0;																			///< Sound out current game info if host
//	virtual void RequestSlotList() = 0;																					///< Pump out the Slot info.
	virtual void RequestSetName( UnicodeString newName ) = 0;													///< Pick a new name
	virtual void RequestLobbyLeave( Bool forced ) = 0;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() = 0;

	// Possible result codes passed to On functions
	enum ReturnType
	{
		RET_OK,							// Any function
		RET_TIMEOUT,				// OnGameJoin/Leave/Start, etc
		RET_GAME_FULL,			// OnGameJoin
		RET_DUPLICATE_NAME,	// OnGameJoin
		RET_CRC_MISMATCH,		// OnGameJoin
		RET_SERIAL_DUPE,		// OnGameJoin
		RET_GAME_STARTED,		// OnGameJoin
		RET_GAME_EXISTS,		// OnGameCreate
		RET_GAME_GONE,			// OnGameJoin
		RET_BUSY,						// OnGameCreate/Join/etc if another action is in progress
		RET_UNKNOWN,				// Default message for oddity
		RET_GUARD_REQUIRED,	// OnGameJoin: O30, the host accepts only players with a verified anti-cheat (appended: an older
							// build shows its "unknown" text)
	};
	UnicodeString getErrorStringFromReturnType( ReturnType ret );

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) = 0;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) = 0;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) = 0;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() = 0;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) = 0;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) = 0;														///< Chat message from someone
	virtual void OnGameStart() = 0;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) = 0;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) = 0;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) = 0;																							///< Your game is created
//	virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) = 0;												///< Someone has morphed

	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) = 0;													///< return a pointer to the most recent game associated to the host IP address
	virtual Bool SetLocalIP( UnsignedInt localIP ) = 0;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) = 0;																		///< For multiple NIC machines
	virtual Bool AmIHost() = 0;																											///< Am I hosting a game?
	virtual inline UnicodeString GetMyName() = 0;																		///< What's my name?
	virtual inline LANGameInfo *GetMyGame() = 0;															          ///< What's my Game?
	virtual void fillInLANMessage( LANMessage *msg ) = 0;																	///< Fill in default params
	virtual void checkMOTD() = 0;
};


/// TheSuperHackers @feature O30 (row 15) The largest opaque anti-cheat payload one MSG_GUARD_DATA carries (GuardLink.h).
enum { LANGuardDataMax = 200 };

/**
 * LAN message class
 */
#pragma pack(push, 1)
struct LANMessage
{
	enum Type				          ///< What kind of message are we?
	{
		// Locating everybody
		MSG_REQUEST_LOCATIONS,	///< Hey, where is everybody?
		MSG_GAME_ANNOUNCE,			///< Here I am, and here's my game info!
		MSG_LOBBY_ANNOUNCE,			///< Hey, I'm in the lobby!

		// Joining games
		MSG_REQUEST_JOIN,				///< Let me in!  Let me in!
		MSG_JOIN_ACCEPT,				///< Okay, you can join.
		MSG_JOIN_DENY,					///< Go away!  We don't want any!

		// Leaving games
		MSG_REQUEST_GAME_LEAVE,	///< I want to leave the game
		MSG_REQUEST_LOBBY_LEAVE,///< I'm leaving the lobby

		// Game options, chat, etc
		MSG_SET_ACCEPT,					///< I'm cool with everything as is.
		MSG_MAP_AVAILABILITY,		///< I do (not) have the map.
		MSG_CHAT,								///< Just spouting my mouth off.
		MSG_GAME_START,					///< Hold on; we're starting!
		MSG_GAME_START_TIMER,		///< The game will start in N seconds
		MSG_GAME_OPTIONS,				///< Here's some info about the game.
		MSG_INACTIVE,						///< I've alt-tabbed out.  Unaccept me cause I'm a poo-flinging monkey.

		MSG_REQUEST_GAME_INFO,	///< For direct connect, get the game info from a specific IP Address

		// TheSuperHackers @feature O26 (row 26) Sent after every lobby or game announce: the sender's files fingerprint
		// (Fingerprint.h), for the "files match" mark of the LAN screen. Appended, so the values above are unchanged; a
		// build without it ignores it (an unknown type).
		MSG_FILES_INFO,

		// TheSuperHackers @feature O30 (row 15) Opaque bytes from the sender's anti-cheat to this player's anti-cheat
		// (GuardLink.h). Appended; a build without it ignores it.
		MSG_GUARD_DATA,

		// TheSuperHackers @feature O31 (row 21, step 3) The room's observers (LanRoom.h). Appended; a build without it
		// ignores it.
		MSG_ROOM,

		// TheSuperHackers @bugfix O62 (H5 #1) The start handshake: the host's check of its slot list's digest, and a
		// player's answer that its own copy has the same digest (StartCheck). Appended.
		MSG_START_CHECK,
		MSG_START_READY,

		// TheSuperHackers @bugfix O74 A player's acknowledgement of START (StartCheck: the digest it started with), sent
		// before it leaves for the match. Appended.
		MSG_START_ACK,
	} messageType;

	WideChar name[g_lanPlayerNameLength+1]; ///< My name, for convenience
	char userName[g_lanLoginNameLength+1];	///< login name, for convenience
	char hostName[g_lanHostNameLength+1];		///< machine name, for convenience

	// No additional data is required for REQUEST_LOCATIONS, LOBBY_ANNOUNCE,
	// REQUEST_LOBBY_LEAVE, GAME_START.
	union
	{
		// StartTimer is sent with GAME_START_TIMER
		struct
		{
			Int seconds;
		} StartTimer;

		// GameJoined is sent with REQUEST_GAME_LEAVE
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
		} GameToLeave;

		// GameInfo if sent with GAME_ANNOUNCE
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			Bool inProgress;
			char options[m_lanMaxOptionsLength+1];
			Bool isDirectConnect;
		} GameInfo;

		// PlayerInfo is sent with REQUEST_GAME_INFO for direct connect games.
		struct
		{
			UnsignedInt ip;
			WideChar playerName[g_lanPlayerNameLength+1];
		} PlayerInfo;

		// GameToJoin is sent with REQUEST_JOIN
		struct
		{
			UnsignedInt gameIP;
			UnsignedInt exeCRC;
			UnsignedInt iniCRC;
			char serial[g_maxSerialLength];
			WideChar gameName[g_lanGameNameLength+1];   ///< O62 (H5 #2): the room asked for (the host checks it is his)
			UnsignedInt protocolMagic;                  ///< O62: LANProtocolMagic
			Int protocol;                               ///< O62: LANProtocolVersion
			char build[16];                             ///< O63: LanBuildIdentity
		} GameToJoin;

		// GameJoined is sent with JOIN_ACCEPT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			Int slotPosition;
			UnsignedInt protocolMagic;   ///< O62: LANProtocolMagic
			Int protocol;                ///< O62: LANProtocolVersion
			char build[16];              ///< O63: LanBuildIdentity
		} GameJoined;

		// GameNotJoined is sent with JOIN_DENY
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			LANAPIInterface::ReturnType reason;
		} GameNotJoined;

		// Accept is sent with SET_ACCEPT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			Bool isAccepted;
		} Accept;

		// Accept is sent with MAP_AVAILABILITY
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt mapCRC;	// to make sure we're talking about the same map
			Bool hasMap;
		} MapStatus;

		// Chat is sent with CHAT
		struct
		{
			WideChar gameName[g_lanGameNameLength+1];
			LANAPIInterface::ChatType chatType;
			WideChar message[g_lanMaxChatLength+1];
		} Chat;

		// GameOptions is sent with GAME_OPTIONS
		struct
		{
			char options[m_lanMaxOptionsLength+1];
			UnsignedInt revisionMagic;   ///< O64 (H6 #4): LANProtocolMagic when the host sends its slot list
			UnsignedInt revision;        ///< O64: grows with every change of the host's slot list
		} GameOptions;

		// FilesInfo is sent with FILES_INFO (O26)
		struct
		{
			UnsignedInt magic;        ///< LANFilesInfoMagic
			UnsignedInt fingerprint;  ///< Fingerprint::overall(), 0: not known
			UnsignedInt exeCRC;
			UnsignedInt iniCRC;
		} FilesInfo;

		// GuardData is sent with GUARD_DATA (O30)
		struct
		{
			UnsignedInt magic;        ///< LANGuardDataMagic
			UnsignedShort length;     ///< bytes used in 'data'
			UnsignedByte data[LANGuardDataMax];
		} GuardData;

		// Room is sent with ROOM (O31; LanRoom.h)
		struct
		{
			UnsignedInt magic;        ///< LANRoomMagic
			UnsignedByte kind;        ///< LanRoom's message kind
			UnsignedByte count;       ///< list: observers; start: players; removed: the reason
			UnsignedShort port;       ///< start: the stream port
			UnsignedInt gameIP;       ///< the room (its host's address)
			UnsignedInt ips[12];      ///< list: the observers; start: the players
			union
			{
				WideChar names[12][g_lanPlayerNameLength+1];   ///< list
				WideChar text[g_lanMaxChatLength+1];           ///< chat: one ready line
			} u;
			UnsignedShort delaySeconds; ///< O33: list, start: the host's delay for outside viewers (10..180 s)
			UnsignedShort flags;        ///< O33: list, start: bit 0 outside viewers allowed; start: bit 1 sent to a player
		} Room;
		// StartCheck is sent with GAME_START, START_CHECK and START_READY (O62)
		struct
		{
			UnsignedInt magic;        ///< LANStartMagic
			UnsignedInt digest;       ///< LanStartDigest of the sender's copy of the room
			WideChar gameName[g_lanGameNameLength+1];
			UnsignedInt revision;     ///< O64 (H6 #4): the revision of the host's slot list it checks
			// TheSuperHackers @feature O89 GAME_START only (not read in START_CHECK and READY): the room observers that get the
			// stream live (and the watching admin), frozen at Start; was the room's own KIND_START to the players, sent
			// once, so a player that lost it held them behind the delay. A player takes it before it acknowledges START.
			UnsignedByte liveCount;
			UnsignedByte liveFlags;   ///< bit 0: outside viewers allowed
			UnsignedShort liveDelaySeconds;   ///< the host's delay for outside viewers (10..180 s)
			UnsignedInt liveIps[13];  ///< LanRoom::MAX_OBSERVERS and the watching admin
		} StartCheck;

		static_assert(sizeof(Room) <= sizeof(GameInfo), "MSG_ROOM must not make LANMessage larger");
		static_assert(sizeof(StartCheck) <= sizeof(GameInfo), "O89: the start's live list must not make LANMessage larger");
		static_assert(sizeof(GameOptions) <= sizeof(GameInfo), "O64: the options' revision must not make LANMessage larger");
		static_assert(sizeof(GuardData) <= sizeof(GameInfo), "MSG_GUARD_DATA must not make LANMessage larger");
		static_assert(ARRAY_SIZE(GameOptions.options) > m_lanMaxOptionsLength, "GameOptions.options buffer must be larger than m_lanMaxOptionsLength");

	};
};
#pragma pack(pop)

static_assert(sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE, "LANMessage struct cannot be larger than the max packet size");

// TheSuperHackers @feature O26 (row 26) What the LAN screen needs beyond the original messages.
static const UnsignedInt LANFilesInfoMagic = 0x46494C31;   ///< "FIL1"
// TheSuperHackers @feature O62 The LAN protocol number. A room's options carry it ("PV=n;", GameInfo), and so do the join
// request and accept (GameToJoin, GameJoined, with LANProtocolMagic); a different number is another version: its room
// is listed as "другая версия" and cannot be joined or watched, a join of it is refused with the version line. Bump it
// whenever the wire of the room, the start, the observers' channel or the stream changes. 1 = O59..O62 (plan3/plan4 and
// older have none: 0); 2 = O63 (the build identity in the join); 3 = O89 (the live list inside GAME_START, the room's
// KIND_START no longer to the players; KIND_START_CANCEL).
// TheSuperHackers @feature O63 Next to the number, the BUILD IDENTITY: the git commit of the build (Version's short hash,
// "~" first when it was built with uncommitted changes), the same for every exe of one build (the product and the
// uitest exe) and not changed by any pack. A room carries it ("BV=<hash>;"), and so do the join request and accept; a
// different identity is "другая версия" as a different number. (The exe CRC differs between the product and uitest
// exes of one build; the ini CRC is the original's and stays unused, see plan1-o63-report.md.)
static const UnsignedInt LANProtocolMagic = 0x3156504C;    ///< "LPV1"
static const Int LANProtocolVersion = 3;
/// O64 (H6, low): the start handshake's magic carries the protocol number ("STR2"): another number is not this game's.
static const UnsignedInt LANStartMagic = 0x00525453u | ((UnsignedInt)('0' + LANProtocolVersion) << 24);
/// O76: a player's answer to START_CHECK (MSG_START_READY) when it cannot start: the map is not on it and no transfer
/// will bring it (an official map, or its seat says it has the map while the file is not there)
static const UnsignedInt LANStartNoMapMagic = LANStartMagic ^ 0x50414D4Eu;
/// O63: this build's identity (letters, digits and '~', at most 15).
AsciiString LanBuildIdentity();
/// O63: the room is this build's: the same protocol number and the same build identity.
Bool LanSameVersion(const GameInfo *game);
/// TheSuperHackers @bugfix O64 (H6 #3) The start digest of a room as its options carry it: the copy goes through the
/// options string and back, so the host's raw map path and names and a player's parsed (portable, lower case) ones give
/// the same number (GameInfo::startDigest of that copy).
UnsignedInt LanStartDigest(LANGameInfo *game);
static const UnsignedInt LANGuardDataMagic = 0x31445247;   ///< "GRD1" (O30)
/// "ROM<n>" (O31; O62/O64: the room messages carry the protocol number, so builds of other numbers ignore each other)
static const UnsignedInt LANRoomMagic = 0x004D4F52u | ((UnsignedInt)('0' + LANProtocolVersion) << 24);
namespace LanFiles
{
	/// The files fingerprint a lobby player or a game host sent (MSG_FILES_INFO); FALSE when none came yet.
	Bool fingerprintOf(UnsignedInt ip, UnsignedInt *fingerprint);
	/// This computer's fingerprint (0 when not computed).
	UnsignedInt own();
}
/// Test switches of the LAN screen (command line): -lanLoopback N binds the lobby to 127.0.0.(10+N) and sends its
/// broadcasts to 127.0.0.10 ... 127.0.0.17 one by one (no other address, no firewall question; two instances on one PC
/// find each other); -lanSample fills the screen's lists with sample games and players (pictures without a network).
namespace LanTest
{
	void setLoopbackSlot(Int slot);
	Int loopbackSlot();          ///< -1: off
	UnsignedInt loopbackIP();    ///< 0: off
	void setSampleData(Bool on);
	Bool sampleData();
	/// O45 (test): -lanTestLoss <percent>: this computer drops that share of the lobby messages it receives (a lossy
	/// link such as a VPN), and writes LanJoin.log in its user data folder (each join request, repeat, answer and
	/// timeout, on the joiner and on the host). -1: off.
	void setLossPercent(Int percent);
	Int lossPercent();
	/// O88 (test): -lanTestLossSeed <n>: the seed of the drops (0: a fresh one per process, from the time and the process
	/// id; without the switch the fixed default, so every process and run drops the same message numbers). Used only with
	/// -lanTestLoss; the first drop decision writes "loss: P %, seed 0x........" to LanJoin.log.
	void setLossSeed(UnsignedInt seed);
	/// O45: TRUE when this received message is dropped. O88: the dropped join requests, join answers and start messages
	/// (START_CHECK, READY, START, ACK) are written to LanJoin.log with their sender.
	Bool dropForTest(UnsignedInt senderIP, const void *data, Int length);
	void joinLog(const char *format, ...);
}


/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPI : public LANAPIInterface
{
public:

	LANAPI();
	virtual ~LANAPI() override;

	virtual void init() override;															///< Initialize or re-initialize the instance
	virtual void reset() override;															///< reset the logic system
	virtual void update() override;														///< update the world
	/// TheSuperHackers @feature O30 (row 15) During a LAN match (the lobby's update does not run then): only the anti-cheat
	/// messages are read and passed on, everything else that arrives is dropped; the match's players are noted for
	/// rechecks and a failed one is named in a message line.
	void pumpGuard();
	/// O30: opaque anti-cheat bytes to one player (GuardLink's sender).
	void sendGuardData(UnsignedInt ip, const UnsignedByte *data, Int length);
	/// O33: a received message's text fields end inside the message (review 7).
	static void terminateStrings(LANMessage *msg);
	/// O31: a MSG_ROOM message (its Room part filled) to one address, or to everybody with 0 (LanRoom.h).
	void sendRoomMessage(LANMessage *msg, UnsignedInt ip);

	virtual void setIsActive(Bool isActive) override;								///< tell TheLAN whether or not

	// Request functions generate network traffic
	virtual void RequestLocations() override;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) override;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) override;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() override;																				///< Tell everyone we're leaving
	virtual void RequestAccept() override;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() override;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) override;						///< Send a chat message
	virtual void RequestPlayerChat( UnicodeString message ) override;
	virtual void RequestGameStart() override;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) override;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) override;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) override;	///< Try to host a game
	virtual void RequestGameAnnounce() override;																			///< Send out game info if host
	virtual void RequestSetName( UnicodeString newName ) override;													///< Pick a new name
//	virtual void RequestSlotList();																					///< Pump out the Slot info.
	virtual void RequestLobbyLeave( Bool forced ) override;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() override;

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) override;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) override;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() override;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) override;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override;														///< Chat message from someone
	virtual void OnGameStart() override;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) override;
	/// TheSuperHackers @feature O60 (row 46) The admin who watches from outside the seats is not in the match: after
	/// OnGameStart he leaves his room on the next update, without a word to the players, and watches from the lobby.
	void OnWatchMatchLeave();
	/// O62 (H5 #1, #7): a start is under way: the countdown runs, or the host waits for the players' READY.
	Bool IsStartPending() const { return m_gameStartTime != 0 || m_startChecking || m_startDelivering; }
	Bool IsStartDelivering() const { return m_startDelivering; }   ///< O76: START is out: the room must not change
	/// O84: the host of the game row selected in the lobby (0: none); its seats and observers are checked (GuardLink)
	void SetLobbySelectedHost(UnsignedInt hostIP) { m_lobbySelectedHost = hostIP; }
	/// O71: the room's keep-alive while update() does not run (the screen changes: LANbuttonPushed, a shell animation);
	/// called by the LAN lobby menu's update instead of update()
	void keepRoomAlive();
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) override;																							///< Your game is created
	//virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame );															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) override;												///< Someone has morphed
	virtual void OnInActive( UnsignedInt IP );																								///< Someone has alt-tabbed out.


	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) override;													///< return a pointer to the most recent game associated to the host IP address
	virtual LANPlayer * LookupPlayer( UnsignedInt playerIP );													///< return a pointer to a player we know about
	virtual Bool SetLocalIP( UnsignedInt localIP ) override;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) override;																		///< For multiple NIC machines
	virtual Bool AmIHost() override;																											///< Am I hosting a game?
	virtual UnicodeString GetMyName() override { return m_name; }                 ///< What's my name?
	AsciiString GetMyUserName() const { return m_userName; }                       ///< O60: login name
	AsciiString GetMyHostName() const { return m_hostName; }                       ///< O60: machine name
	virtual LANGameInfo* GetMyGame() override { return m_currentGame; }					      ///< What's my Game?
	virtual UnsignedInt GetLocalIP() { return m_localIP; }								///< What's my IP?
	virtual void fillInLANMessage( LANMessage *msg ) override;																	///< Fill in default params
	virtual void checkMOTD() override;
protected:

	enum PendingActionType
	{
		ACT_NONE = 0,
		ACT_JOIN,
		ACT_JOINDIRECTCONNECT,
		ACT_LEAVE,
	};

	static const UnsignedInt s_resendDelta; // in ms

protected:
	LANPlayer *					m_lobbyPlayers;			///< List of players in the lobby
	LANGameInfo *				m_games;								///< List of games
	UnicodeString				m_name;							///< Who do we think we are?
	AsciiString					m_userName;						///< login name
	AsciiString					m_hostName;						///< machine name
	UnsignedInt					m_gameStartTime;
	Int									m_gameStartSeconds;

	PendingActionType		m_pendingAction;	///< What action are we performing?
	UnsignedInt					m_expiration;						///< When should we give up on our action?
	UnsignedInt					m_actionTimeout;
	UnsignedInt					m_directConnectRemoteIP;///< The IP address of the game we are direct connecting to.

	// Resend timer ---------------------------------------------------------------------------
	UnsignedInt					m_lastResendTime; // in ms

	Bool								m_isInLANMenu;		///< true while we are in a LAN menu (lobby, game options, direct connect)
	Bool								m_watchMatchLeave;	///< O60: the admin's quiet leave at Start is due (OnWatchMatchLeave)
	Bool								m_startChecking;	///< O62: the host waits for every seated player's READY
	// O70: a joiner's requests until the host's slot list shows them
	enum { CONVERGE_COLOR, CONVERGE_TEMPLATE, CONVERGE_STARTPOS, CONVERGE_TEAM, CONVERGE_COUNT };
	LANGameInfo *				m_convergeGame;
	// O70: the room's keep-alive: the host's slot list and a joiner's HELLO every 2 s; a seat (or the host) silent for
	// 16 s is gone, a seat never heard from after its join is freed after 10 s
	UnsignedInt					m_lastRoomBeat;
	UnsignedInt					m_seatJoinedMs[MAX_SLOTS];   ///< the host: a seat given and not yet heard from (0: none)
	// O71: silence is 30 s; it is not counted while this computer's own loop did not run, nor during a start
	UnsignedInt					m_lastLoopMs;        ///< the last LANAPI::update in a room
	UnsignedInt					m_lobbySelectedHost; ///< O84: SetLobbySelectedHost
	UnsignedInt					m_startSeenMs;       ///< a joiner: the host's last start check or countdown message
	UnsignedInt					m_notListedSinceMs;  ///< O71: a joiner: the host's lists have not had this computer since (0: they have)
	Bool								m_acceptWanted;
	UnsignedInt					m_acceptDigest;
	UnsignedInt					m_acceptLastSent;
	UnsignedInt					m_acceptSince;
	UnsignedInt					m_hostViewMs;
	Bool								m_hostSeesAccepted;
	Bool								m_hostSeesMap;
	Int									m_hostSeesValue[CONVERGE_COUNT];
	UnsignedInt					m_mapLastSent;
	Bool								m_optionPending[CONVERGE_COUNT];
	Int									m_optionWanted[CONVERGE_COUNT];
	UnsignedInt					m_optionSince[CONVERGE_COUNT];
	UnsignedInt					m_optionLastSent[CONVERGE_COUNT];
	UnsignedInt					m_startDigest;		///< O62: GameInfo::startDigest of the host's room now
	UnsignedInt					m_startCheckSince;
	UnsignedInt					m_lastStartCheck;
	Bool								m_startReady[MAX_SLOTS];
	// O74: START is repeated to every seat until it acknowledges, for at most a few seconds; then the host starts
	Bool								m_startDelivering;
	UnsignedInt					m_startDeliverSince;
	UnsignedInt					m_lastStartSend;
	Bool								m_startAcked[MAX_SLOTS];
	Bool								m_startCannot[MAX_SLOTS];   ///< O76: answered the start check with "cannot start: no map"
	Bool								m_inLobby;											///< Are we in the lobby (not in a game)?
	LANGameInfo *				m_currentGame;							///< Pointer to game (setup screen) we are currently in (null for lobby)
	//LANGameInfo *m_currentGameInfo;			///< Pointer to game setup info we are currently in.

	UnsignedInt					m_localIP;
	Transport*					m_transport;

	UnsignedInt					m_broadcastAddr;

	UnsignedInt					m_lastUpdate;
	AsciiString					m_lastGameopt; /// @todo: hack for demo - remove this

	Bool								m_isActive;			///< is the game currently active?

protected:
	void sendMessage(LANMessage *msg, UnsignedInt ip = 0); // Convenience function
	void sendMessageOnly(LANMessage *msg, UnsignedInt ip); ///< O26: sendMessage without the files info after an announce
	void removePlayer(LANPlayer *player);
	void removeGame(LANGameInfo *game);
	void addPlayer(LANPlayer *player);
	void addGame(LANGameInfo *game);
	AsciiString createSlotString();

	// Functions to handle incoming messages -----------------------------------
	void handleRequestLocations( LANMessage *msg, UnsignedInt senderIP );
	void handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestJoin( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinDeny( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleSetAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleHasMap( LANMessage *msg, UnsignedInt senderIP );
	void handleChat( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStart( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP );
	/// O62 (H5 #2): an announce comes from its host (a room) or from a seat of it (a running game: its packet router).
	Bool announceFromItsHost( LANMessage *msg, UnsignedInt senderIP );
	void handleStartCheck( LANMessage *msg, UnsignedInt senderIP );   ///< O62
	void handleStartReady( LANMessage *msg, UnsignedInt senderIP );   ///< O62
	void handleStartAck( LANMessage *msg, UnsignedInt senderIP );     ///< O74
	void pumpStartDelivery();                                         ///< O74
	void sendStartMessage( UnsignedInt ip );                          ///< O74: START with the confirmed digest (0: to all)
	void stopStartDelivery( const char *why );                        ///< O76: START called back while nobody started
	void pumpStartCheck();   ///< O62: the host: options and START_CHECK again, START once every seated player is READY
	void finishStart();      ///< O62: the host: the room's observers, START, the match
	/// TheSuperHackers @bugfix O70 A joiner's room requests (accept, has-map, army/colour/team/start place) are sent
	/// again until the host's slot list shows them (they were sent once: one lost packet left the host with the old
	/// state, e.g. never accepted, and nothing repeated it).
	void pumpConvergence();
	void pumpRoomBeat(UnsignedInt now);            ///< O71: the 2 s keep-alive (host: its list; joiner: HELLO to the host)
	void noteGuardPeers();                         ///< O83: the addresses this player's screens show a shield for
	void noteHeardFrom(UnsignedInt senderIP, Int messageType);   ///< O71/O73: a room message of a seat (host) or of the host (joiner) is a sign of life
	void sendAcceptOnly();
	void noteHostView(const AsciiString &options);   ///< what the host's slot list says about this joiner's seat
	void handleGameOptions( LANMessage *msg, UnsignedInt senderIP );
	void handleInActive( LANMessage *msg, UnsignedInt senderIP );

};
