#include <async/basic.hpp>
#include <async/doorbell.hpp>
#include <async/result.hpp>
#include <arch/bit.hpp>
#include <arch/variable.hpp>
#include <protocols/fs/server.hpp>
#include <cstring>
#include <deque>
#include <iomanip>
#include <random>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#include "checksum.hpp"
#include "ip4.hpp"
#include "tcp4.hpp"

namespace {

constexpr bool debugTcp = false;

struct stl_allocator {
	void *allocate(size_t size) {
		return operator new(size);
	}

	void deallocate(void *p, size_t) {
		return operator delete(p);
	}
};

struct PseudoHeader {
	arch::scalar_storage<uint32_t, arch::big_endian> src;
	arch::scalar_storage<uint32_t, arch::big_endian> dst;
	uint8_t zero = 0;
	uint8_t proto = static_cast<uint8_t>(IpProto::tcp);
	arch::scalar_storage<uint16_t, arch::big_endian> len;
};

static_assert(sizeof(PseudoHeader) == 12);

struct RingBuffer {
	RingBuffer(int shift)
	: storage_{reinterpret_cast<char *>(operator new (1 << shift))}, shift_{shift} { }

	RingBuffer(const RingBuffer &) = delete;

	~RingBuffer() {
		operator delete(storage_);
	}

	RingBuffer &operator= (const RingBuffer &) = delete;

	size_t spaceForEnqueue() {
		return (size_t{1} << shift_) - (enqPtr_ - deqPtr_);
	}

	size_t availableToDequeue() {
		return enqPtr_ - deqPtr_;
	}

	void enqueue(void *data, size_t size) {
		assert(size <= spaceForEnqueue());
		size_t ringSize = size_t{1} << shift_;
		auto wrappedPtr = enqPtr_ & (ringSize - 1);
		auto p = reinterpret_cast<char *>(data);
		size_t bytesUntilEnd = std::min(size, ringSize - wrappedPtr);
		memcpy(storage_ + wrappedPtr, p, bytesUntilEnd);
		memcpy(storage_, p + bytesUntilEnd, size - bytesUntilEnd);
		enqPtr_ += size;
	}

	void dequeue(void *data, size_t size) {
		dequeueLookahead(0, data, size);
		dequeueAdvance(size);
	}

	void dequeueLookahead(size_t offset, void *data, size_t size) {
		assert(offset + size <= availableToDequeue());
		size_t ringSize = size_t{1} << shift_;
		auto wrappedPtr = deqPtr_ & (ringSize - 1);
		auto p = reinterpret_cast<char *>(data);
		size_t bytesUntilEnd = std::min(size, ringSize - wrappedPtr);
		memcpy(p, storage_ + wrappedPtr, bytesUntilEnd);
		memcpy(p + bytesUntilEnd, storage_, size - bytesUntilEnd);
	}

	void dequeueAdvance(size_t size) {
		deqPtr_ += size;
	}

private:
	char *storage_;
	int shift_;
	uint64_t enqPtr_ = 0;
	uint64_t deqPtr_ = 0;
};

// TODO: Use a CSPRNG, see also UDP.
static std::mt19937 globalPrng;

} // namespace

struct TcpHeader {
	static constexpr arch::field<uint16_t, bool> synFlag{1, 1};
	static constexpr arch::field<uint16_t, bool> ackFlag{4, 1};
	static constexpr arch::field<uint16_t, unsigned int> headerWords{12, 4};

	arch::scalar_storage<uint16_t, arch::big_endian> srcPort;
	arch::scalar_storage<uint16_t, arch::big_endian> destPort;
	arch::scalar_storage<uint32_t, arch::big_endian> seqNumber;
	arch::scalar_storage<uint32_t, arch::big_endian> ackNumber;
	arch::bit_storage<uint16_t, arch::big_endian> flags;
	arch::scalar_storage<uint16_t, arch::big_endian> window;
	arch::scalar_storage<uint16_t, arch::big_endian> checksum;
	arch::scalar_storage<uint16_t, arch::big_endian> urgentPointer;
};

static_assert(sizeof(TcpHeader) == 20);

struct TcpPacket {
	arch::dma_buffer_view payload() {
		auto words = header.flags.load() & TcpHeader::headerWords;
		return packet->payload().subview(words * 4);
	}

	bool parse(smarter::shared_ptr<const Ip4Packet> packet) {
		auto ipPayload = packet->payload();
		if (ipPayload.size() < sizeof(TcpHeader))
			return false;

		std::memcpy(&header, ipPayload.data(), sizeof(TcpHeader));
		auto words = header.flags.load() & TcpHeader::headerWords;
		if (words * 4 < sizeof(TcpHeader))
			return false;
		if (ipPayload.size() < words * 4)
			return false;

		if (header.checksum.load()) {
			PseudoHeader pseudo {
				.src = packet->header.source,
				.dst = packet->header.destination,
				.proto = packet->header.protocol,
				.len = ipPayload.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(pseudo));
			csum.update(ipPayload);
			auto result = csum.finalize();
			if (result && ~result)
				return false;
		}

		this->packet = std::move(packet);
		return true;
	}

	TcpHeader header;
	smarter::shared_ptr<const Ip4Packet> packet;
};

namespace {

protocols::fs::Error checkAddress(const void *addrPtr, size_t addrLength, TcpEndpoint &e) {
	struct sockaddr_in sa;
	if (addrLength < sizeof(sa))
		return protocols::fs::Error::illegalArguments;

	std::memcpy(&sa, addrPtr, sizeof(sa));
	if (sa.sin_family != AF_INET)
		return protocols::fs::Error::afNotSupported;

	e.port = arch::from_endian<arch::big_endian, uint16_t>(sa.sin_port);
	e.ipAddress = arch::from_endian<arch::big_endian, uint32_t>(sa.sin_addr.s_addr);
	return protocols::fs::Error::none;
}

} // anonymous namespace

struct Tcp4Socket {
	Tcp4Socket(Tcp4 *parent)
	: parent_(parent), recvRing_{4}, sendRing_{4} {}

	~Tcp4Socket() {
		parent_->unbind(localEp_);
	}

	static auto makeSocket(Tcp4 *parent) {
		auto s = smarter::make_shared<Tcp4Socket>(parent);
		s->holder_ = s;
		async::detach(s->flushOutPackets_());
		return s;
	}

	static async::result<protocols::fs::Error> bind(void *object,
			const char *creds,
			const void *addrPtr, size_t addrLength) {
		auto self = static_cast<Tcp4Socket *>(object);

		if (self->localEp_.port)
			co_return protocols::fs::Error::illegalArguments;

		// Validate the endpoint.
		TcpEndpoint bindEp;
		if (auto e = checkAddress(addrPtr, addrLength, bindEp); e != protocols::fs::Error::none)
			co_return e;

		if (bindEp.ipAddress == INADDR_BROADCAST) {
			std::cout << "netserver: TCP cannot broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		if (!ip4().hasIp(bindEp.ipAddress))
			co_return protocols::fs::Error::addressNotAvailable;

		// Bind the socket.
		if (!bindEp.port) {
			if (!self->bindAvailable(bindEp.ipAddress)) {
				std::cout << "netserver: No source port" << std::endl;
				co_return protocols::fs::Error::addressInUse;
			}
		} else if (!self->parent_->tryBind(self->holder_.lock(), bindEp)) {
			co_return protocols::fs::Error::addressInUse;
		}

		co_return protocols::fs::Error::none;
	}

	static async::result<protocols::fs::Error> connect(void *object,
			const char *creds,
			const void *addrPtr, size_t addrLength) {
		auto self = static_cast<Tcp4Socket *>(object);

		if (self->connectState_ != ConnectState::none)
			co_return protocols::fs::Error::illegalArguments;

		// Validate the endpoint.
		TcpEndpoint connectEp;
		if (auto e = checkAddress(addrPtr, addrLength, connectEp); e != protocols::fs::Error::none)
			co_return e;

		if (connectEp.ipAddress == INADDR_BROADCAST) {
			std::cout << "netserver: TCP cannot broadcast" << std::endl;
			co_return protocols::fs::Error::accessDenied;
		}

		// Bind the socket if necessary.
		if (!self->localEp_.port && !self->bindAvailable()) {
			std::cout << "netserver: No source port" << std::endl;
			co_return protocols::fs::Error::addressNotAvailable;
		}

		// Connect to the remote.
		self->connectState_ = ConnectState::sendSyn;
		self->remoteEp_ = connectEp;
		self->flushEvent_.ring();

		while(true) {
			if(self->connectState_ != ConnectState::sendSyn)
				break;
			co_await self->settleEvent_.async_wait();
		}
		co_return protocols::fs::Error::none;
	}

	static async::result<protocols::fs::RecvResult> recvMsg(void *object,
			const char *creds, uint32_t flags,
			void *data, size_t size,
			void *addrPtr, size_t addrLength, size_t max_ctrl_len) {
		auto self = static_cast<Tcp4Socket *>(object);
		auto p = reinterpret_cast<char *>(data);

		size_t progress = 0;
		while(progress < size) {
			size_t available = self->recvRing_.availableToDequeue();
			if(!available) {
				co_await self->inEvent_.async_wait();
				continue;
			}
			size_t chunk = std::min(available, size - progress);
			self->recvRing_.dequeue(p + progress, chunk);
			self->flushEvent_.ring();
			progress += chunk;
		}

		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(struct sockaddr_in));
		sa.sin_port = arch::to_endian<arch::big_endian, uint16_t>(self->remoteEp_.port);
		sa.sin_addr.s_addr = arch::to_endian<arch::big_endian, uint32_t>(self->remoteEp_.ipAddress);
		memcpy(addrPtr, &sa, std::min(sizeof(struct sockaddr_in), addrLength));

		co_return protocols::fs::RecvData{size, sizeof(struct sockaddr_in), {}};
	}

	static async::result<protocols::fs::SendResult> sendMsg(void *object,
			const char *creds, uint32_t flags,
			void *data, size_t size,
			void *addrPtr, size_t addrSize,
			std::vector<uint32_t> fds) {
		auto self = static_cast<Tcp4Socket *>(object);
		auto p = reinterpret_cast<char *>(data);

		size_t progress = 0;
		while(progress < size) {
			size_t space = self->sendRing_.spaceForEnqueue();
			if(!space) {
				co_await self->settleEvent_.async_wait();
				continue;
			}
			size_t chunk = std::min(space, size - progress);
			self->sendRing_.enqueue(p + progress, chunk);
			self->flushEvent_.ring();
			progress += chunk;
		}

		co_return size;
	}

	constexpr static protocols::fs::FileOperations ops {
		.bind = &bind,
		.connect = &connect,
		.recvMsg = &recvMsg,
		.sendMsg = &sendMsg,
	};

	bool bindAvailable(uint32_t ipAddress = INADDR_ANY) {
		static std::uniform_int_distribution<uint16_t> dist {
			32768, 60999
		};
		auto number = dist(globalPrng);
		auto range = dist.b() - dist.a();
		auto self = holder_.lock();
		for (size_t i = 0; i < range; i++) {
			uint16_t port = dist.a() + ((number + i) % range);
			if (parent_->tryBind(self, { ipAddress, port }))
				return true;
		}
		return false;
	}

private:
	async::result<void> flushOutPackets_();

	void handleInPacket_(TcpPacket packet);

private:
	friend struct Tcp4;

	enum class ConnectState {
		none,
		sendSyn, // Client-side only.
		sendSynAck, // Server-side only.
		connected,
	};

	Tcp4 *parent_;
	TcpEndpoint remoteEp_;
	TcpEndpoint localEp_;
	smarter::weak_ptr<Tcp4Socket> holder_;

	ConnectState connectState_ = ConnectState::none;

	// Out-SN corresponding to the front of sendRing_.
	uint32_t localSettledSn_ = 0;
	// Out-SN that has already been flushed to the IP layer (>= localSettledSn_).
	uint32_t localFlushedSn_ = 0;
	// Out-SN of the end of the remote window (>= localSettledSn_).
	uint32_t localWindowSn_ = 0;
	// In-SN that we already acknowledged.
	uint32_t remoteAckedSn_ = 0;
	// In-SN that we already received (>= remoteAckedSn_).
	uint32_t remoteKnownSn_ = 0;
	// Size of received window that we announced to the remote side.
	uint32_t announcedWindow_ = 0;

	RingBuffer recvRing_;
	RingBuffer sendRing_;

	async::doorbell inEvent_;
	async::doorbell flushEvent_;
	async::doorbell settleEvent_;
};

async::result<void> Tcp4Socket::flushOutPackets_() {
	while(true) {
		if(connectState_ == ConnectState::none) {
			co_await flushEvent_.async_wait();
			continue;
		}

		if(connectState_ == ConnectState::sendSyn) {
			if(localSettledSn_ != localFlushedSn_) {
				co_await flushEvent_.async_wait();
				continue;
			}

			// Obtain a new random sequence number.
			auto randomSn = globalPrng();
			localSettledSn_ = randomSn;
			localFlushedSn_ = randomSn;

			// Construct and transmit the initial SYN packet.
			auto targetInfo = co_await ip4().targetByRemote(remoteEp_.ipAddress);
			if (!targetInfo) {
				// TODO: Return an error to users.
				std::cout << "netserver: Destination unreachable" << std::endl;
				co_return;
			}

			std::vector<char> buf;
			buf.resize(sizeof(TcpHeader));

			auto header = new (buf.data()) TcpHeader {
				.srcPort = localEp_.port,
				.destPort = remoteEp_.port,
				.seqNumber = localFlushedSn_,
				.ackNumber = 0,
				.window = 0,
				.checksum = 0,
				.urgentPointer = 0
			};
			header->flags.store(TcpHeader::headerWords(sizeof(TcpHeader) / 4)
					| TcpHeader::synFlag(true));

			// Fill in the checksum.
			PseudoHeader pseudo {
				.src = targetInfo->source,
				.dst = remoteEp_.ipAddress,
				.len = buf.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(PseudoHeader));
			csum.update(buf.data(), buf.size());
			header->checksum = csum.finalize();

			++localFlushedSn_;

			if(debugTcp)
				std::cout << "netserver: Sending TCP SYN" << std::endl;
			auto error = co_await ip4().sendFrame(std::move(*targetInfo),
				buf.data(), buf.size(), static_cast<uint16_t>(IpProto::tcp));
			if (error != protocols::fs::Error::none) {
				// TODO: Return an error to users.
				std::cout << "netserver: Could not send TCP packet" << std::endl;
				co_return;
			}
		}else{
			assert(connectState_ == ConnectState::connected);
			size_t flushPointer = localFlushedSn_ - localSettledSn_;
			size_t windowPointer = localWindowSn_ - localSettledSn_;

			size_t bytesAvailable = sendRing_.availableToDequeue();
			assert(bytesAvailable >= flushPointer);

			// Check whether we need to send a packet.
			// TODO: Add retransmission here.
			bool wantData = (bytesAvailable > flushPointer && windowPointer > flushPointer);
			bool wantAck = (remoteAckedSn_ != remoteKnownSn_);
			bool wantWindowUpdate = (announcedWindow_ < recvRing_.spaceForEnqueue());

			if(!wantData && !wantAck && !wantWindowUpdate) {
				co_await flushEvent_.async_wait();
				continue;
			}

			// Construct and transmit the TCP packet.
			auto targetInfo = co_await ip4().targetByRemote(remoteEp_.ipAddress);
			if (!targetInfo) {
				// TODO: Return an error to users.
				std::cout << "netserver: Destination unreachable" << std::endl;
				co_return;
			}

			auto chunk = std::min({
				bytesAvailable - flushPointer,
				windowPointer - flushPointer,
				size_t{1000} // TODO: Perform path MTU discovery.
			});

			std::vector<char> buf;
			buf.resize(sizeof(TcpHeader) + chunk);

			auto header = new (buf.data()) TcpHeader {
				.srcPort = localEp_.port,
				.destPort = remoteEp_.port,
				.seqNumber = localFlushedSn_,
				.ackNumber = remoteKnownSn_,
				.window = recvRing_.spaceForEnqueue(),
				.checksum = 0,
				.urgentPointer = 0
			};
			header->flags.store(TcpHeader::headerWords(sizeof(TcpHeader) / 4)
					| TcpHeader::ackFlag(true));

			sendRing_.dequeueLookahead(flushPointer, buf.data() + sizeof(TcpHeader), chunk);

			// Fill in the checksum.
			PseudoHeader pseudo {
				.src = targetInfo->source,
				.dst = remoteEp_.ipAddress,
				.len = buf.size()
			};
			Checksum csum;
			csum.update(&pseudo, sizeof(PseudoHeader));
			csum.update(buf.data(), buf.size());
			header->checksum = csum.finalize();

			localFlushedSn_ += chunk;
			remoteAckedSn_ = remoteKnownSn_;
			announcedWindow_ = recvRing_.spaceForEnqueue();

			if(debugTcp)
				std::cout << "netserver: Sending TCP data (" << chunk << " bytes)" << std::endl;
			auto error = co_await ip4().sendFrame(std::move(*targetInfo),
				buf.data(), buf.size(),
				static_cast<uint16_t>(IpProto::tcp));
			if (error != protocols::fs::Error::none) {
				// TODO: Return an error to users.
				std::cout << "netserver: Could not send TCP packet" << std::endl;
				co_return;
			}
		}
	}
}

void Tcp4Socket::handleInPacket_(TcpPacket packet) {
	if(connectState_ == ConnectState::sendSyn) {
		if(localSettledSn_ == localFlushedSn_) {
			std::cout << "netserver: Rejecting packet before SYN is sent [sendSyn]"
					<< std::endl;
			return;
		}

		if(!(packet.header.flags.load() & TcpHeader::synFlag)) {
			std::cout << "netserver: Rejecting packet without SYN [sendSyn]"
					<< std::endl;
			return;
		}else if(!(packet.header.flags.load() & TcpHeader::ackFlag)) {
			std::cout << "netserver: Rejecting SYN packet without ACK [sendSyn]"
					<< std::endl;
			return;
		}

		if(packet.header.ackNumber.load() != localSettledSn_ + 1) {
			std::cout << "netserver: Rejecting packet with bad ack-number [sendSyn]"
					<< std::endl;
			return;
		}

		++localSettledSn_;
		localWindowSn_ = localSettledSn_ + packet.header.window.load();
		remoteAckedSn_ = packet.header.seqNumber.load();
		remoteKnownSn_ = packet.header.seqNumber.load() + 1;
		connectState_ = ConnectState::connected;
		flushEvent_.ring();
		settleEvent_.ring();
	}else if(connectState_ == ConnectState::connected) {
		if(packet.header.seqNumber.load() == remoteKnownSn_) {
			auto payload = packet.payload();
			size_t chunk = std::min(payload.size(), recvRing_.spaceForEnqueue());
			if(chunk) {
				recvRing_.enqueue(payload.data(), chunk);
				remoteKnownSn_ += chunk;
				if(announcedWindow_ < chunk) {
					announcedWindow_ = 0;
				}else{
					announcedWindow_ -= chunk;
				}
				inEvent_.ring();
				flushEvent_.ring();
			}
		}

		if(packet.header.flags.load() & TcpHeader::ackFlag) {
			size_t validWindow = localFlushedSn_ - localSettledSn_;
			size_t ackPointer = packet.header.ackNumber.load() - localSettledSn_;
			if(ackPointer <= validWindow) {
				localSettledSn_ += ackPointer;
				localWindowSn_ = localSettledSn_ + packet.header.window.load();
				sendRing_.dequeueAdvance(ackPointer);
				settleEvent_.ring();
			}else{
				std::cout << "netserver: Rejecting ack-number outside of valid window"
						<< std::endl;
			}
		}
	}
}

void Tcp4::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet) {
	TcpPacket tcp;
	if (!tcp.parse(std::move(packet))) {
		std::cout << "netserver: Received broken TCP packet" << std::endl;
		return;
	}

	if(debugTcp)
		std::cout << "netserver: Received TCP packet at port " << tcp.header.destPort.load()
				<< " (" << tcp.payload().size() << " bytes)" << std::endl;

	auto it = binds.lower_bound({ 0, packet });
	for (; it != binds.end() && it->first.port == tcp.header.destPort.load(); it++) {
		auto existingEp = it->first;
		if (existingEp.ipAddress == tcp.packet->header.destination
				|| existingEp.ipAddress == INADDR_ANY) {
			it->second->handleInPacket_(std::move(tcp));
			break;
		}
	}
}

bool Tcp4::tryBind(smarter::shared_ptr<Tcp4Socket> socket, TcpEndpoint wantedEp) {
	auto it = binds.lower_bound(wantedEp);
	for (; it != binds.end() && it->first.port == wantedEp.port; it++) {
		auto existingEp = it->first;
		if (existingEp.ipAddress == INADDR_ANY || wantedEp.ipAddress == INADDR_ANY
				|| existingEp.ipAddress == wantedEp.ipAddress) {
			return false;
		}
	}
	socket->localEp_ = wantedEp;
	binds.emplace(wantedEp, std::move(socket));
	return true;
}

bool Tcp4::unbind(TcpEndpoint e) {
	return binds.erase(e) != 0;
}

void Tcp4::serveSocket(helix::UniqueLane lane) {
	using protocols::fs::servePassthrough;
	auto sock = Tcp4Socket::makeSocket(this);
	async::detach(servePassthrough(std::move(lane), std::move(sock),
			&Tcp4Socket::ops));
}
