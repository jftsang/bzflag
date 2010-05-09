/* bzflag
 * Copyright (c) 1993-2010 Tim Riker
 *
 * This package is free software;  you can redistribute it and/or
 * modify it under the terms of the license found in the file
 * named COPYING that should have accompanied this file.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */
#if defined(_MSC_VER)
#  pragma warning(disable: 4786)
#endif

// interface header
#include "ServerLink.h"

#if defined(DEBUG)
#  define NETWORK_STATS
#endif

// system headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>
#include <vector>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

// common implementation headers
#include "ErrorHandler.h"
// invoke persistent rebuilding for current version dates
#include "version.h"
#if defined(NETWORK_STATS)
#include "bzfio.h"
#endif
#include "BzTime.h"

#include "GameTime.h"
#include "bzUnicode.h"

#ifndef BUILDING_BZADMIN
// bzflag local implementation headers
#include "playing.h"
#include "MsgStrings.h"
#endif

#define UDEBUG if (UDEBUGMSG) printf
#define UDEBUGMSG false

#if defined(NETWORK_STATS)
static BzTime	startTime;
static uint32_t		bytesSent;
static uint32_t		bytesReceived;
static uint32_t		packetsSent;
static uint32_t		packetsReceived;
#endif

#if defined(_WIN32)
DWORD ThreadID;		// Thread ID
HANDLE hConnected;	// "Connected" event
HANDLE hThread;		// Connection thread

typedef struct {
  int query;
  CNCTType* addr;
  int saddr;
} TConnect;

TConnect conn;

DWORD WINAPI ThreadConnect(LPVOID params)
{
  TConnect *_conn = (TConnect*)params;
  if(connect(_conn->query, _conn->addr, _conn->saddr) >= 0) {
    SetEvent(hConnected); // Connect successful
  }
  ExitThread(0);
  return 0;
}

#endif // !defined(_WIN32)

// FIXME -- packet recording
FILE* packetStream = NULL;
BzTime packetStartTime;
static const unsigned long serverPacket = 1;
static const unsigned long endPacket = 0;


ServerLink* ServerLink::server = NULL;


ServerLink::ServerLink(const std::string& serverName,
                       const Address& serverAddress, int port)
: state(SocketError) // assume failure
, fd(-1)             // assume failure
, udpLength(0)
, oldNeedForSpeed(false)
, previousFill(0)
, joinServer(serverName)
, joinPort(port)
{
  int i;

  struct protoent* p;
#if defined(_WIN32)
  BOOL off = FALSE;
#else
  int off = 0;
#endif

  // standard server has no special abilities;
  server_abilities = Nothing;

  // queue is empty

  urecvfd = -1;

  ulinkup = false;

  // initialize version to a bogus number
  strcpy(version, "BZFS0000");

  // open connection to server.  first connect to given port.
  // don't wait too long.
  int query = (int)socket(AF_INET, SOCK_STREAM, 0);
  if (query < 0) return;

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = serverAddress;

  UDEBUG("Remote %s\n", inet_ntoa(addr.sin_addr));

  // for UDP, used later
  memcpy((unsigned char *)&usendaddr,(unsigned char *)&addr, sizeof(addr));

  bool okay = true;
  int fdMax = query;
  struct timeval timeout;
  fd_set write_set;
  fd_set read_set;
  int nfound;

#if !defined(_WIN32)
  // for standard BSD sockets

  // Open a connection.
  // we are blocking at this point so we will wait till we connect, or error
  int connectReturn = connect(query, (CNCTType*)&addr, sizeof(addr));

  logDebugMessage(2,"CONNECT:non windows inital connect returned %d\n",connectReturn);

  // check for a real error
  // in progress is a holdover from when we did this as non blocking.
  // we swaped to blockin because we changed from having
  // the server send the first data, to having the client send it.
  int error = 0;
  if (connectReturn != 0) {
    error = getErrno();
    if (error != EINPROGRESS) {
      // if it was a real error, log and bail
      logDebugMessage(1,"CONNECT:error in connect, error returned %d\n",error);

      close(query);
      return;
    }
  }

  // call a select to make sure the socket is good and ready.
  FD_ZERO(&write_set);
  FD_SET((unsigned int)query, &write_set);
  timeout.tv_sec = long(5);
  timeout.tv_usec = 0;
  nfound = select(fdMax + 1, NULL, (fd_set*)&write_set, NULL, &timeout);
  error = getErrno();
  logDebugMessage(2,"CONNECT:non windows inital select nfound = %d error = %d\n",nfound,error);

  // if no sockets are active then we are done.
  if (nfound <= 0) {
    close(query);
    return;
  }

  // if there are any connection errors, check them and we are done
  int       connectError;
  socklen_t errorLen = sizeof(int);
  if (getsockopt(query, SOL_SOCKET, SO_ERROR, &connectError, &errorLen) < 0) {
    close(query);
    return;
  }
  if (connectError != 0) {
    logDebugMessage(2,"CONNECT:non getsockopt connectError = %d\n",connectError);
    close(query);
    return;
  }
#else // Connection timeout for Windows

  // winsock connection
  // Initialize structure
  conn.query = query;
  conn.addr = (CNCTType*)&addr;
  conn.saddr = sizeof(addr);

  // Create event
  hConnected = CreateEvent(NULL, FALSE, FALSE, "Connected Event");

  hThread = CreateThread(NULL, 0, ThreadConnect, &conn, 0, &ThreadID);
  okay = (WaitForSingleObject(hConnected, 5000) == WAIT_OBJECT_0);
  if(!okay)
    TerminateThread(hThread ,1);

  // Do some cleanup
  CloseHandle(hConnected);
  CloseHandle(hThread);

#endif // !defined(_WIN32)

  // if the connection failed for any reason, we can not continue
  if (!okay) {
    close(query);
    return;
  }

  // send out the connect header
  // this will let the server know we are BZFS protocol.
  // after the server gets this it will send back a version for us to check
  int sendRepply = ::send(query,BZ_CONNECT_HEADER,(int)strlen(BZ_CONNECT_HEADER),0);

  logDebugMessage(2,"CONNECT:send in connect returned %d\n",sendRepply);

  // wait to get data back. we are still blocking so these
  // calls should be sync.

  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  FD_SET((unsigned int)query, &read_set);
  FD_SET((unsigned int)query, &write_set);

  timeout.tv_sec = long(10);
  timeout.tv_usec = 0;

  // pick some limit to time out on ( in seconds )
  double thisStartTime = BzTime::getCurrent().getSeconds();
  double connectTimeout = 30.0;
  if (BZDB.isSet("connectionTimeout"))
    connectTimeout = BZDB.eval("connectionTimeout")  ;

  bool gotNetData = false;

  // loop calling select untill we read some data back.
  // its only 8 bytes so it better come back in one packet.
  int loopCount = 0;
  while(!gotNetData) {
    loopCount++;
    nfound = select(fdMax + 1, (fd_set*)&read_set, (fd_set*)&write_set, NULL, &timeout);

    // there has to be at least one socket active, or we are screwed
    if (nfound <= 0) {
      logDebugMessage(1,"CONNECT:select in connect failed, nfound = %d\n",nfound);
      close(query);
      return;
    }

    // try and get data back from the server
    i = recv(query, (char*)version, 8, 0);

    // if we got some, then we are done
    if (i > 0) {
      logDebugMessage(2,"CONNECT:got net data in connect, bytes read = %d\n",i);
      logDebugMessage(2,"CONNECT:Time To Connect = %f\n",(BzTime::getCurrent().getSeconds() - thisStartTime));
      gotNetData = true;
    } else {
      // if we have waited too long, then bail
      if ((BzTime::getCurrent().getSeconds() - thisStartTime) > connectTimeout) {
	logDebugMessage(1,"CONNECT:connect time out failed\n");
	logDebugMessage(2,"CONNECT:connect loop count = %d\n",loopCount);
	close(query);
	return;
      }

      BzTime::sleep(0.25f);
    }
  }

  logDebugMessage(2,"CONNECT:connect loop count = %d\n",loopCount);

  // if we got back less then the expected connect responce (BZFSXXXX)
  // then something went bad, and we are done.
  if (i < 8) {
    close(query);
    return;
  }

  // since we are connected, we can go non blocking
  // on BSD sockets systems
  // all other messages after this are handled via the normal
  // message system
#if !defined(_WIN32)
  if (BzfNetwork::setNonBlocking(query) < 0) {
    close(query);
    return;
  }
#endif

  if (debugLevel >= 1) {
    char cServerVersion[128];
    snprintf(cServerVersion, 128, "Server version: '%8s'",version);
    printError(cServerVersion);
  }

  // FIXME is it ok to try UDP always?
  server_abilities |= CanDoUDP;
  if (strcmp(version, getServerVersion()) != 0) {
    state = BadVersion;

    if (strcmp(version, BanRefusalString) == 0) {
      state = Refused;
      char message[512];
      int len = recv(query, (char*)message, 512, 0);
      if (len > 0) {
	message[len - 1] = 0;
      } else {
	message[0] = 0;
      }
      rejectionMessage = message;
    }

    close(query);
    return;
  }

  // read local player's id
#if !defined(_WIN32)
  FD_ZERO(&read_set);
  FD_SET((unsigned int)query, &read_set);
  timeout.tv_sec = long(5);
  timeout.tv_usec = 0;
  nfound = select(fdMax + 1, (fd_set*)&read_set, NULL, NULL, &timeout);
  if (nfound <= 0) {
    close(query);
    return;
  }
#endif // !defined(_WIN32)
  i = recv(query, (char *) &id, sizeof(id), 0);
  if (i < (int) sizeof(id))
    return;
  if (id == 0xff) {
    state = Rejected;
    close(query);
    return;
  }

#if !defined(_WIN32)
  if (BzfNetwork::setBlocking(query) < 0) {
    close(query);
    return;
  }
#endif // !defined(_WIN32)

  fd = query;

  // turn on TCP no delay
  p = getprotobyname("tcp");
  if (p)
    setsockopt(fd, p->p_proto, TCP_NODELAY, (SSOType)&off, sizeof(off));  // changed

  state = Okay;
#if defined(NETWORK_STATS)
  startTime = BzTime::getCurrent();
  bytesSent = 0;
  bytesReceived = 0;
  packetsSent = 0;
  packetsReceived = 0;
#endif

  // FIXME -- packet recording
  if (getenv("BZFLAGSAVE")) {
    packetStream = fopen(getenv("BZFLAGSAVE"), "w");
    packetStartTime = BzTime::getCurrent();
  }

  return;
}


ServerLink::~ServerLink()
{
  if (state != Okay) return;
  shutdown(fd, SHUT_RDWR);
  close(fd);

  if (urecvfd >= 0)
    close(urecvfd);

  urecvfd = -1;
  ulinkup = false;

  // FIXME -- packet recording
  if (packetStream) {
    long dt = (long)((BzTime::getCurrent() - packetStartTime) * 10000.0f);
    fwrite(&endPacket, sizeof(endPacket), 1, packetStream);
    fwrite(&dt, sizeof(dt), 1, packetStream);
    fclose(packetStream);
  }

#if defined(NETWORK_STATS)
  const float dt = float(BzTime::getCurrent() - startTime);
  logDebugMessage(1,"Server network statistics:\n");
  logDebugMessage(1,"  elapsed time    : %f\n", dt);
  logDebugMessage(1,"  bytes sent      : %d (%f/sec)\n", bytesSent, (float)bytesSent / dt);
  logDebugMessage(1,"  packets sent    : %d (%f/sec)\n", packetsSent, (float)packetsSent / dt);
  if (packetsSent != 0)
    logDebugMessage(1,"  bytes/packet    : %f\n", (float)bytesSent / (float)packetsSent);
  logDebugMessage(1,"  bytes received  : %d (%f/sec)\n", bytesReceived, (float)bytesReceived / dt);
  logDebugMessage(1,"  packets received: %d (%f/sec)\n", packetsReceived, (float)packetsReceived / dt);
  if (packetsReceived != 0)
    logDebugMessage(1,"  bytes/packet    : %f\n", (float)bytesReceived / (float)packetsReceived);
#endif
}


ServerLink* ServerLink::getServer() // const
{
  return server;
}


void ServerLink::setServer(ServerLink* _server)
{
  server = _server;
}


void ServerLink::flush()
{
  if (!previousFill)
    return;
  if (oldNeedForSpeed) {
#ifdef TESTLINK
    if ((random()%TESTQUALTIY) != 0)
#endif
      sendto(urecvfd, (const char *)txbuf, previousFill, 0,
	     &usendaddr, sizeof(usendaddr));
    // we don't care about errors yet
  } else {
    int r = ::send(fd, (const char *)txbuf, previousFill, 0);
    (void)r; // silence g++
#if defined(_WIN32)
    if (r == SOCKET_ERROR) {
      const int e = WSAGetLastError();
      if (e == WSAENETRESET || e == WSAECONNABORTED ||
	  e == WSAECONNRESET || e == WSAETIMEDOUT)
	state = Hungup;
      r = 0;
    }
#endif

#if defined(NETWORK_STATS)
    bytesSent += r;
    packetsSent++;
#endif
  }
  previousFill = 0;
}


void ServerLink::send(uint16_t code, uint16_t len, const void* msg)
{
  if (state != Okay) {
    return;
  }

  bool needForSpeed = false;

  if ((urecvfd >= 0) && ulinkup) {
    switch (code) {
      case MsgShotBegin:
      case MsgShotEnd:
      case MsgHit:
      case MsgPlayerUpdate:
      case MsgPlayerUpdateSmall:
      case MsgGMUpdate:
      case MsgLuaDataFast:
      case MsgUDPLinkRequest:
      case MsgUDPLinkEstablished:{
        needForSpeed = true;
        break;
      }
    }
  }
  // MsgUDPLinkRequest always goes udp
  if (code == MsgUDPLinkRequest) {
    needForSpeed = true;
  }

  if ((needForSpeed != oldNeedForSpeed) ||
      ((previousFill + len + 4) > MaxPacketLen)) {
    flush();
  }
  oldNeedForSpeed = needForSpeed;

  void* buf = txbuf + previousFill;
  buf = nboPackUInt16(buf, len);
  buf = nboPackUInt16(buf, code);
  if (msg && len != 0) {
    buf = nboPackString(buf, msg, len);
    previousFill += len + 4;
  } else {
    previousFill += 4;
  }

#ifndef BUILDING_BZADMIN
  static BZDB_int  debugMessages("debugNetMesg");
  static BZDB_bool debugUpdateMessages("debugNetUpdMesg");
  if ((debugMessages >= 1) && !BZDB.isTrue("_forbidDebug")) {
    if ((code != MsgPlayerUpdateSmall) || debugUpdateMessages) {
      // use the fancier MsgStrings setup
      const int msgLevel = (debugMessages - 1);
      MsgStringList msgList = MsgStrings::msgFromServer(len, code, msg);
      for (size_t i = 0; i < msgList.size(); i++) {
	if (msgList[i].level <= msgLevel) {
	  std::string prefix = "send: ";
	  if (i == 0)
	    prefix += TextUtils::format("%f ",
	      BzTime::getCurrent().getSeconds());
	  for (int lvl = 0; lvl < msgList[i].level; lvl++) {
	    prefix += "  ";
	  }
	  showMessage(prefix + msgList[i].color + msgList[i].text);
	}
      }
    }
  }
#endif
}

#if defined(WIN32) && !defined(HAVE_SOCKLEN_T)
/* This is a really really fugly hack to get around winsock sillyness
 * The newer versions of winsock have a socken_t typedef, and there
 * doesn't seem to be any way to tell the versions apart. However,
 * VC++ helps us out here by treating typedef as #define
 * If we've got a socklen_t typedefed, define HAVE_SOCKLEN_T in config.h
 * to skip this hack */

#ifndef socklen_t
#define socklen_t int
#endif
#endif //WIN32


int ServerLink::read(uint16_t& code, uint16_t& len, void* msg, int blockTime)
{
  code = MsgNull;
  len = 0;

  if (state != Okay) return -1;

  if ((urecvfd >= 0) /* && ulinkup */) {
    int n;

    if (!udpLength) {
      size_t recvlen = sizeof(urecvaddr);
      n = recvfrom(urecvfd, ubuf, MaxPacketLen, 0, &urecvaddr,
		   (socklen_t*) &recvlen);
      if (n > 0) {
	udpLength    = n;
	udpBufferPtr = ubuf;
      }
    }
    if (udpLength) {
      // unpack header and get message
      udpLength -= 4;
      if (udpLength < 0) {
	udpLength = 0;
	return -1;
      }
      udpBufferPtr = (char *)nboUnpackUInt16(udpBufferPtr, len);
      udpBufferPtr = (char *)nboUnpackUInt16(udpBufferPtr, code);
      UDEBUG("<** UDP Packet Code %x Len %x\n",code, len);
      if (len > udpLength) {
	udpLength = 0;
	return -1;
      }
      memcpy((char *)msg, udpBufferPtr, len);
      udpBufferPtr += len;
      udpLength    -= len;
      return 1;
    }
    if (UDEBUGMSG) printError("Fallback to normal TCP receive");
    len = 0;
    code = MsgNull;

    blockTime = 0;
  }

  // block for specified period.  default is no blocking (polling)
  struct timeval timeout;
  timeout.tv_sec = blockTime / 1000;
  timeout.tv_usec = blockTime - 1000 * timeout.tv_sec;

  // only check server
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET((unsigned int)fd, &read_set);
  int nfound = select(fd+1, (fd_set*)&read_set, NULL, NULL,
		      (struct timeval*)(blockTime >= 0 ? &timeout : NULL));
  if (nfound == 0) return 0;
  if (nfound < 0) return -1;

  // printError("<** TCP Packet Code Received %d", time(0));
  // FIXME -- don't really want to take the chance of waiting forever
  // on the remaining select() calls, but if the server and network
  // haven't been hosed then the data will get here soon.  And if the
  // server or network is down then we don't really care anyway.

  // get packet header -- keep trying until we get 4 bytes or an error
  char headerBuffer[4];


  int rlen = 0;
  rlen = recv(fd, (char*)headerBuffer, 4, 0);
  if (!rlen)
    // Socket shutdown Server side
    return -2;

  int tlen = rlen;
  while (rlen >= 1 && tlen < 4) {
    printError("ServerLink::read() loop");
    FD_ZERO(&read_set);
    FD_SET((unsigned int)fd, &read_set);
    nfound = select(fd+1, (fd_set*)&read_set, NULL, NULL, NULL);
    if (nfound == 0) continue;
    if (nfound < 0) return -1;
    rlen = recv(fd, (char*)headerBuffer + tlen, 4 - tlen, 0);
    if (rlen > 0)
      tlen += rlen;
    else if (rlen == 0)
      // Socket shutdown Server side
      return -2;
  }
  if (tlen < 4) {
    return -1;
  }
#if defined(NETWORK_STATS)
  bytesReceived += 4;
  packetsReceived++;
#endif

  // unpack header and get message
  void* buf = headerBuffer;
  buf = nboUnpackUInt16(buf, len);
  buf = nboUnpackUInt16(buf, code);

  //printError("Code is %02x",code);
  if (len > MaxPacketLen)
    return -1;
  if (len > 0) {
    rlen = recv(fd, (char*)msg, int(len), 0);
    if (!rlen)
      // Socket shutdown Server side
      return -2;
  } else {
    rlen = 0;
  }
#if defined(NETWORK_STATS)
  if (rlen >= 0) bytesReceived += rlen;
#endif
  if (rlen == int(len)) goto success;	// got whole thing

  // keep reading until we get the whole message
  tlen = rlen;
  while (rlen >= 1 && tlen < int(len)) {
    FD_ZERO(&read_set);
    FD_SET((unsigned int)fd, &read_set);
    nfound = select(fd+1, (fd_set*)&read_set, 0, 0, NULL);
    if (nfound == 0) continue;
    if (nfound < 0) return -1;
    rlen = recv(fd, (char*)msg + tlen, int(len) - tlen, 0);
    if (rlen > 0)
      tlen += rlen;
    else if (rlen == 0)
      // Socket shutdown Server side
      return -2;
#if defined(NETWORK_STATS)
    if (rlen >= 0) bytesReceived += rlen;
#endif
  }
  if (tlen < int(len)) return -1;

 success:
  // FIXME -- packet recording
  if (packetStream) {
    long dt = (long)((BzTime::getCurrent() - packetStartTime) * 10000.0f);
    fwrite(&serverPacket, sizeof(serverPacket), 1, packetStream);
    fwrite(&dt, sizeof(dt), 1, packetStream);
    fwrite(headerBuffer, 4, 1, packetStream);
    fwrite(msg, len, 1, packetStream);
  }
  return 1;
}


void ServerLink::sendCaps(PlayerId _id, bool downloads, bool sounds)
{
  if (state != Okay)
    return;
  char msg[3] = {0};
  void* buf = msg;

  buf = nboPackUInt8(buf, uint8_t(_id));
  buf = nboPackUInt8(buf, downloads ? 1 : 0);
  buf = nboPackUInt8(buf, sounds ? 1 : 0);

  send(MsgCapBits, (uint16_t)((char*)buf - msg), msg);
}


void ServerLink::sendEnter(PlayerId _id, PlayerType type, NetworkUpdates updates, TeamColor team,
                           const char* name,
                           const char* token,
                           const char* referrer)
{
  joinCallsign = name;

  if (state != Okay) return;
  char msg[MaxPacketLen] = {0};
  void* buf = msg;

  buf = nboPackUInt8(buf, uint8_t(_id));
  buf = nboPackUInt16(buf, uint16_t(type));
  buf = nboPackUInt16(buf, uint16_t(updates));
  buf = nboPackInt16(buf, int16_t(team));

  ::strncpy((char*)buf, name, CallSignLen - 1);
  buf = (void*)((char*)buf + CallSignLen);
  ::strncpy((char*)buf, token, TokenLen - 1);
  buf = (void*)((char*)buf + TokenLen);
  ::strncpy((char*)buf, getAppVersion(), VersionLen - 1);
  buf = (void*)((char*)buf + VersionLen);
  ::strncpy((char*)buf, referrer, ReferrerLen - 1);
  buf = (void*)((char*)buf + ReferrerLen);

  send(MsgEnter, (uint16_t)((char*)buf - msg), msg);
}


bool ServerLink::readEnter (std::string& reason,
     uint16_t& code, uint16_t& rejcode)
{
  // wait for response
  uint16_t len;
  char msg[MaxPacketLen];

  while (true) {
    if (this->read(code, len, msg, -1) < 0) {
      reason = "Communication error joining game [No immediate response].";
      return false;
    }

    if (code == MsgAccept) {
      return true;
    } else if (code == MsgSuperKill) {
      reason = "Server forced disconnection.";
      return false;
    } else if (code == MsgReject) {
      void *buf;
      char buffer[MessageLen];
      buf = nboUnpackUInt16 (msg, rejcode); // filler for now
      buf = nboUnpackString (buf, buffer, MessageLen);
      buffer[MessageLen - 1] = '\0';
      reason = buffer;
      return false;
    }
    // ignore other codes so that bzadmin doesn't choke
    // on the MsgMessage's that the server can send before
    // the MsgAccept (authorization holdoff, etc...)
  }

  return true;
}


#ifndef BUILDING_BZADMIN
void ServerLink::sendCaptureFlag(TeamColor team)
{
  char msg[3];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  nboPackInt16(buf, int16_t(team));
  send(MsgCaptureFlag, sizeof(msg), msg);
}


void ServerLink::sendDropFlag(const fvec3& position)
{
  char msg[13];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackFVec3(buf, position);
  send(MsgDropFlag, sizeof(msg), msg);
}


void ServerLink::sendKilled(const PlayerId victim,
 		       const PlayerId killer,
 		       int reason, int shotId,
 		       const FlagType* flagType,
 		       int phydrv)
{
  char msg[6 + FlagPackSize + 4];
  void* buf = msg;

  buf = nboPackUInt8(buf, uint8_t(victim));
  buf = nboPackUInt8(buf, killer);
  buf = nboPackUInt16(buf, int16_t(reason));
  buf = nboPackInt16(buf, int16_t(shotId));
  buf = flagType->pack(buf);

  if (reason == PhysicsDriverDeath) {
    buf = nboPackInt32(buf, phydrv);
  }

  send(MsgKilled, (uint16_t)((char*)buf - (char*)msg), msg);
}


void ServerLink::sendPlayerUpdate(Player* player)
{
  // Send the time frozen at each start of scene iteration, as all
  // dead reckoning use that
  char msg[PlayerUpdatePLenMax];
  void* buf = msg;
  buf = nboPackUInt8(buf, player->getId());
  buf = nboPackDouble(buf, GameTime::getStepTime());

  // code will be MsgPlayerUpdate or MsgPlayerUpdateSmall
  uint16_t code;
  buf = player->pack(buf, code);

  // variable length
  const int len = (const int)((char*)buf - (char*)msg);

  send(code, len, msg);
}


void ServerLink::sendShotBegin(const FiringInfo& info)
{
  char msg[35];
  void* buf = msg;

  buf = nboPackUInt8(buf, info.shot.player);
  buf = nboPackUInt16(buf, info.shot.id);
  buf = nboPackDouble(buf, GameTime::getStepTime());
  buf = nboPackFVec3(buf, info.shot.pos);
  buf = nboPackFVec3(buf, info.shot.vel);

  send(MsgShotBegin, sizeof(msg), msg);
}


void ServerLink::sendShotEnd(const PlayerId& source,
  int shotId, int reason)
{
  char msg[PlayerIdPLen + 4];
  void* buf = msg;
  buf = nboPackUInt8(buf, source);
  buf = nboPackInt16(buf, int16_t(shotId));
  buf = nboPackUInt16(buf, uint16_t(reason));
  send(MsgShotEnd, sizeof(msg), msg);
}


void ServerLink::sendOSVersion(const PlayerId player, const std::string &vers)
{
  char *msg = new char[vers.size() + 10];
  void *buf = msg;
  buf = nboPackUInt8(buf, player);
  buf = nboPackStdString(buf, vers);
  send(MsgQueryOS, (char *)buf - msg, msg);
  delete[] msg;
}


void ServerLink::sendHit(const PlayerId &source, const PlayerId &shooter,
  int shotId)
{
  char msg[80];
  void* buf = msg;
  buf = nboPackUInt8(buf, source);
  buf = nboPackUInt8(buf, shooter);
  buf = nboPackInt16(buf, int16_t(shotId));
  send(MsgHit, sizeof(msg), msg);
}
#endif


void ServerLink::sendVarRequest()
{
  send(MsgSetVar, 0, NULL);
}


void ServerLink::sendAlive(const PlayerId playerId)
{
  char msg[1];

  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(playerId));

  send(MsgAlive, sizeof(msg), msg);
}


void ServerLink::sendTeleport(int from, int to)
{
  char msg[5];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackUInt16(buf, uint16_t(from));
  buf = nboPackUInt16(buf, uint16_t(to));
  send(MsgTeleport, sizeof(msg), msg);
}


void ServerLink::sendShotInfo(const ShotPath& shotPath,
                              char infoType, const fvec3& pos,
                              uint32_t obstacleGUID,
                              int linkSrcID, int linkDstID)
{
  char msg[64];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackInt16(buf, int16_t(shotPath.getShotId()));
  buf = shotPath.getFlag()->pack(buf);
  buf = nboPackUInt8(buf, uint8_t(infoType));
  buf = nboPackFVec3(buf, pos);
  switch (infoType) {
    case ShotInfoTeleport: {
      buf = nboPackUInt16(buf, uint16_t(linkSrcID));
      buf = nboPackUInt16(buf, uint16_t(linkDstID));
      break;
    }
    case ShotInfoStopped:
    case ShotInfoRicochet: {
      buf = nboPackUInt32(buf, obstacleGUID);
      break;
    }
    case ShotInfoExpired: {
      break;
    }
  }
  send(MsgShotInfo, (char*)buf - msg, msg);
}


void ServerLink::sendCustomData(const std::string &key, const std::string &value)
{
  if (key.size()+value.size() >= MaxPacketLen)
    return;

  char msg[MaxPacketLen];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackStdString(buf, key);
  buf = nboPackStdString(buf, value);
  send(MsgPlayerData, (uint16_t)((char*)buf - msg), msg);
}


bool ServerLink::sendLuaData(PlayerId srcPlayerID, int16_t srcScriptID,
                             PlayerId dstPlayerID, int16_t dstScriptID,
                             uint8_t status, const std::string& data)
{
  if (srcPlayerID != getId()) {
    return false;
  }

  const size_t headerSize =
    (sizeof(uint16_t) * 2) + // code and len
    (sizeof(uint8_t)  * 2) + // player ids
    (sizeof(int16_t)  * 2) + // script ids
    (sizeof(uint8_t))      + // status bits
    (sizeof(uint32_t));      // data count

  if ((headerSize + data.size()) > MaxPacketLen) {
    return false;
  }

  char msg[MaxPacketLen];
  void* buf = msg;
  buf = nboPackUInt8(buf, srcPlayerID);
  buf = nboPackInt16(buf, srcScriptID);
  buf = nboPackUInt8(buf, dstPlayerID);
  buf = nboPackInt16(buf, dstScriptID);
  buf = nboPackUInt8(buf, status);
  buf = nboPackStdString(buf, data);

  uint16_t code = (status & MsgLuaDataUdpBit) ? MsgLuaDataFast : MsgLuaData;
  send(code, (uint16_t)((char*)buf - msg), msg);

  return true;
}


void ServerLink::sendTransferFlag(const PlayerId& from, const PlayerId& to)
{
  char msg[PlayerIdPLen*2];
  void* buf = msg;
  buf = nboPackUInt8(buf, from);
  buf = nboPackUInt8(buf, to);
  send(MsgTransferFlag, sizeof(msg), msg);
}


void ServerLink::sendNewRabbit()
{
  char msg[1];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  send(MsgNewRabbit, sizeof(msg), msg);
}


void ServerLink::sendPaused(bool paused)
{
  char msg[2];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackUInt8(buf, paused ? PauseCodeEnable : PauseCodeDisable);
  send(MsgPause, sizeof(msg), msg);
}


void ServerLink::sendNewPlayer(int botID, TeamColor team)
{
  char msg[4];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackUInt8(buf, uint8_t(botID));
  buf = nboPackInt16(buf, int16_t(team));

  send(MsgNewPlayer, sizeof(msg), msg);
}


void ServerLink::sendExit()
{
  char msg[1];

  msg[0] = getId();

  send(MsgExit, sizeof(msg), msg);
  flush();
}


void ServerLink::sendAutoPilot(bool autopilot)
{
  char msg[2];
  void* buf = msg;
  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackUInt8(buf, uint8_t(autopilot));
  send(MsgAutoPilot, sizeof(msg), msg);
}


void ServerLink::sendMessage(const PlayerId& to, const char message[MessageLen])
{
  // ensure that we aren't sending a partial multibyte character
  UTF8StringItr itr = message;
  UTF8StringItr prev = itr;
  while (*itr && (itr.getBufferFromHere() - message) < MessageLen)
    prev = itr++;
  if ((itr.getBufferFromHere() - message) >= MessageLen)
    *(const_cast<char*>(prev.getBufferFromHere())) = '\0';

  char msg[MaxPacketLen];
  void* buf = msg;

  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackUInt8(buf, uint8_t(to));
  buf = nboPackString(buf, message, MessageLen);

  send(MsgMessage, (uint16_t)((char *)buf - msg), msg);
}


void ServerLink::sendLagPing(char pingRequest[2])
{
  char msg[3];
  void* buf = msg;

  buf = nboPackUInt8(buf, uint8_t(getId()));
  buf = nboPackString(buf, pingRequest, 2);

  send(MsgLagPing, sizeof(msg), msg);
}


void ServerLink::sendUDPlinkRequest()
{
  if ((server_abilities & CanDoUDP) != CanDoUDP)
    return; // server does not support udp (future list server test)

  char msg[1];
  unsigned short localPort;
  void* buf = msg;

  struct sockaddr_in serv_addr;

  if ((urecvfd = (int)socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    return; // we cannot comply
  }
#if 1
  size_t addr_len = sizeof(serv_addr);
  if (getsockname(fd, (struct sockaddr*)&serv_addr, (socklen_t*) &addr_len) < 0) {
    printError("Error: getsockname() failed, cannot get TCP port?");
    return;
  }
  if (bind(urecvfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
    printError("Error: getsockname() failed, cannot get TCP port?");
    return;  // we cannot get udp connection, bail out
  }

#else
  // TODO if nobody complains kill this old port 17200 code
  for (int port=17200; port < 65000; port++) {
    ::memset((unsigned char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if (bind(urecvfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == 0) {
      break;
    }
  }
#endif
  localPort = ntohs(serv_addr.sin_port);
  memcpy((char *)&urecvaddr,(char *)&serv_addr, sizeof(serv_addr));

  if (debugLevel >= 1) {
    std::vector<std::string> args;
    char lps[10];
    sprintf(lps, "%d", localPort);
    args.push_back(lps);
    printError("Network: Created local UDP downlink port {1}", &args);
  }

  buf = nboPackUInt8(buf, id);

  if (BzfNetwork::setNonBlocking(urecvfd) < 0) {
    printError("Error: Unable to set NonBlocking for UDP receive socket");
  }

  send(MsgUDPLinkRequest, sizeof(msg), msg);
}


// heard back from server that we can send udp
void ServerLink::enableOutboundUDP()
{
  ulinkup = true;
  if (debugLevel >= 1)
    printError("Server got our UDP, using UDP to server");
}


// confirm that server can send us UDP
void ServerLink::confirmIncomingUDP()
{
  // This is really a hack. enableOutboundUDP will be setting this
  // but frequently the udp handshake will finish first so might as
  // well start with udp as soon as we can
  ulinkup = true;

  if (debugLevel >= 1)
    printError("Got server's UDP packet back, server using UDP");
  send(MsgUDPLinkEstablished, 0, NULL);
}


// Local Variables: ***
// mode: C++ ***
// tab-width: 8 ***
// c-basic-offset: 2 ***
// indent-tabs-mode: t ***
// End: ***
// ex: shiftwidth=2 tabstop=8
