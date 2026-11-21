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

#pragma once

#include "GameNetwork/NetworkDefs.h"
#include "Utility/stringex.h"

class AsciiString;
class UnicodeString;
class GameMessage;

class NetCommandRef;
class NetCommandMsg;
class NetGameCommandMsg;
class NetAckCommandMsg;
class NetAckBothCommandMsg;
class NetAckStage1CommandMsg;
class NetAckStage2CommandMsg;
class NetFrameCommandMsg;
class NetPlayerLeaveCommandMsg;
class NetRunAheadMetricsCommandMsg;
class NetRunAheadCommandMsg;
class NetDestroyPlayerCommandMsg;
class NetKeepAliveCommandMsg;
class NetDisconnectKeepAliveCommandMsg;
class NetDisconnectPlayerCommandMsg;
class NetPacketRouterQueryCommandMsg;
class NetPacketRouterAckCommandMsg;
class NetDisconnectChatCommandMsg;
class NetChatCommandMsg;
class NetDisconnectVoteCommandMsg;
class NetProgressCommandMsg;
class NetWrapperCommandMsg;
class NetFileCommandMsg;
class NetFileAnnounceCommandMsg;
class NetFileProgressCommandMsg;
class NetDisconnectFrameCommandMsg;
class NetDisconnectScreenOffCommandMsg;
class NetFrameResendRequestCommandMsg;
class NetLoadCompleteCommandMsg;
class NetTimeOutGameStartCommandMsg;

////////////////////////////////////////////////////////////////////////////////
// Helper class to pass buffer pointer and size
// Does not take ownership of the buffer.
////////////////////////////////////////////////////////////////////////////////

class NetPacketBuf
{
public:
	NetPacketBuf(const UnsignedByte *data, size_t size)
		: m_data(data)
		, m_size(size)
	{}

	const UnsignedByte *data() const
	{
		return m_data;
	}

	UnsignedByte operator[](size_t index) const
	{
		return m_data[index];
	}

	size_t size() const
	{
		return m_size;
	}

	NetPacketBuf offset(size_t size) const
	{
		const size_t safeSize = min(size, m_size);
		return NetPacketBuf(m_data + safeSize, m_size - safeSize);
	}

private:
	const UnsignedByte *m_data;
	size_t m_size;
};

////////////////////////////////////////////////////////////////////////////////
// Helper functions for raw byte data handling
////////////////////////////////////////////////////////////////////////////////

namespace network
{

template<typename T>
size_t readObject(T &value, NetPacketBuf src)
{
	const size_t readLen = min(sizeof(value), src.size());
	memcpy(&value, src.data(), readLen);
	return readLen;
}

inline size_t readBytes(UnsignedByte *dest, size_t destLen, NetPacketBuf src)
{
	const size_t readLen = min(destLen, src.size());
	memcpy(dest, src.data(), readLen);
	return readLen;
}

// The text of a game packet is made of 2-byte UTF-16 units (WireChar) on every platform; the counts are units.
// Number of units of 'value' that go on the wire: all of it, but at most maxUnits (a surrogate pair is never cut).
inline size_t wireTextUnits(const UnicodeString &value, size_t maxUnits)
{
#if RTS_WIRE_CONVERT
	return WireText::fitUnits(value.str(), static_cast<size_t>(value.getLength()), maxUnits);
#else
	return std::min<size_t>(value.getLength(), maxUnits);
#endif
}

inline size_t readStringWithoutNull(UnicodeString &str, size_t maxStrLen, NetPacketBuf src)
{
	const size_t strLen = min(maxStrLen, src.size() / sizeof(WireChar));
	const size_t cpyLen = strLen * sizeof(WireChar);

	if (strLen > 0)
	{
#if RTS_WIRE_CONVERT
		WireChar *units = new WireChar[strLen];
		memcpy(units, src.data(), cpyLen);
		WideChar *strBuf = str.getBufferForRead(strLen);
		const size_t n = WireText::decode(strBuf, strLen, units, strLen);
		strBuf[n] = 0;
		delete[] units;
#else
		WideChar *strBuf = str.getBufferForRead(strLen);
		memcpy(strBuf, src.data(), cpyLen);
		strBuf[strLen] = 0;
#endif
	}
	return cpyLen;
}

inline size_t readStringWithNull(AsciiString &str, size_t maxStrLen, NetPacketBuf src)
{
	const size_t realStrLen = strnlen(reinterpret_cast<const char*>(src.data()), src.size());
	const size_t usedStrLen = min(realStrLen, maxStrLen);
	const size_t realCpyLen = realStrLen * sizeof(char);
	const size_t usedCpyLen = usedStrLen * sizeof(char);

	if (usedStrLen > 0)
	{
		char *strBuf = str.getBufferForRead(usedStrLen);
		memcpy(strBuf, src.data(), usedCpyLen);
		strBuf[usedStrLen] = 0;
	}
	return realCpyLen + sizeof(char);
}

template<typename T>
size_t writePrimitive(UnsignedByte *dest, T value)
{
	memcpy(dest, &value, sizeof(value));
	return sizeof(value);
}

template<typename T>
size_t writeObject(UnsignedByte *dest, const T &value)
{
	memcpy(dest, &value, sizeof(value));
	return sizeof(value);
}

inline size_t writeBytes(UnsignedByte *dest, const UnsignedByte *src, size_t len)
{
	memcpy(dest, src, len);
	return len;
}

inline size_t writeStringWithoutNull(UnsignedByte *dest, const UnicodeString &value, size_t maxLen)
{
#if RTS_WIRE_CONVERT
	const size_t units = wireTextUnits(value, maxLen);
	WireChar *out = new WireChar[units + 1];
	const size_t written = WireText::encode(out, units, value.str(), static_cast<size_t>(value.getLength()));
	memcpy(dest, out, written * sizeof(WireChar));
	delete[] out;
	return written * sizeof(WireChar);
#else
	const size_t copyLen = std::min<size_t>(value.getLength(), maxLen);
	const size_t copyBytes = copyLen * sizeof(WideChar);
	memcpy(dest, value.str(), copyBytes);
	return copyBytes;
#endif
}

inline size_t writeStringWithNull(UnsignedByte *dest, const AsciiString &value)
{
	memcpy(dest, value.str(), value.getByteCount() + 1);
	return static_cast<size_t>(value.getByteCount() + 1);
}

} // namespace network

// Ensure structs are packed to 1-byte alignment for network protocol compatibility
#pragma pack(push, 1)

////////////////////////////////////////////////////////////////////////////////
// Network packet field type definitions
////////////////////////////////////////////////////////////////////////////////

typedef UnsignedByte NetPacketFieldType;

namespace NetPacketFieldTypes
{
	constexpr const NetPacketFieldType CommandType = 'T';
	constexpr const NetPacketFieldType Relay = 'R';
	constexpr const NetPacketFieldType Frame = 'F';
	constexpr const NetPacketFieldType PlayerId = 'P';
	constexpr const NetPacketFieldType CommandId = 'C';
	constexpr const NetPacketFieldType Data = 'D';
	constexpr const NetPacketFieldType Repeat = 'Z';
}

////////////////////////////////////////////////////////////////////////////////
// Common packet field structures
////////////////////////////////////////////////////////////////////////////////

struct NetPacketCommandTypeField
{
	NetPacketCommandTypeField() : fieldType(NetPacketFieldTypes::CommandType) {}
	const NetPacketFieldType fieldType;
	UnsignedByte commandType;
};

struct NetPacketRelayField
{
	NetPacketRelayField() : fieldType(NetPacketFieldTypes::Relay) {}
	const NetPacketFieldType fieldType;
	UnsignedByte relay;
};

struct NetPacketFrameField
{
	NetPacketFrameField() : fieldType(NetPacketFieldTypes::Frame) {}
	const NetPacketFieldType fieldType;
	UnsignedInt frame;
};

struct NetPacketPlayerIdField
{
	NetPacketPlayerIdField() : fieldType(NetPacketFieldTypes::PlayerId) {}
	const NetPacketFieldType fieldType;
	UnsignedByte playerId;
};

struct NetPacketCommandIdField
{
	NetPacketCommandIdField() : fieldType(NetPacketFieldTypes::CommandId) {}
	const NetPacketFieldType fieldType;
	UnsignedShort commandId;
};

struct NetPacketDataField
{
	NetPacketDataField() : fieldType(NetPacketFieldTypes::Data) {}
	const NetPacketFieldType fieldType;
};

struct NetPacketRepeatField
{
	NetPacketRepeatField() : fieldType(NetPacketFieldTypes::Repeat) {}
	const NetPacketFieldType fieldType;
};

////////////////////////////////////////////////////////////////////////////////
// Packed Network structures
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// NetPacketRepeatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRepeatCommand
{
	struct CommandBase
	{
		NetPacketRepeatField repeat;
	};

	static size_t getSize()
	{
		return sizeof(CommandBase);
	}
	static size_t copyBytes(UnsignedByte *buffer)
	{
		CommandBase base;
		return network::writeObject(buffer, base);
	}
};

////////////////////////////////////////////////////////////////////////////////
// SmallNetPacketCommandBase
////////////////////////////////////////////////////////////////////////////////

struct SmallNetPacketCommandBaseSelect
{
	UnsignedByte useCommandType : 1;
	UnsignedByte useRelay : 1;
	UnsignedByte useFrame : 1;
	UnsignedByte usePlayerId : 1;
	UnsignedByte useCommandId : 1;
};

struct SmallNetPacketCommandBase
{
	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize(const SmallNetPacketCommandBaseSelect *select = nullptr);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref, const SmallNetPacketCommandBaseSelect *select = nullptr);
	static size_t readMessage(NetCommandRef *&ref, CommandBase &base, NetPacketBuf buf);
private:
	static NetCommandMsg *constructNetCommandMsg(const CommandBase &base);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketCommandTemplate
////////////////////////////////////////////////////////////////////////////////

template<typename Base, typename Data>
struct NetPacketCommandTemplate
{
	typedef Base CommandBase;
	typedef Data CommandData;

	static size_t getSize(const NetCommandMsg &msg)
	{
		size_t size = 0;
		size += CommandBase::getSize();
		size += CommandData::getSize(msg);
		return size;
	}
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
	{
		size_t size = 0;
		size += CommandBase::copyBytes(buffer + size, ref);
		size += CommandData::copyBytes(buffer + size, ref);
		return size;
	}
};

////////////////////////////////////////////////////////////////////////////////
// SmallNetPacketCommandTemplate
////////////////////////////////////////////////////////////////////////////////

template<typename Base, typename Data>
struct SmallNetPacketCommandTemplate
{
	typedef Base CommandBase;
	typedef Data CommandData;

	static size_t getSize(const NetCommandMsg &msg, const SmallNetPacketCommandBaseSelect *select = nullptr)
	{
		size_t size = 0;
		size += CommandBase::getSize(select);
		size += CommandData::getSize(msg);
		return size;
	}
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref, const SmallNetPacketCommandBaseSelect *select = nullptr)
	{
		size_t size = 0;
		size += CommandBase::copyBytes(buffer + size, ref, select);
		size += CommandData::copyBytes(buffer + size, ref);
		return size;
	}
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketNoData
////////////////////////////////////////////////////////////////////////////////

struct NetPacketNoData
{
	typedef void CommandMsg;

	static size_t getSize(const NetCommandMsg &) { return 0; }
	static size_t copyBytes(UnsignedByte *, const NetCommandRef &) { return 0; }
	static size_t readMessage(NetCommandRef &, NetPacketBuf) { return 0; }
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketAckCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketAckCommandData
{
	typedef NetAckCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort commandId; // Command ID being acknowledged
		UnsignedByte originalPlayerId; // Original player who sent the command
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketAckCommandBase
{
	typedef NetAckCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		//NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFrameCommandData
{
	typedef NetFrameCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort commandCount;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketFrameCommandBase
{
	typedef NetFrameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketPlayerLeaveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketPlayerLeaveCommandData
{
	typedef NetPlayerLeaveCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte leavingPlayerId;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketPlayerLeaveCommandBase
{
	typedef NetPlayerLeaveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadMetricsCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRunAheadMetricsCommandData
{
	typedef NetRunAheadMetricsCommandMsg CommandMsg;

	struct FixedData
	{
		Real averageLatency;
		UnsignedShort averageFps;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketRunAheadMetricsCommandBase
{
	typedef NetRunAheadMetricsCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRunAheadCommandData
{
	typedef NetRunAheadCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort runAhead;
		UnsignedByte frameRate;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketRunAheadCommandBase
{
	typedef NetRunAheadCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDestroyPlayerCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDestroyPlayerCommandData
{
	typedef NetDestroyPlayerCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt playerIndex;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDestroyPlayerCommandBase
{
	typedef NetDestroyPlayerCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketKeepAliveCommandBase
{
	typedef NetKeepAliveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectKeepAliveCommandBase
{
	typedef NetDisconnectKeepAliveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectPlayerCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectPlayerCommandData
{
	typedef NetDisconnectPlayerCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte disconnectSlot;
		UnsignedInt disconnectFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDisconnectPlayerCommandBase
{
	typedef NetDisconnectPlayerCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterQueryCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRouterQueryCommandBase
{
	typedef NetPacketRouterQueryCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterAckCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRouterAckCommandBase
{
	typedef NetPacketRouterAckCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectVoteCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectVoteCommandData
{
	typedef NetDisconnectVoteCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte slot;
		UnsignedInt voteFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDisconnectVoteCommandBase
{
	typedef NetDisconnectVoteCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketChatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketChatCommandData
{
	typedef NetChatCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketChatCommandBase
{
	typedef NetChatCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectChatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectChatCommandData
{
	typedef NetDisconnectChatCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDisconnectChatCommandBase
{
	typedef NetDisconnectChatCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketGameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketGameCommandData
{
	typedef NetGameCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketGameCommandBase
{
	typedef NetGameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketWrapperCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketWrapperCommandData
{
	typedef NetWrapperCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort wrappedCommandId;
		UnsignedInt chunkNumber;
		UnsignedInt numChunks;
		UnsignedInt totalDataLength;
		UnsignedInt dataLength;
		UnsignedInt dataOffset;
	};

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketWrapperCommandBase
{
	typedef NetWrapperCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileCommandData
{
	typedef NetFileCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketFileCommandBase
{
	typedef NetFileCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileAnnounceCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileAnnounceCommandData
{
	typedef NetFileAnnounceCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketFileAnnounceCommandBase
{
	typedef NetFileAnnounceCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileProgressCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileProgressCommandData
{
	typedef NetFileProgressCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort fileId;
		Int progress;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketFileProgressCommandBase
{
	typedef NetFileProgressCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketProgressCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketProgressCommandData
{
	typedef NetProgressCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte percentage;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketProgressCommandBase
{
	typedef NetProgressCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketLoadCompleteCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketLoadCompleteCommandBase
{
	typedef NetLoadCompleteCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketTimeOutGameStartCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketTimeOutGameStartCommandBase
{
	typedef NetTimeOutGameStartCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectFrameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectFrameCommandData
{
	typedef NetDisconnectFrameCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt disconnectFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDisconnectFrameCommandBase
{
	typedef NetDisconnectFrameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectScreenOffCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectScreenOffCommandData
{
	typedef NetDisconnectScreenOffCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt newFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketDisconnectScreenOffCommandBase
{
	typedef NetDisconnectScreenOffCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameResendRequestCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFrameResendRequestCommandData
{
	typedef NetFrameResendRequestCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt frameToResend;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
	static size_t readMessage(NetCommandRef &ref, NetPacketBuf buf);
};

struct NetPacketFrameResendRequestCommandBase
{
	typedef NetFrameResendRequestCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		//NetPacketFrameField frame;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////

struct NetPacketAckCommand                      : public NetPacketCommandTemplate<NetPacketAckCommandBase, NetPacketAckCommandData> {};
struct NetPacketFrameCommand                    : public NetPacketCommandTemplate<NetPacketFrameCommandBase, NetPacketFrameCommandData> {};
struct NetPacketPlayerLeaveCommand              : public NetPacketCommandTemplate<NetPacketPlayerLeaveCommandBase, NetPacketPlayerLeaveCommandData> {};
struct NetPacketRunAheadMetricsCommand          : public NetPacketCommandTemplate<NetPacketRunAheadMetricsCommandBase, NetPacketRunAheadMetricsCommandData> {};
struct NetPacketRunAheadCommand                 : public NetPacketCommandTemplate<NetPacketRunAheadCommandBase, NetPacketRunAheadCommandData> {};
struct NetPacketDestroyPlayerCommand            : public NetPacketCommandTemplate<NetPacketDestroyPlayerCommandBase, NetPacketDestroyPlayerCommandData> {};
struct NetPacketKeepAliveCommand                : public NetPacketCommandTemplate<NetPacketKeepAliveCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectKeepAliveCommand      : public NetPacketCommandTemplate<NetPacketDisconnectKeepAliveCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectPlayerCommand         : public NetPacketCommandTemplate<NetPacketDisconnectPlayerCommandBase, NetPacketDisconnectPlayerCommandData> {};
struct NetPacketRouterQueryCommand              : public NetPacketCommandTemplate<NetPacketRouterQueryCommandBase, NetPacketNoData> {};
struct NetPacketRouterAckCommand                : public NetPacketCommandTemplate<NetPacketRouterAckCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectVoteCommand           : public NetPacketCommandTemplate<NetPacketDisconnectVoteCommandBase, NetPacketDisconnectVoteCommandData> {};
struct NetPacketChatCommand                     : public NetPacketCommandTemplate<NetPacketChatCommandBase, NetPacketChatCommandData> {};
struct NetPacketDisconnectChatCommand           : public NetPacketCommandTemplate<NetPacketDisconnectChatCommandBase, NetPacketDisconnectChatCommandData> {};
struct NetPacketGameCommand                     : public NetPacketCommandTemplate<NetPacketGameCommandBase, NetPacketGameCommandData> {};
struct NetPacketWrapperCommand                  : public NetPacketCommandTemplate<NetPacketWrapperCommandBase, NetPacketWrapperCommandData> {};
struct NetPacketFileCommand                     : public NetPacketCommandTemplate<NetPacketFileCommandBase, NetPacketFileCommandData> {};
struct NetPacketFileAnnounceCommand             : public NetPacketCommandTemplate<NetPacketFileAnnounceCommandBase, NetPacketFileAnnounceCommandData> {};
struct NetPacketFileProgressCommand             : public NetPacketCommandTemplate<NetPacketFileProgressCommandBase, NetPacketFileProgressCommandData> {};
struct NetPacketProgressCommand                 : public NetPacketCommandTemplate<NetPacketProgressCommandBase, NetPacketProgressCommandData> {};
struct NetPacketLoadCompleteCommand             : public NetPacketCommandTemplate<NetPacketLoadCompleteCommandBase, NetPacketNoData> {};
struct NetPacketTimeOutGameStartCommand         : public NetPacketCommandTemplate<NetPacketTimeOutGameStartCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectFrameCommand          : public NetPacketCommandTemplate<NetPacketDisconnectFrameCommandBase, NetPacketDisconnectFrameCommandData> {};
struct NetPacketDisconnectScreenOffCommand      : public NetPacketCommandTemplate<NetPacketDisconnectScreenOffCommandBase, NetPacketDisconnectScreenOffCommandData> {};
struct NetPacketFrameResendRequestCommand       : public NetPacketCommandTemplate<NetPacketFrameResendRequestCommandBase, NetPacketFrameResendRequestCommandData> {};

struct SmallNetPacketAckCommand                 : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketAckCommandData> {};
struct SmallNetPacketFrameCommand               : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFrameCommandData> {};
struct SmallNetPacketPlayerLeaveCommand         : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketPlayerLeaveCommandData> {};
struct SmallNetPacketRunAheadMetricsCommand     : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketRunAheadMetricsCommandData> {};
struct SmallNetPacketRunAheadCommand            : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketRunAheadCommandData> {};
struct SmallNetPacketDestroyPlayerCommand       : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDestroyPlayerCommandData> {};
struct SmallNetPacketKeepAliveCommand           : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectKeepAliveCommand : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectPlayerCommand    : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectPlayerCommandData> {};
struct SmallNetPacketRouterQueryCommand         : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketRouterAckCommand           : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectVoteCommand      : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectVoteCommandData> {};
struct SmallNetPacketChatCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketChatCommandData> {};
struct SmallNetPacketDisconnectChatCommand      : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectChatCommandData> {};
struct SmallNetPacketGameCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketGameCommandData> {};
struct SmallNetPacketWrapperCommand             : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketWrapperCommandData> {};
struct SmallNetPacketFileCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileCommandData> {};
struct SmallNetPacketFileAnnounceCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileAnnounceCommandData> {};
struct SmallNetPacketFileProgressCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileProgressCommandData> {};
struct SmallNetPacketProgressCommand            : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketProgressCommandData> {};
struct SmallNetPacketLoadCompleteCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketTimeOutGameStartCommand    : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectFrameCommand     : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectFrameCommandData> {};
struct SmallNetPacketDisconnectScreenOffCommand : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectScreenOffCommandData> {};
struct SmallNetPacketFrameResendRequestCommand  : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFrameResendRequestCommandData> {};

// Restore normal struct packing
#pragma pack(pop)

// TheSuperHackers @feature M2 (plan10) The packet structs are wire formats (packed to 1 byte): the sizes are the Windows x64
// sizes and must be the same on every compiler and platform. A changed number here is a changed protocol.
static_assert(sizeof(NetPacketCommandTypeField) == 2, "NetPacketCommandTypeField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRelayField) == 2, "NetPacketRelayField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameField) == 5, "NetPacketFrameField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketPlayerIdField) == 2, "NetPacketPlayerIdField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketCommandIdField) == 3, "NetPacketCommandIdField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDataField) == 1, "NetPacketDataField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRepeatField) == 1, "NetPacketRepeatField is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRepeatCommand) == 1, "NetPacketRepeatCommand is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRepeatCommand::CommandBase) == 1, "NetPacketRepeatCommand::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(SmallNetPacketCommandBase) == 1, "SmallNetPacketCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(SmallNetPacketCommandBase::CommandBase) == 15, "SmallNetPacketCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketNoData) == 1, "NetPacketNoData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketAckCommandData) == 1, "NetPacketAckCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketAckCommandData::FixedData) == 3, "NetPacketAckCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketAckCommandBase) == 1, "NetPacketAckCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketAckCommandBase::CommandBase) == 5, "NetPacketAckCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameCommandData) == 1, "NetPacketFrameCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameCommandData::FixedData) == 2, "NetPacketFrameCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameCommandBase) == 1, "NetPacketFrameCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameCommandBase::CommandBase) == 15, "NetPacketFrameCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketPlayerLeaveCommandData) == 1, "NetPacketPlayerLeaveCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketPlayerLeaveCommandData::FixedData) == 1, "NetPacketPlayerLeaveCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketPlayerLeaveCommandBase) == 1, "NetPacketPlayerLeaveCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketPlayerLeaveCommandBase::CommandBase) == 15, "NetPacketPlayerLeaveCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadMetricsCommandData) == 1, "NetPacketRunAheadMetricsCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadMetricsCommandData::FixedData) == 6, "NetPacketRunAheadMetricsCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadMetricsCommandBase) == 1, "NetPacketRunAheadMetricsCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadMetricsCommandBase::CommandBase) == 10, "NetPacketRunAheadMetricsCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadCommandData) == 1, "NetPacketRunAheadCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadCommandData::FixedData) == 3, "NetPacketRunAheadCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadCommandBase) == 1, "NetPacketRunAheadCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRunAheadCommandBase::CommandBase) == 15, "NetPacketRunAheadCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDestroyPlayerCommandData) == 1, "NetPacketDestroyPlayerCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDestroyPlayerCommandData::FixedData) == 4, "NetPacketDestroyPlayerCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDestroyPlayerCommandBase) == 1, "NetPacketDestroyPlayerCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDestroyPlayerCommandBase::CommandBase) == 15, "NetPacketDestroyPlayerCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketKeepAliveCommandBase) == 1, "NetPacketKeepAliveCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketKeepAliveCommandBase::CommandBase) == 7, "NetPacketKeepAliveCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectKeepAliveCommandBase) == 1, "NetPacketDisconnectKeepAliveCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectKeepAliveCommandBase::CommandBase) == 7, "NetPacketDisconnectKeepAliveCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectPlayerCommandData) == 1, "NetPacketDisconnectPlayerCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectPlayerCommandData::FixedData) == 5, "NetPacketDisconnectPlayerCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectPlayerCommandBase) == 1, "NetPacketDisconnectPlayerCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectPlayerCommandBase::CommandBase) == 10, "NetPacketDisconnectPlayerCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRouterQueryCommandBase) == 1, "NetPacketRouterQueryCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRouterQueryCommandBase::CommandBase) == 7, "NetPacketRouterQueryCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRouterAckCommandBase) == 1, "NetPacketRouterAckCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketRouterAckCommandBase::CommandBase) == 7, "NetPacketRouterAckCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectVoteCommandData) == 1, "NetPacketDisconnectVoteCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectVoteCommandData::FixedData) == 5, "NetPacketDisconnectVoteCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectVoteCommandBase) == 1, "NetPacketDisconnectVoteCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectVoteCommandBase::CommandBase) == 10, "NetPacketDisconnectVoteCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketChatCommandData) == 1, "NetPacketChatCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketChatCommandBase) == 1, "NetPacketChatCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketChatCommandBase::CommandBase) == 15, "NetPacketChatCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectChatCommandData) == 1, "NetPacketDisconnectChatCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectChatCommandBase) == 1, "NetPacketDisconnectChatCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectChatCommandBase::CommandBase) == 7, "NetPacketDisconnectChatCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketGameCommandData) == 1, "NetPacketGameCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketGameCommandBase) == 1, "NetPacketGameCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketGameCommandBase::CommandBase) == 15, "NetPacketGameCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketWrapperCommandData) == 1, "NetPacketWrapperCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketWrapperCommandData::FixedData) == 22, "NetPacketWrapperCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketWrapperCommandBase) == 1, "NetPacketWrapperCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketWrapperCommandBase::CommandBase) == 10, "NetPacketWrapperCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileCommandData) == 1, "NetPacketFileCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileCommandBase) == 1, "NetPacketFileCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileCommandBase::CommandBase) == 10, "NetPacketFileCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileAnnounceCommandData) == 1, "NetPacketFileAnnounceCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileAnnounceCommandBase) == 1, "NetPacketFileAnnounceCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileAnnounceCommandBase::CommandBase) == 10, "NetPacketFileAnnounceCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileProgressCommandData) == 1, "NetPacketFileProgressCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileProgressCommandData::FixedData) == 6, "NetPacketFileProgressCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileProgressCommandBase) == 1, "NetPacketFileProgressCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFileProgressCommandBase::CommandBase) == 10, "NetPacketFileProgressCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketProgressCommandData) == 1, "NetPacketProgressCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketProgressCommandData::FixedData) == 1, "NetPacketProgressCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketProgressCommandBase) == 1, "NetPacketProgressCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketProgressCommandBase::CommandBase) == 7, "NetPacketProgressCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketLoadCompleteCommandBase) == 1, "NetPacketLoadCompleteCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketLoadCompleteCommandBase::CommandBase) == 10, "NetPacketLoadCompleteCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketTimeOutGameStartCommandBase) == 1, "NetPacketTimeOutGameStartCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketTimeOutGameStartCommandBase::CommandBase) == 10, "NetPacketTimeOutGameStartCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectFrameCommandData) == 1, "NetPacketDisconnectFrameCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectFrameCommandData::FixedData) == 4, "NetPacketDisconnectFrameCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectFrameCommandBase) == 1, "NetPacketDisconnectFrameCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectFrameCommandBase::CommandBase) == 10, "NetPacketDisconnectFrameCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandData) == 1, "NetPacketDisconnectScreenOffCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandData::FixedData) == 4, "NetPacketDisconnectScreenOffCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandBase) == 1, "NetPacketDisconnectScreenOffCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandBase::CommandBase) == 10, "NetPacketDisconnectScreenOffCommandBase::CommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameResendRequestCommandData) == 1, "NetPacketFrameResendRequestCommandData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameResendRequestCommandData::FixedData) == 4, "NetPacketFrameResendRequestCommandData::FixedData is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameResendRequestCommandBase) == 1, "NetPacketFrameResendRequestCommandBase is a wire format: its size must be the same on every platform");
static_assert(sizeof(NetPacketFrameResendRequestCommandBase::CommandBase) == 10, "NetPacketFrameResendRequestCommandBase::CommandBase is a wire format: its size must be the same on every platform");
