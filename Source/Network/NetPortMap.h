#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RTE {

	/// The port-map wire codecs: NAT-PMP and PCP on the gateway's UDP 5351, and the UPnP IGD's
	/// SOAP bodies plus the SSDP/description/reply parsers. All pure functions - the selftest
	/// drives them without a socket ever existing.
	namespace NetPortMapCodec {
		inline constexpr uint16_t c_NatPmpPort = 5351; // NAT-PMP and PCP share the gateway's 5351.

		// NAT-PMP (RFC 6886): version 0, opcodes 0 (external address), 1 (TCP map), 2 (UDP map).
		std::vector<uint8_t> EncodeNatPmpAddressRequest();
		std::vector<uint8_t> EncodeNatPmpMapRequest(uint16_t internalPort, uint16_t suggestedExternalPort, uint32_t lifetimeS);
		/// True when the reply is a well-formed external-address response. resultCode 0 means the
		/// externalAddr (network order) is valid; anything else is the router's refusal.
		bool DecodeNatPmpAddressResponse(const uint8_t* data, size_t size, uint32_t& externalAddr, uint16_t& resultCode);
		/// True when the reply is a well-formed UDP/TCP map response for `opcode` (2 = UDP).
		bool DecodeNatPmpMapResponse(const uint8_t* data, size_t size, uint8_t opcode, uint16_t& internalPort, uint16_t& externalPort, uint32_t& lifetimeS, uint16_t& resultCode);

		// PCP v2 (RFC 6887): version 2, MAP opcode 1, protocol 17 (UDP). The request's client
		// address is the IPv4-mapped form of the primary local address.
		std::vector<uint8_t> EncodePcpMapRequest(uint32_t localAddr, uint16_t internalPort, uint16_t suggestedExternalPort, uint32_t lifetimeS, const uint8_t nonce[12]);
		/// True when the reply is a well-formed MAP response echoing `nonce`. externalAddr is the
		/// assigned external address (network order), valid only when resultCode is 0.
		bool DecodePcpMapResponse(const uint8_t* data, size_t size, const uint8_t nonce[12], uint16_t& internalPort, uint16_t& externalPort, uint32_t& externalAddr, uint32_t& lifetimeS, uint8_t& resultCode);

		// UPnP IGD (WANIPConnection:1 / WANPPPConnection:1). The service type fills both the SOAP
		// element's namespace and the SOAPAction header.
		std::string BuildIgdSoapAddPortMapping(const std::string& serviceType, uint16_t externalPort, uint16_t internalPort, const std::string& internalClient, uint32_t leaseS);
		std::string BuildIgdSoapDeletePortMapping(const std::string& serviceType, uint16_t externalPort);
		std::string BuildIgdSoapGetExternalIp(const std::string& serviceType);
		/// Splits a SOAP reply into "the action was accepted" vs a Fault. Returns true on a 2xx-shaped
		/// success body; on a Fault, false with faultCode (718 = ConflictInMappingEntry) and detail.
		bool ParseIgdSoapReply(const std::string& body, const char* acceptedElement, long& faultCode, std::string& detail);
		bool ParseIgdExternalIp(const std::string& body, std::string& externalIp);
		/// The LOCATION header value of one SSDP 200-OK reply; "" when the reply carries none.
		std::string ParseSsdpLocation(const std::string& reply);
		/// The serviceType and controlURL of the first WANIPConnection (preferred) or
		/// WANPPPConnection service in a description document; false when neither is present.
		bool ParseIgdService(const std::string& descriptionXml, std::string& serviceType, std::string& controlUrl);
		/// Resolves a possibly-relative control URL against the description's URL.
		std::string ResolveIgdUrl(const std::string& baseUrl, const std::string& controlUrl);
		/// True when two dotted IPv4 addresses share a /24 prefix (the IGD must be on the host's subnet).
		bool SameSlash24(const std::string& addrA, const std::string& addrB);
	} // namespace NetPortMapCodec

	/// Everything the mapping chain needs from the network, behind one seam: the engine build
	/// implements it over real sockets on the worker thread; the selftest scripts canned replies
	/// and records the call order to prove the NAT-PMP -> PCP -> UPnP fallback.
	class NetPortMapWan {
	public:
		virtual ~NetPortMapWan() = default;
		/// The default route's next hop; "" when no gateway exists. Never a hard-coded address.
		virtual std::string DefaultGateway() = 0;
		/// The primary local IPv4 (the /24 gate and NewInternalClient); "" when undiscoverable.
		virtual std::string LocalAddress() = 0;
		/// One UDP request/response pair to host:port. False on timeout or socket error.
		virtual bool UdpExchange(const std::string& host, uint16_t port, const std::vector<uint8_t>& request, std::vector<uint8_t>& reply, uint32_t timeoutMs) = 0;
		/// One M-SEARCH for `st` on 239.255.255.250:1900; returns every 200-OK body inside windowMs.
		virtual std::vector<std::string> SsdpDiscover(const std::string& st, uint32_t windowMs) = 0;
		/// Plain-HTTP GET (the description document); false on transport failure.
		virtual bool HttpGet(const std::string& url, std::string& body, uint32_t timeoutMs) = 0;
		/// Plain-HTTP SOAP POST; status is the HTTP status code (0 when no reply arrived).
		virtual bool HttpPostSoap(const std::string& url, const std::string& soapAction, const std::string& body, long& status, std::string& replyBody, uint32_t timeoutMs) = 0;
	};

	/// The host's router port-mapping client. Game-thread only, like NetDirectoryClient: Request()
	/// hands the work to a worker thread that owns every socket, Update() collects the finished
	/// result and drives lease renewal, Release() deletes the mapping with a bounded wait so the
	/// removal reply is logged. NAT-PMP is tried first, then PCP on the same gateway port, then
	/// UPnP IGD discovery.
	class NetPortMap {
	public:
		enum class Method : uint8_t { None, NatPmp, Pcp, Upnp };
		static const char* MethodName(Method method);

		struct Result {
			Method method = Method::None;
			std::string externalIp;
			uint16_t internalPort = 0;
			uint16_t externalPort = 0;
			uint32_t leaseS = 0;
			uint64_t leaseExpiresMs = 0; //!< Steady clock; Update() renews as it approaches.
			std::string error;         //!< Why the whole chain gave up; empty on success.
			// What a later Release() needs to undo this exact mapping.
			std::string gateway;
			uint16_t gatewayPort = 0;
			std::string controlUrl;
			std::string serviceType;
		};

		struct Options {
			std::string gateway;      //!< Test seam "a.b.c.d[:port]"; empty reads the routing table.
			std::string igdLocation;  //!< Test seam: the description URL, skipping SSDP and the /24 gate.
			std::string localAddress; //!< Test seam for LocalAddress().
			NetPortMapWan* wan = nullptr; //!< Test seam: a scripted Wan instead of real sockets.
		};

		/// The -net-port-map-gateway/-net-port-map-igd test seams, stored process-wide so both the
		/// probe and NetMatchService::Start see them. Empty strings disable each override.
		static void SetProbeOverrides(const std::string& gateway, const std::string& igdLocation);
		static Options ProbeOverrides();

		static constexpr uint32_t c_DefaultLeaseS = 600;
		static constexpr uint64_t c_MapBudgetMs = 10000; //!< How long a Request may take before the caller stops waiting on it.
		static constexpr uint64_t c_ReleaseBudgetMs = 3000;

		NetPortMap() = default;
		NetPortMap(const NetPortMap&) = delete;
		NetPortMap& operator=(const NetPortMap&) = delete;
		~NetPortMap() { Release(); }

		void Request(uint16_t internalUdpPort, uint32_t leaseSeconds, const Options& options = Options());
		/// Collects a finished worker, renews a mapped lease at its half-life. Never blocks a frame.
		void Update(uint64_t nowMs);
		/// Stops the worker, then deletes the mapping (lifetime-0 for NAT-PMP/PCP, DeletePortMapping
		/// for UPnP) and logs the reply, all inside the release budget. Safe when never requested.
		void Release();

		bool Done() const { return m_Done; }        //!< The current request produced a result.
		bool Mapped() const { return m_Mapped; }    //!< A mapping is (believed) held right now.
		const Result& GetResult() const { return m_Result; }

		/// The whole chain, blocking; the worker thread calls it with real sockets, the selftest
		/// calls it with a scripted Wan. `stop` (may be null) abandons between steps.
		static Result RunMappingChain(NetPortMapWan& wan, uint16_t internalPort, uint32_t leaseS, const Options& options, const std::atomic<bool>* stop);
		/// Deletes a mapping the same way it was made. Returns the reply detail for the log.
		static std::string RunReleaseChain(NetPortMapWan& wan, const Result& mapped, const Options& options, const std::atomic<bool>* stop);

	private:
		void JoinWorker();
		void StartWorker(bool renewal);

		std::thread m_Worker;
		std::atomic<bool> m_Cancel{false};
		std::mutex m_Mutex;
		std::atomic<bool> m_ResultReady{false};
		Result m_PendingResult;     //!< Written by the worker under m_Mutex, read by Update().
		Result m_Result;          //!< The game thread's view: last landed result.
		Options m_Options;
		uint16_t m_Port = 0;
		uint32_t m_LeaseS = 0;
		uint64_t m_RenewAtMs = UINT64_MAX;
		bool m_Done = false;
		bool m_Mapped = false;
		bool m_Released = false; //!< The delete for m_Result already ran; Release() stays idempotent.
	};

	namespace NetPortMapSelfTest { int Run(); }

} // namespace RTE
