#include "stdafx.h"
#include "InputOutputStream.h"
#include "Socket.h"
#include "Connection.h"
#include "ThreadName.h"
#include "compression.h"
#include "..\Minecraft.Client\PS3\PS3Extras\ShutdownManager.h"
#include "..\Minecraft.Client\Common\Network\NetworkPlayerInterface.h"

// Timeout disconnects are important for TCP: a client that loses its network
// without closing the socket must not stay in the world forever. Keep this
// enabled in debug builds too so the timeout path is actually exercised.
#ifndef CONNECTION_ENABLE_TIMEOUT_DISCONNECT
#define CONNECTION_ENABLE_TIMEOUT_DISCONNECT 1
#endif

int Connection::readThreads = 0;
int Connection::writeThreads = 0;

// Client-side flag: a TCP peer disconnected. The renderer uses it to stop
// rebuilding chunks so the main thread can reach the exit action promptly.
// Only set for client connections (never for the server's player connections).
volatile bool g_tcpPeerDisconnected = false;

int Connection::readSizes[256];
int Connection::writeSizes[256];



void Connection::_init()
{
//	printf("Con:0x%x init\n",this);
	InitializeCriticalSection(&writeLock);
	InitializeCriticalSection(&threadCounterLock);
	InitializeCriticalSection(&incoming_cs);

	running = true;
	quitting = false;
	disconnected = false;
	disconnectReason = DisconnectPacket::eDisconnect_None;
	noInputTicks = 0;
	estimatedRemaining = 0;
	fakeLag = 0;
	slowWriteDelay = 50;

	saqThreadID = 0;
	closeThreadID = 0;

	tickCount = 0;

}

// 4J Jev, need to delete the critical section.
Connection::~Connection()
{
	// 4J Stu - Just to be sure, make sure the read and write threads terminate themselves before the connection object is destroyed
	running = false;
	if( dis ) dis->close();		// The input stream needs closed before the readThread, or the readThread
								// may get stuck whilst blocking waiting on a read
#if defined(_WINDOWS64) || defined(_XBOX)
	if( socket ) socket->shutdownTcp();	// also wakes a write thread blocked in send()
#endif
	// The read/write loops wait on these events with a 100 ms timeout. Wake
	// them immediately so disconnecting a player does not cost ~100 ms (and
	// hundreds of players do not look like ghosts while their threads sleep).
	if (m_hWakeReadThread) m_hWakeReadThread->Set();
	if (m_hWakeWriteThread) m_hWakeWriteThread->Set();
	readThread->WaitForCompletion(INFINITE);
	writeThread->WaitForCompletion(INFINITE);

	DeleteCriticalSection(&writeLock);
	DeleteCriticalSection(&threadCounterLock);
	DeleteCriticalSection(&incoming_cs);

	delete m_hWakeReadThread;
	delete m_hWakeWriteThread;

	// These should all have been destroyed in close() but no harm in checking again
	delete byteArrayDos;
	byteArrayDos = NULL;
	delete baos;
	baos = NULL;
	if( bufferedDos )
	{
		bufferedDos->deleteChildStream();
		delete bufferedDos;
		bufferedDos = NULL;
	}
	delete dis;
	dis = NULL;

	if( socket )
	{
		INetworkPlayer *socketPlayer = socket->getPlayer();
		if( socketPlayer != NULL ) socketPlayer->SetSocket(NULL);
		bool wasTcp = socket->isTcp();
		socket->close(false);
		if( wasTcp ) delete socket;
		socket = NULL;
	}
}

Connection::Connection(Socket *socket, const wstring& id, PacketListener *packetListener) // throws IOException
{
	_init();

	this->socket = socket;

	address = socket->getRemoteSocketAddress();

	this->packetListener = packetListener;

	//try {
	socket->setSoTimeout(30000);
	socket->setTrafficClass(IPTOS_THROUGHPUT | IPTOS_LOWDELAY);

	/* 4J JEV no catch
	} catch (SocketException e) {
	// catching this exception because it (apparently?) causes problems
	// on OSX Tiger
	System.err.println(e.getMessage());
	}*/

	dis = new DataInputStream(socket->getInputStream(packetListener->isServerPacketListener()));
		
	sos = socket->getOutputStream(packetListener->isServerPacketListener());
	bufferedDos = new DataOutputStream(new BufferedOutputStream(sos, SEND_BUFFER_SIZE));
	baos = new ByteArrayOutputStream( SEND_BUFFER_SIZE );
	byteArrayDos = new DataOutputStream(baos);

	m_hWakeReadThread = new C4JThread::Event;
	m_hWakeWriteThread = new C4JThread::Event;

	const char *szId = wstringtofilename(id);
	char readThreadName[256];
	char writeThreadName[256];
	sprintf(readThreadName,"%s read\n",szId);
	sprintf(writeThreadName,"%s write\n",szId);

	readThread = new C4JThread(runRead, (void*)this, readThreadName, READ_STACK_SIZE);
	writeThread = new C4JThread(runWrite, this, writeThreadName, WRITE_STACK_SIZE);
	readThread->SetProcessor(CPU_CORE_CONNECTIONS);
	writeThread->SetProcessor(CPU_CORE_CONNECTIONS );
#ifdef __ORBIS__
	readThread->SetPriority(THREAD_PRIORITY_BELOW_NORMAL);	// On Orbis, this core is also used for Matching 2, and that priority of that seems to be always at default no matter what we set it to. Prioritise this below Matching 2.
	writeThread->SetPriority(THREAD_PRIORITY_BELOW_NORMAL);	// On Orbis, this core is also used for Matching 2, and that priority of that seems to be always at default no matter what we set it to. Prioritise this below Matching 2.
#endif

	readThread->Run();
	writeThread->Run();


	/* 4J JEV, java:	
	new Thread(wstring(id).append(L" read thread")) {

	};

	writeThread = new Thread(id + " write thread") {
	public void run() {

	};

	readThread->start();
	writeThread->start();
	*/
}


void Connection::setListener(PacketListener *packetListener)
{
	this->packetListener = packetListener;
}

void Connection::send(shared_ptr<Packet> packet)
{
	if (quitting) 	return;

	MemSect(15);
	// 4J Jev, synchronized (&writeLock)
	EnterCriticalSection(&writeLock);

	estimatedRemaining += packet->getEstimatedSize() + 1;
	if (packet->shouldDelay) 
	{
		// 4J We have delayed it enough by putting it in the slow queue, so don't delay when we actually send it
		packet->shouldDelay = false;
		outgoing_slow.push(packet);
	}
	else
	{
		outgoing.push(packet);
	}

	// 4J Jev, end synchronized.
	LeaveCriticalSection(&writeLock);
	MemSect(0);
}


void Connection::queueSend(shared_ptr<Packet> packet)
{
	if (quitting) return;
	estimatedRemaining += packet->getEstimatedSize() + 1;
	outgoing_slow.push(packet);
}

bool Connection::writeTick()
{
	bool didSomething = false;

	// 4J Stu - If the connection is closed and the output stream has been deleted
	if(bufferedDos==NULL || byteArrayDos==NULL)
		return didSomething;

	// try {
	if (!outgoing.empty() && (fakeLag == 0 || System::currentTimeMillis() - outgoing.front()->createTime >= fakeLag))
	{
		shared_ptr<Packet> packet;

		EnterCriticalSection(&writeLock);

		packet = outgoing.front();
		outgoing.pop();
		estimatedRemaining -= packet->getEstimatedSize() + 1;

		LeaveCriticalSection(&writeLock);

		if (socket != NULL && socket->isTcp())
		{
			Packet::writeTcpFrame(packet, bufferedDos, byteArrayDos, baos);
		}
		else
		{
			Packet::writePacket(packet, bufferedDos);
		}
		

#ifndef _CONTENT_PACKAGE
		// 4J Added for debugging
		if( !socket->isLocal() ) 
			Packet::recordOutgoingPacket(packet);
#endif

		// 4J Stu - Changed this so that rather than writing to the network stream through a buffered stream we want to:
		// a) Only push whole "game" packets to QNet, rather than amalgamated chunks of data that may include many packets, and partial packets
		// b) To be able to change the priority and queue of a packet if required
		//sos->writeWithFlags( baos->buf, 0, baos->size(), 0 );
		//baos->reset();

		writeSizes[packet->getId()] += packet->getEstimatedSize() + 1;
		didSomething = true;
	}

	if ((slowWriteDelay-- <= 0) && !outgoing_slow.empty() && (fakeLag == 0 || System::currentTimeMillis() - outgoing_slow.front()->createTime >= fakeLag))
	{
		shared_ptr<Packet> packet;

		//synchronized (writeLock) {

		EnterCriticalSection(&writeLock);

		packet = outgoing_slow.front();
		outgoing_slow.pop();
		estimatedRemaining -= packet->getEstimatedSize() + 1;

		LeaveCriticalSection(&writeLock);

		// If the shouldDelay flag is still set at this point then we want to write it to QNet as a single packet with priority flags
		// Otherwise just buffer the packet with other outgoing packets as the java game did
		if (socket != NULL && socket->isTcp())
		{
			// TCP has no QNet priority flags; keep the packet on the same
			// framed byte stream as everything else so ordering is preserved.
			Packet::writeTcpFrame(packet, bufferedDos, byteArrayDos, baos);
		}
		else if(packet->shouldDelay)
		{
			Packet::writePacket(packet, byteArrayDos);

			// 4J Stu - Changed this so that rather than writing to the network stream through a buffered stream we want to:
			// a) Only push whole "game" packets to QNet, rather than amalgamated chunks of data that may include many packets, and partial packets
			// b) To be able to change the priority and queue of a packet if required
			int flags = QNET_SENDDATA_LOW_PRIORITY | QNET_SENDDATA_SECONDARY;
			sos->writeWithFlags( baos->buf, 0, baos->size(), flags  );
			baos->reset();
		}
		else
		{
			Packet::writePacket(packet, bufferedDos);
		}

#ifndef _CONTENT_PACKAGE
		// 4J Added for debugging
		if( !socket->isLocal() ) 
			Packet::recordOutgoingPacket(packet);
#endif	

		writeSizes[packet->getId()] += packet->getEstimatedSize() + 1;
		slowWriteDelay = 0;
		didSomething = true;
	}
	/* 4J JEV, removed try/catch
	} catch (Exception e) {
	if (!disconnected) handleException(e);
	return false;
	} */

	return didSomething;
}


void Connection::flush()
{
	// TODO 4J Stu - How to interrupt threads? Or do we need to change the multithreaded functions a bit more
	//readThread.interrupt();
	//writeThread.interrupt();
	m_hWakeReadThread->Set();
	m_hWakeWriteThread->Set();
}


bool Connection::readTick()
{
	bool didSomething = false;

	// 4J Stu - If the connection has closed and the input stream has been deleted
	if(dis==NULL)
		return didSomething;

	//try {

	// Windows64 TCP has no message boundaries. Use the framed reader so a
	// damaged packet cannot shift every following packet in the stream.
	shared_ptr<Packet> packet;
	if (socket != NULL && socket->isTcp())
	{
		packet = Packet::readTcpFrame(dis, packetListener->isServerPacketListener());
	}
	else
	{
		packet = Packet::readPacket(dis, packetListener->isServerPacketListener());
	}

	if (packet != NULL)
	{
		readSizes[packet->getId()] += packet->getEstimatedSize() + 1;
		EnterCriticalSection(&incoming_cs);
		if(!quitting)
		{
			incoming.push(packet);
		}
		LeaveCriticalSection(&incoming_cs);
		didSomething = true;
	} 
	else
	{
//		printf("Con:0x%x readTick close EOS\n",this);

		// 4J Stu - Remove this line
		// Fix for #10410 - UI: If the player is removed from a splitscreened host�s game, the next game that player joins will produce a message stating that the host has left.
		//close(DisconnectPacket::eDisconnect_EndOfStream);

		// TCP: readPacket returns NULL when the peer closed the socket (EOF).
		// Mark the connection disconnected so Connection::tick() calls
		// onDisconnect on the main thread. That removes the player from the
		// player list and broadcasts the entity removal, instead of leaving a
		// frozen ghost player behind.
		//
		// Do NOT call close() here: it waits for this read thread to finish
		// and would deadlock.
		if (socket != NULL && socket->isTcp())
		{
			if (!packetListener->isServerPacketListener())
			{
				g_tcpPeerDisconnected = true;
			}
			disconnected = true;
			disconnectReason = DisconnectPacket::eDisconnect_EndOfStream;
			disconnectReasonObjects = NULL;
		}
	}


	/* 4J JEV, removed try/catch
	} catch (Exception e) {
	if (!disconnected) handleException(e);
	return false;
	} */

	return didSomething;
}


/* 4J JEV, removed try/catch
void handleException(Exception e)
{
e.printStackTrace();
close("disconnect.genericReason", "Internal exception: " + e.toString());
}*/


void Connection::close(DisconnectPacket::eDisconnectReason reason, ...)
{
//	printf("Con:0x%x close\n",this);
	if (!running) return;
//	printf("Con:0x%x close doing something\n",this);
	disconnected = true;

	va_list input;
	va_start( input, reason );

	disconnectReason = reason;//va_arg( input, const wstring );

	vector<void *> objs = vector<void *>();
	void *i = NULL;
	while (i != NULL)
	{
		i = va_arg( input, void* );
		objs.push_back(i);
	}

	if( objs.size() )
	{
		disconnectReasonObjects = &objs[0];
	}
	else
	{
		disconnectReasonObjects = NULL;
	}

	//	int count = 0, sum = 0, i = first;
	//	va_list marker;
	//	
	//	va_start( marker, first );
	//	while( i != -1 )
	//	{
	//	   sum += i;
	//	   count++;
	//	   i = va_arg( marker, int);
	//	}
	//	va_end( marker );
	//	return( sum ? (sum / count) : 0 );


//	CreateThread(NULL, 0, runClose, this, 0, &closeThreadID);

	running = false;
	if (!packetListener->isServerPacketListener())
	{
		g_tcpPeerDisconnected = true;
	}

	if( dis ) dis->close();		// The input stream needs closed before the readThread, or the readThread
						// may get stuck whilst blocking waiting on a read
#if defined(_WINDOWS64) || defined(_XBOX)
	if( socket ) socket->shutdownTcp();	// also wakes a write thread blocked in send()
#endif
	if (m_hWakeReadThread) m_hWakeReadThread->Set();
	if (m_hWakeWriteThread) m_hWakeWriteThread->Set();

	// Make sure that the read & write threads are dead before we go and kill the streams that they depend on
	readThread->WaitForCompletion(INFINITE);
	writeThread->WaitForCompletion(INFINITE);

	delete dis;
	dis = NULL;

	if( bufferedDos )
	{
		bufferedDos->close();
		bufferedDos->deleteChildStream();
		delete bufferedDos;
		bufferedDos = NULL;
	}
	if( byteArrayDos )
	{
		byteArrayDos->close();
		delete byteArrayDos;
		byteArrayDos = NULL;
	}
	if( socket )
	{
		INetworkPlayer *socketPlayer = socket->getPlayer();
		if( socketPlayer != NULL ) socketPlayer->SetSocket(NULL);
		bool wasTcp = socket->isTcp();
		socket->close(packetListener->isServerPacketListener());
		if( wasTcp ) delete socket;
		socket = NULL;
	}
}

void Connection::tick()
{
	if (estimatedRemaining > 1 * 1024 * 1024)
	{
		close(DisconnectPacket::eDisconnect_Overflow);
	}
	EnterCriticalSection(&incoming_cs);
	bool empty = incoming.empty();
	LeaveCriticalSection(&incoming_cs);
	if (empty)
	{
#if CONNECTION_ENABLE_TIMEOUT_DISCONNECT
		if (noInputTicks++ == MAX_TICKS_WITHOUT_INPUT)
		{
			close(DisconnectPacket::eDisconnect_TimeOut);
		}
#endif
	}
	// 4J Stu - Moved this a bit later in the function to stop the race condition of Disconnect packets not being processed when local client leaves
	//else if( socket && socket->isClosing() )
	//{
	//	close(DisconnectPacket::eDisconnect_Closed);
	//}
	else
	{
		noInputTicks = 0;

	}

	// 4J Added - Send a KeepAlivePacket every now and then to ensure that our read and write threads don't timeout
	tickCount++;
    if (tickCount % 20 == 0)
	{
        send( shared_ptr<KeepAlivePacket>( new KeepAlivePacket() ) );
    }

	// 4J Stu - 1.8.2 changed from 100 to 1000
	int max = 1000;

	// 4J-PB - NEEDS CHANGED!!!
	// If we can call connection.close from within a packet->handle, then we can lockup because the loop below has locked incoming_cs, and the connection.close will flag the read and write threads for the connection to close.
	// they are running on other threads, and will try to lock incoming_cs
	// We got this with a pre-login packet of a player who wasn't allowed to play due to parental controls, so was kicked out
	// This has been changed to use a eAppAction_ExitPlayerPreLogin which will run in the main loop, so the connection will not be ticked at that point


	EnterCriticalSection(&incoming_cs);
	// 4J Stu - If disconnected, then we shouldn't process incoming packets
	std::vector< shared_ptr<Packet> > packetsToHandle;
	// Movement is state, not an event. A client (or a test bot) that sends
	// faster than the tick rate can queue hundreds of MovePlayerPackets per
	// tick; handling all of them is wasted work and used to make a single
	// tick take minutes at 200+ players. Keep the newest one and process the
	// other packet types in order.
	bool coalesceMoves = packetListener->isServerPacketListener();
	shared_ptr<MovePlayerPacket> pendingPos = nullptr;
	shared_ptr<MovePlayerPacket> pendingRot = nullptr;
	while (!disconnected && !g_NetworkManager.IsLeavingGame() && g_NetworkManager.IsInSession() && !incoming.empty() && max-- >= 0)
	{
		shared_ptr<Packet> packet = incoming.front();
		incoming.pop();
		shared_ptr<MovePlayerPacket> move = dynamic_pointer_cast<MovePlayerPacket>(packet);
		if (coalesceMoves && move != nullptr && (move->hasPos || move->hasRot))
		{
			// Keep the newest position and rotation separately; a later Rot
			// packet must not discard an earlier Pos update. The newest
			// packet is enough: intermediate positions are not replayed.
			if (move->hasPos) pendingPos = move;
			if (move->hasRot) pendingRot = move;
		}
		else
		{
			packetsToHandle.push_back(packet);
		}
	}
	if (pendingPos != nullptr)
	{
		// Process the latest position before the other queued packets so
		// actions/chat see the current position.
		packetsToHandle.insert(packetsToHandle.begin(), pendingPos);
		if (pendingRot != nullptr && pendingRot != pendingPos)
		{
			packetsToHandle.insert(packetsToHandle.begin() + 1, pendingRot);
		}
	}
	else if (pendingRot != nullptr)
	{
		packetsToHandle.insert(packetsToHandle.begin(), pendingRot);
	}
	LeaveCriticalSection(&incoming_cs);

	// MGH - moved the packet handling outside of the incoming_cs block, as it was locking up sometimes when disconnecting
	for(int i=0; i<packetsToHandle.size();i++)
	{
		// If the peer disconnected while these packets were queued (TCP EOF or
		// a kick), do not keep processing a potentially large chunk backlog
		// before onDisconnect runs. This was making the client take tens of
		// seconds to return to the menu after a kick.
		if (disconnected) break;
		PIXBeginNamedEvent(0,"Handling packet %d\n",packetsToHandle[i]->getId());
		packetsToHandle[i]->handle(packetListener);
		PIXEndNamedEvent();
	}
	flush();

	// 4J Stu - Moved this a bit later in the function to stop the race condition of Disconnect packets not being processed when local client leaves
	if( socket && socket->isClosing() )
	{
		close(DisconnectPacket::eDisconnect_Closed);
	}

	// 4J - split the following condition (used to be disconnect && iscoming.empty()) so we can wrap the access in a critical section
	if (disconnected)
	{
		// The read thread may have queued packets before it saw EOF. The
		// drain loop above deliberately stops when disconnected, so those
		// packets would keep incoming non-empty forever and onDisconnect
		// would never run (the ghost-player bug). Drop the stale queue and
		// notify the listener immediately.
		EnterCriticalSection(&incoming_cs);
		while (!incoming.empty()) incoming.pop();
		LeaveCriticalSection(&incoming_cs);
		app.DebugPrintf("Connection::tick: calling onDisconnect reason=%d\n", (int)disconnectReason);
		packetListener->onDisconnect(disconnectReason, disconnectReasonObjects);
		disconnected = false; // 4J added - don't keep sending this every tick
	}
}

SocketAddress *Connection::getRemoteAddress()
{
	return (SocketAddress *) address;
}

void Connection::sendAndQuit()
{
	if (quitting)
	{
		return;
	}
//	printf("Con:0x%x send & quit\n",this);
	flush();
	quitting = true;
	// TODO 4J Stu - How to interrupt threads? Or do we need to change the multithreaded functions a bit more
	//readThread.interrupt();

#if 1
	// 4J - this used to be in a thread but not sure why, and is causing trouble for us if we kill the connection
	// whilst the thread is still expecting to be able to send a packet a couple of seconds after starting it
	if (running)
	{
		// 4J TODO writeThread.interrupt();
		close(DisconnectPacket::eDisconnect_Closed);
	}
#else
	CreateThread(NULL, 0, runSendAndQuit, this, 0, &saqThreadID);
#endif
}

int Connection::countDelayedPackets()
{
	return (int)outgoing_slow.size();
}


int Connection::runRead(void* lpParam)
{
	ShutdownManager::HasStarted(ShutdownManager::eConnectionReadThreads);
	Connection *con = (Connection *)lpParam;

	if (con == NULL)
	{
#ifdef __PS3__
		ShutdownManager::HasFinished(ShutdownManager::eConnectionReadThreads);
#endif
		return 0;
	}

	Compression::UseDefaultThreadStorage();

	CRITICAL_SECTION *cs = &con->threadCounterLock;

	EnterCriticalSection(cs);
	con->readThreads++;
	LeaveCriticalSection(cs);

	//try {

	MemSect(19);
	while (con->running && !con->quitting && ShutdownManager::ShouldRun(ShutdownManager::eConnectionReadThreads))
	{
		while (con->readTick())
			;

		// try {
		//Sleep(100L);
		// TODO - 4J Stu - 1.8.2 changes these sleeps to 2L, but not sure whether we should do that as well
		con->m_hWakeReadThread->WaitForSignal(100L);
	}
	MemSect(0);

	/* 4J JEV, removed try/catch
	} catch (InterruptedException e) {
	}
	}
	} finally {
	synchronized (threadCounterLock) {
	readThreads--;
	}
	} */

	ShutdownManager::HasFinished(ShutdownManager::eConnectionReadThreads);
	return 0;
}

int Connection::runWrite(void* lpParam)
{
	ShutdownManager::HasStarted(ShutdownManager::eConnectionWriteThreads);
	Connection *con = dynamic_cast<Connection *>((Connection *) lpParam);

	if (con == NULL)
	{
		ShutdownManager::HasFinished(ShutdownManager::eConnectionWriteThreads);
		return 0;
	}

	Compression::UseDefaultThreadStorage();

	CRITICAL_SECTION *cs = &con->threadCounterLock;

	EnterCriticalSection(cs);
	con->writeThreads++;
	LeaveCriticalSection(cs);

	// 4J Stu - Adding this to force us to run through the writeTick at least once after the event is fired
	// Otherwise there is a race between the calling thread setting the running flag and this loop checking the condition
	DWORD waitResult = WAIT_TIMEOUT;

	while ((con->running || waitResult == WAIT_OBJECT_0 ) && ShutdownManager::ShouldRun(ShutdownManager::eConnectionWriteThreads))
	{
		while (con->writeTick())
			;

		//Sleep(100L);	
		// TODO - 4J Stu - 1.8.2 changes these sleeps to 2L, but not sure whether we should do that as well	
		waitResult = con->m_hWakeWriteThread->WaitForSignal(100L);

		if (con->bufferedDos != NULL) con->bufferedDos->flush();
		//if (con->byteArrayDos != NULL) con->byteArrayDos->flush();
	}


	// 4J was in a finally block.
	EnterCriticalSection(cs);
	con->writeThreads--;
	LeaveCriticalSection(cs);

	ShutdownManager::HasFinished(ShutdownManager::eConnectionWriteThreads);
	return 0;
}

int Connection::runClose(void* lpParam)
{
	Connection *con = dynamic_cast<Connection *>((Connection *) lpParam);

	if (con == NULL) return 0;

	//try {

	Sleep(2000);
	if (con->running)
	{
		// 4J TODO writeThread.interrupt();
		con->close(DisconnectPacket::eDisconnect_Closed);
	}

	/* 4J Jev, removed try/catch
	} catch (Exception e) {
	e.printStackTrace();
	} */

	return 1;
}

int Connection::runSendAndQuit(void* lpParam)
{
	Connection *con = dynamic_cast<Connection *>((Connection *) lpParam);
//	printf("Con:0x%x runSendAndQuit\n",con);

	if (con == NULL) return 0;

	//try {

	Sleep(2000);
	if (con->running)
	{
		// 4J TODO writeThread.interrupt();
		con->close(DisconnectPacket::eDisconnect_Closed);
//		printf("Con:0x%x runSendAndQuit close\n",con);
	}

//	printf("Con:0x%x runSendAndQuit end\n",con);
	/* 4J Jev, removed try/catch
	} catch (Exception e) {
	e.printStackTrace();
	} */

	return 0;
}
