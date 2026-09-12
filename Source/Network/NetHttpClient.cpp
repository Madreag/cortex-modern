#include "NetHttpClient.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <iostream>

namespace RTE {

	NetHttpClient::~NetHttpClient() {
		Cancel();
	}

	void NetHttpClient::Start(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, const std::string& certPinSha256) {
		if (m_Started.exchange(true)) {
			Finish(Response{0, "", "client already used"});
			return;
		}
#ifdef _WIN32
		m_Done = false;
		m_CancelRequested = false;
		m_Worker = std::thread(&NetHttpClient::WorkerMain, this, method, url, headers, body, certPinSha256);
#else
		(void)method; (void)url; (void)headers; (void)body; (void)certPinSha256;
		Finish(Response{0, "", "http client not available on this platform"});
#endif
	}

	NetHttpClient::PollResult NetHttpClient::Poll() {
		return m_Done.load() ? PollResult::Done : PollResult::Pending;
	}

	NetHttpClient::Response NetHttpClient::GetResponse() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Response;
	}

	void NetHttpClient::Finish(const Response& response) {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Response = response;
		}
		m_Done = true;
	}

#ifdef _WIN32

	namespace {
		struct WinHttpHandle {
			HINTERNET handle = nullptr;
			~WinHttpHandle() { if (handle) WinHttpCloseHandle(handle); }
			operator HINTERNET() const { return handle; }
		};

		// NONLS is defined project-wide, so MultiByteToWideChar is unavailable; decode UTF-8 here.
		std::wstring ToWide(const std::string& text) {
			std::wstring out;
			out.reserve(text.size());
			size_t i = 0;
			while (i < text.size()) {
				const unsigned char lead = static_cast<unsigned char>(text[i]);
				uint32_t cp = 0;
				size_t extra = 0;
				if (lead < 0x80) { cp = lead; }
				else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F; extra = 1; }
				else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F; extra = 2; }
				else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07; extra = 3; }
				else { out.push_back(L'?'); ++i; continue; }
				if (i + extra >= text.size()) { out.push_back(L'?'); break; }
				bool valid = true;
				for (size_t k = 1; k <= extra; ++k) {
					const unsigned char cont = static_cast<unsigned char>(text[i + k]);
					if ((cont & 0xC0) != 0x80) { valid = false; break; }
					cp = (cp << 6) | (cont & 0x3F);
				}
				if (!valid) { out.push_back(L'?'); ++i; continue; }
				i += extra + 1;
				if (cp <= 0xFFFF) {
					out.push_back(static_cast<wchar_t>(cp));
				} else {
					cp -= 0x10000;
					out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
					out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
				}
			}
			return out;
		}

		std::string HexEncode(const uint8_t* data, size_t size) {
			static const char hex[] = "0123456789abcdef";
			std::string out(size * 2, '0');
			for (size_t i = 0; i < size; ++i) {
				out[i * 2] = hex[(data[i] >> 4) & 0x0F];
				out[i * 2 + 1] = hex[data[i] & 0x0F];
			}
			return out;
		}

		std::string Sha256Hex(const uint8_t* data, size_t size) {
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::array<uint8_t, 32> digest{};
			const bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
				BCryptHashData(hash, const_cast<uint8_t*>(data), static_cast<ULONG>(size), 0) == 0 &&
				BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
			if (hash) BCryptDestroyHash(hash);
			BCryptCloseAlgorithmProvider(algorithm, 0);
			return ok ? HexEncode(digest.data(), digest.size()) : std::string();
		}

		bool IsHexPin(const std::string& pin) {
			return pin.size() == 64 && std::all_of(pin.begin(), pin.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
		}

		std::string WinHttpError(const char* step, DWORD code) {
			if (code == ERROR_WINHTTP_SECURE_FAILURE) return std::string(step) + ": certificate verification failed";
			if (code == ERROR_WINHTTP_TIMEOUT) return std::string(step) + ": timed out";
			if (code == ERROR_WINHTTP_OPERATION_CANCELLED) return "cancelled";
			if (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return std::string(step) + ": cannot connect";
			return std::string(step) + ": winhttp error " + std::to_string(code);
		}

		constexpr size_t c_MaxResponseBytes = 4 * 1024 * 1024;

		const char* AsyncStepName(DWORD api) {
			switch (api) {
				case API_SEND_REQUEST: return "send";
				case API_RECEIVE_RESPONSE: return "receive";
				case API_QUERY_DATA_AVAILABLE: return "query data";
				case API_READ_DATA: return "read";
				case API_WRITE_DATA: return "write";
				case API_GET_PROXY_FOR_URL: return "proxy detection";
				default: return "request";
			}
		}

		/// All mutable state shared between the worker thread, Cancel(), and the WinHTTP status
		/// callbacks. Owned by the worker; freed only after the request handle's HANDLE_CLOSING
		/// (guaranteed to be a handle's last callback) so no callback outlives the state.
		struct AsyncRequest {
			std::atomic<bool>* cancelRequested = nullptr;   //!< Points at NetHttpClient::m_CancelRequested.
			std::recursive_mutex handleMutex;   //!< Recursive: WinHTTP may invoke completions inline on the calling thread.
			std::mutex dataMutex;           //!< Guards statusCode/body/error below.
			HINTERNET request = nullptr;
			HANDLE doneEvent = nullptr;          //!< Signaled when the request reaches a terminal state.
			HANDLE requestClosedEvent = nullptr; //!< Signaled by the request's HANDLE_CLOSING callback.
			std::atomic<bool> requestClosed{false};
			std::atomic<bool> callbackArmed{false};
			std::string certPin;
			std::wstring headerBlock;
			std::string requestBody;   //!< Must stay valid until the request handle is closed (lpOptional rule).
			std::string readBuf;       //!< Scratch for the in-flight WinHttpReadData.
			long statusCode = 0;
			std::string body;
			std::string error;
		};

		// Claims and closes the request handle; the documented way to cancel an in-progress
		// asynchronous request. Every close goes through here so the handle is closed once.
		void CloseRequestHandle(AsyncRequest* st) {
			HINTERNET handle = nullptr;
			{
				std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
				if (st->request != nullptr) {
					handle = st->request;
					st->request = nullptr;
					st->requestClosed = true;
				}
			}
			if (handle != nullptr) {
				WinHttpCloseHandle(handle);
			}
		}

		void SignalDone(AsyncRequest* st) {
			if (st->doneEvent != nullptr) SetEvent(st->doneEvent);
		}

		void FailWith(AsyncRequest* st, const char* step, DWORD code) {
			{
				std::lock_guard<std::mutex> lock(st->dataMutex);
				if (st->error.empty()) st->error = WinHttpError(step, code);
			}
			SignalDone(st);
		}

		// 1 = DER hash matches the pin, 0 = context unavailable, -1 = mismatch.
		int VerifyPinnedCert(AsyncRequest* st, HINTERNET request) {
			PCCERT_CONTEXT certificate = nullptr;
			DWORD certSize = sizeof(certificate);
			if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &certSize)) {
				return 0;
			}
			const std::string actual = Sha256Hex(certificate->pbCertEncoded, certificate->cbCertEncoded);
			CertFreeCertificateContext(certificate);
			if (actual.empty() || actual != st->certPin) {
				return -1;
			}
			return 1;
		}

		void AbortRequest(AsyncRequest* st, const char* error) {
			{
				std::lock_guard<std::mutex> lock(st->dataMutex);
				if (st->error.empty()) st->error = error;
			}
			CloseRequestHandle(st);
			SignalDone(st);
		}

		// Every WinHTTP call on the request handle runs under st->handleMutex so the handle is
		// never closed mid-call; CloseRequestHandle acquires the same mutex before closing.
		void HttpStatusCallbackImpl(HINTERNET request, AsyncRequest* st, DWORD status, LPVOID info, DWORD infoLength) {
			switch (status) {
				case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST: {
					// Delivered after the TLS handshake and before the request is written: the one
					// point where the certificate can be checked with no request bytes on the wire.
					int pin = 1;
					bool live = false;
					{
						std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
						live = st->request != nullptr && !st->cancelRequested->load();
						if (live && !st->certPin.empty()) pin = VerifyPinnedCert(st, st->request);
					}
					if (!live) {
						CloseRequestHandle(st);
						SignalDone(st);
					} else if (pin != 1) {
						AbortRequest(st, pin < 0 ? "certificate pin mismatch" : "could not read server certificate");
					}
					return;
				}
				case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: {
					int pin = 1;
					bool live = false;
					const char* failStep = nullptr;
					DWORD failCode = 0;
					{
						std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
						live = st->request != nullptr && !st->cancelRequested->load();
						if (live && !st->certPin.empty()) pin = VerifyPinnedCert(st, st->request);
						if (live && pin == 1 && !WinHttpReceiveResponse(st->request, nullptr)) {
							failCode = GetLastError();
							if (failCode != ERROR_IO_PENDING) failStep = "receive";
						}
					}
					if (!live) return;
					if (pin != 1) {
						AbortRequest(st, pin < 0 ? "certificate pin mismatch" : "could not read server certificate");
					} else if (failStep != nullptr) {
						FailWith(st, failStep, failCode);
					}
					return;
				}
				case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
					DWORD code = 0;
					DWORD codeSize = sizeof(code);
					bool live = false;
					bool gotCode = false;
					const char* failStep = nullptr;
					DWORD failCode = 0;
					{
						std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
						live = st->request != nullptr && !st->cancelRequested->load();
						if (live) {
							gotCode = WinHttpQueryHeaders(st->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX) != FALSE;
							if (!WinHttpQueryDataAvailable(st->request, nullptr)) {
								failCode = GetLastError();
								if (failCode != ERROR_IO_PENDING) failStep = "query data";
							}
						}
					}
					if (!live) return;
					if (gotCode) {
						std::lock_guard<std::mutex> dataLock(st->dataMutex);
						st->statusCode = static_cast<long>(code);
					}
					if (failStep != nullptr) FailWith(st, failStep, failCode);
					return;
				}
				case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
					const DWORD available = *static_cast<const DWORD*>(info);
					if (available == 0) {
						SignalDone(st);
						return;
					}
					bool live = false;
					bool tooLarge = false;
					const char* failStep = nullptr;
					DWORD failCode = 0;
					{
						std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
						live = st->request != nullptr && !st->cancelRequested->load();
						if (live) {
							{
								std::lock_guard<std::mutex> dataLock(st->dataMutex);
								if (st->body.size() + available > c_MaxResponseBytes) {
									if (st->error.empty()) st->error = "response body too large";
									tooLarge = true;
								}
							}
							if (!tooLarge) {
								st->readBuf.assign(available, '\0');
								if (!WinHttpReadData(st->request, st->readBuf.data(), available, nullptr)) {
									failCode = GetLastError();
									if (failCode != ERROR_IO_PENDING) failStep = "read";
								}
							}
						}
					}
					if (!live) return;
					if (tooLarge) {
						CloseRequestHandle(st);
						SignalDone(st);
					} else if (failStep != nullptr) {
						FailWith(st, failStep, failCode);
					}
					return;
				}
				case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
					if (infoLength == 0) {
						SignalDone(st);
						return;
					}
					{
						std::lock_guard<std::mutex> dataLock(st->dataMutex);
						st->body.append(static_cast<const char*>(info), infoLength);
					}
					bool live = false;
					const char* failStep = nullptr;
					DWORD failCode = 0;
					{
						std::lock_guard<std::recursive_mutex> lock(st->handleMutex);
						live = st->request != nullptr && !st->cancelRequested->load();
						if (live && !WinHttpQueryDataAvailable(st->request, nullptr)) {
							failCode = GetLastError();
							if (failCode != ERROR_IO_PENDING) failStep = "query data";
						}
					}
					if (failStep != nullptr) FailWith(st, failStep, failCode);
					return;
				}
				case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
					const WINHTTP_ASYNC_RESULT* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
					FailWith(st, AsyncStepName(result->dwResult), result->dwError);
					return;
				}
				case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
					// Guaranteed last callback; the release signal stays the last act on this state.
					SignalDone(st);
					if (st->requestClosedEvent != nullptr) SetEvent(st->requestClosedEvent);
					return;
				default:
					return;
			}
		}

		void CALLBACK HttpStatusCallback(HINTERNET request, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLength) {
			if (context == 0) return;   // session and connection handles carry no context
			AsyncRequest* st = reinterpret_cast<AsyncRequest*>(context);
			try {
				HttpStatusCallbackImpl(request, st, status, info, infoLength);
			} catch (...) {
				// A callback must not unwind through WinHTTP's C frames; fail the request instead.
				try { FailWith(st, "request", ERROR_WINHTTP_INTERNAL_ERROR); } catch (...) {}
			}
		}
	}

	void NetHttpClient::Cancel() {
		m_CancelRequested = true;
		{
			// Holding m_HandleMutex through the close keeps the worker from freeing the state
			// mid-cancel; the worker only clears m_Async under the same mutex.
			std::lock_guard<std::mutex> lock(m_HandleMutex);
			AsyncRequest* async = static_cast<AsyncRequest*>(m_Async);
			if (async != nullptr) {
				// Closing the request handle terminates the in-progress asynchronous request;
				// the documented cancellation path (closing a synchronous request is forbidden).
				CloseRequestHandle(async);
			}
		}
		if (m_Worker.joinable()) m_Worker.join();
	}

	void NetHttpClient::WorkerMain(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256) {
		try {
			WorkerMainImpl(std::move(method), std::move(url), std::move(headers), std::move(body), std::move(certPinSha256));
		} catch (const std::exception& e) {
			Finish(Response{0, "", std::string("worker exception: ") + e.what()});
		} catch (...) {
			Finish(Response{0, "", "worker exception"});
		}
	}

	void NetHttpClient::WorkerMainImpl(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256) {
		Response response;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_TotalTimeoutMs);
		const auto remainingMs = [&]() -> DWORD {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			return left > 0 ? static_cast<DWORD>(left) : 0;
		};
		const auto aborted = [&]() -> bool {
			if (m_CancelRequested.load()) { response.error = "cancelled"; return true; }
			if (remainingMs() == 0) { response.error = "timed out"; return true; }
			return false;
		};

		AsyncRequest* async = new AsyncRequest();
		async->cancelRequested = &m_CancelRequested;
		async->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		async->requestClosedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		{
			std::lock_guard<std::mutex> lock(m_HandleMutex);
			m_Async = async;
		}
		// Every exit goes through here. After the request handle closes, its HANDLE_CLOSING is the
		// last callback it sends; waiting for it keeps WinHTTP threads off this state before it is freed.
		const auto finish = [&](Response& result) {
			CloseRequestHandle(async);
			bool closingArrived = true;
			if (async->requestClosed.load() && async->callbackArmed.load() && async->requestClosedEvent != nullptr) {
				closingArrived = WaitForSingleObject(async->requestClosedEvent, 60000) == WAIT_OBJECT_0;
			}
			{
				std::lock_guard<std::mutex> lock(m_HandleMutex);
				m_Async = nullptr;
			}
			if (closingArrived) {
				if (async->requestClosedEvent != nullptr) CloseHandle(async->requestClosedEvent);
				if (async->doneEvent != nullptr) CloseHandle(async->doneEvent);
				delete async;
			} else {
				// HANDLE_CLOSING is documented to always follow a close; if it did not, a
				// callback may still be live, so the state and the flag it points at are
				// leaked rather than freed.
				async->cancelRequested = new std::atomic<bool>(async->cancelRequested->load());
				std::cerr << "[net-http] request state leaked: HANDLE_CLOSING never arrived" << std::endl;
			}
			Finish(result);
		};
		if (async->doneEvent == nullptr || async->requestClosedEvent == nullptr) {
			response.error = "could not create wait events";
			finish(response);
			return;
		}

		const std::wstring wideUrl = ToWide(url);
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
			response.error = "could not parse url";
			finish(response);
			return;
		}
		const std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
		if (scheme != L"https") {
			response.error = "only https urls are supported";
			finish(response);
			return;
		}
		if (!certPinSha256.empty() && !IsHexPin(certPinSha256)) {
			response.error = "malformed certificate pin";
			finish(response);
			return;
		}
		async->certPin = certPinSha256;
		std::transform(async->certPin.begin(), async->certPin.end(), async->certPin.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		if (path.empty()) path = L"/";

		WinHttpHandle session{WinHttpOpen(L"CortexCommand/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
		if (!session) {
			response.error = WinHttpError("open", GetLastError());
			finish(response);
			return;
		}
		WinHttpSetTimeouts(session, c_ConnectTimeoutMs, c_ConnectTimeoutMs, c_TotalTimeoutMs, c_TotalTimeoutMs);
		WinHttpHandle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
		if (!connection) {
			response.error = WinHttpError("connect", GetLastError());
			finish(response);
			return;
		}
		HINTERNET request = WinHttpOpenRequest(connection, ToWide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (request == nullptr) {
			response.error = WinHttpError("open request", GetLastError());
			finish(response);
			return;
		}
		{
			std::lock_guard<std::recursive_mutex> lock(async->handleMutex);
			async->request = request;
		}
		{
			DWORD_PTR context = reinterpret_cast<DWORD_PTR>(async);
			if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
				response.error = WinHttpError("set context", GetLastError());
				finish(response);
				return;
			}
		}
		if (!async->certPin.empty()) {
			// Pinned mode: let the handshake run past the untrusted CA, then verify the DER hash
			// ourselves in the SENDING_REQUEST callback before any request byte leaves.
			DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
			if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) {
				response.error = WinHttpError("set security flags", GetLastError());
				finish(response);
				return;
			}
		}
		{
			const DWORD left = remainingMs();
			WinHttpSetTimeouts(request, left < c_ConnectTimeoutMs ? left : c_ConnectTimeoutMs, left < c_ConnectTimeoutMs ? left : c_ConnectTimeoutMs, left, left);
		}
		if (WinHttpSetStatusCallback(request, &HttpStatusCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
			response.error = WinHttpError("set status callback", GetLastError());
			finish(response);
			return;
		}
		async->callbackArmed = true;
		if (aborted()) {
			finish(response);
			return;
		}
		for (const auto& [name, value] : headers) {
			async->headerBlock += ToWide(name) + L": " + ToWide(value) + L"\r\n";
		}
		async->requestBody = body;
		{
			std::lock_guard<std::recursive_mutex> lock(async->handleMutex);
			if (async->request == nullptr || m_CancelRequested.load()) {
				response.error = "cancelled";
			} else if (!WinHttpSendRequest(request, async->headerBlock.c_str(), static_cast<DWORD>(-1), async->requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : async->requestBody.data(), static_cast<DWORD>(async->requestBody.size()), static_cast<DWORD>(async->requestBody.size()), 0)) {
				const DWORD code = GetLastError();
				if (code != ERROR_IO_PENDING) response.error = WinHttpError("send", code);
			}
		}
		if (response.error.empty()) {
			const bool timedOut = WaitForSingleObject(async->doneEvent, remainingMs()) == WAIT_TIMEOUT;
			{
				std::lock_guard<std::mutex> lock(async->dataMutex);
				if (!async->error.empty()) {
					response.error = async->error;
				} else if (timedOut) {
					response.error = "timed out";
				} else if (m_CancelRequested.load()) {
					response.error = "cancelled";
				} else {
					response.statusCode = async->statusCode;
					response.body = std::move(async->body);
				}
			}
		}
		finish(response);
	}

#else

	void NetHttpClient::Cancel() {
		m_CancelRequested = true;
		if (m_Worker.joinable()) m_Worker.join();
	}

	void NetHttpClient::WorkerMain(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256) {
		WorkerMainImpl(std::move(method), std::move(url), std::move(headers), std::move(body), std::move(certPinSha256));
	}

	void NetHttpClient::WorkerMainImpl(std::string, std::string, std::vector<std::pair<std::string, std::string>>, std::string, std::string) {}

#endif

} // namespace RTE
