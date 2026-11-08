#include "stdafx.h"
#include "InputOutputStream.h"
#include "SocketAddress.h"
#include "Socket.h"
#include "ThreadName.h"
#include "..\Minecraft.Client\ServerConnection.h"
#include "..\Minecraft.Client\PS3\PS3Extras\ShutdownManager.h"
#include "Packet.h"
#include "DisconnectPacket.h"

// This current socket implementation is for the creation of a single local link. 2 sockets can be created, one for either end of this local
// link, the end (0 or 1) is passed as a parameter to the ctor.

CRITICAL_SECTION Socket::s_hostQueueLock[2]; 
std::queue<byte> Socket::s_hostQueue[2];
Socket::SocketOutputStreamLocal *Socket::s_hostOutStream[2];
Socket::SocketInputStreamLocal *Socket::s_hostInStream[2];
ServerConnection *Socket::s_serverConnection = NULL;

void Socket::Initialise(ServerConnection *serverConnection)
{
	s_serverConnection = serverConnection;

	// Only initialise everything else once - just setting up static data, one time xrnm things, thread for ticking sockets
	static bool init = false;
	if( init )
	{
		for( int i = 0; i < 2; i++ )
		{
			if(TryEnterCriticalSection(&s_hostQueueLock[i]))
			{			
				// Clear the queue
				std::queue<byte> empty;
				std::swap( s_hostQueue[i], empty );
				LeaveCriticalSection(&s_hostQueueLock[i]);
			}
			s_hostOutStream[i]->m_streamOpen = true;
			s_hostInStream[i]->m_streamOpen = true;
		}
		return;
	}
	init = true;

	for( int i = 0; i < 2; i++ )
	{
		InitializeCriticalSection(&Socket::s_hostQueueLock[i]);
		s_hostOutStream[i] = new SocketOutputStreamLocal(i);
		s_hostInStream[i] = new SocketInputStreamLocal(i);
	}
}

#if defined(_WINDOWS64) || defined(_XBOX)
bool Socket::s_wsaInit = false;

void Socket::ensureWsa()
{
	if (!s_wsaInit)
	{
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
		s_wsaInit = true;
	}
}

class Socket::SocketInputStreamTcp : public InputStream
{
	SOCKET m_socket;
	bool m_open;
public:
	SocketInputStreamTcp(SOCKET s) : m_socket(s), m_open(true) {}

	virtual int read()
	{
		if (!m_open) return -1;
		char c;
		int n = recv(m_socket, &c, 1, 0);
		return (n == 1) ? (int)(unsigned char)c : -1;
	}
	virtual int read(byteArray b) { return read(b, 0, b.length); }
	virtual int read(byteArray b, unsigned int offset, unsigned int length)
	{
		if (!m_open) return -1;
		unsigned int total = 0;
		while (total < length)
		{
			int n = recv(m_socket, (char *)b.data + offset + total, (int)(length - total), 0);
			if (n <= 0)
			{
				m_open = false;
				return (total > 0) ? (int)total : -1;
			}
			total += (unsigned int)n;
		}
		return (int)total;
	}
	virtual void close()
	{
		m_open = false;
		// Wake up a blocking recv() in the connection read thread. Without
		// this, Connection::close() waits forever for the read thread to
		// finish and the client never returns to the main menu.
		shutdown(m_socket, SD_BOTH);
	}
	virtual __int64 skip(__int64 n) { return n; }
	virtual void flush() {}
};

class Socket::SocketOutputStreamTcp : public SocketOutputStream
{
	SOCKET m_socket;
	bool m_open;
public:
	SocketOutputStreamTcp(SOCKET s) : m_socket(s), m_open(true) {}

	virtual void write(unsigned int b)
	{
		if (!m_open) return;
		char c = (char)b;
		if (send(m_socket, &c, 1, 0) != 1) m_open = false;
	}
	virtual void write(byteArray b) { write(b, 0, b.length); }
	virtual void write(byteArray b, unsigned int offset, unsigned int length)
	{
		if (!m_open) return;
		unsigned int total = 0;
		while (total < length)
		{
			int n = send(m_socket, (const char *)b.data + offset + total, (int)(length - total), 0);
			if (n <= 0)
			{
				m_open = false;
				return;
			}
			total += (unsigned int)n;
		}
	}
	virtual void writeWithFlags(byteArray b, unsigned int offset, unsigned int length, int flags) { write(b, offset, length); }
	virtual void close() { m_open = false; }
	virtual void flush() {}
};

Socket::Socket(SOCKET tcpSocket, bool serverEnd)
{
	ensureWsa();
	m_isTcp = true;
	m_tcpSocket = tcpSocket;
	m_hostServerConnection = false;
	m_hostLocal = false;
	m_end = serverEnd ? SOCKET_SERVER_END : SOCKET_CLIENT_END;
	for (int i = 0; i < 2; i++) m_endClosed[i] = false;
	m_socketClosedEvent = new C4JThread::Event;
	createdOk = (tcpSocket != INVALID_SOCKET);
	networkPlayerSmallId = 0;
	m_tcpIn = createdOk ? new SocketInputStreamTcp(tcpSocket) : NULL;
	m_tcpOut = createdOk ? new SocketOutputStreamTcp(tcpSocket) : NULL;
}

Socket::Socket(const wstring& host, int port)
{
	ensureWsa();
	m_isTcp = true;
	m_tcpSocket = INVALID_SOCKET;
	m_hostServerConnection = false;
	m_hostLocal = false;
	m_end = SOCKET_CLIENT_END;
	for (int i = 0; i < 2; i++) m_endClosed[i] = false;
	m_socketClosedEvent = new C4JThread::Event;
	createdOk = false;
	networkPlayerSmallId = 0;
	m_tcpIn = NULL;
	m_tcpOut = NULL;

#ifdef _XBOX
	// Xbox 360 has no GetAddrInfoW; the dedicated server address is an IPv4
	// literal from server.txt, so inet_addr is sufficient.
	char hostNarrow[128];
	int hostLen = 0;
	while (hostLen < 127 && hostLen < (int)host.size())
	{
		hostNarrow[hostLen] = (char)host[hostLen];
		++hostLen;
	}
	hostNarrow[hostLen] = 0;

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.s_addr = inet_addr(hostNarrow);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s != INVALID_SOCKET)
	{
		if (connect(s, (sockaddr *)&addr, sizeof(addr)) == 0)
		{
			m_tcpSocket = s;
			createdOk = true;
		}
		else
		{
			closesocket(s);
		}
	}
#else
	wchar_t portStr[16];
	swprintf_s(portStr, 16, L"%d", port);
	addrinfoW hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfoW *result = NULL;
	if (GetAddrInfoW(host.c_str(), portStr, &hints, &result) == 0)
	{
		for (addrinfoW *ptr = result; ptr != NULL; ptr = ptr->ai_next)
		{
			SOCKET s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
			if (s == INVALID_SOCKET) continue;
			if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0)
			{
				m_tcpSocket = s;
				createdOk = true;
				break;
			}
			closesocket(s);
		}
		FreeAddrInfoW(result);
	}
#endif
	if (createdOk)
	{
		m_tcpIn = new SocketInputStreamTcp(m_tcpSocket);
		m_tcpOut = new SocketOutputStreamTcp(m_tcpSocket);
	}
}

static DWORD WINAPI TcpListenerThread(LPVOID param)
{
	int port = (int)(INT_PTR)param;
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((u_short)port);
	if (::bind(ls, (sockaddr *)&addr, sizeof(addr)) != 0) return 0;
	if (listen(ls, SOMAXCONN) != 0) return 0;

	FILE *f = NULL;
#ifdef _WINDOWS64
	fopen_s(&f, "H:\\Temp\\tcp_server_accept.txt", "a");
#endif
	while (true)
	{
		SOCKET s = accept(ls, NULL, NULL);
		if (s == INVALID_SOCKET) break;
		if (f) { fprintf(f, "accepted=%llu\n", (unsigned long long)s); fflush(f); }
		Socket *sock = new Socket(s, true);
		if (g_NetworkManager.AddTcpPlayer(sock, L"TCPPlayer", false, false))
		{
			Socket::addIncomingSocket(sock);
		}
		else
		{
			if (f) { fprintf(f, "rejected: no free small id\n"); fflush(f); }
			// Tell the client why it was rejected instead of silently
			// resetting the connection.
			Socket::SocketOutputStream *out = sock->getOutputStream(true);
			if (out != NULL)
			{
				DataOutputStream dos(out);
				Packet::writePacket(shared_ptr<Packet>(new DisconnectPacket(DisconnectPacket::eDisconnect_ServerFull)), &dos);
				dos.flush();
			}
			sock->close(true);
			delete sock;
		}
	}
	if (f) fclose(f);
	return 0;
}

void Socket::startTcpListener(int port)
{
	CreateThread(NULL, 0, TcpListenerThread, (LPVOID)(INT_PTR)port, 0, NULL);
}

Socket::~Socket()
{
	// The TCP streams are owned by the socket. Connection deletes its
	// DataInputStream wrapper but not the underlying stream.
	delete m_tcpIn;
	m_tcpIn = NULL;
	delete m_tcpOut;
	m_tcpOut = NULL;
}

void Socket::shutdownTcp()
{
	if (m_isTcp && m_tcpSocket != INVALID_SOCKET)
	{
		shutdown(m_tcpSocket, SD_BOTH);
	}
}
#endif

Socket::Socket(bool response)
{
	m_hostServerConnection = true;
	m_hostLocal = true;
#if defined(_WINDOWS64) || defined(_XBOX)
	m_isTcp = false;
	m_tcpSocket = INVALID_SOCKET;
	m_tcpIn = NULL;
	m_tcpOut = NULL;
#endif
	if( response )
	{
		m_end = SOCKET_SERVER_END;
	}
	else
	{
		m_end = SOCKET_CLIENT_END;
		Socket *socket = new Socket(1);
		s_serverConnection->NewIncomingSocket(socket);
	}

	for( int i = 0; i < 2; i++ )
	{		
		m_endClosed[i] = false;
	}
	m_socketClosedEvent = NULL;
	createdOk = true;
	networkPlayerSmallId = g_NetworkManager.GetHostPlayer()->GetSmallId();
}

Socket::Socket(INetworkPlayer *player, bool response /* = false*/, bool hostLocal /*= false*/)
{
	m_hostServerConnection = false;
	m_hostLocal = hostLocal;
#if defined(_WINDOWS64) || defined(_XBOX)
	m_isTcp = false;
	m_tcpSocket = INVALID_SOCKET;
	m_tcpIn = NULL;
	m_tcpOut = NULL;
#endif

	for( int i = 0; i < 2; i++ )
	{
		InitializeCriticalSection(&m_queueLockNetwork[i]);		
		m_inputStream[i] = NULL;
		m_outputStream[i] = NULL;		
		m_endClosed[i] = false;
	}
	
	if(!response || hostLocal)
	{
		m_inputStream[0] = new SocketInputStreamNetwork(this,0);
		m_outputStream[0] = new SocketOutputStreamNetwork(this,0);
		m_end = SOCKET_CLIENT_END;
	}
	if(response || hostLocal)
	{
		m_inputStream[1] = new SocketInputStreamNetwork(this,1);
		m_outputStream[1] = new SocketOutputStreamNetwork(this,1);
		m_end = SOCKET_SERVER_END;
	}
	m_socketClosedEvent = new C4JThread::Event;
	//printf("New socket made %s\n", player->GetGamertag() );
	networkPlayerSmallId = player->GetSmallId();
	createdOk = true;
}

SocketAddress *Socket::getRemoteSocketAddress()
{
	return NULL;
}

INetworkPlayer *Socket::getPlayer()
{
	return g_NetworkManager.GetPlayerBySmallId(networkPlayerSmallId);
}

void Socket::setPlayer(INetworkPlayer *player)
{
	if(player!=NULL)
	{
		networkPlayerSmallId = player->GetSmallId();
	}
	else
	{
		networkPlayerSmallId = 0;
	}
}

void Socket::pushDataToQueue(const BYTE * pbData, DWORD dwDataSize, bool fromHost /*= true*/)
{
	int queueIdx = SOCKET_CLIENT_END;
	if(!fromHost)
		queueIdx = SOCKET_SERVER_END;

	if( queueIdx != m_end && !m_hostLocal )
	{
		app.DebugPrintf("SOCKET: Error pushing data to queue. End is %d but queue idx id %d\n", m_end, queueIdx);
		return;
	}

	EnterCriticalSection(&m_queueLockNetwork[queueIdx]);
	for( unsigned int i = 0; i < dwDataSize; i++ )
	{
		m_queueNetwork[queueIdx].push(*pbData++);
	}
	LeaveCriticalSection(&m_queueLockNetwork[queueIdx]);
}

void Socket::addIncomingSocket(Socket *socket)
{
	if( s_serverConnection != NULL )
	{
		s_serverConnection->NewIncomingSocket(socket);
	}
}

InputStream *Socket::getInputStream(bool isServerConnection)
{
#if defined(_WINDOWS64) || defined(_XBOX)
	if (m_isTcp) return m_tcpIn;
#endif
	if( !m_hostServerConnection )
	{
		if( m_hostLocal )
		{
			if( isServerConnection )
			{
				return m_inputStream[SOCKET_SERVER_END];
			}
			else
			{
				return m_inputStream[SOCKET_CLIENT_END];
			}
		}
		else
		{
			return m_inputStream[m_end];
		}
	}
	else
	{
		return s_hostInStream[m_end];
	}
}

void Socket::setSoTimeout(int a )
{
#if defined(_WINDOWS64) || defined(_XBOX)
	if (m_isTcp && m_tcpSocket != INVALID_SOCKET)
	{
		DWORD ms = (DWORD)a;
		setsockopt(m_tcpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
		return;
	}
#endif
}

void Socket::setTrafficClass( int a )
{
}

Socket::SocketOutputStream *Socket::getOutputStream(bool isServerConnection)
{
#if defined(_WINDOWS64) || defined(_XBOX)
	if (m_isTcp) return m_tcpOut;
#endif
	if( !m_hostServerConnection )
	{
		if( m_hostLocal )
		{
			if( isServerConnection )
			{
				return m_outputStream[SOCKET_SERVER_END];
			}
			else
			{
				return m_outputStream[SOCKET_CLIENT_END];
			}
		}
		else
		{
			return m_outputStream[m_end];
		}
	}
	else
	{
		return s_hostOutStream[ 1 - m_end ];
	}
}

bool Socket::close(bool isServerConnection)
{
#if defined(_WINDOWS64) || defined(_XBOX)
	if (m_isTcp)
	{
		if (m_tcpSocket != INVALID_SOCKET)
		{
			shutdown(m_tcpSocket, SD_BOTH);
			closesocket(m_tcpSocket);
			m_tcpSocket = INVALID_SOCKET;
		}
		m_endClosed[m_end] = true;
		if (m_socketClosedEvent != NULL) m_socketClosedEvent->Set();
		createdOk = false;
		return true;
	}
#endif
	bool allClosed = false;
	if( m_hostLocal )
	{
		if( isServerConnection )
		{
			m_endClosed[SOCKET_SERVER_END] = true;
			if(m_endClosed[SOCKET_CLIENT_END])
			{
				allClosed = true;
			}
		}
		else
		{
			m_endClosed[SOCKET_CLIENT_END] = true;
			if(m_endClosed[SOCKET_SERVER_END])
			{
				allClosed = true;
			}
		}
	}
	else
	{
		allClosed = true;
		m_endClosed[m_end] = true;
	}
	if( allClosed && m_socketClosedEvent != NULL )
	{
		m_socketClosedEvent->Set();
	}
	if(allClosed) createdOk = false;
	return allClosed;
}

/////////////////////////////////// Socket for input, on local connection ////////////////////

Socket::SocketInputStreamLocal::SocketInputStreamLocal(int queueIdx)
{
	m_streamOpen = true;
	m_queueIdx = queueIdx;
}

// Try and get an input byte, blocking until one is available
int Socket::SocketInputStreamLocal::read()
{
	while(m_streamOpen && ShutdownManager::ShouldRun(ShutdownManager::eConnectionReadThreads))
	{
		if(TryEnterCriticalSection(&s_hostQueueLock[m_queueIdx]))
		{
			if( s_hostQueue[m_queueIdx].size() )
			{
				byte retval = s_hostQueue[m_queueIdx].front();
				s_hostQueue[m_queueIdx].pop();
				LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
				return retval;
			}
			LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
		}
		Sleep(1);
	}
	return -1;
}

// Try and get an input array of bytes, blocking until enough bytes are available
int Socket::SocketInputStreamLocal::read(byteArray b)
{
	return read(b, 0, b.length);
}

// Try and get an input range of bytes, blocking until enough bytes are available
int Socket::SocketInputStreamLocal::read(byteArray b, unsigned int offset, unsigned int length)
{
	while(m_streamOpen)
	{
		if(TryEnterCriticalSection(&s_hostQueueLock[m_queueIdx]))
		{
			if( s_hostQueue[m_queueIdx].size() >= length )
			{
				for( unsigned int i = 0; i < length; i++ )
				{
					b[i+offset] = s_hostQueue[m_queueIdx].front();
					s_hostQueue[m_queueIdx].pop();
				}
				LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
				return length;
			}
			LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
		}
		Sleep(1);
	}
	return -1;
}

void Socket::SocketInputStreamLocal::close()
{
	m_streamOpen = false;
	EnterCriticalSection(&s_hostQueueLock[m_queueIdx]);
	s_hostQueue[m_queueIdx].empty();
	LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
}

/////////////////////////////////// Socket for output, on local connection ////////////////////

Socket::SocketOutputStreamLocal::SocketOutputStreamLocal(int queueIdx)
{
	m_streamOpen = true;
	m_queueIdx = queueIdx;
}

void Socket::SocketOutputStreamLocal::write(unsigned int b)
{
	if( m_streamOpen != true )
	{
		return;
	}
	EnterCriticalSection(&s_hostQueueLock[m_queueIdx]);
	s_hostQueue[m_queueIdx].push((byte)b);
	LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
}

void Socket::SocketOutputStreamLocal::write(byteArray b)
{
	write(b, 0, b.length);
}

void Socket::SocketOutputStreamLocal::write(byteArray b, unsigned int offset, unsigned int length)
{
	if( m_streamOpen != true )
	{
		return;
	}
	MemSect(12);
	EnterCriticalSection(&s_hostQueueLock[m_queueIdx]);
	for( unsigned int i = 0; i < length; i++ )
	{
		s_hostQueue[m_queueIdx].push(b[offset+i]);
	}
	LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
	MemSect(0);
}

void Socket::SocketOutputStreamLocal::close()
{
	m_streamOpen = false;
	EnterCriticalSection(&s_hostQueueLock[m_queueIdx]);
	s_hostQueue[m_queueIdx].empty();
	LeaveCriticalSection(&s_hostQueueLock[m_queueIdx]);
}

/////////////////////////////////// Socket for input, on network connection ////////////////////

Socket::SocketInputStreamNetwork::SocketInputStreamNetwork(Socket *socket, int queueIdx)
{
	m_streamOpen = true;
	m_queueIdx = queueIdx;
	m_socket = socket;
}

// Try and get an input byte, blocking until one is available
int Socket::SocketInputStreamNetwork::read()
{
	while(m_streamOpen && ShutdownManager::ShouldRun(ShutdownManager::eConnectionReadThreads))
	{
		if(TryEnterCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]))
		{
			if( m_socket->m_queueNetwork[m_queueIdx].size() )
			{
				byte retval = m_socket->m_queueNetwork[m_queueIdx].front();
				m_socket->m_queueNetwork[m_queueIdx].pop();
				LeaveCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]);
				return retval;
			}
			LeaveCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]);
		}
		Sleep(1);
	}
	return -1;
}

// Try and get an input array of bytes, blocking until enough bytes are available
int Socket::SocketInputStreamNetwork::read(byteArray b)
{
	return read(b, 0, b.length);
}

// Try and get an input range of bytes, blocking until enough bytes are available
int Socket::SocketInputStreamNetwork::read(byteArray b, unsigned int offset, unsigned int length)
{
	while(m_streamOpen)
	{
		if(TryEnterCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]))
		{
			if( m_socket->m_queueNetwork[m_queueIdx].size() >= length )
			{
				for( unsigned int i = 0; i < length; i++ )
				{
					b[i+offset] = m_socket->m_queueNetwork[m_queueIdx].front();
					m_socket->m_queueNetwork[m_queueIdx].pop();
				}
				LeaveCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]);
				return length;
			}
			LeaveCriticalSection(&m_socket->m_queueLockNetwork[m_queueIdx]);
		}
		Sleep(1);
	}
	return -1;
}

void Socket::SocketInputStreamNetwork::close()
{
	m_streamOpen = false;
}

/////////////////////////////////// Socket for output, on network connection ////////////////////

Socket::SocketOutputStreamNetwork::SocketOutputStreamNetwork(Socket *socket, int queueIdx)
{
	m_queueIdx = queueIdx;
	m_socket = socket;
	m_streamOpen = true;
}

void Socket::SocketOutputStreamNetwork::write(unsigned int b)
{
	if( m_streamOpen != true ) return;
	byteArray barray;
	byte bb;
	bb = (byte)b;
	barray.data = &bb;
	barray.length = 1;
	write(barray, 0, 1);

}

void Socket::SocketOutputStreamNetwork::write(byteArray b)
{
	write(b, 0, b.length);
}

void Socket::SocketOutputStreamNetwork::write(byteArray b, unsigned int offset, unsigned int length)
{	
	writeWithFlags(b, offset, length, 0);
}

void Socket::SocketOutputStreamNetwork::writeWithFlags(byteArray b, unsigned int offset, unsigned int length, int flags)
{
	if( m_streamOpen != true ) return;
	if( length == 0 ) return;

	// If this is a local connection, don't bother going through QNet as it just delivers it straight anyway
	if( m_socket->m_hostLocal )
	{
		// We want to write to the queue for the other end of this socket stream
		int queueIdx = m_queueIdx;
		if(queueIdx == SOCKET_CLIENT_END)
			queueIdx = SOCKET_SERVER_END;
		else
			queueIdx = SOCKET_CLIENT_END;

		EnterCriticalSection(&m_socket->m_queueLockNetwork[queueIdx]);
		for( unsigned int i = 0; i < length; i++ )
		{
			m_socket->m_queueNetwork[queueIdx].push(b[offset+i]);
		}
		LeaveCriticalSection(&m_socket->m_queueLockNetwork[queueIdx]);
	}
	else
	{
		XRNM_SEND_BUFFER buffer;
		buffer.pbyData = &b[offset];
		buffer.dwDataSize = length;

		INetworkPlayer *hostPlayer = g_NetworkManager.GetHostPlayer();
		if(hostPlayer == NULL)
		{
			app.DebugPrintf("Trying to write to network, but the hostPlayer is NULL\n");
			return;
		}
		INetworkPlayer *socketPlayer = m_socket->getPlayer();
		if(socketPlayer == NULL)
		{
			app.DebugPrintf("Trying to write to network, but the socketPlayer is NULL\n");
			return;
		}

		if( m_queueIdx == SOCKET_SERVER_END )
		{
			//printf( "Sent %u bytes of data from \"%ls\" to \"%ls\"\n",
			//buffer.dwDataSize,
			//hostPlayer->GetGamertag(),
			//m_socket->networkPlayer->GetGamertag());

			hostPlayer->SendData(socketPlayer, buffer.pbyData, buffer.dwDataSize, QNET_SENDDATA_RELIABLE | QNET_SENDDATA_SEQUENTIAL | flags);

	// 		DWORD queueSize = hostPlayer->GetSendQueueSize( NULL, QNET_GETSENDQUEUESIZE_BYTES  );
	// 		if( queueSize > 24000 )
	// 		{
	// 			//printf("Queue size is: %d, forcing doWork()\n",queueSize);
	// 			g_NetworkManager.DoWork();
	// 		}
		}
		else
		{
			//printf( "Sent %u bytes of data from \"%ls\" to \"%ls\"\n",
			//buffer.dwDataSize,
			//m_socket->networkPlayer->GetGamertag(),
			//hostPlayer->GetGamertag());

			socketPlayer->SendData(hostPlayer, buffer.pbyData, buffer.dwDataSize, QNET_SENDDATA_RELIABLE | QNET_SENDDATA_SEQUENTIAL | flags);
		}
	}
}

void Socket::SocketOutputStreamNetwork::close()
{
	m_streamOpen = false;
}