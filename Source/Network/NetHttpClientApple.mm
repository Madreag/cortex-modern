#include "NetHttpClientApple.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <os/lock.h>
#include <time.h>

namespace {
	constexpr NSUInteger c_MaxResponseBytes = 4 * 1024 * 1024;

	uint64_t NowMs() {
		return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
	}

	bool IsHexPin(const char* pin) {
		size_t length = 0;
		for (; pin[length] != '\0'; ++length) {
			if (isxdigit(static_cast<unsigned char>(pin[length])) == 0) {
				return false;
			}
		}
		return length == 64;
	}

	NSString* Latin1(const char* text) {
		return [NSString stringWithCString:text encoding:NSISOLatin1StringEncoding];
	}

	/// Lowercase hex SHA-256 of the leaf certificate's DER bytes; nil when the chain is unavailable.
	NSString* LeafSha256Hex(SecTrustRef trust) {
		NSArray* chain = trust != nullptr ? CFBridgingRelease(SecTrustCopyCertificateChain(trust)) : nil;
		if (chain.count == 0) {
			return nil;
		}
		NSData* der = CFBridgingRelease(SecCertificateCopyData((__bridge SecCertificateRef)chain[0]));
		unsigned char digest[CC_SHA256_DIGEST_LENGTH];
		CC_SHA256(der.bytes, static_cast<CC_LONG>(der.length), digest);
		NSMutableString* hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
		for (unsigned char byte : digest) {
			[hex appendFormat:@"%02x", byte];
		}
		return hex;
	}
}

@interface RTENetHttpAppleRequest : NSObject <NSURLSessionDataDelegate>
- (instancetype)initWithDone:(NetHttpAppleDoneFn)done context:(void*)context pin:(NSString*)pin totalTimeoutMs:(int)totalTimeoutMs;
- (void)startRequest:(NSURLRequest*)request configuration:(NSURLSessionConfiguration*)configuration;
- (void)deliverStatus:(long)statusCode body:(NSData*)body error:(NSString*)error;
- (bool)cancelAndWait:(int)waitMs;
@end

@implementation RTENetHttpAppleRequest {
	os_unfair_lock _lock;   // Guards _done, _context and _delivered between the delegate queue and Cancel.
	NetHttpAppleDoneFn _done;
	void* _context;
	bool _delivered;
	dispatch_semaphore_t _deliveredSignal;
	NSString* _pin;
	uint64_t _totalTimeoutMs;
	uint64_t _startMs;
	NSURLSession* _session;
	NSURLSessionDataTask* _task;
	long _statusCode;
	bool _responded;
	NSMutableData* _body;
	NSString* _error;
}

- (instancetype)initWithDone:(NetHttpAppleDoneFn)done context:(void*)context pin:(NSString*)pin totalTimeoutMs:(int)totalTimeoutMs {
	if ((self = [super init])) {
		_lock = OS_UNFAIR_LOCK_INIT;
		_done = done;
		_context = context;
		_deliveredSignal = dispatch_semaphore_create(0);
		_pin = pin;
		_totalTimeoutMs = static_cast<uint64_t>(totalTimeoutMs);
		_body = [NSMutableData data];
	}
	return self;
}

- (void)startRequest:(NSURLRequest*)request configuration:(NSURLSessionConfiguration*)configuration {
	_startMs = NowMs();
	_session = [NSURLSession sessionWithConfiguration:configuration delegate:self delegateQueue:nil];
	_task = [_session dataTaskWithRequest:request];
	[_task resume];
}

- (void)deliverStatus:(long)statusCode body:(NSData*)body error:(NSString*)error {
	os_unfair_lock_lock(&_lock);
	if (_done != nullptr && !_delivered) {
		_done(_context, statusCode, body.length > 0 ? static_cast<const char*>(body.bytes) : "", body.length, error.length > 0 ? error.UTF8String : "");
	}
	_delivered = true;
	os_unfair_lock_unlock(&_lock);
	dispatch_semaphore_signal(_deliveredSignal);
}

- (bool)cancelAndWait:(int)waitMs {
	[_task cancel];
	[_session invalidateAndCancel];
	(void)dispatch_semaphore_wait(_deliveredSignal, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(waitMs) * static_cast<int64_t>(NSEC_PER_MSEC)));
	os_unfair_lock_lock(&_lock);
	const bool delivered = _delivered;
	_done = nullptr;
	_context = nullptr;
	os_unfair_lock_unlock(&_lock);
	return delivered;
}

- (void)noteError:(NSString*)error {
	if (_error == nil) {
		_error = error;
	}
}

- (NSString*)describeError:(NSError*)error {
	NSString* step = _responded ? @"receive" : @"send";
	if (![error.domain isEqualToString:NSURLErrorDomain]) {
		return [NSString stringWithFormat:@"%@: %@ error %ld", step, error.domain, static_cast<long>(error.code)];
	}
	switch (error.code) {
		case NSURLErrorCancelled:
			return @"cancelled";
		case NSURLErrorTimedOut:
			return NowMs() - _startMs >= _totalTimeoutMs ? @"timed out" : [step stringByAppendingString:@": timed out"];
		case NSURLErrorCannotFindHost:
		case NSURLErrorCannotConnectToHost:
		case NSURLErrorDNSLookupFailed:
		case NSURLErrorNotConnectedToInternet:
			return [step stringByAppendingString:@": cannot connect"];
		case NSURLErrorSecureConnectionFailed:
		case NSURLErrorServerCertificateHasBadDate:
		case NSURLErrorServerCertificateUntrusted:
		case NSURLErrorServerCertificateHasUnknownRoot:
		case NSURLErrorServerCertificateNotYetValid:
		case NSURLErrorClientCertificateRejected:
		case NSURLErrorClientCertificateRequired:
			return [step stringByAppendingString:@": certificate verification failed"];
		default:
			return [NSString stringWithFormat:@"%@: nsurl error %ld", step, static_cast<long>(error.code)];
	}
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
	if (_pin.length == 0 || ![challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust]) {
		completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return;
	}
	// Decided inside the TLS handshake, so a refused certificate never sees a request byte.
	SecTrustRef trust = challenge.protectionSpace.serverTrust;
	NSString* leaf = LeafSha256Hex(trust);
	if ([leaf isEqualToString:_pin]) {
		completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:trust]);
		return;
	}
	[self noteError:leaf == nil ? @"could not read server certificate" : @"certificate pin mismatch"];
	completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completionHandler {
	// WinHTTP's default policy: follow https redirects, hand an https-to-http one back unfollowed.
	completionHandler([request.URL.scheme.lowercaseString isEqualToString:@"https"] ? request : nil);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
	_responded = true;
	_statusCode = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)response).statusCode : 0;
	completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
	if (_body.length + data.length > c_MaxResponseBytes) {
		[self noteError:@"response body too large"];
		[dataTask cancel];
		return;
	}
	[_body appendData:data];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
	[session finishTasksAndInvalidate];
	NSString* failure = _error != nil ? _error : (error != nil ? [self describeError:error] : nil);
	if (failure != nil) {
		[self deliverStatus:0 body:nil error:failure];
	} else {
		[self deliverStatus:_statusCode body:_body error:nil];
	}
}

@end

void* NetHttpAppleStart(const char* method, const char* url, const char* const* headerNames, const char* const* headerValues, size_t headerCount, const char* body, size_t bodySize, const char* certPinSha256, int connectTimeoutMs, int totalTimeoutMs, NetHttpAppleDoneFn done, void* context) {
	@autoreleasepool {
		RTENetHttpAppleRequest* request = [[RTENetHttpAppleRequest alloc] initWithDone:done context:context pin:[Latin1(certPinSha256) lowercaseString] totalTimeoutMs:totalTimeoutMs];
		NSString* urlText = [NSString stringWithUTF8String:url];
		NSURL* address = urlText != nil ? [NSURL URLWithString:urlText] : nil;
		NSString* refusal = nil;
		if (address == nil || address.host.length == 0) {
			refusal = @"could not parse url";
		} else if (![address.scheme.lowercaseString isEqualToString:@"https"]) {
			refusal = @"only https urls are supported";
		} else if (certPinSha256[0] != '\0' && !IsHexPin(certPinSha256)) {
			refusal = @"malformed certificate pin";
		}
		if (refusal != nil) {
			[request deliverStatus:0 body:nil error:refusal];
			return nullptr;
		}

		NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		configuration.timeoutIntervalForRequest = connectTimeoutMs / 1000.0;
		configuration.timeoutIntervalForResource = totalTimeoutMs / 1000.0;
		configuration.HTTPShouldSetCookies = NO;
		NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:address];
		// A request's own timeoutInterval (60 s unless set) takes precedence over the configuration's.
		urlRequest.timeoutInterval = configuration.timeoutIntervalForRequest;
		urlRequest.HTTPMethod = Latin1(method);
		[urlRequest setValue:@"CortexCommand/1.0" forHTTPHeaderField:@"User-Agent"];
		for (size_t i = 0; i < headerCount; ++i) {
			[urlRequest setValue:Latin1(headerValues[i]) forHTTPHeaderField:Latin1(headerNames[i])];
		}
		if (bodySize > 0) {
			urlRequest.HTTPBody = [NSData dataWithBytes:body length:bodySize];
		}
		[request startRequest:urlRequest configuration:configuration];
		return (__bridge_retained void*)request;
	}
}

bool NetHttpAppleCancel(void* request, int waitMs) {
	@autoreleasepool {
		RTENetHttpAppleRequest* owned = (__bridge_transfer RTENetHttpAppleRequest*)request;
		return [owned cancelAndWait:waitMs];
	}
}
