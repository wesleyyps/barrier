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

#include "barrier/ClientApp.h"

#include "barrier/ArgParser.h"
#include "barrier/protocol_types.h"
#include "client/Client.h"
#include "common/DataDirectories.h"
#include "server/Config.h"
#include <fstream>
#include <sstream>

#include <set>
#if SYSAPI_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "arch/Arch.h"
#include "arch/IArchTaskBarReceiver.h"
#include "barrier/ClientArgs.h"
#include "barrier/Screen.h"
#include "barrier/XScreen.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/String.h"
#include "base/TMethodEventJob.h"
#include "base/log_outputters.h"
#include "common/Version.h"
#include "mt/Thread.h"
#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"
#include "net/XSocket.h"

#if WINAPI_MSWINDOWS
#include "platform/MSWindowsScreen.h"
#elif WINAPI_XWINDOWS
#include "platform/XWindowsScreen.h"
#if BARRIER_BUILD_WAYLAND
#include "platform/WaylandScreen.h"
#endif
#include <cstdlib>
#elif WINAPI_CARBON
#include "platform/OSXScreen.h"
#endif

#ifdef __APPLE__
#include "platform/OSXDragSimulator.h"
#endif

#include <iostream>
#include <sstream>
#include <stdio.h>

#define RETRY_TIME 1.0

ClientApp::ClientApp(IEventQueue *events,
                     CreateTaskBarReceiverFunc createTaskBarReceiver)
    : App(events, createTaskBarReceiver, new ClientArgs()), m_client(nullptr),
      m_clientScreen(nullptr), m_serverAddress(nullptr),
      m_currentFallbackIndex(0), m_usingFallback(false),
      m_failbackInProgress(false), m_failbackProbeTimer(nullptr) {}

ClientApp::~ClientApp() {
  cleanupFailbackProbe();
  delete m_serverAddress;
}

static std::set<std::string> getLocalInterfaceIPs() {
  std::set<std::string> ips;
#if !SYSAPI_WIN32
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr != nullptr && ifa->ifa_addr->sa_family == AF_INET) {
        struct sockaddr_in *sa =
            reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        char addr[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &(sa->sin_addr), addr, sizeof(addr)) !=
            nullptr) {
          std::string ipStr(addr);
          if (ipStr != "127.0.0.1") {
            ips.insert(ipStr);
          }
        }
      }
    }
    freeifaddrs(ifaddr);
  }
#endif
  return ips;
}

void ClientApp::parseArgs(int argc, const char *const *argv) {
  ArgParser argParser(this);
  bool result = argParser.parseClientArgs(args(), argc, argv);

  if (!result || args().m_shouldExit) {
    m_bye(kExitArgs);
  } else {
    if (args().m_configFile.empty()) {
      std::vector<std::string> candidates;
      const char *home = getenv("HOME");
      if (home != nullptr) {
        candidates.push_back(std::string(home) + "/barrier.conf");
        candidates.push_back(std::string(home) + "/.barrier.conf");
      }
      barrier::fs::path profilePath = argsBase().m_profileDirectory;
      if (profilePath.empty()) {
        profilePath = barrier::DataDirectories::profile();
      }
      if (!profilePath.empty()) {
        candidates.push_back((profilePath / "barrier.conf").u8string());
        candidates.push_back((profilePath / ".barrier.conf").u8string());
      }
      candidates.push_back(
          (barrier::DataDirectories::systemconfig() / "barrier.conf")
              .u8string());
      candidates.push_back("/etc/barrier.conf");

      for (const auto &path : candidates) {
        if (barrier::fs::exists(path)) {
          args().m_configFile = path;
          LOG((CLOG_NOTE "Using default configuration file \"%s\"",
               path.c_str()));
          break;
        }
      }
    }

    // Read configuration file if specified
    if (!args().m_configFile.empty()) {
      LOG((CLOG_DEBUG "opening configuration \"%s\"",
           args().m_configFile.c_str()));
      std::ifstream configStream(args().m_configFile.c_str());
      if (!configStream.is_open()) {
        LOG((CLOG_ERR "cannot open configuration \"%s\"",
             args().m_configFile.c_str()));
        m_bye(kExitConfig);
      }

      try {
        Config config(m_events);
        configStream >> config;
        LOG((CLOG_DEBUG "configuration read successfully"));

        if (!config.getCryptoEnabled()) {
          argsBase().m_enableCrypto = false;
          LOG((CLOG_NOTE "Crypto disabled via config file"));
        }

        // Legacy interactive drag-drop disabled unconditionally in favor of
        // clipboard file copy & paste
        argsBase().m_enableDragDrop = false;

        if (argsBase().m_dropTarget.empty()) {
          std::string target = config.getDropTarget(args().m_name);
          if (!target.empty()) {
            argsBase().m_dropTarget = target;
            LOG((CLOG_NOTE "Drop target set to %s via config file",
                 argsBase().m_dropTarget.c_str()));
          }
        }

        auto localIps = getLocalInterfaceIPs();

        // Resolve client screen name from local network IP if current name is
        // not in config
        std::string canonName = config.getCanonicalName(args().m_name);
        bool nameInConfig = false;
        for (Config::const_iterator sc = config.begin(); sc != config.end();
             ++sc) {
          if (*sc == args().m_name || *sc == canonName) {
            nameInConfig = true;
            args().m_name = *sc;
            break;
          }
        }

        if (!nameInConfig) {
          const auto &nodes = config.getNetworkNodes();
          for (const auto &kv : nodes) {
            if (localIps.find(kv.second.ip) != localIps.end()) {
              args().m_name = kv.first;
              LOG((CLOG_NOTE "Client screen name dynamically resolved to "
                             "\"%s\" by local IP (%s)",
                   args().m_name.c_str(), kv.second.ip.c_str()));
              break;
            }
          }
        }

        int srvPort = config.getPort();
        if (srvPort <= 0) {
          srvPort = kDefaultPort;
        }

        // Build candidate server list from section: network (excluding own
        // local interfaces)
        const auto &nodes = config.getNetworkNodes();
        std::vector<std::pair<std::string, std::string>> remoteCandidates;
        m_clusterCandidates.clear();

        std::string configuredSrvIp = config.getServerIp();
        std::string configuredSrvName = config.getServerName();
        if (!configuredSrvIp.empty() &&
            localIps.find(configuredSrvIp) == localIps.end()) {
          remoteCandidates.push_back({configuredSrvName, configuredSrvIp});
          m_clusterCandidates.push_back(configuredSrvIp);
        }

        for (const auto &kv : nodes) {
          const std::string &nodeName = kv.first;
          const std::string &nodeIp = kv.second.ip;
          if (nodeIp.empty() || localIps.find(nodeIp) != localIps.end()) {
            continue;
          }
          bool already = false;
          for (const auto &c : remoteCandidates) {
            if (c.second == nodeIp) {
              already = true;
              break;
            }
          }
          if (!already) {
            remoteCandidates.push_back({nodeName, nodeIp});
          }
          if (std::find(m_clusterCandidates.begin(), m_clusterCandidates.end(),
                        nodeIp) == m_clusterCandidates.end()) {
            m_clusterCandidates.push_back(nodeIp);
          }
        }

        if (args().m_barrierAddress.empty()) {
          std::string activeIp;
          std::string activeName;

          for (const auto &candidate : remoteCandidates) {
            LOG((CLOG_DEBUG1 "Probing potential server %s (%s:%d)...",
                 candidate.first.c_str(), candidate.second.c_str(), srvPort));
            if (probeServerAddress(candidate.second, srvPort, 150)) {
              activeIp = candidate.second;
              activeName = candidate.first;
              LOG((CLOG_NOTE "Active server auto-detected at %s:%d (%s)",
                   activeIp.c_str(), srvPort, activeName.c_str()));
              break;
            }
          }

          if (!activeIp.empty()) {
            args().m_barrierAddress = activeIp + ":" + std::to_string(srvPort);
          } else if (!configuredSrvIp.empty()) {
            args().m_barrierAddress =
                configuredSrvIp + ":" + std::to_string(srvPort);
            LOG((CLOG_NOTE "No active server responded during probe; "
                           "defaulting to configured server %s:%d (%s)",
                 configuredSrvIp.c_str(), srvPort, configuredSrvName.c_str()));
          }
        }

        // Register all other remote candidates for dynamic failover
        for (const auto &candidate : remoteCandidates) {
          if (args().m_barrierAddress.find(candidate.second) != 0) {
            if (std::find(m_fallbackServerAddresses.begin(),
                          m_fallbackServerAddresses.end(), candidate.second) ==
                m_fallbackServerAddresses.end()) {
              m_fallbackServerAddresses.push_back(candidate.second);
            }
          }
        }
      } catch (XConfigRead &e) {
        LOG((CLOG_ERR "FATAL: %s", e.what()));
        m_bye(kExitConfig);
      }
    }

    // Check again if we have a server address (from args or from config)
    if (args().m_barrierAddress.empty()) {
      LOG((CLOG_PRINT "%s: a server address or name is required (pass on "
                      "command line or set 'server' in config file)" BYE,
           args().m_exename.c_str()));
      m_bye(kExitArgs);
    }

    // save server address
    if (!args().m_barrierAddress.empty()) {
      m_primaryServerAddress = args().m_barrierAddress;
      m_primaryServerHost = m_primaryServerAddress;
      size_t colon = m_primaryServerHost.find(':');
      if (colon != std::string::npos) {
        m_primaryServerHost = m_primaryServerHost.substr(0, colon);
      }
      loadFallbackAddressesFromCache();
      try {
        *m_serverAddress =
            NetworkAddress(args().m_barrierAddress, kDefaultPort);
        m_serverAddress->resolve();
      } catch (XSocketAddress &e) {
        // allow an address that we can't look up if we're restartable.
        // we'll try to resolve the address each time we connect to the
        // server.  a bad port will never get better.  patch by Brent
        // Priddy.
        if (!args().m_restartable || e.getError() == XSocketAddress::kBadPort) {
          LOG((CLOG_PRINT "%s: %s" BYE, args().m_exename.c_str(), e.what(),
               args().m_exename.c_str()));
          m_bye(kExitFailed);
        }
      }
    }
  }
}

void ClientApp::help() {
#if WINAPI_XWINDOWS
#define WINAPI_ARG " [--display <display>] [--no-xinitthreads]"
#define WINAPI_INFO                                                            \
  "      --display <display>  connect to the X server at <display>\n"          \
  "      --no-xinitthreads    do not call XInitThreads()\n"
#else
#define WINAPI_ARG ""
#define WINAPI_INFO ""
#endif

  std::ostringstream buffer;
  buffer
      << "Start the barrier client and connect to a remote server component.\n"
      << "\n"
      << "Usage: " << args().m_exename << " [--yscroll <delta>]" << WINAPI_ARG
      << HELP_SYS_ARGS << HELP_COMMON_ARGS << " <server-address>\n"
      << "\n"
      << "Options:\n"
      << HELP_COMMON_INFO_1 << WINAPI_INFO << HELP_SYS_INFO
      << "      --yscroll <delta>    defines the vertical scrolling delta, "
         "which is\n"
      << "                           120 by default.\n"
      << HELP_COMMON_INFO_2 << "\n"
      << "Default options are marked with a *\n"
      << "\n"
      << "The server address is of the form: [<hostname>][:<port>]. The "
         "hostname\n"
      << "must be the address or hostname of the server. Placing brackets "
         "around\n"
      << "an IPv6 address is required when also specifying a port number and \n"
      << "optional otherwise. The default port number is " << kDefaultPort
      << ".\n";

  LOG((CLOG_PRINT "%s", buffer.str().c_str()));
}

const char *ClientApp::daemonName() const {
#if SYSAPI_WIN32
  return "Barrier Client";
#elif SYSAPI_UNIX
  return "barrierc";
#endif
}

const char *ClientApp::daemonInfo() const {
#if SYSAPI_WIN32
  return "Allows another computer to share it's keyboard and mouse with this "
         "computer.";
#elif SYSAPI_UNIX
  return "";
#endif
}

barrier::Screen *ClientApp::createScreen() {
#if WINAPI_MSWINDOWS
  return new barrier::Screen(new MSWindowsScreen(false, args().m_noHooks,
                                                 args().m_stopOnDeskSwitch,
                                                 m_events),
                             m_events);
#elif WINAPI_XWINDOWS
#if BARRIER_BUILD_WAYLAND
  const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
  if (waylandDisplay != nullptr) {
    return new barrier::Screen(new WaylandScreen(false, m_events), m_events);
  }
#endif
  return new barrier::Screen(new XWindowsScreen(new XWindowsImpl(),
                                                args().m_display, false,
                                                args().m_disableXInitThreads,
                                                args().m_yscroll, m_events),
                             m_events);
#elif WINAPI_CARBON
  return new barrier::Screen(new OSXScreen(m_events, false), m_events);
#endif
}

void ClientApp::updateStatus() { updateStatus(""); }

void ClientApp::updateStatus(const String &msg) {
  if (m_taskBarReceiver != nullptr) {
    m_taskBarReceiver->updateStatus(m_client, msg);
  }
}

void ClientApp::resetRestartTimeout() {
  // retry time can nolonger be changed
  // s_retryTime = 0.0;
}

double ClientApp::nextRestartTimeout() {
  // retry at a constant rate (Issue 52)
  return RETRY_TIME;

  /*
  // choose next restart timeout.  we start with rapid retries
  // then slow down.
  if (s_retryTime < 1.0) {
  s_retryTime = 1.0;
  }
  else if (s_retryTime < 3.0) {
  s_retryTime = 3.0;
  }
  else {
  s_retryTime = 5.0;
  }
  return s_retryTime;
  */
}

void ClientApp::handleScreenError(const Event &, void *) {
  LOG((CLOG_CRIT "error on screen"));
  m_events->addEvent(Event(Event::kQuit));
}

barrier::Screen *ClientApp::openClientScreen() {
  barrier::Screen *screen = createScreen();
  if (!argsBase().m_dropTarget.empty()) {
    screen->setDropTarget(argsBase().m_dropTarget);
  }
  screen->setEnableDragDrop(false);
  m_events->adoptHandler(
      m_events->forIScreen().error(), screen->getEventTarget(),
      new TMethodEventJob<ClientApp>(this, &ClientApp::handleScreenError));
  return screen;
}

void ClientApp::closeClientScreen(barrier::Screen *screen) {
  if (screen != nullptr) {
    m_events->removeHandler(m_events->forIScreen().error(),
                            screen->getEventTarget());
    delete screen;
  }
}

void ClientApp::handleClientRestart(const Event &, void *vtimer) {
  // discard old timer
  auto *timer = static_cast<EventQueueTimer *>(vtimer);
  m_events->deleteTimer(timer);
  m_events->removeHandler(Event::kTimer, timer);

  // reconnect
  startClient();
}

void ClientApp::scheduleClientRestart(double retryTime) {
  // install a timer and handler to retry later
  LOG((CLOG_DEBUG "retry in %.0f seconds", retryTime));
  EventQueueTimer *timer = m_events->newOneShotTimer(retryTime, nullptr);
  m_events->adoptHandler(Event::kTimer, timer,
                         new TMethodEventJob<ClientApp>(
                             this, &ClientApp::handleClientRestart, timer));
}

void ClientApp::handleClientConnected(const Event &, void *) {
  // using CLOG_PRINT here allows the GUI to see that the client is connected
  // regardless of which log level is set
  LOG((CLOG_PRINT "connected to server"));
  resetRestartTimeout();
  m_failbackInProgress = false;
  updateStatus();

  std::string connectedHost =
      (m_serverAddress != nullptr) ? m_serverAddress->getHostname() : "";
  int connectedPort =
      (m_serverAddress != nullptr && m_serverAddress->getPort() != 0)
          ? m_serverAddress->getPort()
          : kDefaultPort;

  bool isClusterNode =
      (std::find(m_clusterCandidates.begin(), m_clusterCandidates.end(),
                 connectedHost) != m_clusterCandidates.end());
  if (isClusterNode) {
    m_primaryServerAddress =
        connectedHost + ":" + std::to_string(connectedPort);
    m_primaryServerHost = connectedHost;
    m_usingFallback = false;
    cleanupFailbackProbe();
    LOG((CLOG_NOTE "Active cluster server confirmed as primary: %s",
         m_primaryServerAddress.c_str()));
  } else if (m_usingFallback) {
    LOG((CLOG_NOTE "connected to server via fallback address (%s). Scheduling "
                   "failback probe for primary (%s)",
         connectedHost.c_str(), m_primaryServerAddress.c_str()));
    scheduleFailbackProbe();
  }
}

void ClientApp::handleClientFailed(const Event &e, void *) {
  auto *info = static_cast<Client::FailInfo *>(e.getData());

  updateStatus(String("Failed to connect to server: ") + info->m_what);

  if (m_failbackInProgress) {
    m_failbackInProgress = false;
    LOG((CLOG_NOTE "dynamic failover: failback in progress, reconnecting to "
                   "primary (%s)...",
         m_primaryServerAddress.c_str()));
    if (args().m_restartable && !m_suspended) {
      scheduleClientRestart(0.1);
    }
    delete info;
    return;
  }

  bool switched = false;
  if (args().m_restartable && info->m_retry) {
    switched = selectNextAddress();
  }

  if (!args().m_restartable || !info->m_retry) {
    LOG((CLOG_ERR "failed to connect to server: %s", info->m_what.c_str()));
    m_events->addEvent(Event(Event::kQuit));
  } else {
    LOG((CLOG_WARN "failed to connect to server: %s", info->m_what.c_str()));
    if (!m_suspended) {
      double retry = switched ? 0.5 : nextRestartTimeout();
      scheduleClientRestart(retry);
    }
  }
  delete info;
}

void ClientApp::handleClientDisconnected(const Event &, void *) {
  LOG((CLOG_NOTE "disconnected from server"));
  cleanupFailbackProbe();
  if (!args().m_restartable) {
    m_events->addEvent(Event(Event::kQuit));
  } else if (!m_suspended) {
    if (m_failbackInProgress) {
      m_failbackInProgress = false;
      LOG((CLOG_NOTE "dynamic failover: reconnecting to primary server (%s)...",
           m_primaryServerAddress.c_str()));
      scheduleClientRestart(0.1);
    } else {
      bool switched = false;
      if (!m_fallbackServerAddresses.empty()) {
        switched = selectNextAddress();
      }
      double retry = switched ? 0.5 : nextRestartTimeout();
      scheduleClientRestart(retry);
    }
  }
  updateStatus();
}

Client *ClientApp::openClient(const String &name, const NetworkAddress &address,
                              barrier::Screen *screen) {
  auto *client = new Client(
      m_events, name, address,
      new TCPSocketFactory(m_events, getSocketMultiplexer()), screen, args());

  try {
    m_events->adoptHandler(m_events->forClient().connected(),
                           client->getEventTarget(),
                           new TMethodEventJob<ClientApp>(
                               this, &ClientApp::handleClientConnected));

    m_events->adoptHandler(
        m_events->forClient().connectionFailed(), client->getEventTarget(),
        new TMethodEventJob<ClientApp>(this, &ClientApp::handleClientFailed));

    m_events->adoptHandler(m_events->forClient().disconnected(),
                           client->getEventTarget(),
                           new TMethodEventJob<ClientApp>(
                               this, &ClientApp::handleClientDisconnected));

    m_events->adoptHandler(
        m_events->forClient().serverAddressesReceived(),
        client->getEventTarget(),
        new TMethodEventJob<ClientApp>(
            this, &ClientApp::handleServerAddressesReceived));

  } catch (std::bad_alloc &ba) {
    delete client;
    throw ba;
  }

  return client;
}

void ClientApp::closeClient(Client *client) {
  if (client == nullptr) {
    return;
  }

  m_events->removeHandler(m_events->forClient().connected(), client);
  m_events->removeHandler(m_events->forClient().connectionFailed(), client);
  m_events->removeHandler(m_events->forClient().disconnected(), client);
  m_events->removeHandler(m_events->forClient().serverAddressesReceived(),
                          client);
  cleanupFailbackProbe();
  delete client;
}

int ClientApp::foregroundStartup(int argc, char **argv) {
  initApp(argc, argv);

  // never daemonize
  return mainLoop();
}

bool ClientApp::startClient() {
  double retryTime;
  barrier::Screen *clientScreen = nullptr;
  try {
    if (m_clientScreen == nullptr) {
      clientScreen = openClientScreen();
      m_client = openClient(args().m_name, *m_serverAddress, clientScreen);
      m_clientScreen = clientScreen;
      LOG((CLOG_NOTE "started client"));
    } else if (m_client != nullptr && m_serverAddress != nullptr) {
      m_client->setServerAddress(*m_serverAddress);
    }

    m_client->connect();

    updateStatus();
    return true;
  } catch (XScreenUnavailable &e) {
    LOG((CLOG_WARN "secondary screen unavailable: %s", e.what()));
    closeClientScreen(clientScreen);
    updateStatus(String("secondary screen unavailable: ") + e.what());
    retryTime = e.getRetryTime();
  } catch (XScreenOpenFailure &e) {
    LOG((CLOG_CRIT "failed to start client: %s", e.what()));
    closeClientScreen(clientScreen);
    return false;
  } catch (XBase &e) {
    LOG((CLOG_CRIT "failed to start client: %s", e.what()));
    closeClientScreen(clientScreen);
    return false;
  }

  if (args().m_restartable) {
    scheduleClientRestart(retryTime);
    return true;
  }
  // don't try again
  return false;
}

void ClientApp::stopClient() {
  cleanupFailbackProbe();
  closeClient(m_client);
  closeClientScreen(m_clientScreen);
  m_client = nullptr;
  m_clientScreen = nullptr;
}

int ClientApp::mainLoop() {
  // create socket multiplexer.  this must happen after daemonization
  // on unix because threads evaporate across a fork().
  setSocketMultiplexer(std::make_unique<SocketMultiplexer>());

  writePidFile();

  // start client, etc
  appUtil().startNode();

  // init ipc client after node start, since create a new screen wipes out
  // the event queue (the screen ctors call adoptBuffer).
  if (argsBase().m_enableIpc) {
    initIpcClient();
  }

  // run event loop.  if startClient() failed we're supposed to retry
  // later.  the timer installed by startClient() will take care of
  // that.
  DAEMON_RUNNING(true);

#ifdef MAC_OS_X_VERSION_10_7

  Thread thread([this]() { run_events_loop(); });

  // wait until carbon loop is ready
  auto *screen = dynamic_cast<OSXScreen *>(m_clientScreen->getPlatformScreen());
  screen->waitForCarbonLoop();

  runCocoaApp();
#else
  m_events->loop();
#endif

  DAEMON_RUNNING(false);

  // close down
  LOG((CLOG_DEBUG1 "stopping client"));
  stopClient();
  updateStatus();
  LOG((CLOG_NOTE "stopped client"));

  if (argsBase().m_enableIpc) {
    cleanupIpcClient();
  }

  removePidFile();

  return kExitSuccess;
}

static int daemonMainLoopStatic(int argc, const char **argv) {
  return ClientApp::instance().daemonMainLoop(argc, argv);
}

int ClientApp::standardStartup(int argc, char **argv) {
  initApp(argc, argv);

  // daemonize if requested
  if (args().m_daemon) {
    return ARCH->daemonize(daemonName(), &daemonMainLoopStatic);
  }
  return mainLoop();
}

int ClientApp::runInner(int argc, char **argv, ILogOutputter *outputter,
                        StartupFunc startup) {
  // general initialization
  m_serverAddress = new NetworkAddress;
  argsBase().m_exename = ArgParser::parse_exename(argv[0]);

  // install caller's output filter
  if (outputter != nullptr) {
    CLOG->insert(outputter);
  }

  int result;
  try {
    // run
    result = startup(argc, argv);
  } catch (...) {
    if (m_taskBarReceiver != nullptr) {
      // done with task bar receiver
      delete m_taskBarReceiver;
    }

    delete m_serverAddress;

    throw;
  }

  if (m_taskBarReceiver != nullptr) {
    // done with task bar receiver
    delete m_taskBarReceiver;
  }

  delete m_serverAddress;
  m_serverAddress = nullptr;

  return result;
}

void ClientApp::startNode() {
  // start the client.  if this return false then we've failed and
  // we shouldn't retry.
  LOG((CLOG_DEBUG1 "starting client"));
  if (!startClient()) {
    m_bye(kExitFailed);
  }
}

void ClientApp::loadConfig() { loadConfig(args().m_configFile); }

String ClientApp::getConfigFilePath() const { return args().m_configFile; }

bool ClientApp::loadConfig(const String &pathname) {
  if (pathname.empty()) {
    return false;
  }

  try {
    std::ifstream configStream(pathname.c_str());
    if (!configStream) {
      LOG((CLOG_WARN "cannot read configuration \"%s\"", pathname.c_str()));
      return false;
    }

    Config config(m_events);
    configStream >> config;

    // Dynamically configure log file based on configuration
    if (args().m_logFile == NULL) {
      std::string logPath = config.getLogFilename();
      if (logPath.empty()) {
        size_t pos = pathname.find_last_of("/\\");
        if (pos != String::npos) {
          logPath = pathname.substr(0, pos) + "/barrier.log";
        } else {
          logPath = "barrier.log";
        }
      }
      char *logFileStr = new char[logPath.length() + 1];
      strcpy(logFileStr, logPath.c_str());
      args().m_logFile = logFileStr;
      setupFileLogging();
    }

    if (!config.getServerIp().empty() && args().m_barrierAddress.empty()) {
      args().m_barrierAddress = config.getServerIp();
      m_primaryServerAddress = args().m_barrierAddress;
      m_primaryServerHost = m_primaryServerAddress;
      size_t colon = m_primaryServerHost.find(':');
      if (colon != std::string::npos) {
        m_primaryServerHost = m_primaryServerHost.substr(0, colon);
      }
      loadFallbackAddressesFromCache();
      try {
        if (m_serverAddress == nullptr)
          m_serverAddress = new NetworkAddress;
        *m_serverAddress =
            NetworkAddress(args().m_barrierAddress, kDefaultPort);
        m_serverAddress->resolve();
        LOG((CLOG_NOTE "Server IP dynamically set to %s via config options",
             args().m_barrierAddress.c_str()));
      } catch (...) {
        LOG((CLOG_WARN "Failed to resolve Server IP from config"));
      }
    }

    const auto &nodes = config.getNetworkNodes();
    auto localIps = getLocalInterfaceIPs();
    for (const auto &kv : nodes) {
      const std::string &nodeIp = kv.second.ip;
      if (!nodeIp.empty() && localIps.find(nodeIp) == localIps.end()) {
        if (std::find(m_clusterCandidates.begin(), m_clusterCandidates.end(),
                      nodeIp) == m_clusterCandidates.end()) {
          m_clusterCandidates.push_back(nodeIp);
        }
      }
    }

    return true;
  } catch (XConfigRead &e) {
    LOG((CLOG_ERR "configuration error: %s", e.what()));
    return false;
  }
}

void ClientApp::handleServerAddressesReceived(const Event &e, void *) {
  auto *addrsStr = static_cast<String *>(e.getData());
  if (addrsStr == nullptr) {
    return;
  }

  LOG((CLOG_NOTE "dynamic failover: processing server advertised addresses: %s",
       addrsStr->c_str()));

  std::stringstream ss(*addrsStr);
  std::string ip;

  while (std::getline(ss, ip, ',')) {
    ip.erase(0, ip.find_first_not_of(" \t\r\n"));
    ip.erase(ip.find_last_not_of(" \t\r\n") + 1);
    if (ip.empty())
      continue;

    if (ip != m_primaryServerHost && ip != m_primaryServerAddress) {
      if (std::find(m_fallbackServerAddresses.begin(),
                    m_fallbackServerAddresses.end(),
                    ip) == m_fallbackServerAddresses.end()) {
        m_fallbackServerAddresses.push_back(ip);
      }
    }
  }

  for (const auto &candidate : m_clusterCandidates) {
    if (candidate != m_primaryServerHost &&
        candidate != m_primaryServerAddress) {
      if (std::find(m_fallbackServerAddresses.begin(),
                    m_fallbackServerAddresses.end(),
                    candidate) == m_fallbackServerAddresses.end()) {
        m_fallbackServerAddresses.push_back(candidate);
      }
    }
  }

  saveFallbackAddressesToCache();
  LOG((CLOG_NOTE
       "dynamic failover: %d total fallback/cluster address(es) registered",
       (int)m_fallbackServerAddresses.size()));

  delete addrsStr;
}

bool ClientApp::selectNextAddress() {
  std::vector<std::string> allTargets;
  if (!m_primaryServerHost.empty()) {
    allTargets.push_back(m_primaryServerHost);
  }
  for (const auto &addr : m_fallbackServerAddresses) {
    if (std::find(allTargets.begin(), allTargets.end(), addr) ==
        allTargets.end()) {
      allTargets.push_back(addr);
    }
  }
  for (const auto &addr : m_clusterCandidates) {
    if (std::find(allTargets.begin(), allTargets.end(), addr) ==
        allTargets.end()) {
      allTargets.push_back(addr);
    }
  }

  if (allTargets.empty()) {
    return false;
  }

  int port = (m_serverAddress != nullptr && m_serverAddress->getPort() != 0)
                 ? m_serverAddress->getPort()
                 : kDefaultPort;

  std::string currentHost =
      (m_serverAddress != nullptr) ? m_serverAddress->getHostname() : "";

  // 1. FAST PROBE: Check if any alternative candidate is actively listening on
  // port 24800 right now
  for (const auto &target : allTargets) {
    if (target != currentHost) {
      if (probeServerAddress(target, port, 100)) {
        LOG((CLOG_NOTE "dynamic failover: auto-detected active server at "
                       "%s:%d, switching...",
             target.c_str(), port));
        m_usingFallback = (target != m_primaryServerHost);
        try {
          if (m_serverAddress == nullptr) {
            m_serverAddress = new NetworkAddress;
          }
          *m_serverAddress = NetworkAddress(target, port);
          m_serverAddress->resolve();
          if (m_client != nullptr) {
            m_client->setServerAddress(*m_serverAddress);
          }
          return true;
        } catch (...) {
        }
      }
    }
  }

  // 2. ROUND-ROBIN FALLBACK: If no server responded to quick probe, try next
  // candidate
  m_currentFallbackIndex = (m_currentFallbackIndex + 1) % allTargets.size();
  std::string nextTarget = allTargets[m_currentFallbackIndex];
  LOG((CLOG_NOTE "dynamic failover: probe found no active server; trying next "
                 "target %s:%d (index %zu/%zu)",
       nextTarget.c_str(), port, m_currentFallbackIndex + 1,
       allTargets.size()));

  m_usingFallback = (nextTarget != m_primaryServerHost);
  try {
    if (m_serverAddress == nullptr) {
      m_serverAddress = new NetworkAddress;
    }
    *m_serverAddress = NetworkAddress(nextTarget, port);
    m_serverAddress->resolve();
    if (m_client != nullptr) {
      m_client->setServerAddress(*m_serverAddress);
    }
    return true;
  } catch (...) {
    return false;
  }
}

void ClientApp::resetToPrimaryAddress() {
  if (m_usingFallback) {
    m_usingFallback = false;
    m_currentFallbackIndex = 0;
    cleanupFailbackProbe();
    LOG((
        CLOG_NOTE
        "dynamic failover: resetting connection target to primary address (%s)",
        m_primaryServerAddress.c_str()));
    try {
      if (m_serverAddress == nullptr)
        m_serverAddress = new NetworkAddress;
      std::string cleanHost = m_primaryServerAddress;
      int cleanPort = kDefaultPort;
      size_t colon = cleanHost.find(':');
      if (colon != std::string::npos) {
        cleanPort = std::stoi(cleanHost.substr(colon + 1));
        cleanHost = cleanHost.substr(0, colon);
      }
      *m_serverAddress = NetworkAddress(cleanHost, cleanPort);
      m_serverAddress->resolve();
      if (m_client != nullptr) {
        m_client->setServerAddress(*m_serverAddress);
      }
    } catch (...) {
    }
  }
}

void ClientApp::scheduleFailbackProbe() {
  cleanupFailbackProbe();
  if (!m_usingFallback || m_primaryServerAddress.empty()) {
    return;
  }
  // Probe primary server every 5 seconds
  EventQueueTimer *timer = m_events->newTimer(5.0, nullptr);
  m_events->adoptHandler(Event::kTimer, timer,
                         new TMethodEventJob<ClientApp>(
                             this, &ClientApp::handleFailbackProbe, timer));
  m_failbackProbeTimer = timer;
}

void ClientApp::cleanupFailbackProbe() {
  if (m_failbackProbeTimer != nullptr) {
    m_events->deleteTimer(m_failbackProbeTimer);
    m_failbackProbeTimer = nullptr;
  }
}

void ClientApp::handleFailbackProbe(const Event &, void *vtimer) {
  m_events->deleteTimer(static_cast<EventQueueTimer *>(vtimer));
  m_failbackProbeTimer = nullptr;

  if (!m_usingFallback || m_client == nullptr || !m_client->isConnected()) {
    return;
  }

  if (probeServerAddress(m_primaryServerAddress, kDefaultPort)) {
    LOG((CLOG_NOTE "dynamic failover: primary cable server (%s) is back "
                   "online! Switching from fallback back to primary.",
         m_primaryServerAddress.c_str()));
    m_failbackInProgress = true;
    resetToPrimaryAddress();
    if (m_client != nullptr) {
      m_client->disconnect(nullptr);
    }
  } else {
    scheduleFailbackProbe();
  }
}

bool ClientApp::probeServerAddress(const std::string &host, int port,
                                   int timeoutMs) {
  std::string cleanHost = host;
  int cleanPort = port;
  size_t colon = cleanHost.find(':');
  if (colon != std::string::npos) {
    cleanPort = std::stoi(cleanHost.substr(colon + 1));
    cleanHost = cleanHost.substr(0, colon);
  }
#if SYSAPI_WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return false;
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
  struct addrinfo hints, *resAddr = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", cleanPort);
  if (getaddrinfo(cleanHost.c_str(), portStr, &hints, &resAddr) != 0 ||
      resAddr == nullptr) {
    closesocket(s);
    return false;
  }
  connect(s, resAddr->ai_addr, (int)resAddr->ai_addrlen);
  freeaddrinfo(resAddr);
  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(s, &wset);
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  int res = select((int)s + 1, nullptr, &wset, nullptr, &tv);
  closesocket(s);
  return (res > 0);
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return false;
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  struct addrinfo hints, *resAddr = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", cleanPort);
  if (getaddrinfo(cleanHost.c_str(), portStr, &hints, &resAddr) != 0 ||
      resAddr == nullptr) {
    close(s);
    return false;
  }
  int res = connect(s, resAddr->ai_addr, resAddr->ai_addrlen);
  freeaddrinfo(resAddr);
  if (res == 0) {
    close(s);
    return true;
  }
  if (errno == EINPROGRESS) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(s + 1, nullptr, &wset, nullptr, &tv);
    if (sel > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
        close(s);
        return true;
      }
    }
  }
  close(s);
  return false;
#endif
}

String ClientApp::getFallbackCachePath() const {
  barrier::fs::path profile_path = barrier::DataDirectories::profile();
  if (!profile_path.empty()) {
    try {
      barrier::fs::create_directories(profile_path);
      return (profile_path / "server_fallback.cache").u8string();
    } catch (...) {
    }
  }
  return "/tmp/barrier_server_fallback.cache";
}

void ClientApp::saveFallbackAddressesToCache() {
  String path = getFallbackCachePath();
  if (path.empty() || m_fallbackServerAddresses.empty())
    return;
  std::ofstream f(path.c_str());
  if (f.is_open()) {
    for (const auto &addr : m_fallbackServerAddresses) {
      f << addr << "\n";
    }
    LOG((CLOG_DEBUG1 "saved fallback addresses to cache: %s", path.c_str()));
  }
}

void ClientApp::loadFallbackAddressesFromCache() {
  String path = getFallbackCachePath();
  if (path.empty())
    return;
  std::ifstream f(path.c_str());
  if (!f.is_open())
    return;
  std::string line;
  while (std::getline(f, line)) {
    line.erase(0, line.find_first_not_of(" \t\r\n"));
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    if (!line.empty() && line != m_primaryServerHost &&
        line != m_primaryServerAddress) {
      if (std::find(m_fallbackServerAddresses.begin(),
                    m_fallbackServerAddresses.end(),
                    line) == m_fallbackServerAddresses.end()) {
        m_fallbackServerAddresses.push_back(line);
      }
    }
  }
  if (!m_fallbackServerAddresses.empty()) {
    LOG((CLOG_NOTE "dynamic failover: %d fallback address(es) registered",
         (int)m_fallbackServerAddresses.size()));
  }
}
