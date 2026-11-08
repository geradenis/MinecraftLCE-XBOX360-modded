#pragma once
using namespace std;

class Packet;
class PacketListener;
class DataInputStream;
class DataOutputStream;
class ByteArrayOutputStream;

#define PACKET_ENABLE_STAT_TRACKING 0

class Packet;

typedef shared_ptr<Packet> (*packetCreateFn)();

class Packet
{
public:
	class PacketStatistics 
	{
	private:
		int count;
		int totalSize;

		// 4J Added
		__int64 countSamples[512];
		__int64 sizeSamples[512];
		int samplesPos;
		__int64 firstSampleTime;


	public:
		const int id;

	public:
		PacketStatistics(int id) : id( id ), count( 0 ), totalSize( 0 ), samplesPos( 0 ), firstSampleTime( 0 ) { countSamples[0] = 0; sizeSamples[0] = 0; }
		void addPacket(int bytes);
		int getCount();
		double getAverageSize();

		// 4J Added
		void renderStats();
		__int64 getCountSample(int samplePos);
		wstring getLegendString();
	};

	// 4J JEV, replaces the static blocks.
	static void staticCtor();

public:
	static unordered_map<int, packetCreateFn> idToCreateMap; // IntHashMap in 1.8.2 ... needed? // Made public in 1.0.1

	static unordered_set<int> clientReceivedPackets;
	static unordered_set<int> serverReceivedPackets;
	static unordered_set<int> sendToAnyClientPackets;

	// Set by readUtf when a string length is invalid (negative or too long).
	// Packet::readPacket checks it and drops the connection instead of
	// asserting: this is almost always a client/server build mismatch or a
	// desynchronised packet stream.
	// The Xbox 360 XDK compiler (VS2010) has no thread_local; the Xbox client
	// has a single TCP connection, so a plain static is sufficient there.
#if defined(_XBOX)
	static bool s_readError;
#else
	static thread_local bool s_readError;
#endif

	// 4J Stu - Added the sendToAnyClient param so we can limit some packets to be only sent to one player on a system
	// 4J Stu - Added renderStats param for use in debugging
	static void map(int id, bool receiveOnClient, bool receiveOnServer, bool sendToAnyClient, bool renderStats, const type_info& clazz, packetCreateFn );

public:
	const __int64 createTime;

	Packet();

	static shared_ptr<Packet> getPacket(int id);

	// 4J Added
	static bool canSendToAnyClient(shared_ptr<Packet> packet);

	static void writeBytes(DataOutputStream *dataoutputstream, byteArray bytes);
	static byteArray readBytes(DataInputStream *datainputstream);

	virtual int getId() = 0;

	bool shouldDelay;

private:
	// 4J Added to track stats for packets that are going out via QNet
	static unordered_map<int, PacketStatistics *> outgoingStatistics; // IntHashMap in 1.8.2 ... needed?
	static vector<PacketStatistics *> renderableStats;
	static int renderPos;
public:
	static void recordOutgoingPacket(shared_ptr<Packet> packet);
	static void renderPacketStats(int id);
	static void renderAllPacketStats();
	static void renderAllPacketStatsKey();
	static __int64 getIndexedStatValue(unsigned int samplePos, unsigned int renderableId);

private :
	static unordered_map<int, PacketStatistics *> statistics;
	//static int nextPrint;

public:
	static shared_ptr<Packet> readPacket(DataInputStream *dis, bool isServer);
	static void writePacket(shared_ptr<Packet> packet, DataOutputStream *dos);

	// TCP framing for the Windows64 transport. QNet preserved message
	// boundaries; raw TCP does not. Every packet is prefixed with a small
	// header (magic, version, payload length) so a damaged packet can be
	// rejected without shifting every following packet in the stream.
	static const int TCP_FRAME_MAGIC = 0x4C434546; // "LCEF"
	static const int TCP_FRAME_VERSION = 1;
	static const int TCP_FRAME_MAX_PAYLOAD = 32 * 1024 * 1024;
	static shared_ptr<Packet> readTcpFrame(DataInputStream *dis, bool isServer);
	static void writeTcpFrame(shared_ptr<Packet> packet, DataOutputStream *dos,
		DataOutputStream *payloadDos, ByteArrayOutputStream *payload);

	static void writeUtf(const wstring& value, DataOutputStream *dos);
	static wstring readUtf(DataInputStream *dis, int maxLength);
	virtual void read(DataInputStream *dis) = 0; // throws IOException = 0; TODO 4J JEV, should this declare a throws?
	virtual void write(DataOutputStream *dos) = 0; // throws IOException = 0; TODO 4J JEV, should this declare a throws?
	virtual void handle(PacketListener *listener) = 0;
	virtual int getEstimatedSize() = 0;
	virtual bool canBeInvalidated();
	virtual bool isInvalidatedBy(shared_ptr<Packet> packet);
	virtual bool isAync();

	// 4J Stu - Brought these functions forward for enchanting/game rules
	static shared_ptr<ItemInstance> readItem(DataInputStream *dis);
	static void writeItem(shared_ptr<ItemInstance> item, DataOutputStream *dos);
	static CompoundTag *readNbt(DataInputStream *dis);

protected:
	static void writeNbt(CompoundTag *tag, DataOutputStream *dos);
};