/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "server/Server.h"
#include <algorithm>
#include <array>
#include <cstdint>

#include "server/ClientProxy.h"
#include "server/ClientProxyUnknown.h"
#include "server/PrimaryClient.h"
#include "server/ClientListener.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/DropHelper.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/XScreen.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/KeyState.h"
#include "barrier/Screen.h"
#include "barrier/PacketStreamFilter.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/XSocket.h"
#include "mt/Thread.h"
#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"

#if !defined(_WIN32)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#endif

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <ctime>
#include <stdexcept>

namespace {
static bool wildcardMatch(const std::string& pattern, const std::string& str)
{
	if (pattern.empty() || pattern == "*") {
		return true;
	}
	size_t p = 0, s = 0;
	size_t starIdx = std::string::npos, match = 0;
	while (s < str.size()) {
		if (p < pattern.size() && (pattern[p] == '?' || tolower(pattern[p]) == tolower(str[s]))) {
			p++;
			s++;
		} else if (p < pattern.size() && pattern[p] == '*') {
			starIdx = p;
			match = s;
			p++;
		} else if (starIdx != std::string::npos) {
			p = starIdx + 1;
			match++;
			s = match;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}
}

//
// Server
//

Server::Server(
		Config& config,
		PrimaryClient* primaryClient,
		barrier::Screen* screen,
		IEventQueue* events,
		ServerArgs const& args) :
	m_mock(false),
	m_primaryClient(primaryClient),
	m_active(primaryClient),
	m_seqNum(0),
	m_xDelta(0),
	m_yDelta(0),
	m_xDelta2(0),
	m_yDelta2(0),
	m_config(&config),
	m_inputFilter(config.getInputFilter()),
	m_activeSaver(nullptr),
	m_switchDir(kNoDirection),
	m_switchScreen(nullptr),
	m_switchWaitDelay(0.0),
	m_switchWaitTimer(nullptr),
	m_switchTwoTapDelay(0.0),
	m_switchTwoTapEngaged(false),
	m_switchTwoTapArmed(false),
	m_switchTwoTapZone(3),
	m_switchNeedsShift(false),
	m_switchNeedsControl(false),
	m_switchNeedsAlt(false),
	m_relativeMoves(false),
	m_keyboardBroadcasting(false),
	m_lockedToScreen(false),
	m_screen(screen),
	m_events(events),
	m_sendFileThread(nullptr),
	m_writeToDropDirThread(nullptr),
	m_ignoreFileTransfer(false),
	m_enableClipboard(true),
	m_sendDragInfoThread(nullptr),
	m_waitDragInfoThread(true),
	m_args(args)
{
	// must have a primary client and it must have a canonical name
	assert(m_primaryClient != NULL);
	assert(config.isScreen(primaryClient->getName()));
	assert(m_screen != NULL);

    std::string primaryName = getName(primaryClient);

	// clear clipboards
	for (auto & clipboard : m_clipboards) {
			clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			(void)clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData   = clipboard.m_clipboard.marshall();
	}

	// install event handlers
	m_events->adoptHandler(Event::kTimer, this,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchWaitTimeout));
	m_events->adoptHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyDownEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyUpEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyRepeatEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonDownEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonUpEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionPrimaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionSecondaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleWheelEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverActivatedEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverDeactivatedEvent));
	m_events->adoptHandler(m_events->forServer().switchToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchToScreenEvent));
  m_events->adoptHandler(m_events->forServer().toggleScreen(),
              m_inputFilter,
              new TMethodEventJob<Server>(this,
                &Server::handleToggleScreenEvent));
	m_events->adoptHandler(m_events->forServer().switchInDirection(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchInDirectionEvent));
	m_events->adoptHandler(m_events->forServer().keyboardBroadcast(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyboardBroadcastEvent));
	m_events->adoptHandler(m_events->forServer().lockCursorToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleLockCursorToScreenEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputBeginEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputEndEvent));

	if (m_args.m_enableDragDrop) {
		m_events->adoptHandler(m_events->forFile().fileChunkSending(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileChunkSendingEvent));
		m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileRecieveCompletedEvent));
	}

	// add connection
	addClient(m_primaryClient);

	// set initial configuration
	setConfig(config);

	// enable primary client
	m_primaryClient->enable();
	m_inputFilter->setPrimaryClient(m_primaryClient);

	// Determine if scroll lock is already set. If so, lock the cursor to the primary screen
	if ((m_primaryClient->getToggleMask() & KeyModifierScrollLock) != 0u) {
		LOG((CLOG_NOTE "Scroll Lock is on, locking cursor to screen"));
		m_lockedToScreen = true;
	}

}

// NOLINTNEXTLINE(bugprone-exception-escape)
Server::~Server()
{
	// Always remove event handlers, even in mock mode — the TMethodEventJob
	// objects are heap-allocated and must be freed regardless.
	m_events->removeHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter);
	m_events->removeHandler(m_events->forServer().switchToScreen(),
							m_inputFilter);
	m_events->removeHandler(m_events->forServer().toggleScreen(),
							m_inputFilter);
	m_events->removeHandler(m_events->forServer().switchInDirection(),
							m_inputFilter);
	m_events->removeHandler(m_events->forServer().keyboardBroadcast(),
							m_inputFilter);
	m_events->removeHandler(m_events->forServer().lockCursorToScreen(),
							m_inputFilter);
	m_events->removeHandler(Event::kTimer, this);

	if (m_args.m_enableDragDrop) {
		m_events->removeHandler(m_events->forFile().fileChunkSending(), this);
		m_events->removeHandler(m_events->forFile().fileRecieveCompleted(), this);
	}

	stopSwitch();

	if (m_sendFileThread != nullptr) {
		StreamChunker::interruptFile();
		delete m_sendFileThread;
		m_sendFileThread = nullptr;
	}

	if (m_mock) {
		// Mock mode: disconnect() crashes because forceLeaveClient uses a mock
		// PrimaryClient without a real screen. Manually delete all secondary clients.
		
		// Create a copy of clients to safely iterate and remove them
		std::vector<BaseClientProxy*> clientsToDelete;
		for (auto & m_client : m_clients) {
			if (m_client.second != m_primaryClient) {
				clientsToDelete.push_back(m_client.second);
			}
		}
		
		for (BaseClientProxy* client : clientsToDelete) {
			removeClient(client);
			m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
			delete client;
		}
		
		for (auto & m_oldClient : m_oldClients) {
			BaseClientProxy* client = m_oldClient.first;
			if (m_oldClient.second) {
				m_events->deleteTimer(m_oldClient.second);
				m_events->removeHandler(Event::kTimer, client);
			}
			m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
			delete client;
		}

		// Primary client is a mock object — skip disable, but we must removeClient
		// to clean up its event handlers (shapeChanged, clipboardGrabbed, clipboardChanged).
		removeClient(m_primaryClient);
		return;
	}

	// force immediate disconnection of secondary clients (real network clients,
	// even in mock mode — their destructor removes event handlers).
	disconnect();
	for (auto & m_oldClient : m_oldClients) {
		BaseClientProxy* client = m_oldClient.first;
		m_events->deleteTimer(m_oldClient.second);
		m_events->removeHandler(Event::kTimer, client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		delete client;
	}

	// remove input filter
	m_inputFilter->setPrimaryClient(nullptr);

	// disable and disconnect primary client
	m_primaryClient->disable();
	removeClient(m_primaryClient);
}

bool
Server::setConfig(const Config& config)
{
	// refuse configuration if it doesn't include the primary screen
	if (!config.isScreen(m_primaryClient->getName())) {
		return false;
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	processOptions();

	// add ScrollLock as a hotkey to lock to the screen.  this was a
	// built-in feature in earlier releases and is now supported via
	// the user configurable hotkey mechanism.  if the user has already
	// registered ScrollLock for something else then that will win but
	// we will unfortunately generate a warning.  if the user has
	// configured a LockCursorToScreenAction then we don't add
	// ScrollLock as a hotkey.
	if (!m_config->hasLockToScreenAction()) {
		IPlatformScreen::KeyInfo* key =
			IPlatformScreen::KeyInfo::alloc(kKeyScrollLock, 0, 0, 0);
		InputFilter::Rule rule(new InputFilter::KeystrokeCondition(m_events, key));
		rule.adoptAction(new InputFilter::LockCursorToScreenAction(m_events), true);
		m_inputFilter->addFilterRule(rule);
	}

	// tell primary screen about reconfiguration
	m_primaryClient->reconfigure(getActivePrimarySides());

	// tell all (connected) clients about current options
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		sendOptions(client);
	}

	return true;
}

void
Server::adoptClient(BaseClientProxy* client)
{
	assert(client != NULL);

	// watch for client disconnection
	m_events->adoptHandler(m_events->forClientProxy().disconnected(), client,
							new TMethodEventJob<Server>(this,
								&Server::handleClientDisconnected, client));

	// name must be in our configuration
	if (!m_config->isScreen(client->getName())) {
		LOG((CLOG_WARN "unrecognised client name \"%s\", check server config", client->getName().c_str()));
		closeClient(client, kMsgEUnknown);
		return;
	}

	// add client to client list
	if (!addClient(client)) {
		// can only have one screen with a given name at any given time
		LOG((CLOG_WARN "a client with name \"%s\" is already connected", getName(client).c_str()));
		closeClient(client, kMsgEBusy);
		return;
	}
	LOG((CLOG_NOTE "client \"%s\" has connected", getName(client).c_str()));

	// send configuration options to client
	sendOptions(client);

	// send server physical addresses for dynamic failover
	auto* proxy = dynamic_cast<ClientProxy*>(client);
	if (proxy != nullptr) {
		sendServerAddresses(proxy);
	}

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != nullptr) {
		client->screensaver(true);
	}

	// send notification
	auto* info =
		new Server::ScreenConnectedInfo(getName(client));
	Event event(m_events->forServer().connected(),
				m_primaryClient->getEventTarget());
	event.setDataObject(info);
	m_events->addEvent(event);
}

void
Server::disconnect()
{
	// close all secondary clients
	if (m_clients.size() > 1 || !m_oldClients.empty()) {
		Config emptyConfig(m_events);
		closeClients(emptyConfig);
	}
	else {
		m_events->addEvent(Event(m_events->forServer().disconnected(), this));
	}
}

UInt32
Server::getNumClients() const
{
	return static_cast<SInt32>(m_clients.size());
}

void
Server::getClients(std::vector<std::string>& list) const
{
	list.clear();
	for (const auto & m_client : m_clients) {
		list.push_back(m_client.first);
	}
}

std::string Server::getName(const BaseClientProxy* client) const
{
    std::string name = m_config->getCanonicalName(client->getName());
	if (name.empty()) {
		name = client->getName();
	}
	return name;
}

UInt32
Server::getActivePrimarySides() const
{
	UInt32 sides = 0;
	if (!isLockedToScreenServer()) {
		if (hasAnyNeighbor(m_primaryClient, kLeft)) {
			sides |= kLeftMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kRight)) {
			sides |= kRightMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kTop)) {
			sides |= kTopMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kBottom)) {
			sides |= kBottomMask;
		}
	}
	return sides;
}

bool
Server::isLockedToScreenServer() const
{
	// locked if scroll-lock is toggled on
	return m_lockedToScreen;
}

bool
Server::isLockedToScreen() const
{
	// locked if we say we're locked
	if (isLockedToScreenServer()) {
		return true;
	}

	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// not locked
	return false;
}

SInt32
Server::getJumpZoneSize(BaseClientProxy* client) const
{
	if (client == m_primaryClient) {
		return m_primaryClient->getJumpZoneSize();
	}
			return 0;

}

void
Server::switchScreen(BaseClientProxy* dst,
				SInt32 x, SInt32 y, bool forScreensaver)
{
	assert(dst != NULL);

#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	LOG((CLOG_INFO "switch from \"%s\" to \"%s\" at %d,%d", getName(m_active).c_str(), getName(dst).c_str(), x, y));

	// stop waiting to switch
	stopSwitch();

	// record new position
	m_x       = x;
	m_y       = y;
	m_xDelta  = 0;
	m_yDelta  = 0;
	m_xDelta2 = 0;
	m_yDelta2 = 0;

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		// leave active screen
		if (!m_active->leave()) {
			// cannot leave screen
			LOG((CLOG_WARN "can't leave screen"));
			return;
		}

		// update the primary client's clipboards if we're leaving the
		// primary screen.
		if (m_active == m_primaryClient && m_enableClipboard) {
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				ClipboardInfo& clipboard = m_clipboards[id];
				if (clipboard.m_clipboardOwner == getName(m_primaryClient)) {
					onClipboardChanged(m_primaryClient,
						id, clipboard.m_clipboardSeqNum);
				}
			}
		}

		// cut over
		m_active = dst;

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);

		if (m_enableClipboard) {
			// send the clipboard data to new active screen
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				m_active->setClipboard(id, &m_clipboards[id].m_clipboard);
			}
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}
	else {
		m_active->mouseMove(x, y);
	}
}

void
Server::jumpToScreen(BaseClientProxy* newScreen)
{
	assert(newScreen != NULL);

	// record the current cursor position on the active screen
	m_active->setJumpCursorPos(m_x, m_y);

	// get the last cursor position on the target screen
	SInt32 x;
	SInt32 y;
	newScreen->getJumpCursorPos(x, y);

	// Ensure cursor does not land on a display claimed by an active standalone monitor
	std::vector<DisplayInfo> displays = newScreen->getDisplays();
	std::vector<DisplayInfo> hostDisplays;
	for (const auto& disp : displays) {
		bool claimed = false;
		for (const auto& kv : m_activeMonitors) {
			if (kv.second.m_host == newScreen) {
				if (kv.second.m_display.m_x == disp.m_x && kv.second.m_display.m_y == disp.m_y &&
				    kv.second.m_display.m_w == disp.m_w && kv.second.m_display.m_h == disp.m_h) {
					claimed = true;
					break;
				}
			}
		}
		if (!claimed) {
			hostDisplays.push_back(disp);
		}
	}

	bool insideHost = false;
	for (const auto& disp : hostDisplays) {
		if (x >= disp.m_x && x < disp.m_x + disp.m_w &&
		    y >= disp.m_y && y < disp.m_y + disp.m_h) {
			insideHost = true;
			break;
		}
	}

	if (!insideHost && !hostDisplays.empty()) {
		const DisplayInfo* targetDisp = &hostDisplays[0];
		for (const auto& disp : hostDisplays) {
			if (disp.m_isPrimary) {
				targetDisp = &disp;
				break;
			}
		}
		x = targetDisp->m_x + targetDisp->m_w / 2;
		y = targetDisp->m_y + targetDisp->m_h / 2;
	}

	switchScreen(newScreen, x, y, false);
}

void
Server::jumpToScreen(BaseClientProxy* newScreen, SInt32 x, SInt32 y)
{
	assert(newScreen != NULL);

	// record the current cursor position on the active screen
	m_active->setJumpCursorPos(m_x, m_y);

	switchScreen(newScreen, x, y, false);
}

void
Server::updateActiveMonitors()
{
	m_activeMonitors.clear();
	const auto& monConfigs = m_config->getMonitors();
	if (monConfigs.empty()) {
		return;
	}

	for (const auto& pair : monConfigs) {
		const Config::MonitorConfig& cfg = pair.second;
		bool found = false;

		for (BaseClientProxy* client : m_clientSet) {
			if (client == nullptr) continue;
			std::vector<DisplayInfo> displays = client->getDisplays();
			for (const auto& disp : displays) {
				bool matched = false;
				if (!cfg.matchPattern.empty()) {
					if (wildcardMatch(cfg.matchPattern, disp.m_name) ||
					    wildcardMatch(cfg.matchPattern, disp.m_id)) {
						matched = true;
					}
				}
				else if (wildcardMatch(cfg.name, disp.m_name) ||
				         wildcardMatch(cfg.name, disp.m_id)) {
					matched = true;
				}

				if (matched) {
					m_activeMonitors[cfg.name] = ActiveMonitor(cfg.name, client, disp);
					LOG((CLOG_NOTE "Monitor '%s' (%s, id=%s) matched to client '%s' at (%d,%d %dx%d)",
						cfg.name.c_str(), disp.m_name.c_str(), disp.m_id.c_str(), getName(client).c_str(),
						disp.m_x, disp.m_y, disp.m_w, disp.m_h));
					found = true;
					break;
				}
			}
			if (found) break;
		}

		if (!found) {
			LOG((CLOG_DEBUG1 "Monitor '%s' not active on any connected client", cfg.name.c_str()));
		}
	}
}

bool
Server::getActiveMonitorShape(BaseClientProxy* client, SInt32 x, SInt32 y,
                              SInt32& ax, SInt32& ay, SInt32& aw, SInt32& ah,
                              std::string& monitorName) const
{
	if (client == nullptr) {
		return false;
	}

	std::vector<DisplayInfo> displays = client->getDisplays();
	if (!displays.empty()) {
		auto resolveMonName = [this, client](const DisplayInfo& disp) -> std::string {
			for (const auto& kv : m_activeMonitors) {
				if (kv.second.m_host == client &&
				    kv.second.m_display.m_x == disp.m_x &&
				    kv.second.m_display.m_y == disp.m_y) {
					return kv.first;
				}
			}
			const auto& monConfigs = m_config->getMonitors();
			for (const auto& monCfg : monConfigs) {
				const Config::MonitorConfig& cfg = monCfg.second;
				if (!cfg.matchPattern.empty()) {
					if (wildcardMatch(cfg.matchPattern, disp.m_name) ||
					    wildcardMatch(cfg.matchPattern, disp.m_id)) {
						return cfg.name;
					}
				}
				else if (wildcardMatch(cfg.name, disp.m_name) ||
				         wildcardMatch(cfg.name, disp.m_id)) {
					return cfg.name;
				}
			}
			return getName(client);
		};

		// 1. Check exact containment inside display bounds
		for (const auto& disp : displays) {
			if (x >= disp.m_x && x < disp.m_x + disp.m_w &&
			    y >= disp.m_y && y < disp.m_y + disp.m_h) {
				ax = disp.m_x;
				ay = disp.m_y;
				aw = disp.m_w;
				ah = disp.m_h;
				monitorName = resolveMonName(disp);
				return true;
			}
		}

		// 2. Nearest display search if slightly outside (tolerance up to 50px)
		const DisplayInfo* bestDisp = nullptr;
		int64_t minDistanceSq = -1;
		for (const auto& disp : displays) {
			SInt32 cx = std::max(disp.m_x, std::min(x, disp.m_x + disp.m_w - 1));
			SInt32 cy = std::max(disp.m_y, std::min(y, disp.m_y + disp.m_h - 1));
			int64_t distSq = static_cast<int64_t>(x - cx) * (x - cx) + static_cast<int64_t>(y - cy) * (y - cy);
			if (minDistanceSq < 0 || distSq < minDistanceSq) {
				minDistanceSq = distSq;
				bestDisp = &disp;
			}
		}

		if (bestDisp != nullptr && minDistanceSq <= 2500) {
			ax = bestDisp->m_x;
			ay = bestDisp->m_y;
			aw = bestDisp->m_w;
			ah = bestDisp->m_h;
			monitorName = resolveMonName(*bestDisp);
			return true;
		}
	}

	client->getShape(ax, ay, aw, ah);
	monitorName = getName(client);
	return false;
}

bool
Server::hasLocalDisplayInDirection(BaseClientProxy* client, SInt32 x, SInt32 y,
                                   SInt32 ax, SInt32 ay, SInt32 aw, SInt32 ah,
                                   EDirection dir, std::string& outLocalMonName) const
{
	if (client == nullptr) {
		return false;
	}

	std::vector<DisplayInfo> displays = client->getDisplays();
	if (displays.size() <= 1) {
		return false;
	}

	auto resolveMonName = [this, client](const DisplayInfo& disp) -> std::string {
		for (const auto& kv : m_activeMonitors) {
			if (kv.second.m_host == client &&
			    kv.second.m_display.m_x == disp.m_x &&
			    kv.second.m_display.m_y == disp.m_y) {
				return kv.first;
			}
		}
		const auto& monConfigs = m_config->getMonitors();
		for (const auto& monCfg : monConfigs) {
			const Config::MonitorConfig& cfg = monCfg.second;
			if (!cfg.matchPattern.empty()) {
				if (wildcardMatch(cfg.matchPattern, disp.m_name) ||
				    wildcardMatch(cfg.matchPattern, disp.m_id)) {
					return cfg.name;
				}
			}
			else if (wildcardMatch(cfg.name, disp.m_name) ||
			         wildcardMatch(cfg.name, disp.m_id)) {
				return cfg.name;
			}
		}
		return getName(client);
	};

	for (const auto& disp : displays) {
		if (disp.m_x == ax && disp.m_y == ay && disp.m_w == aw && disp.m_h == ah) {
			continue;
		}

		bool adjacent = false;
		switch (dir) {
		case kLeft:
			if (std::abs((disp.m_x + disp.m_w) - ax) <= 50 &&
			    std::max(ay, disp.m_y) < std::min(ay + ah, disp.m_y + disp.m_h)) {
				adjacent = true;
			}
			break;

		case kRight:
			if (std::abs(disp.m_x - (ax + aw)) <= 50 &&
			    std::max(ay, disp.m_y) < std::min(ay + ah, disp.m_y + disp.m_h)) {
				adjacent = true;
			}
			break;

		case kTop:
			if (std::abs((disp.m_y + disp.m_h) - ay) <= 50 &&
			    std::max(ax, disp.m_x) < std::min(ax + aw, disp.m_x + disp.m_w)) {
				adjacent = true;
			}
			break;

		case kBottom:
			if (std::abs(disp.m_y - (ay + ah)) <= 50 &&
			    std::max(ax, disp.m_x) < std::min(ax + aw, disp.m_x + disp.m_w)) {
				adjacent = true;
			}
			break;

		default:
			break;
		}

		if (adjacent) {
			outLocalMonName = resolveMonName(disp);
			return true;
		}
	}

	return false;
}



float
Server::mapToFraction(BaseClientProxy* client,
				EDirection dir, SInt32 x, SInt32 y) const
{
	SInt32 sx;
	SInt32 sy;
	SInt32 sw;
	SInt32 sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		return static_cast<float>(y - sy + 0.5F) / static_cast<float>(sh);

	case kTop:
	case kBottom:
		return static_cast<float>(x - sx + 0.5F) / static_cast<float>(sw);

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
	return 0.0F;
}

void
Server::mapToPixel(BaseClientProxy* client,
				EDirection dir, float f, SInt32& x, SInt32& y) const
{
	SInt32 sx;
	SInt32 sy;
	SInt32 sw;
	SInt32 sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		y = static_cast<SInt32>(f * sh) + sy;
		break;

	case kTop:
	case kBottom:
		x = static_cast<SInt32>(f * sw) + sx;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
}

bool
Server::hasAnyNeighbor(BaseClientProxy* client, EDirection dir) const
{
	assert(client != NULL);

	if (m_config->hasNeighbor(getName(client), dir)) {
		return true;
	}

	for (const auto& kv : m_activeMonitors) {
		if (kv.second.m_host == client) {
			if (m_config->hasNeighbor(kv.first, dir)) {
				return true;
			}
		}
	}

	return false;
}

BaseClientProxy*
Server::getNeighbor(BaseClientProxy* src,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get source screen/monitor name
	std::string srcName = getName(src);
	SInt32 ax, ay, aw, ah;
	std::string monName;
	float t;
	if (getActiveMonitorShape(src, x, y, ax, ay, aw, ah, monName) &&
	    (m_config->isMonitor(monName) || m_config->hasNeighbor(monName, dir))) {
		srcName = monName;
		switch (dir) {
		case kLeft:
		case kRight:
			t = static_cast<float>(y - ay + 0.5F) / static_cast<float>(ah);
			break;
		case kTop:
		case kBottom:
			t = static_cast<float>(x - ax + 0.5F) / static_cast<float>(aw);
			break;
		default:
			t = 0.0F;
			break;
		}
	}
	else {
		t = mapToFraction(src, dir, x, y);
	}

	if (t < 0.0F) t = 0.0F;
	if (t >= 1.0F) t = 0.9999F;

	assert(!srcName.empty());
	LOG((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));

	// search for the closest neighbor that exists in direction dir
	float tTmp;
	for (;;) {
		std::string dstName(m_config->getNeighbor(srcName, dir, t, &tTmp));

		// if nothing in that direction then return NULL. if the
		// destination is the source then we can make no more
		// progress in this direction.  since we haven't found a
		// connected neighbor we return NULL.
		if (dstName.empty()) {
			LOG((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));
			return nullptr;
		}

		if (tTmp < 0.0F) tTmp = 0.0F;
		if (tTmp >= 1.0F) tTmp = 0.9999F;

		// check if destination is an active monitor
		auto monIt = m_activeMonitors.find(dstName);
		if (monIt != m_activeMonitors.end() && monIt->second.m_host != nullptr) {
			BaseClientProxy* dstClient = monIt->second.m_host;
			const DisplayInfo& disp = monIt->second.m_display;
			LOG((CLOG_DEBUG2 "\"%s\" (monitor on \"%s\") is on %s of \"%s\" at %f",
				dstName.c_str(), getName(dstClient).c_str(), Config::dirName(dir), srcName.c_str(), t));

			switch (dir) {
			case kLeft:
				x = disp.m_x + disp.m_w - 1;
				y = disp.m_y + static_cast<SInt32>(tTmp * disp.m_h);
				break;
			case kRight:
				x = disp.m_x;
				y = disp.m_y + static_cast<SInt32>(tTmp * disp.m_h);
				break;
			case kTop:
				x = disp.m_x + static_cast<SInt32>(tTmp * disp.m_w);
				y = disp.m_y + disp.m_h - 1;
				break;
			case kBottom:
				x = disp.m_x + static_cast<SInt32>(tTmp * disp.m_w);
				y = disp.m_y;
				break;
			default:
				break;
			}
			if (x < disp.m_x) x = disp.m_x;
			else if (x >= disp.m_x + disp.m_w) x = disp.m_x + disp.m_w - 1;
			if (y < disp.m_y) y = disp.m_y;
			else if (y >= disp.m_y + disp.m_h) y = disp.m_y + disp.m_h - 1;

			return dstClient;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.
		auto index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\" at %f", dstName.c_str(), Config::dirName(dir), srcName.c_str(), t));
			BaseClientProxy* dstClient = index->second;
			std::vector<DisplayInfo> dstDisplays = dstClient->getDisplays();
			std::vector<DisplayInfo> targetDisplays;
			for (const auto& disp : dstDisplays) {
				bool claimedByOther = false;
				for (const auto& kv : m_activeMonitors) {
					if (kv.first != dstName && kv.second.m_host == dstClient) {
						if (kv.second.m_display.m_x == disp.m_x && kv.second.m_display.m_y == disp.m_y &&
						    kv.second.m_display.m_w == disp.m_w && kv.second.m_display.m_h == disp.m_h) {
							claimedByOther = true;
							break;
						}
					}
				}
				if (!claimedByOther) {
					targetDisplays.push_back(disp);
				}
			}
			if (targetDisplays.empty()) {
				targetDisplays = dstDisplays;
			}

			SInt32 sx, sy, sw, sh;
			if (!targetDisplays.empty()) {
				SInt32 minX = targetDisplays[0].m_x;
				SInt32 minY = targetDisplays[0].m_y;
				SInt32 maxX = targetDisplays[0].m_x + targetDisplays[0].m_w;
				SInt32 maxY = targetDisplays[0].m_y + targetDisplays[0].m_h;
				for (size_t i = 1; i < targetDisplays.size(); ++i) {
					minX = std::min(minX, targetDisplays[i].m_x);
					minY = std::min(minY, targetDisplays[i].m_y);
					maxX = std::max(maxX, targetDisplays[i].m_x + targetDisplays[i].m_w);
					maxY = std::max(maxY, targetDisplays[i].m_y + targetDisplays[i].m_h);
				}
				sx = minX;
				sy = minY;
				sw = maxX - minX;
				sh = maxY - minY;
			}
			else {
				dstClient->getShape(sx, sy, sw, sh);
			}

			switch (dir) {
			case kLeft:
				x = sx + sw - 1;
				y = sy + static_cast<SInt32>(tTmp * sh);
				break;
			case kRight:
				x = sx;
				y = sy + static_cast<SInt32>(tTmp * sh);
				break;
			case kTop:
				x = sx + static_cast<SInt32>(tTmp * sw);
				y = sy + sh - 1;
				break;
			case kBottom:
				x = sx + static_cast<SInt32>(tTmp * sw);
				y = sy;
				break;
			default:
				break;
			}
			if (x < sx) x = sx;
			else if (x >= sx + sw) x = sx + sw - 1;
			if (y < sy) y = sy;
			else if (y >= sy + sh) y = sy + sh - 1;

			if (!targetDisplays.empty()) {
				bool insideAny = false;
				for (const auto& disp : targetDisplays) {
					if (x >= disp.m_x && x < disp.m_x + disp.m_w &&
					    y >= disp.m_y && y < disp.m_y + disp.m_h) {
						insideAny = true;
						break;
					}
				}
				if (!insideAny) {
					const DisplayInfo* bestEdgeDisp = nullptr;
					for (const auto& disp : targetDisplays) {
						if (dir == kLeft) {
							if (bestEdgeDisp == nullptr || (disp.m_x + disp.m_w > bestEdgeDisp->m_x + bestEdgeDisp->m_w)) {
								bestEdgeDisp = &disp;
							}
						}
						else if (dir == kRight) {
							if (bestEdgeDisp == nullptr || (disp.m_x < bestEdgeDisp->m_x)) {
								bestEdgeDisp = &disp;
							}
						}
						else if (dir == kTop) {
							if (bestEdgeDisp == nullptr || (disp.m_y + disp.m_h > bestEdgeDisp->m_y + bestEdgeDisp->m_h)) {
								bestEdgeDisp = &disp;
							}
						}
						else if (dir == kBottom) {
							if (bestEdgeDisp == nullptr || (disp.m_y < bestEdgeDisp->m_y)) {
								bestEdgeDisp = &disp;
							}
						}
					}
					if (bestEdgeDisp != nullptr) {
						if (x < bestEdgeDisp->m_x) x = bestEdgeDisp->m_x;
						else if (x >= bestEdgeDisp->m_x + bestEdgeDisp->m_w) x = bestEdgeDisp->m_x + bestEdgeDisp->m_w - 1;
						if (y < bestEdgeDisp->m_y) y = bestEdgeDisp->m_y;
						else if (y >= bestEdgeDisp->m_y + bestEdgeDisp->m_h) y = bestEdgeDisp->m_y + bestEdgeDisp->m_h - 1;
					}
				}
			}

			return dstClient;
		}

		// skip over unconnected screen
		LOG((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), Config::dirName(dir), srcName.c_str()));
		srcName = dstName;

		// use position on skipped screen
		t = tTmp;
	}
}

BaseClientProxy*
Server::mapToNeighbor(BaseClientProxy* src,
				EDirection srcSide, SInt32& x, SInt32& y) const
{
	assert(src != NULL);

	BaseClientProxy* dst = getNeighbor(src, srcSide, x, y);
	if (dst == nullptr) {
		return nullptr;
	}

	avoidJumpZone(dst, srcSide, x, y);
	return dst;
}

void
Server::avoidJumpZone(BaseClientProxy* dst,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// we only need to avoid jump zones on the primary screen
	if (dst != m_primaryClient) {
		return;
	}

	const std::string dstName(getName(dst));
	SInt32 dx;
	SInt32 dy;
	SInt32 dw;
	SInt32 dh;
	std::string monName;
	getActiveMonitorShape(dst, x, y, dx, dy, dw, dh, monName);
	float t = (dir == kLeft || dir == kRight) ?
		static_cast<float>(y - dy + 0.5F) / static_cast<float>(dh) :
		static_cast<float>(x - dx + 0.5F) / static_cast<float>(dw);
	SInt32 z = getJumpZoneSize(dst);

	// move in far enough to avoid the jump zone.  if entering a side
	// that doesn't have a neighbor (i.e. an asymmetrical side) then we
	// don't need to move inwards because that side can't provoke a jump.
	switch (dir) {
	case kLeft:
		if ((!m_config->getNeighbor(dstName, kRight, t, nullptr).empty() ||
		     (!monName.empty() && !m_config->getNeighbor(monName, kRight, t, nullptr).empty())) &&
			x > dx + dw - 1 - z)
			x = dx + dw - 1 - z;
		break;

	case kRight:
		if ((!m_config->getNeighbor(dstName, kLeft, t, nullptr).empty() ||
		     (!monName.empty() && !m_config->getNeighbor(monName, kLeft, t, nullptr).empty())) &&
			x < dx + z)
			x = dx + z;
		break;

	case kTop:
		if ((!m_config->getNeighbor(dstName, kBottom, t, nullptr).empty() ||
		     (!monName.empty() && !m_config->getNeighbor(monName, kBottom, t, nullptr).empty())) &&
			y > dy + dh - 1 - z)
			y = dy + dh - 1 - z;
		break;

	case kBottom:
		if ((!m_config->getNeighbor(dstName, kTop, t, nullptr).empty() ||
		     (!monName.empty() && !m_config->getNeighbor(monName, kTop, t, nullptr).empty())) &&
			y < dy + z)
			y = dy + z;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
	}
}

bool
Server::isSwitchOkay(BaseClientProxy* newScreen,
				EDirection dir, SInt32 x, SInt32 y,
				SInt32 xActive, SInt32 yActive)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), Config::dirName(dir)));

	// is there a neighbor?
	if (newScreen == nullptr) {
		// there's no neighbor.  we don't want to switch and we don't
		// want to try to switch later.
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(dir)));
		stopSwitch();
		return false;
	}

	// should we switch or not?
	bool preventSwitch = false;
	bool allowSwitch   = false;

	// note if the switch direction has changed.  save the new
	// direction and screen if so.
	bool isNewDirection  = (dir != m_switchDir);
	if (isNewDirection || m_switchScreen == nullptr) {
		m_switchDir    = dir;
		m_switchScreen = newScreen;
	}

	// is this a double tap and do we care?
	if (!allowSwitch && m_switchTwoTapDelay > 0.0) {
		if (isNewDirection ||
			!isSwitchTwoTapStarted() || !shouldSwitchTwoTap()) {
			// tapping a different or new edge or second tap not
			// fast enough.  prepare for second tap.
			preventSwitch = true;
			startSwitchTwoTap();
		}
		else {
			// got second tap
			allowSwitch = true;
		}
	}

	// if waiting before a switch then prepare to switch later
	if (!allowSwitch && m_switchWaitDelay > 0.0) {
		if (isNewDirection || !isSwitchWaitStarted()) {
			startSwitchWait(x, y);
		}
		preventSwitch = true;
	}

	// are we in a locked corner?  first check if screen has the option set
	// and, if not, check the global options.
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(m_active));
	if (options == nullptr || options->count(kOptionScreenSwitchCorners) == 0) {
		options = m_config->getOptions("");
	}
	if (options != nullptr && options->count(kOptionScreenSwitchCorners) > 0) {
		// get corner mask and size
		auto i =
			options->find(kOptionScreenSwitchCorners);
		auto corners = static_cast<UInt32>(i->second);
		i = options->find(kOptionScreenSwitchCornerSize);
		SInt32 size = 0;
		if (i != options->end()) {
			size = i->second;
		}

		// see if we're in a locked corner
		if ((getCorner(m_active, xActive, yActive, size) & corners) != 0) {
			// yep, no switching
			LOG((CLOG_DEBUG1 "locked in corner"));
			preventSwitch = true;
			stopSwitch();
		}
	}

	// ignore if mouse is locked to screen and don't try to switch later
	if (!preventSwitch && isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		preventSwitch = true;
		stopSwitch();
	}

	// check for optional needed modifiers
	KeyModifierMask mods = this->m_primaryClient->getToggleMask();

	if (!preventSwitch && (
			(this->m_switchNeedsShift && ((mods & KeyModifierShift) != KeyModifierShift)) ||
			(this->m_switchNeedsControl && ((mods & KeyModifierControl) != KeyModifierControl)) ||
			(this->m_switchNeedsAlt && ((mods & KeyModifierAlt) != KeyModifierAlt))
		)) {
		LOG((CLOG_DEBUG1 "need modifiers to switch"));
		preventSwitch = true;
		stopSwitch();
	}

	return !preventSwitch;
}

void
Server::noSwitch(SInt32 x, SInt32 y)
{
	armSwitchTwoTap(x, y);
	stopSwitchWait();
}

void
Server::stopSwitch()
{
	if (m_switchScreen != nullptr) {
		m_switchScreen = nullptr;
		m_switchDir    = kNoDirection;
		stopSwitchTwoTap();
		stopSwitchWait();
	}
}

void
Server::startSwitchTwoTap()
{
	m_switchTwoTapEngaged = true;
	m_switchTwoTapArmed   = false;
	m_switchTwoTapTimer.reset();
	LOG((CLOG_DEBUG1 "waiting for second tap"));
}

void
Server::armSwitchTwoTap(SInt32 x, SInt32 y)
{
	if (m_switchTwoTapEngaged) {
		if (m_switchTwoTapTimer.getTime() > m_switchTwoTapDelay) {
			// second tap took too long.  disengage.
			stopSwitchTwoTap();
		}
		else if (!m_switchTwoTapArmed) {
			// still time for a double tap.  see if we left the tap
			// zone and, if so, arm the two tap.
			SInt32 ax;
			SInt32 ay;
			SInt32 aw;
			SInt32 ah;
			m_active->getShape(ax, ay, aw, ah);
			SInt32 tapZone = m_primaryClient->getJumpZoneSize();
			if (tapZone < m_switchTwoTapZone) {
				tapZone = m_switchTwoTapZone;
			}
			if (x >= ax + tapZone && x < ax + aw - tapZone &&
				y >= ay + tapZone && y < ay + ah - tapZone) {
				// win32 can generate bogus mouse events that appear to
				// move in the opposite direction that the mouse actually
				// moved.  try to ignore that crap here.
				switch (m_switchDir) {
				case kLeft:
					m_switchTwoTapArmed = (m_xDelta > 0 && m_xDelta2 > 0);
					break;

				case kRight:
					m_switchTwoTapArmed = (m_xDelta < 0 && m_xDelta2 < 0);
					break;

				case kTop:
					m_switchTwoTapArmed = (m_yDelta > 0 && m_yDelta2 > 0);
					break;

				case kBottom:
					m_switchTwoTapArmed = (m_yDelta < 0 && m_yDelta2 < 0);
					break;

				default:
					break;
				}
			}
		}
	}
}

void
Server::stopSwitchTwoTap()
{
	m_switchTwoTapEngaged = false;
	m_switchTwoTapArmed   = false;
}

bool
Server::isSwitchTwoTapStarted() const
{
	return m_switchTwoTapEngaged;
}

bool
Server::shouldSwitchTwoTap() const
{
	// this is the second tap if two-tap is armed and this tap
	// came fast enough
	return (m_switchTwoTapArmed &&
			m_switchTwoTapTimer.getTime() <= m_switchTwoTapDelay);
}

void
Server::startSwitchWait(SInt32 x, SInt32 y)
{
	stopSwitchWait();
	m_switchWaitX     = x;
	m_switchWaitY     = y;
	m_switchWaitTimer = m_events->newOneShotTimer(m_switchWaitDelay, this);
	LOG((CLOG_DEBUG1 "waiting to switch"));
}

void
Server::stopSwitchWait()
{
	if (m_switchWaitTimer != nullptr) {
		m_events->deleteTimer(m_switchWaitTimer);
		m_switchWaitTimer = nullptr;
	}
}

bool
Server::isSwitchWaitStarted() const
{
	return (m_switchWaitTimer != nullptr);
}

UInt32
Server::getCorner(BaseClientProxy* client,
				SInt32 x, SInt32 y, SInt32 size) const
{
	assert(client != NULL);

	// get client screen/monitor shape
	SInt32 ax;
	SInt32 ay;
	SInt32 aw;
	SInt32 ah;
	std::string monName;
	if (!getActiveMonitorShape(client, x, y, ax, ay, aw, ah, monName)) {
		client->getShape(ax, ay, aw, ah);
	}

	// check for x,y on the left or right
	SInt32 xSide;
	if (x <= ax) {
		xSide = -1;
	}
	else if (x >= ax + aw - 1) {
		xSide = 1;
	}
	else {
		xSide = 0;
	}

	// check for x,y on the top or bottom
	SInt32 ySide;
	if (y <= ay) {
		ySide = -1;
	}
	else if (y >= ay + ah - 1) {
		ySide = 1;
	}
	else {
		ySide = 0;
	}

	// if against the left or right then check if y is within size
	if (xSide != 0) {
		if (y < ay + size) {
			return (xSide < 0) ? kTopLeftMask : kTopRightMask;
		}
		if (y >= ay + ah - size) {
			return (xSide < 0) ? kBottomLeftMask : kBottomRightMask;
		}
	}

	// if against the left or right then check if y is within size
	if (ySide != 0) {
		if (x < ax + size) {
			return (ySide < 0) ? kTopLeftMask : kBottomLeftMask;
		}
		if (x >= ax + aw - size) {
			return (ySide < 0) ? kTopRightMask : kBottomRightMask;
		}
	}

	return kNoCornerMask;
}

void
Server::stopRelativeMoves()
{
	if (m_relativeMoves && m_active != m_primaryClient) {
		// warp to the center of the active client so we know where we are
		SInt32 ax;
		SInt32 ay;
		SInt32 aw;
		SInt32 ah;
		m_active->getShape(ax, ay, aw, ah);
		m_x       = ax + (aw >> 1);
		m_y       = ay + (ah >> 1);
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		LOG((CLOG_DEBUG2 "synchronize move on %s by %d,%d", getName(m_active).c_str(), m_x, m_y));
		m_active->mouseMove(m_x, m_y);
	}
}

void
Server::sendOptions(BaseClientProxy* client) const
{
	OptionsList optionsList;

	// look up options for client
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(client));
	if (options != nullptr) {
		// convert options to a more convenient form for sending
		optionsList.reserve(2 * options->size());
		for (auto option : *options) {
			optionsList.push_back(option.first);
			optionsList.push_back(static_cast<UInt32>(option.second));
		}
	}

	// look up global options
	options = m_config->getOptions("");
	if (options != nullptr) {
		// convert options to a more convenient form for sending
		optionsList.reserve(optionsList.size() + 2 * options->size());
		for (auto option : *options) {
			optionsList.push_back(option.first);
			optionsList.push_back(static_cast<UInt32>(option.second));
		}
	}

	// send the options
	client->resetOptions();
	client->setOptions(optionsList);
}

std::vector<std::string>
Server::discoverLocalAddresses() const
{
	std::vector<std::string> result;
#if !defined(_WIN32)
	struct ifaddrs* ifaddr = nullptr;
	if (getifaddrs(&ifaddr) == -1) {
		return result;
	}

	for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
			continue;
		}

		// Must be UP and not LOOPBACK
		if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) {
			continue;
		}

		std::string name = ifa->ifa_name ? ifa->ifa_name : "";

		// Filter out virtual/container/bridge/VPN interfaces
		if (name.rfind("docker", 0) == 0 ||
			name.rfind("br-", 0) == 0 ||
			name.rfind("veth", 0) == 0 ||
			name.rfind("virbr", 0) == 0 ||
			name.rfind("tun", 0) == 0 ||
			name.rfind("tap", 0) == 0 ||
			name.rfind("utun", 0) == 0) {
			continue;
		}

		char host[NI_MAXHOST];
		if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
						host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
			std::string ip = host;
			if (std::find(result.begin(), result.end(), ip) == result.end()) {
				result.push_back(ip);
			}
		}
	}
	freeifaddrs(ifaddr);
#endif
	return result;
}

void
Server::sendServerAddresses(ClientProxy* client) const
{
	std::vector<std::string> addrs = discoverLocalAddresses();
	if (addrs.empty()) {
		return;
	}

	std::string joined;
	for (size_t i = 0; i < addrs.size(); ++i) {
		if (i > 0) {
			joined += ",";
		}
		joined += addrs[i];
	}

	LOG((CLOG_NOTE "advertising server physical addresses to client \"%s\": %s",
		 getName(client).c_str(), joined.c_str()));
	String payload = joined;
	ProtocolUtil::writef(client->getStream(), kMsgDServerAddresses, &payload);
}

void
Server::processOptions()
{
	const Config::ScreenOptions* options = m_config->getOptions("");
	if (options == nullptr) {
		return;
	}

	m_switchNeedsShift = false;		// it seems if I don't add these
	m_switchNeedsControl = false;	// lines, the 'reload config' option
	m_switchNeedsAlt = false;		// doesn't work correct.

	bool newRelativeMoves = m_relativeMoves;
	for (auto option : *options) {
		const OptionID id       = option.first;
		const OptionValue value = option.second;
		if (id == kOptionScreenSwitchDelay) {
			m_switchWaitDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchWaitDelay < 0.0) {
				m_switchWaitDelay = 0.0;
			}
			stopSwitchWait();
		}
		else if (id == kOptionScreenSwitchTwoTap) {
			m_switchTwoTapDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchTwoTapDelay < 0.0) {
				m_switchTwoTapDelay = 0.0;
			}
			stopSwitchTwoTap();
		}
		else if (id == kOptionScreenSwitchNeedsControl) {
			m_switchNeedsControl = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsShift) {
			m_switchNeedsShift = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsAlt) {
			m_switchNeedsAlt = (value != 0);
		}
		else if (id == kOptionRelativeMouseMoves) {
			newRelativeMoves = (value != 0);
		}
		else if (id == kOptionClipboardSharing) {
			m_enableClipboard = (value != 0);

			if (m_enableClipboard == false) {
				LOG((CLOG_NOTE "clipboard sharing is disabled"));
			}
		}
	}
	if (m_relativeMoves && !newRelativeMoves) {
		stopRelativeMoves();
	}
	m_relativeMoves = newRelativeMoves;
}

void
Server::handleShapeChanged(const Event&, void* vclient)
{
	// ignore events from unknown clients
	auto* client = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(client) == 0) {
		return;
	}

	LOG((CLOG_DEBUG "screen \"%s\" shape changed", getName(client).c_str()));
	updateActiveMonitors();

	// update jump coordinate
	SInt32 x;
	SInt32 y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// update the mouse coordinates
	if (client == m_active) {
		m_x = x;
		m_y = y;
	}

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimary(m_x, m_y);
		}
		else {
			onMouseMoveSecondary(0, 0);
		}
	}
}

void
Server::handleClipboardGrabbed(const Event& event, void* vclient)
{
	if (!m_enableClipboard) {
		return;
	}

	// ignore events from unknown clients
	auto* grabber = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(grabber) == 0) {
		return;
	}
	const auto* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	ClipboardInfo& clipboard = m_clipboards[info->m_id];
	if (grabber != m_primaryClient &&
		info->m_sequenceNumber < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", getName(grabber).c_str(), info->m_id));
		return;
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;

	// clear the clipboard data (since it's not known at this point)
	if (clipboard.m_clipboard.open(0)) {
		(void)clipboard.m_clipboard.empty();
		clipboard.m_clipboard.close();
	}
	clipboard.m_clipboardData = clipboard.m_clipboard.marshall();

	// tell all other screens to take ownership of clipboard.  tell the
	// grabber that it's clipboard isn't dirty.
	for (auto & m_client : m_clients) {
		BaseClientProxy* client = m_client.second;
		if (client == grabber) {
			client->setClipboardDirty(info->m_id, false);
		}
		else {
			client->grabClipboard(info->m_id);
		}
	}
}

void
Server::handleClipboardChanged(const Event& event, void* vclient)
{
	// ignore events from unknown clients
	auto* sender = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(sender) == 0) {
		return;
	}
	const auto* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());
	onClipboardChanged(sender, info->m_id, info->m_sequenceNumber);
}

void
Server::handleKeyDownEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyDown(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyUpEvent(const Event& event, void*)
{
	auto* info =
		 static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyUp(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyRepeatEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyRepeat(info->m_key, info->m_mask, info->m_count, info->m_button);
}

void
Server::handleButtonDownEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseDown(info->m_button);
}

void
Server::handleButtonUpEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseUp(info->m_button);
}

void
Server::handleMotionPrimaryEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMovePrimary(info->m_x, info->m_y);
}

void
Server::handleMotionSecondaryEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMoveSecondary(info->m_x, info->m_y);
}

void
Server::handleWheelEvent(const Event& event, void*)
{
	auto* info =
		static_cast<IPlatformScreen::WheelInfo*>(event.getData());
	onMouseWheel(info->m_xDelta, info->m_yDelta);
}

void
Server::handleScreensaverActivatedEvent(const Event&, void*)
{
	onScreensaver(true);
}

void
Server::handleScreensaverDeactivatedEvent(const Event&, void*)
{
	onScreensaver(false);
}

void
Server::handleSwitchWaitTimeout(const Event&, void*)
{
	// ignore if mouse is locked to screen
	if (isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		stopSwitch();
		return;
	}

	// switch screen
	switchScreen(m_switchScreen, m_switchWaitX, m_switchWaitY, false);
}

void
Server::handleClientDisconnected(const Event&, void* vclient)
{
	// client has disconnected.  it might be an old client or an
	// active client.  we don't care so just handle it both ways.
	auto* client = static_cast<BaseClientProxy*>(vclient);
	removeActiveClient(client);
	removeOldClient(client);

	delete client;
}

void
Server::handleClientCloseTimeout(const Event&, void* vclient)
{
	// client took too long to disconnect.  just dump it.
	auto* client = static_cast<BaseClientProxy*>(vclient);
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	removeOldClient(client);

	delete client;
}

void
Server::handleSwitchToScreenEvent(const Event& event, void*)
{
	auto* info =
		static_cast<SwitchToScreenInfo*>(event.getData());

	std::string screenName = info->m_screen;
	auto monIt = m_activeMonitors.find(screenName);
	if (monIt != m_activeMonitors.end() && monIt->second.m_host != nullptr) {
		BaseClientProxy* host = monIt->second.m_host;
		SInt32 mx = monIt->second.m_display.m_x + monIt->second.m_display.m_w / 2;
		SInt32 my = monIt->second.m_display.m_y + monIt->second.m_display.m_h / 2;
		jumpToScreen(host, mx, my);
		return;
	}

	ClientList::const_iterator index = m_clients.find(screenName);
	if (index == m_clients.end()) {
		std::string canon = m_config->getCanonicalName(screenName);
		if (!canon.empty()) {
			index = m_clients.find(canon);
		}
	}

	if (index == m_clients.end()) {
		LOG((CLOG_DEBUG1 "screen \"%s\" not active", screenName.c_str()));
	}
	else {
		BaseClientProxy* client = index->second;
		std::vector<DisplayInfo> displays = client->getDisplays();
		std::vector<DisplayInfo> hostDisplays;
		for (const auto& disp : displays) {
			bool claimed = false;
			for (const auto& kv : m_activeMonitors) {
				if (kv.second.m_host == client) {
					if (kv.second.m_display.m_x == disp.m_x && kv.second.m_display.m_y == disp.m_y &&
					    kv.second.m_display.m_w == disp.m_w && kv.second.m_display.m_h == disp.m_h) {
						claimed = true;
						break;
					}
				}
			}
			if (!claimed) {
				hostDisplays.push_back(disp);
			}
		}

		if (!hostDisplays.empty()) {
			const DisplayInfo* targetDisp = &hostDisplays[0];
			for (const auto& disp : hostDisplays) {
				if (disp.m_isPrimary) {
					targetDisp = &disp;
					break;
				}
			}
			SInt32 hx = targetDisp->m_x + targetDisp->m_w / 2;
			SInt32 hy = targetDisp->m_y + targetDisp->m_h / 2;
			jumpToScreen(client, hx, hy);
		}
		else {
			jumpToScreen(client);
		}
	}
}

void
Server::handleToggleScreenEvent(const Event& event, void*)
{
  std::string current = getName(m_active);
  ClientList::const_iterator index = m_clients.find(current);
  if (index == m_clients.end()) {
    LOG((CLOG_DEBUG1 "screen \"%s\" not active", current.c_str()));
  }
  else {
    ++index;
    if (index == m_clients.end()) {
      index = m_clients.begin();
    }
    jumpToScreen(index->second);
  }
}


void
Server::handleSwitchInDirectionEvent(const Event& event, void*)
{
	auto* info =
		static_cast<SwitchInDirectionInfo*>(event.getData());

	// jump to screen in chosen direction from center of this screen
	SInt32 x = m_x;
	SInt32 y = m_y;
	BaseClientProxy* newScreen =
		getNeighbor(m_active, info->m_direction, x, y);
	if (newScreen == nullptr) {
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(info->m_direction)));
	}
	else {
		jumpToScreen(newScreen, x, y);
	}
}

void
Server::handleKeyboardBroadcastEvent(const Event& event, void*)
{
	auto* info = static_cast<KeyboardBroadcastInfo*>(event.getData());

	// choose new state
	bool newState;
	switch (info->m_state) {
	case KeyboardBroadcastInfo::kOff:
		newState = false;
		break;

	default:
	case KeyboardBroadcastInfo::kOn:
		newState = true;
		break;

	case KeyboardBroadcastInfo::kToggle:
		newState = !m_keyboardBroadcasting;
		break;
	}

	// enter new state
	if (newState != m_keyboardBroadcasting ||
		info->m_screens != m_keyboardBroadcastingScreens) {
		m_keyboardBroadcasting        = newState;
		m_keyboardBroadcastingScreens = info->m_screens;
		LOG((CLOG_DEBUG "keyboard broadcasting %s: %s", m_keyboardBroadcasting ? "on" : "off", m_keyboardBroadcastingScreens.c_str()));
	}
}

void
Server::handleLockCursorToScreenEvent(const Event& event, void*)
{
	auto* info = static_cast<LockCursorToScreenInfo*>(event.getData());

	// choose new state
	bool newState;
	switch (info->m_state) {
	case LockCursorToScreenInfo::kOff:
		newState = false;
		break;

	default:
	case LockCursorToScreenInfo::kOn:
		newState = true;
		break;

	case LockCursorToScreenInfo::kToggle:
		newState = !m_lockedToScreen;
		break;
	}

	// enter new state
	if (newState != m_lockedToScreen) {
		m_lockedToScreen = newState;
		LOG((CLOG_NOTE "cursor %s current screen", m_lockedToScreen ? "locked to" : "unlocked from"));

		m_primaryClient->reconfigure(getActivePrimarySides());
		if (!isLockedToScreenServer()) {
			stopRelativeMoves();
		}
	}
}

void
Server::handleFakeInputBeginEvent(const Event&, void*)
{
	m_primaryClient->fakeInputBegin();
}

void
Server::handleFakeInputEndEvent(const Event&, void*)
{
	m_primaryClient->fakeInputEnd();
}

void
Server::handleFileChunkSendingEvent(const Event& event, void*)
{
	onFileChunkSending(event.getDataObject());
}

void
Server::handleFileRecieveCompletedEvent(const Event& event, void*)
{
	onFileRecieveCompleted();
}

void
Server::onClipboardChanged(BaseClientProxy* sender,
				ClipboardID id, UInt32 seqNum)
{
	ClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", getName(sender).c_str(), id));
		return;
	}

	// should be the expected client
	assert(sender == m_clients.find(clipboard.m_clipboardOwner)->second);

	// get data
	sender->getClipboard(id, &clipboard.m_clipboard);

	// ignore if data hasn't changed
    std::string data = clipboard.m_clipboard.marshall();
	if (data == clipboard.m_clipboardData) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	clipboard.m_clipboardData = data;

	// tell all clients except the sender that the clipboard is dirty
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->setClipboardDirty(id, client != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, &clipboard.m_clipboard);
}

void
Server::onScreensaver(bool activated)
{
	LOG((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			switchScreen(m_primaryClient, 0, 0, true);
		}
	}
	else {
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != nullptr && m_activeSaver != m_primaryClient) {
			// check position
			BaseClientProxy* screen = m_activeSaver;
			SInt32 x;
			SInt32 y;
			SInt32 w;
			SInt32 h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = getJumpZoneSize(screen);
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			switchScreen(screen, m_xSaver, m_ySaver, false);
		}

		// reset state
		m_activeSaver = nullptr;
	}

	// send message to all clients
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->screensaver(activated);
	}
}

void
Server::onKeyDown(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyDown(id, mask, button);
	}
	else {
		if ((screens == nullptr) && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyDown(id, mask, button);
			}
		}
	}
}

void
Server::onKeyUp(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyUp(id, mask, button);
	}
	else {
		if ((screens == nullptr) && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyUp(id, mask, button);
			}
		}
	}
}

void
Server::onKeyRepeat(KeyID id, KeyModifierMask mask,
				SInt32 count, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
	assert(m_active != NULL);

	// relay
	m_active->keyRepeat(id, mask, count, button);
}

void
Server::onMouseDown(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseDown id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseDown(id);

	// reset this variable back to default value true
	m_waitDragInfoThread = true;
}

void
Server::onMouseUp(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseUp id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseUp(id);

	if (m_ignoreFileTransfer) {
		m_ignoreFileTransfer = false;
		return;
	}

	if (m_args.m_enableDragDrop) {
		if (!m_screen->isOnScreen()) {
            std::string& file = m_screen->getDraggingFilename();
			if (!file.empty()) {
				sendFileToClient(file.c_str());
			}
		}

		// always clear dragging filename
		m_screen->clearDraggingFilename();
	}
}

bool
Server::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	LOG((CLOG_DEBUG4 "onMouseMovePrimary %d,%d", x, y));

	// mouse move on primary (server's) screen
	if (m_active != m_primaryClient) {
		// stale event -- we're actually on a secondary screen
		return false;
	}

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = x - m_x;
	m_yDelta  = y - m_y;

	// save position
	m_x       = x;
	m_y       = y;

	// get screen shape
	SInt32 ax;
	SInt32 ay;
	SInt32 aw;
	SInt32 ah;
	std::string monName;
	getActiveMonitorShape(m_active, x, y, ax, ay, aw, ah, monName);
	SInt32 zoneSize = getJumpZoneSize(m_active);

	// clamp position to screen
	SInt32 xc = x;
	SInt32 yc = y;
	if (xc < ax + zoneSize) {
		xc = ax;
	}
	else if (xc >= ax + aw - zoneSize) {
		xc = ax + aw - 1;
	}
	if (yc < ay + zoneSize) {
		yc = ay;
	}
	else if (yc >= ay + ah - zoneSize) {
		yc = ay + ah - 1;
	}

	// see if we should change screens
	// when the cursor is in a corner, there may be a screen either
	// horizontally or vertically.  check both directions.
	EDirection dirh = kNoDirection;
	EDirection dirv = kNoDirection;
	if (x < ax + zoneSize) {
		dirh = kLeft;
	}
	else if (x >= ax + aw - zoneSize) {
		dirh = kRight;
	}
	if (y < ay + zoneSize) {
		dirv = kTop;
	}
	else if (y >= ay + ah - zoneSize) {
		dirv = kBottom;
	}
	if (dirh == kNoDirection && dirv == kNoDirection) {
		// still on local screen
		noSwitch(x, y);
		return false;
	}

	// check both horizontally and vertically
	std::array<EDirection, 2> dirs = {{dirh, dirv}};
	for (int i = 0; i < 2; ++i) {
		EDirection dir = dirs[i];
		if (dir == kNoDirection) {
			continue;
		}

		bool hasNeighbor = false;
		if (m_config->isMonitor(monName) && m_config->hasNeighbor(monName, dir)) {
			hasNeighbor = true;
		}
		else {
			std::string nextLocalMon;
			bool hasLocal = hasLocalDisplayInDirection(m_active, xc, yc, ax, ay, aw, ah, dir, nextLocalMon);
			if (hasLocal) {
				if (m_config->isMonitor(nextLocalMon) && m_config->hasNeighbor(monName, dir)) {
					hasNeighbor = true;
				}
				else {
					hasNeighbor = false;
				}
			}
			else if (m_config->hasNeighbor(getName(m_active), dir)) {
				hasNeighbor = true;
			}
		}

		if (!hasNeighbor) {
			continue;
		}

		SInt32 targetX = xc;
		SInt32 targetY = yc;

		// get jump destination
		BaseClientProxy* newScreen = mapToNeighbor(m_active, dir, targetX, targetY);

		// should we switch or not?
		if (newScreen != nullptr && isSwitchOkay(newScreen, dir, targetX, targetY, xc, yc)) {
			if (m_args.m_enableDragDrop
				&& m_screen->isDraggingStarted()
				&& m_active != newScreen
				&& m_waitDragInfoThread) {
				if (m_sendDragInfoThread == nullptr) {
                    m_sendDragInfoThread = new Thread([this, newScreen]()
                                                      { send_drag_info_thread(newScreen); });
				}

				return false;
			}

			// switch screen
			switchScreen(newScreen, targetX, targetY, false);
			m_waitDragInfoThread = true;
			return true;
		}
	}

	return false;
}

void Server::send_drag_info_thread(BaseClientProxy* newScreen)
{
	m_dragFileList.clear();
    std::string& dragFileList = m_screen->getDraggingFilename();
	if (!dragFileList.empty()) {
		DragInformation di;
		di.setFilename(dragFileList);
		m_dragFileList.push_back(di);
	}

#ifdef __APPLE__
	// on mac it seems that after faking a LMB up, system would signal back
	// to barrier a mouse up event, which doesn't happen on windows. as a
	// result, barrier would send dragging file to client twice. This variable
	// is used to ignore the first file sending.
	m_ignoreFileTransfer = true;
#endif

	// send drag file info to client if there is any
	if (m_dragFileList.size() > 0) {
		sendDragInfo(newScreen);
		m_dragFileList.clear();
	}
	m_waitDragInfoThread = false;
	m_sendDragInfoThread = nullptr;
}

void
Server::sendDragInfo(BaseClientProxy* newScreen)
{
    std::string infoString;
	UInt32 fileCount = DragInformation::setupDragInfo(m_dragFileList, infoString);

	if (fileCount > 0) {
		char* info = nullptr;
		size_t size = infoString.size();
		info = new char[size];
		memcpy(info, infoString.c_str(), size);

		LOG((CLOG_DEBUG2 "sending drag information to client"));
		LOG((CLOG_DEBUG3 "dragging file list: %s", info));
		LOG((CLOG_DEBUG3 "dragging file list string size: %i", size));
		newScreen->sendDragInfo(fileCount, info, size);
	}
}

void
Server::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	LOG((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));

	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// stale event -- we're actually on the primary screen
		return;
	}

	// if doing relative motion on secondary screens and we're locked
	// to the screen (which activates relative moves) then send a
	// relative mouse motion.  when we're doing this we pretend as if
	// the mouse isn't actually moving because we're expecting some
	// program on the secondary screen to warp the mouse on us, so we
	// have no idea where it really is.
	if (m_relativeMoves && isLockedToScreenServer()) {
		LOG((CLOG_DEBUG2 "relative move on %s by %d,%d", getName(m_active).c_str(), dx, dy));
		m_active->mouseRelativeMove(dx, dy);
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = dx;
	m_yDelta  = dy;

	// accumulate motion
	m_x      += dx;
	m_y      += dy;

	// get the monitor shape where the cursor was before this move
	SInt32 ax;
	SInt32 ay;
	SInt32 aw;
	SInt32 ah;
	std::string monName;
	getActiveMonitorShape(m_active, xOld, yOld, ax, ay, aw, ah, monName);

	// clamp position to current monitor
	SInt32 xc = m_x;
	SInt32 yc = m_y;
	if (xc < ax) {
		xc = ax;
	}
	else if (xc >= ax + aw) {
		xc = ax + aw - 1;
	}
	if (yc < ay) {
		yc = ay;
	}
	else if (yc >= ay + ah) {
		yc = ay + ah - 1;
	}

	// check if cursor crossed any boundary of current monitor
	EDirection dir = kNoDirection;
	if (m_x < ax) {
		dir = kLeft;
	}
	else if (m_x > ax + aw - 1) {
		dir = kRight;
	}
	else if (m_y < ay) {
		dir = kTop;
	}
	else if (m_y > ay + ah - 1) {
		dir = kBottom;
	}

	bool jump = false;
	BaseClientProxy* newScreen = m_active;

	if (dir != kNoDirection) {
		// Check if current monitor or screen has a link configured in this direction
		bool hasNeighbor = false;
		if (m_config->isMonitor(monName) && m_config->hasNeighbor(monName, dir)) {
			hasNeighbor = true;
		}
		else {
			std::string nextLocalMon;
			bool hasLocal = hasLocalDisplayInDirection(m_active, xc, yc, ax, ay, aw, ah, dir, nextLocalMon);
			if (hasLocal) {
				if (m_config->isMonitor(nextLocalMon) && m_config->hasNeighbor(monName, dir)) {
					hasNeighbor = true;
				}
				else {
					hasNeighbor = false;
				}
			}
			else if (m_config->hasNeighbor(getName(m_active), dir)) {
				hasNeighbor = true;
			}
		}

		if (hasNeighbor) {
			SInt32 targetX = xc;
			SInt32 targetY = yc;
			newScreen = mapToNeighbor(m_active, dir, targetX, targetY);
			if (newScreen != nullptr && isSwitchOkay(newScreen, dir, targetX, targetY, xc, yc)) {
				m_x = targetX;
				m_y = targetY;
				jump = true;
			}
			else {
				newScreen = m_active;
				m_x = xc;
				m_y = yc;
			}
		}
		else {
			// No barrier link in this direction.
			// Check if (m_x, m_y) falls within another display on the same client.
			SInt32 nax, nay, naw, nah;
			std::string nextMon;
			if (getActiveMonitorShape(m_active, m_x, m_y, nax, nay, naw, nah, nextMon) &&
			    (nax != ax || nay != ay || nextMon != monName)) {
				// Transitioned to another monitor on the same machine locally!
				// Keep m_x and m_y as is.
			}
			else {
				// Hit the boundary of the client desktop with no neighbor. Clamp.
				m_x = xc;
				m_y = yc;
			}
		}
	}
	else {
		// Within the same monitor
		if (m_switchScreen != nullptr) {
			bool clearWait;
			SInt32 zoneSize = m_primaryClient->getJumpZoneSize();
			switch (m_switchDir) {
			case kLeft:
				clearWait = (m_x >= ax + zoneSize);
				break;
			case kRight:
				clearWait = (m_x <= ax + aw - 1 - zoneSize);
				break;
			case kTop:
				clearWait = (m_y >= ay + zoneSize);
				break;
			case kBottom:
				clearWait = (m_y <= ay + ah - 1 - zoneSize);
				break;
			default:
				clearWait = false;
				break;
			}
			if (clearWait) {
				noSwitch(m_x, m_y);
			}
		}
	}

	if (jump) {
		if (m_sendFileThread != nullptr) {
			StreamChunker::interruptFile();
			delete m_sendFileThread;
			m_sendFileThread = nullptr;
		}

		switchScreen(newScreen, m_x, m_y, false);
		return;
	}
	else {
		// warp cursor if it moved.
		if (m_x != xOld || m_y != yOld) {
			LOG((CLOG_DEBUG2 "move on %s to %d,%d", getName(m_active).c_str(), m_x, m_y));
			m_active->mouseMove(m_x, m_y);
		}
	}
}

void
Server::onMouseWheel(SInt32 xDelta, SInt32 yDelta)
{
	LOG((CLOG_DEBUG1 "onMouseWheel %+d,%+d", xDelta, yDelta));
	assert(m_active != NULL);

	// relay
	m_active->mouseWheel(xDelta, yDelta);
}

void
Server::onFileChunkSending(const void* data)
{
	auto* chunk = static_cast<FileChunk*>(const_cast<void*>(data));

	LOG((CLOG_DEBUG1 "sending file chunk"));
	assert(m_active != NULL);

	// relay
	m_active->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
}

void
Server::onFileRecieveCompleted()
{
	if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this]() { write_to_drop_dir_thread(); });
	}
}

void Server::write_to_drop_dir_thread()
{
	LOG((CLOG_DEBUG "starting write to drop dir thread"));

	while (m_screen->isFakeDraggingStarted()) {
		ARCH->sleep(.1f);
	}

	DropHelper::writeToDir(m_screen->getDropTarget(), m_fakeDragFileList,
					m_receivedFileData);
}

bool
Server::addClient(BaseClientProxy* client)
{
    std::string name = getName(client);
	if (m_clients.count(name) != 0) {
		return false;
	}

	// add event handlers
	m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleShapeChanged, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardGrabbed, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardChanged, client));

	// add to list
	m_clientSet.insert(client);
	m_clients.insert(std::make_pair(name, client));

	// initialize client data
	SInt32 x;
	SInt32 y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
	updateActiveMonitors();

	return true;
}

bool
Server::removeClient(BaseClientProxy* client)
{
	// return false if not in list
	auto i = m_clientSet.find(client);
	if (i == m_clientSet.end()) {
		return false;
	}

	// remove event handlers
	m_events->removeHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget());

	// remove from list
	m_clients.erase(getName(client));
	m_clientSet.erase(i);
	updateActiveMonitors();

	return true;
}

void
Server::closeClient(BaseClientProxy* client, const char* msg)
{
	assert(client != m_primaryClient);
	assert(msg != NULL);

	// send message to client.  this message should cause the client
	// to disconnect.  we add this client to the closed client list
	// and install a timer to remove the client if it doesn't respond
	// quickly enough.  we also remove the client from the active
	// client list since we're not going to listen to it anymore.
	// note that this method also works on clients that are not in
	// the m_clients list.  adoptClient() may call us with such a
	// client.
	LOG((CLOG_NOTE "disconnecting client \"%s\"", getName(client).c_str()));

	// send message
	// FIXME -- avoid type cast (kinda hard, though)
	(static_cast<ClientProxy*>(client))->close(msg);

	// install timer.  wait timeout seconds for client to close.
	double timeout = 5.0;
	EventQueueTimer* timer = m_events->newOneShotTimer(timeout, nullptr);
	m_events->adoptHandler(Event::kTimer, timer,
							new TMethodEventJob<Server>(this,
								&Server::handleClientCloseTimeout, client));

	// move client to closing list
	removeClient(client);
	m_oldClients.insert(std::make_pair(client, timer));

	// if this client is the active screen then we have to
	// jump off of it
	forceLeaveClient(client);
}

void
Server::closeClients(const Config& config)
{
	// collect the clients that are connected but are being dropped
	// from the configuration (or who's canonical name is changing).
	typedef std::set<BaseClientProxy*> RemovedClients;
	RemovedClients removed;
	for (auto & m_client : m_clients) {
		if (!config.isCanonicalName(m_client.first)) {
			removed.insert(m_client.second);
		}
	}

	// don't close the primary client
	removed.erase(m_primaryClient);

	// now close them.  we collect the list then close in two steps
	// because closeClient() modifies the collection we iterate over.
	for (auto index : removed) {
		closeClient(index, kMsgCClose);
	}
}

void
Server::removeActiveClient(BaseClientProxy* client)
{
	if (removeClient(client)) {
		forceLeaveClient(client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::removeOldClient(BaseClientProxy* client)
{
	auto i = m_oldClients.find(client);
	if (i != m_oldClients.end()) {
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		m_events->removeHandler(Event::kTimer, i->second);
		m_events->deleteTimer(i->second);
		m_oldClients.erase(i);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::forceLeaveClient(BaseClientProxy* client)
{
	BaseClientProxy* active =
		(m_activeSaver != nullptr) ? m_activeSaver : m_active;
	if (active == client) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);

		// stop waiting to switch to this client
		if (active == m_switchScreen) {
			stopSwitch();
		}

		// don't notify active screen since it has probably already
		// disconnected.
		LOG((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", getName(active).c_str(), getName(m_primaryClient).c_str(), m_x, m_y));

		// cut over
		m_active = m_primaryClient;

		// enter new screen (unless we already have because of the
		// screen saver)
		if (m_activeSaver == nullptr) {
			m_primaryClient->enter(m_x, m_y, m_seqNum,
								m_primaryClient->getToggleMask(), false);
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == client) {
		m_activeSaver = nullptr;
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
}


//
// Server::ClipboardInfo
//

Server::ClipboardInfo::ClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0)
{
	// do nothing
}


//
// Server::LockCursorToScreenInfo
//

Server::LockCursorToScreenInfo*
Server::LockCursorToScreenInfo::alloc(State state)
{
	auto* info =
		static_cast<LockCursorToScreenInfo*>(malloc(sizeof(LockCursorToScreenInfo)));
	info->m_state = state;
	return info;
}


//
// Server::SwitchToScreenInfo
//

Server::SwitchToScreenInfo*
Server::SwitchToScreenInfo::alloc(const std::string& screen)
{
	auto* info =
		static_cast<SwitchToScreenInfo*>(malloc(sizeof(SwitchToScreenInfo) +
								screen.size()));
	memcpy(info->m_screen, screen.c_str(), screen.size() + 1);
	return info;
}


//
// Server::SwitchInDirectionInfo
//

Server::SwitchInDirectionInfo*
Server::SwitchInDirectionInfo::alloc(EDirection direction)
{
	auto* info =
		static_cast<SwitchInDirectionInfo*>(malloc(sizeof(SwitchInDirectionInfo)));
	info->m_direction = direction;
	return info;
}

//
// Server::KeyboardBroadcastInfo
//

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state)
{
	auto* info =
		static_cast<KeyboardBroadcastInfo*>(malloc(sizeof(KeyboardBroadcastInfo)));
	info->m_state      = state;
	info->m_screens[0] = '\0';
	return info;
}

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state, const std::string& screens)
{
	auto* info =
		static_cast<KeyboardBroadcastInfo*>(malloc(sizeof(KeyboardBroadcastInfo) +
								screens.size()));
	info->m_state = state;
	memcpy(info->m_screens, screens.c_str(), screens.size() + 1);
	return info;
}

bool
Server::isReceivedFileSizeValid()
{
	return m_expectedFileSize == m_receivedFileData.size();
}

void
Server::sendFileToClient(const std::string& filename)
{
	if (m_sendFileThread != nullptr) {
		StreamChunker::interruptFile();
		delete m_sendFileThread;
		m_sendFileThread = nullptr;
	}

    m_sendFileThread = new Thread([this, filename]() { send_file_thread(filename); });
}

void Server::send_file_thread(std::string filename)
{
	try {
		LOG((CLOG_DEBUG "sending file to client, filename=%s", filename.c_str()));
		StreamChunker::sendFile(filename.c_str(), m_events, this);
	}
	catch (std::runtime_error &error) {
		LOG((CLOG_ERR "failed sending file chunks, error: %s", error.what()));
	}
}

void
Server::dragInfoReceived(UInt32 fileNum, std::string content)
{
	if (!m_args.m_enableDragDrop) {
		LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
		return;
	}

	DragInformation::parseDragInfo(m_fakeDragFileList, fileNum, content);

	m_screen->startDraggingFiles(m_fakeDragFileList);
}
