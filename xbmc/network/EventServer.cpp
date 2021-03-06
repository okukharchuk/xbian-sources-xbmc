/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EventServer.h"
#include "EventClient.h"
#include "EventPacket.h"
#include "Socket.h"
#include "Zeroconf.h"
#include "guilib/GUIAudioManager.h"
#include "input/actions/ActionTranslator.h"
#include "input/Key.h"
#include "interfaces/builtins/Builtins.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/SystemInfo.h"
#include "Application.h"
#include "ServiceBroker.h"
#include "Util.h"

#include <cassert>
#include <map>
#include <queue>

using namespace EVENTSERVER;
using namespace EVENTPACKET;
using namespace EVENTCLIENT;
using namespace SOCKETS;

/************************************************************************/
/* CEventServer                                                         */
/************************************************************************/
CEventServer* CEventServer::m_pInstance = NULL;
CEventServer::CEventServer() : CThread("EventServer")
{
  m_pSocket       = NULL;
  m_pPacketBuffer = NULL;
  m_bStop         = false;
  m_bRunning      = false;
  m_bRefreshSettings = false;

  // default timeout in ms for receiving a single packet
  m_iListenTimeout = 1000;
}

void CEventServer::RemoveInstance()
{
  if (m_pInstance)
  {
    delete m_pInstance;
    m_pInstance=NULL;
  }
}

CEventServer* CEventServer::GetInstance()
{
  if (!m_pInstance)
  {
    m_pInstance = new CEventServer();
  }
  return m_pInstance;
}

void CEventServer::StartServer()
{
  CSingleLock lock(m_critSection);
  if (m_bRunning)
    return;

  // set default port
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  m_iPort = settings->GetInt(CSettings::SETTING_SERVICES_ESPORT);
  assert(m_iPort <= 65535 && m_iPort >= 1);

  // max clients
  m_iMaxClients = settings->GetInt(CSettings::SETTING_SERVICES_ESMAXCLIENTS);
  if (m_iMaxClients < 0)
  {
    CLog::Log(LOGERROR, "ES: Invalid maximum number of clients specified %d", m_iMaxClients);
    m_iMaxClients = 20;
  }

  CThread::Create();
}

void CEventServer::StopServer(bool bWait)
{
  CZeroconf::GetInstance()->RemoveService("services.eventserver");
  StopThread(bWait);
}

void CEventServer::Cleanup()
{
  if (m_pSocket)
  {
    m_pSocket->Close();
    delete m_pSocket;
    m_pSocket = NULL;
  }

  if (m_pPacketBuffer)
  {
    free(m_pPacketBuffer);
    m_pPacketBuffer = NULL;
  }
  CSingleLock lock(m_critSection);

  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();
  while (iter != m_clients.end())
  {
    if (iter->second)
    {
      delete iter->second;
    }
    m_clients.erase(iter);
    iter =  m_clients.begin();
  }
}

int CEventServer::GetNumberOfClients()
{
  CSingleLock lock(m_critSection);
  return m_clients.size();
}

void CEventServer::Process()
{
  while(!m_bStop)
  {
    Run();
    if (!m_bStop)
      Sleep(1000);
  }
}

void CEventServer::Run()
{
  CSocketListener listener;
  int packetSize = 0;

  CLog::Log(LOGNOTICE, "ES: Starting UDP Event server on port %d", m_iPort);

  Cleanup();

  // create socket and initialize buffer
  m_pSocket = CSocketFactory::CreateUDPSocket();
  if (!m_pSocket)
  {
    CLog::Log(LOGERROR, "ES: Could not create socket, aborting!");
    return;
  }
  m_pPacketBuffer = (unsigned char *)malloc(PACKET_SIZE);

  if (!m_pPacketBuffer)
  {
    CLog::Log(LOGERROR, "ES: Out of memory, could not allocate packet buffer");
    return;
  }

  // bind to IP and start listening on port
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  int port_range = settings->GetInt(CSettings::SETTING_SERVICES_ESPORTRANGE);
  if (port_range < 1 || port_range > 100)
  {
    CLog::Log(LOGERROR, "ES: Invalid port range specified %d, defaulting to 10", port_range);
    port_range = 10;
  }
  if (!m_pSocket->Bind(!settings->GetBool(CSettings::SETTING_SERVICES_ESALLINTERFACES), m_iPort, port_range))
  {
    CLog::Log(LOGERROR, "ES: Could not listen on port %d", m_iPort);
    return;
  }

  // publish service
  std::vector<std::pair<std::string, std::string> > txt;
  CZeroconf::GetInstance()->PublishService("servers.eventserver",
                               "_xbmc-events._udp",
                               CSysInfo::GetDeviceName(),
                               m_iPort,
                               txt);

  // add our socket to the 'select' listener
  listener.AddSocket(m_pSocket);

  m_bRunning = true;

  while (!m_bStop)
  {
    try
    {
      // start listening until we timeout
      if (listener.Listen(m_iListenTimeout))
      {
        CAddress addr;
        if ((packetSize = m_pSocket->Read(addr, PACKET_SIZE, (void *)m_pPacketBuffer)) > -1)
        {
          ProcessPacket(addr, packetSize);
        }
      }
    }
    catch (...)
    {
      CLog::Log(LOGERROR, "ES: Exception caught while listening for socket");
      break;
    }

    // process events and queue the necessary actions and button codes
    ProcessEvents();

    // refresh client list
    RefreshClients();

    // broadcast
    // BroadcastBeacon();
  }

  CLog::Log(LOGNOTICE, "ES: UDP Event server stopped");
  m_bRunning = false;
  Cleanup();
}

void CEventServer::ProcessPacket(CAddress& addr, int pSize)
{
  // check packet validity
  CEventPacket* packet = new CEventPacket(pSize, m_pPacketBuffer);
  if(packet == NULL)
  {
    CLog::Log(LOGERROR, "ES: Out of memory, cannot accept packet");
    return;
  }

  unsigned int clientToken;

  if (!packet->IsValid())
  {
    CLog::Log(LOGDEBUG, "ES: Received invalid packet");
    delete packet;
    return;
  }

  clientToken = packet->ClientToken();
  if (!clientToken)
    clientToken = addr.ULong(); // use IP if packet doesn't have a token

  CSingleLock lock(m_critSection);

  // first check if we have a client for this address
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.find(clientToken);

  if ( iter == m_clients.end() )
  {
    if ( m_clients.size() >= (unsigned int)m_iMaxClients)
    {
      CLog::Log(LOGWARNING, "ES: Cannot accept any more clients, maximum client count reached");
      delete packet;
      return;
    }

    // new client
    CEventClient* client = new CEventClient ( addr );
    if (client==NULL)
    {
      CLog::Log(LOGERROR, "ES: Out of memory, cannot accept new client connection");
      delete packet;
      return;
    }

    m_clients[clientToken] = client;
  }
  m_clients[clientToken]->AddPacket(packet);
}

void CEventServer::RefreshClients()
{
  CSingleLock lock(m_critSection);
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();

  while ( iter != m_clients.end() )
  {
    if (! (iter->second->Alive()))
    {
      CLog::Log(LOGNOTICE, "ES: Client %s from %s timed out", iter->second->Name().c_str(),
                iter->second->Address().Address());
      delete iter->second;
      m_clients.erase(iter);
      iter = m_clients.begin();
    }
    else
    {
      if (m_bRefreshSettings)
      {
        iter->second->RefreshSettings();
      }
      ++iter;
    }
  }
  m_bRefreshSettings = false;
}

void CEventServer::ProcessEvents()
{
  CSingleLock lock(m_critSection);
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();

  while (iter != m_clients.end())
  {
    iter->second->ProcessEvents();
    ++iter;
  }
}

bool CEventServer::ExecuteNextAction()
{
  CSingleLock lock(m_critSection);

  CEventAction actionEvent;
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();

  bool ret(true);

  while (iter != m_clients.end())
  {
    if (iter->second->GetNextAction(actionEvent))
    {
      // Leave critical section before processing action
      lock.Leave();
      switch(actionEvent.actionType)
      {
      case AT_EXEC_BUILTIN:
        ret = CBuiltins::GetInstance().Execute(actionEvent.actionName) == 0 ? true : false;
        break;

      case AT_BUTTON:
        {
          unsigned int actionID;
          CActionTranslator::TranslateString(actionEvent.actionName, actionID);
          CAction action(actionID, 1.0f, 0.0f, actionEvent.actionName);
          CGUIComponent* gui = CServiceBroker::GetGUI();
          if (gui)
            gui->GetAudioManager().PlayActionSound(action);

          ret = g_application.OnAction(action);
        }
        break;
      }
      return ret;
    }
    ++iter;
  }

  return false;
}

unsigned int CEventServer::GetButtonCode(std::string& strMapName, bool& isAxis, float& fAmount, bool &isJoystick)
{
  CSingleLock lock(m_critSection);
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();
  unsigned int bcode = 0;

  while (iter != m_clients.end())
  {
    bcode = iter->second->GetButtonCode(strMapName, isAxis, fAmount, isJoystick);
    if (bcode)
      return bcode;
    ++iter;
  }
  return bcode;
}

bool CEventServer::GetMousePos(float &x, float &y)
{
  CSingleLock lock(m_critSection);
  std::map<unsigned long, CEventClient*>::iterator iter = m_clients.begin();

  while (iter != m_clients.end())
  {
    if (iter->second->GetMousePos(x, y))
      return true;
    ++iter;
  }
  return false;
}
