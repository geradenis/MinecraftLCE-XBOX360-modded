#pragma once
#include <xrnm.h>
#include <queue>
#include <qnet.h>
#if defined(_WINDOWS64)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#elif defined(_XBOX)
// Xbox 360 Winsock (WSA) API. Xenia implements WSAStartup/socket/connect/
// send/recv, so the same TCP transport can be used by the Xbox client.
#include <winsockx.h>
#pragma comment(lib, "xnet.lib")
#endif
#include "InputStream.h"
#include "OutputStream.h"

#define SOCKET_CLIENT_END 0
#define SOCKET_SERVER_END 1

class SocketAddress;
class ServerConnection;

class Socket
{
public:
	// 4J Added so we can add a priority write function
	class SocketOutputStream : public OutputStream
	{
	public:
		// The flags are those that can be used for the QNet SendData function
		virtual void writeWithFlags(byteArray b, unsigned int offset, unsigned int length, int flags) { write(b, offset, length); }
	};

private:
	class SocketInputStreamLocal : public InputStream
	{
	public:
		bool m_streamOpen;
	private:
		int	m_queueIdx;
	public:
		SocketInputStreamLocal(int queueIdx);

		virtual int read();
		virtual int read(byteArray b);
		virtual int read(byteArray b, unsigned int offset, unsigned int length);
		virtual void close();
		virtual __int64 skip(__int64 n) { return n; } // 4J Stu - Not implemented
		virtual void flush() {}
	};

	class SocketOutputStreamLocal : public SocketOutputStream
	{
	public:
		bool m_streamOpen;
	private:
		int m_queueIdx;
	public:
		SocketOutputStreamLocal(int queueIdx);

		virtual void write(unsigned int b);
		virtual void write(byteArray b);
		virtual void write(byteArray b, unsigned int offset, unsigned int length);
		virtual void close();
		virtual void flush() {}
	};

	class SocketInputStreamNetwork : public InputStream
	{
		bool m_streamOpen;
		int	m_queueIdx;
		Socket *m_socket;
	public:
		SocketInputStreamNetwork(Socket *socket, int queueIdx);

		virtual int read();
		virtual int read(byteArray b);
		virtual int read(byteArray b, unsigned int offset, unsigned int length);
		virtual void close();
		virtual __int64 skip(__int64 n) { return n; } // 4J Stu - Not implemented
		virtual void flush() {}
	};
	class SocketOutputStreamNetwork : public SocketOutputStream
	{
		bool m_streamOpen;
		int	m_queueIdx;
		Socket *m_socket;
	public:
		SocketOutputStreamNetwork(Socket *socket, int queueIdx);

		virtual void write(unsigned int b);
		virtual void write(byteArray b);
		virtual void write(byteArray b, unsigned int offset, unsigned int length);
		virtual void writeWithFlags(byteArray b, unsigned int offset, unsigned int length, int flags);
		virtual void close();
		virtual void flush() {}
	};
	
	bool m_hostServerConnection;			// true if this is the connection between the host player and server
	bool m_hostLocal;						// true if this player on the same machine as the host
	int m_end;								// 0 for client side or 1 for host side

	// For local connections between the host player and the server
	static CRITICAL_SECTION s_hostQueueLock[2]; 
	static std::queue<byte> s_hostQueue[2];
	static SocketOutputStreamLocal *s_hostOutStream[2];
	static SocketInputStreamLocal *s_hostInStream[2];

	// For network connections
	std::queue<byte> m_queueNetwork[2];		// For input data
	CRITICAL_SECTION m_queueLockNetwork[2];  // For input data
	SocketInputStreamNetwork *m_inputStream[2];
	SocketOutputStreamNetwork *m_outputStream[2];
	bool m_endClosed[2];

	// Host only connection class
	static ServerConnection *s_serverConnection;

	BYTE networkPlayerSmallId;

#if defined(_WINDOWS64) || defined(_XBOX)
	// TCP transport for the dedicated server / direct connect. The Xbox build
	// uses it when a server.txt with the server address is present.
	bool m_isTcp;
	SOCKET m_tcpSocket;
	class SocketInputStreamTcp;
	class SocketOutputStreamTcp;
	SocketInputStreamTcp *m_tcpIn;
	SocketOutputStreamTcp *m_tcpOut;
	static bool s_wsaInit;
	static void ensureWsa();
#endif

public:	
	C4JThread::Event* m_socketClosedEvent;

#if defined(_WINDOWS64) || defined(_XBOX)
	Socket(SOCKET tcpSocket, bool serverEnd);
	Socket(const wstring& host, int port);
	static void startTcpListener(int port);
	bool isTcp() const { return m_isTcp; }
	// Unblock any thread blocked in recv()/send() without invalidating the
	// handle (Socket::close() still owns the handle).
	void shutdownTcp();
	~Socket();
#endif

	INetworkPlayer *getPlayer();
	void setPlayer(INetworkPlayer *player);

public:
	static void Initialise(ServerConnection *serverConnection);
	Socket(bool response = false);								// 4J - Create a local socket, for end 0 or 1 of a connection
	Socket(INetworkPlayer *player, bool response  = false, bool hostLocal = false);		// 4J - Create a socket for an INetworkPlayer
	SocketAddress *getRemoteSocketAddress();
	void pushDataToQueue(const BYTE * pbData, DWORD dwDataSize, bool fromHost = true);
	static void addIncomingSocket(Socket *socket);
	InputStream *getInputStream(bool isServerConnection);
	void setSoTimeout(int a );
	void setTrafficClass( int a );
	SocketOutputStream *getOutputStream(bool isServerConnection);
	bool close(bool isServerConnection);
	bool createdOk;
	bool isLocal() { return m_hostLocal; }

	bool isClosing() { return m_endClosed[SOCKET_CLIENT_END] || m_endClosed[SOCKET_SERVER_END]; }
	BYTE getSmallId() { return networkPlayerSmallId; }
};