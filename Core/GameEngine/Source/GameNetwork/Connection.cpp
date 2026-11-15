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


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/Connection.h"
#include "GameNetwork/networkutil.h"
#include "GameNetwork/NetLab.h"
#include "GameLogic/GameLogic.h"

enum { MaxQuitFlushTime = 30000 }; // wait this many milliseconds at most to retry things before quitting

// TheSuperHackers @tweak O23 (item 18) Loss handling of the game connections. A lost packet used to cost the whole
// lockstep a stall of about the retry time, which was fixed at 2 seconds (NetLab S19: 85 to 100 % of the stalled time
// under 80 or 200 ms delay with 1 or 3 % loss followed a dropped packet). Now
// - every command that needs an ack and is frame-synchronised (frame info, game command, run-ahead, player leave and
//   destroy) also rides along in the next ExtraCopies packets to that connection while it is not acked, if it fits in a
//   packet that goes out anyway or in a new one (it is small): a single lost packet costs one packet interval;
// - the retry timer follows the measured round trip (smoothed, plus four mean deviations, between RetryTimeMin and the
//   old 2 seconds), and backs off (doubles once per send round that had to retry, up to 2 seconds, until the next clean
//   round trip) so a frozen peer is not flooded and a sudden rise of the delay is learned.
// The receiver already ignores a command it has (FrameData::addCommand, ConnectionManager::processNetCommand for an old
// frame) and acks every copy, so the wire format and the lockstep are unchanged; only the sending side decides more.
enum
{
	MaxStaleRetries = 5,	// O35: see doSend
	ExtraCopies = 2,
	RetryTimeStart = 1000,	// until the first round trip was measured
	RetryTimeMin = 100,
	RetryTimeMax = 2000,
};

static Bool isCopiedCommand(const NetCommandMsg *msg)
{
	return CommandRequiresAck(msg) && IsCommandSynchronized(msg->getNetCommandType());
}

/**
 * The constructor.
 */
Connection::Connection() {
	m_transport = nullptr;
	m_user = nullptr;
	m_netCommandList = nullptr;
	m_retryTime = RetryTimeStart; // O23: was 2 seconds for ever; now until the first round trip is measured
	m_smoothedRtt = 0.0f;
	m_rttDeviation = 0.0f;
	m_haveRtt = FALSE;
	m_numRetries = 0;
	m_retryMetricsTime = 0;
	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_isQuitting = false;
	m_quitTime = 0;
	m_averageLatency = 0.0f;
	Int i;
	for(i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; i++)
	{
		m_latencies[i] = 0.0f;
	}
}

/**
 * The destructor.
 */
Connection::~Connection() {
	deleteInstance(m_user);
	m_user = nullptr;

	deleteInstance(m_netCommandList);
	m_netCommandList = nullptr;
}

/**
 * Initialize the connection and any subsystems.
 */
void Connection::init() {
	m_transport = nullptr;

	deleteInstance(m_user);
	m_user = nullptr;

	if (m_netCommandList == nullptr) {
		m_netCommandList = newInstance(NetCommandList);
		m_netCommandList->init();
	}
	m_netCommandList->reset();

	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_numRetries = 0;
	m_retryMetricsTime = 0;
	m_retryTime = RetryTimeStart;
	m_smoothedRtt = 0.0f;
	m_rttDeviation = 0.0f;
	m_haveRtt = FALSE;

	for (Int i = 0; i < CONNECTION_LATENCY_HISTORY_LENGTH; ++i) {
		m_latencies[i] = 0;
	}
	m_averageLatency = 0;
	m_isQuitting = FALSE;
	m_quitTime = 0;
}

/**
 * Take the connection back to the initial state.
 */
void Connection::reset() {
	init();
}

/**
 * Doesn't really do anything.
 */
void Connection::update() {
}

/**
 * Attach the transport object that this connection should use.
 */
void Connection::attachTransport(Transport *transport) {
	m_transport = transport;
}

/**
 * Assign this connection a user.  This is the user to whome we send all our packetized goodies.
 */
void Connection::setUser(User *user) {
	deleteInstance(m_user);
	m_user = user;
}

/**
 * Return the user object.
 */
User * Connection::getUser() {
	return m_user;
}

/**
 * Add this network command to the send queue for this connection.
 * The relay is the mask specifying the people the person we are sending to should send to.
 * The relay mostly has to do with the packet router.
 */
void Connection::sendNetCommandMsg(NetCommandMsg *msg, UnsignedByte relay) {
	if (m_isQuitting)
		return;

	if (m_netCommandList != nullptr) {
		NetPacket packet;

		// check to see if this command will fit in a packet.  If not, we need to split it up.
		// we are splitting up the command here so that the retry logic will not try to
		// resend the ENTIRE command (i.e. multiple packets work of data) and only do the retry
		// one wrapper command at a time.

		NetCommandRef *tempref = NEW_NETCOMMANDREF(msg);
		if (packet.addCommand(tempref)) {
			deleteInstance(tempref);
			tempref = nullptr;
		} else {
			tempref->setRelay(relay);

			// the message doesn't fit in a single packet, need to split it up.
			if (NetCommandList* list = NetPacket::ConstructBigCommandList(tempref)) {
				for (NetCommandRef* ref1 = list->getFirstMessage(); ref1 != nullptr; ref1 = ref1->getNext()) {
					if (NetCommandRef* ref2 = m_netCommandList->addMessage(ref1->getCommand())) {
						ref2->setRelay(relay);
					}
				}

				deleteInstance(list);
				list = nullptr;
			}

			deleteInstance(tempref);
			tempref = nullptr;

			return;
		}

		// the message fits in a packet, add to the command list normally.
		NetCommandRef *ref = m_netCommandList->addMessage(msg);

		if (ref != nullptr) {

/*
#if defined(RTS_DEBUG)
			if (msg->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added game command %d to net command list for frame %d.",
					msg->getID(), msg->getExecutionFrame()));
			} else if (msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added frame info for frame %d", msg->getExecutionFrame()));
			}
#endif // RTS_DEBUG
*/

			ref->setRelay(relay);
		}
	}
}

void Connection::clearCommandsExceptFrom( Int playerIndex )
{
	NetCommandRef *tmp = m_netCommandList->getFirstMessage();
	while (tmp)
	{
		NetCommandRef *next = tmp->getNext();
		NetCommandMsg *msg = tmp->getCommand();

		if (msg->getPlayerID() != playerIndex)
		{
			DEBUG_LOG(("Connection::clearCommandsExceptFrom(%d) - clearing a command from player %d for frame %d",
				playerIndex, tmp->getCommand()->getPlayerID(), tmp->getCommand()->getExecutionFrame()));

			m_netCommandList->removeMessage(tmp);
			deleteInstance(tmp);
		}

		tmp = next;
	}
}

Bool Connection::isQueueEmpty() {
	if (m_netCommandList->getFirstMessage() == nullptr) {
		return TRUE;
	}
	return FALSE;
}

void Connection::setQuitting()
{
	m_isQuitting = TRUE;
	m_quitTime = timeGetTime();
	DEBUG_LOG(("Connection::setQuitting() at time %d", m_quitTime));
}

/**
 * This is the good part. We take all the network commands queued up for this connection,
 * packetize them and put them on the transport's send queue for actual sending.
 */
UnsignedInt Connection::doSend() {
	Int numpackets = 0;
	time_t curtime = timeGetTime();
	Bool couldQueue = TRUE;

	// Do this check first, since it's an important fail-safe
	if (m_isQuitting && curtime > m_quitTime + MaxQuitFlushTime)
	{
		DEBUG_LOG(("Timed out a quitting connection.  Deleting all %d messages", m_netCommandList->length()));
		m_netCommandList->reset();
		return 0;
	}

	if ((curtime - m_lastTimeSent) < m_frameGrouping) {
//		DEBUG_LOG(("not sending packet, time = %d, m_lastFrameSent = %d, m_frameGrouping = %d", curtime, m_lastTimeSent, m_frameGrouping));
		return 0;
	}

	// iterate through all the messages and put them into a packet(s).
	NetCommandRef *msg = m_netCommandList->getFirstMessage();
	Bool backedOff = FALSE;	// O23

	while ((msg != nullptr) && couldQueue) {
		NetPacket packet;
		packet.setAddress(m_user->GetIPAddr(), m_user->GetPort());

		Bool notDone = TRUE;

		// add the command messages until either we run out of messages or the packet is full.
		while ((msg != nullptr) && notDone) {
			NetCommandRef *next = msg->getNext(); // Need this since msg could be deleted

			time_t timeLastSent = msg->getTimeLastSent();

			// TheSuperHackers @bugfix O35 A frame-synchronised command for a frame more than MAX_FRAMES_AHEAD behind this
			// computer's logic, that the timer already sent MaxStaleRetries times without an ack, is given up: no player can
			// still wait for it (the lockstep keeps every player within the run-ahead of the others; a player that lost it
			// asks for the frame again). Frame data resent after a request is the only such case; without an ack it used
			// to be sent again every 2 s for the rest of the match. So no command is retried for ever.
			if ((timeLastSent != -1) && (msg->getTimerRetries() >= MaxStaleRetries) && IsCommandSynchronized(msg->getCommand()->getNetCommandType())
				&& (TheGameLogic != nullptr) && (msg->getCommand()->getExecutionFrame() + MAX_FRAMES_AHEAD < TheGameLogic->getFrame())) {
				m_netCommandList->removeMessage(msg);
				deleteInstance(msg);
				msg = next;
				continue;
			}
			if (((curtime - timeLastSent) > m_retryTime) || (timeLastSent == -1)) {
				notDone = packet.addCommand(msg);
				if (notDone) {
					// the msg command was added to the packet.
					if (CommandRequiresAck(msg->getCommand())) {
						if (timeLastSent != -1) {
							++m_numRetries;
							NetLab::countRetry();   // TheSuperHackers @feature NetLab measurement (a counter only)
							msg->noteTimerRetry();
							// O23: back-off, once per send round with a retry: the retry time doubles (up to RetryTimeMax)
							// until a clean round trip sets it again. Without it a jump of the line's delay above the
							// retry time would leave only retried commands, whose acks give no sample (Karn), and the
							// retry time (and the run-ahead's latency) would never learn the new delay.
							if (!backedOff) {
								m_retryTime = (m_retryTime * 2 > RetryTimeMax) ? (time_t)RetryTimeMax : m_retryTime * 2;
								backedOff = TRUE;
								NetLab::noteRetryTime((Int)m_retryTime);
							}
						} else {
							msg->setTimeFirstSent(curtime);
						}
						doRetryMetrics();
						msg->setTimeLastSent(curtime);
					} else {
						m_netCommandList->removeMessage(msg);
						deleteInstance(msg);
					}
				}
			} else if ((msg->getCopiesSent() < ExtraCopies) && (timeLastSent != curtime) && (msg->getTimeLastCopy() != curtime)
				&& isCopiedCommand(msg->getCommand())) {
				// O23: an extra copy of a command sent in an earlier packet and not acked yet. It never ends the packet: if it
				// does not fit, the next commands still get their chance and the copy waits for the next packet.
				if (packet.addCommand(msg)) {
					msg->noteCopySent(curtime);
					NetLab::countCopy();
				}
			}
			msg = next;
		}

		if (msg != nullptr) {
			DEBUG_LOG(("didn't finish sending all commands in connection"));
		}

		++numpackets;

		/// @todo Make the act of giving the transport object a packet to send more efficient.  Make the transport take a NetPacket object rather than the raw data, thus avoiding an extra memcpy.
		if (packet.getNumCommands() > 0) {
			// If the packet actually has any information to give, give it to the transport object
			// for transmission.
			couldQueue = m_transport->queueSend(packet.getAddr(), packet.getPort(), packet.getData(), packet.getLength());
			m_lastTimeSent = curtime;
		}
	}

	return numpackets;
}

NetCommandRef * Connection::processAck(NetAckStage1CommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetAckBothCommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetCommandMsg *msg) {
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) {
		NetAckStage1CommandMsg *ackmsg = (NetAckStage1CommandMsg *)msg;
		return processAck(ackmsg);
	}

	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		NetAckBothCommandMsg *ackmsg = (NetAckBothCommandMsg *)msg;
		return processAck(ackmsg);
	}

	return nullptr;
}

/**
 * The person we are sending to has ack'd one of the messages we sent him.
 * Take that message off the list of commands to send.
 */
NetCommandRef * Connection::processAck(UnsignedShort commandID, UnsignedByte originalPlayerID) {
	NetCommandRef *temp = m_netCommandList->getFirstMessage();
	while ((temp != nullptr) && ((temp->getCommand()->getID() != commandID) || (temp->getCommand()->getPlayerID() != originalPlayerID))) {

		// cycle through the commands till we find the one we need to remove.
		// Need to check for both the command ID and the player ID.
		temp = temp->getNext();
	}
	if (temp == nullptr) {
		return nullptr;
	}

#if defined(RTS_DEBUG)
	Bool doDebug = FALSE;
	if (temp->getCommand()->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) {
		doDebug = TRUE;
	}
#endif

	const time_t now = timeGetTime();
	Int index = temp->getCommand()->getID() % CONNECTION_LATENCY_HISTORY_LENGTH;
	m_averageLatency -= ((Real)(m_latencies[index])) / CONNECTION_LATENCY_HISTORY_LENGTH;
	Real lat = now - temp->getTimeLastSent();
	m_averageLatency += lat / CONNECTION_LATENCY_HISTORY_LENGTH;
	m_latencies[index] = lat;

	// O23: a round trip sample for the retry time, from the first send (an extra copy that arrived instead counts with
	// its delay, which is what a command really needs), but not from a command the retry timer sent again: its ack may
	// belong to any of the sends (Karn).
	if (!temp->wasRetriedByTimer() && temp->getTimeFirstSent() != -1) {
		updateRetryTime((Real)(now - temp->getTimeFirstSent()));
	}

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG(("Connection::processAck - disconnect frame command %d found, removing from command list.", commandID));
	}
#endif
	m_netCommandList->removeMessage(temp);
	return temp;
}

void Connection::setFrameGrouping(time_t frameGrouping) {
	m_frameGrouping = frameGrouping;
//	m_retryTime = frameGrouping * 4;
}

/**
 * O23: a new round trip sample (milliseconds) for the retry time: smoothed as TCP does (1/8 of the error into the mean,
 * 1/4 into the mean deviation), retry time = mean + 4 deviations, at least RetryTimeMin and one packet interval more than
 * the mean, at most RetryTimeMax.
 */
void Connection::updateRetryTime(Real sampleMs) {
	if (sampleMs < 0.0f) {
		return;
	}
	if (!m_haveRtt) {
		m_smoothedRtt = sampleMs;
		m_rttDeviation = sampleMs / 2.0f;
		m_haveRtt = TRUE;
	} else {
		const Real err = sampleMs - m_smoothedRtt;
		m_smoothedRtt += err / 8.0f;
		m_rttDeviation += (fabsf(err) - m_rttDeviation) / 4.0f;
	}
	Real retry = m_smoothedRtt + 4.0f * m_rttDeviation;
	Real lowest = m_smoothedRtt + (Real)m_frameGrouping;
	if (lowest < (Real)RetryTimeMin) {
		lowest = (Real)RetryTimeMin;
	}
	if (retry < lowest) {
		retry = lowest;
	}
	if (retry > (Real)RetryTimeMax) {
		retry = (Real)RetryTimeMax;
	}
	m_retryTime = (time_t)retry;
	NetLab::noteRetryTime((Int)m_retryTime);
}

void Connection::doRetryMetrics() {
	static Int numSeconds = 0;
	time_t curTime = timeGetTime();

	if ((curTime - m_retryMetricsTime) > 10000) {
		m_retryMetricsTime = curTime;
		++numSeconds;
//		DEBUG_LOG(("Retries in the last 10 seconds = %d, average latency = %fms", m_numRetries, m_averageLatency));
		m_numRetries = 0;
//		m_retryTime = m_averageLatency * 1.5;
	}
}

#if defined(RTS_DEBUG)
void Connection::debugPrintCommands() {
	NetCommandRef *ref = m_netCommandList->getFirstMessage();
	while (ref != nullptr) {
		DEBUG_LOG(("Connection::debugPrintCommands - ID: %d\tType: %s\tRelay: 0x%X for frame %d",
			ref->getCommand()->getID(), GetNetCommandTypeAsString(ref->getCommand()->getNetCommandType()),
			ref->getRelay(), ref->getCommand()->getExecutionFrame()));
		ref = ref->getNext();
	}
}
#endif
