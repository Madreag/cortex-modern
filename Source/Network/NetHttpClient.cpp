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

namespace RTE {

	NetHttpClient::~NetHttpClient() {
		Cancel();
	}

	void NetHttpClient::Start(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, const std::string& certPinSha256) {
#ifdef _WIN32
		if (m_Worker.joinable()) return;
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

	void NetHttpClient::Cancel() {
		m_CancelRequested = true;
		if (m_Worker.joinable()) m_Worker.join();
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
			if (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return std::string(step) + ": cannot connect";
			return std::string(step) + ": winhttp error " + std::to_string(code);
		}

		constexpr size_t c_MaxResponseBytes = 4 * 1024 * 1024;
	}

	void NetHttpClient::WorkerMain(std::string method, std::string url, std::vector<std::pair<std::string, std::string>> headers, std::string body, std::string certPinSha256) {
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

		const std::wstring wideUrl = ToWide(url);
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
			response.error = "could not parse url";
			Finish(response);
			return;
		}
		const std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
		if (scheme != L"https") {
			response.error = "only https urls are supported";
			Finish(response);
			return;
		}
		if (!certPinSha256.empty() && !IsHexPin(certPinSha256)) {
			response.error = "malformed certificate pin";
			Finish(response);
			return;
		}
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		if (path.empty()) path = L"/";

		WinHttpHandle session{WinHttpOpen(L"CortexCommand/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
		if (!session) {
			response.error = WinHttpError("open", GetLastError());
			Finish(response);
			return;
		}
		WinHttpSetTimeouts(session, c_ConnectTimeoutMs, c_ConnectTimeoutMs, c_TotalTimeoutMs, c_TotalTimeoutMs);
		WinHttpHandle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
		if (!connection) {
			response.error = WinHttpError("connect", GetLastError());
			Finish(response);
			return;
		}
		WinHttpHandle request{WinHttpOpenRequest(connection, ToWide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
		if (!request) {
			response.error = WinHttpError("open request", GetLastError());
			Finish(response);
			return;
		}
		if (!certPinSha256.empty()) {
			// Pinned mode: let the handshake run past the untrusted CA, then verify the DER hash ourselves.
			DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
			if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) {
				response.error = WinHttpError("set security flags", GetLastError());
				Finish(response);
				return;
			}
		}
		{
			const DWORD left = remainingMs();
			WinHttpSetTimeouts(request, left < c_ConnectTimeoutMs ? left : c_ConnectTimeoutMs, left < c_ConnectTimeoutMs ? left : c_ConnectTimeoutMs, left, left);
		}
		if (aborted()) {
			Finish(response);
			return;
		}

		std::wstring headerBlock;
		for (const auto& [name, value] : headers) {
			headerBlock += ToWide(name) + L": " + ToWide(value) + L"\r\n";
		}
		LPVOID optionalData = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
		const DWORD optionalSize = static_cast<DWORD>(body.size());
		if (!WinHttpSendRequest(request, headerBlock.c_str(), static_cast<DWORD>(-1), optionalData, optionalSize, optionalSize, 0)) {
			response.error = WinHttpError("send", GetLastError());
			Finish(response);
			return;
		}

		// The handshake is complete once the request went out; the cert context is available here,
		// but on some stacks only after the response headers land, so retry once below.
		// Returns: 0 = no context yet, 1 = pin verified, -1 = error already set on response.
		auto verifyPin = [&]() -> int {
			PCCERT_CONTEXT certificate = nullptr;
			DWORD certSize = sizeof(certificate);
			if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &certSize)) {
				return 0;
			}
			const std::string actual = Sha256Hex(certificate->pbCertEncoded, certificate->cbCertEncoded);
			CertFreeCertificateContext(certificate);
			std::string expected = certPinSha256;
			std::transform(expected.begin(), expected.end(), expected.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
			if (actual.empty() || actual != expected) {
				response.error = "certificate pin mismatch";
				return -1;
			}
			return 1;
		};

		bool responseReceived = false;
		if (!certPinSha256.empty()) {
			int pin = verifyPin();
			if (pin == 0) {
				if (!WinHttpReceiveResponse(request, nullptr)) {
					response.error = WinHttpError("receive", GetLastError());
					Finish(response);
					return;
				}
				responseReceived = true;
				pin = verifyPin();
				if (pin == 0) {
					response.error = "could not read server certificate";
					Finish(response);
					return;
				}
			}
			if (pin < 0) {
				Finish(response);
				return;
			}
		}
		if (!responseReceived && !WinHttpReceiveResponse(request, nullptr)) {
			response.error = WinHttpError("receive", GetLastError());
			Finish(response);
			return;
		}
		if (aborted()) {
			Finish(response);
			return;
		}

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
			response.statusCode = static_cast<long>(status);
		}

		std::string& out = response.body;
		while (response.error.empty()) {
			const DWORD left = remainingMs();
			if (left == 0) {
				response.error = "timed out";
				break;
			}
			WinHttpSetTimeouts(request, 0, 0, left, left);
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available)) {
				response.error = WinHttpError("query data", GetLastError());
				break;
			}
			if (available == 0) break;
			if (out.size() + available > c_MaxResponseBytes) {
				response.error = "response body too large";
				break;
			}
			const size_t offset = out.size();
			out.resize(offset + available);
			DWORD read = 0;
			if (!WinHttpReadData(request, out.data() + offset, available, &read)) {
				response.error = WinHttpError("read", GetLastError());
				break;
			}
			out.resize(offset + read);
			if (read == 0) break;
			if (m_CancelRequested.load()) response.error = "cancelled";
		}
		Finish(response);
	}

#else

	void NetHttpClient::WorkerMain(std::string, std::string, std::vector<std::pair<std::string, std::string>>, std::string, std::string) {}

#endif

} // namespace RTE
