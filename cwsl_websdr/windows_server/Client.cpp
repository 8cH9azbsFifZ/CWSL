//
// Client.cpp  -  network client for WEBSDR
//
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "Client.h"

int SF = 16;  // ScalingFactor is set at "start iq"

///////////////////////////////////////////////////////////////////////////////
// Parse client command
char *Client::ParseCommand(char *cmd)
{char *t[16];
 int i;
 
 // to be safe ...
 if (cmd == NULL) return cs_inv_cmd;
 
 // tokenize
 t[0] = strtok(cmd, cs_sep);
 for (i = 0; (i < 15) && (t[i] != NULL); i++) t[i+1] = strtok(NULL, cs_sep);
 t[15] = NULL;
  
 // parse commands
 if (t[0] == NULL) return cs_inv_cmd;
  else
 if (_stricmp(t[0], "attach") == 0) return CmdAttach(t);
  else
 if (_stricmp(t[0], "detach") == 0) return CmdDetach(t);
  else
 if (_stricmp(t[0], "start") == 0) return CmdStart(t);
  else
 if (_stricmp(t[0], "stop") == 0) return CmdStop(t);
  else
 if (_stricmp(t[0], "quit") == 0) return cs_quit;
  else
    
 // unsupported command
 return cs_notimp_cmd;
}

///////////////////////////////////////////////////////////////////////////////
// Service for command "attach"
char *Client::CmdAttach(char **arg)
{static char ret[128];

 // check current state
 if (m_SM.IsOpen()) return cs_attached;

 // get receiver number
 if (arg[1] == NULL) return cs_inv_cmd;
 m_rx = atoi(arg[1]);
 
 // create name of the shared memory
 sprintf(ret, "CWSL%dBand", m_rx);
 
 // try to open it
 if (!m_SM.Open(ret)) return cs_cant_attach;

 // get info about it's data and save it
 memcpy(&m_Hdr, m_SM.GetHeader(), sizeof(m_Hdr));
 
 // format response and return it
 sprintf(ret, "%s SampleRate=%d BlockInSamples=%d L0=%d\n", cs_ok, m_Hdr.SampleRate, m_Hdr.BlockInSamples, m_Hdr.L0);
 return ret;
}


///////////////////////////////////////////////////////////////////////////////
// Service for command "detach"
char *Client::CmdDetach(char **arg)
{int rx;

 // check current state
 if (!m_SM.IsOpen()) return cs_detached;

 // get receiver number
 if (arg[1] == NULL) return cs_inv_cmd;
 rx = atoi(arg[1]);
 
 // check receiver number
 if (rx != m_rx) return cs_other_rx;

 // close shared memory
 m_SM.Close();

 // return success
 return cs_ok;
}


///////////////////////////////////////////////////////////////////////////////
// Service for command "start"
char *Client::CmdStart(char **arg)
{BOOL iq;
 int port; 

 // which data ?
 if (arg[1] == NULL) return cs_inv_cmd;
  else
 if (_stricmp(arg[1], "iq") == 0) iq = TRUE;  
  else
 return cs_inv_cmd;
 
 // get port
 if (arg[2] == NULL) return cs_inv_cmd;
 port = atoi(arg[2]);
 if (port < 1) return cs_inv_cmd;

 // get SF
 if (arg[3] == NULL) return cs_inv_cmd;
 SF = atoi(arg[3]);
 if (SF < 1 || SF > 24) return cs_inv_cmd;

 // for bandscope it is all ...
 if (!iq) return cs_ok;

 // check current state
 if (!m_SM.IsOpen()) return cs_detached;
 if (isIqStarted()) return cs_started;

 // fill client address
 m_Addr.sin_family = AF_INET;
 m_Addr.sin_port = htons(port);

 // try to start iq thread
 if (!iqStart()) return cs_cant_start;

 // success
 return cs_ok;
}

///////////////////////////////////////////////////////////////////////////////
// Service for command "stop"
char *Client::CmdStop(char **arg)
{BOOL iq;

 // which data ?
 if (arg[1] == NULL) return cs_inv_cmd;
  else
 if (_stricmp(arg[1], "iq") == 0) iq = TRUE;  
  else
 if (_stricmp(arg[1], "bandscope") == 0) iq = FALSE;  
  else
 return cs_inv_cmd;
 
 // for bandscope it is all ...
 if (!iq) return cs_ok;

 // check current state
 if (!m_SM.IsOpen()) return cs_detached;
 if (!isIqStarted()) return cs_not_started;

 // stop sending
 iqStop();

 // success
 return cs_ok;
}

///////////////////////////////////////////////////////////////////////////////
// Start iq worker thread
BOOL Client::iqStart(void)
{DWORD ID;

 // for sure
 iqStop();
 
 // create sender socket
 m_iqSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 if (m_iqSocket == INVALID_SOCKET) return FALSE;
 
 // start worker thread
 m_hThrd = CreateThread(NULL, 0, Client_iqWorker, this, 0, &ID);
 if (m_hThrd == NULL) {closesocket(m_iqSocket); m_iqSocket = INVALID_SOCKET; return FALSE;}
 
 // close handle to the thread
 CloseHandle(m_hThrd);
 
 // we have success
 return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// Stop iq worker thread
void Client::iqStop(void)
{
 // close the iq socket
 if (m_iqSocket != INVALID_SOCKET) closesocket(m_iqSocket);
 m_iqSocket = INVALID_SOCKET;
 
 // if worker running ...
 if (m_hThrd != NULL)
 {// ... wait for it - but not for long
  ::WaitForSingleObject(m_hThrd, 100);
 }
 m_hThrd = NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Real worker function
DWORD WINAPI Client_iqWorker(LPVOID lpParam)
{Client *pInst = (Client *)lpParam;
 
 // run worker function
 if (pInst != NULL) pInst->iqWorker();
 
 // clean handle to this thread
 pInst->m_hThrd = NULL; 
 
 // that's all folks
 return 0;
}

///////////////////////////////////////////////////////////////////////////////
// IQ worker function
void Client::iqWorker(void)
{ 
 float norm;
 float iBuf[BUFFER_SIZE*2];
 short oBuf[BUFFER_SIZE*2]; 
 int i;
 unsigned short offset;
 unsigned short length;
 int BIS;
 
 // clear old data
 m_SM.ClearBytesToRead();
 BIS = m_Hdr.BlockInSamples;

 norm = (float)(1.0 / pow(2.0, 0.0+(float)(SF)));

 // main loop
 while (m_iqSocket != INVALID_SOCKET)
 {// wait for new data
  m_SM.WaitForNewData(100);
  
  // can we still run ?
  if (m_iqSocket == INVALID_SOCKET) break;
  
  // read block of data
  if (!m_SM.Read((PBYTE)iBuf, 2*sizeof(float)*BIS)) continue;
   
  // convert data into shorts
  for( i = 0; i < 2*BIS; i++) {
	 oBuf[i] = (short)(iBuf[i]*norm); 
  }
  
  // send data 
  offset = 0;
  length = 2 * sizeof(short) * BIS;
  while(offset < 2 * sizeof(short) * BIS)
  {
   // fill packet header
   if (length > DATA_SIZE) length = DATA_SIZE; // trim to max length

   // send it
   if (m_iqSocket == INVALID_SOCKET) break;
   sendto(m_iqSocket, ((char *)oBuf)+offset, length, 0, (struct sockaddr*)&m_Addr, sizeof(m_Addr));  
   
   // update offset
   offset += length;
   length = 2 * sizeof(short) * BIS - offset;
  }
 }
}
