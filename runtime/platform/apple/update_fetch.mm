// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See update/fetch.h.
//
// macOS and iOS: NSURLSession (Foundation only, so it builds for both), in
// an ephemeral session of its own: no cookies sent or kept, no cache, no
// stored credentials, the system's proxy settings and certificates. Besides
// our headers CFNetwork sends Connection, Accept-Encoding and Cache-Control
// (no-cache); Accept-Language is set to a fixed "en". Manual
// reference counting like the rest of the runtime: the session is
// invalidated (which releases it) whether the request ends or is cut off.
//
// The completion handler can run after Fetch has given up waiting (the
// deadline is the request's timeout plus two seconds): what it writes lives
// in a shared_ptr the block holds, so it never touches a returned frame.
#include <update/fetch.h>

#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>

#import <Foundation/Foundation.h>

namespace update
{
    namespace
    {
        struct Pending
        {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
            Fetched result;
        };

        // NSURLErrorDomain's codes as the transport's outcomes.
        Transport FromError(NSError* error)
        {
            if (![error.domain isEqualToString:NSURLErrorDomain])
                return Transport::Failed;
            switch (error.code)
            {
            case NSURLErrorNotConnectedToInternet:
            case NSURLErrorCannotFindHost:
            case NSURLErrorCannotConnectToHost:
            case NSURLErrorNetworkConnectionLost:
            case NSURLErrorDNSLookupFailed:
            case NSURLErrorInternationalRoamingOff:
            case NSURLErrorDataNotAllowed:
            case NSURLErrorCallIsActive:
                return Transport::Offline;
            case NSURLErrorTimedOut:
                return Transport::Timeout;
            case NSURLErrorSecureConnectionFailed:
            case NSURLErrorServerCertificateHasBadDate:
            case NSURLErrorServerCertificateUntrusted:
            case NSURLErrorServerCertificateHasUnknownRoot:
            case NSURLErrorServerCertificateNotYetValid:
            case NSURLErrorClientCertificateRejected:
            case NSURLErrorClientCertificateRequired:
            case NSURLErrorAppTransportSecurityRequiresSecureConnection:
                return Transport::Secure;
            case NSURLErrorDataLengthExceedsMaximum:
                return Transport::TooLarge;
            case NSURLErrorBadServerResponse:
            case NSURLErrorCannotDecodeRawData:
            case NSURLErrorCannotDecodeContentData:
            case NSURLErrorCannotParseResponse:
                return Transport::Garbled;
            default:
                return Transport::Failed;
            }
        }

        NSString* String(const std::string& text)
        {
            return [NSString stringWithUTF8String:text.c_str()] ?: @"";
        }
    }

    Fetched Fetch(const Request& request)
    {
        auto pending = std::make_shared<Pending>();
        NSURLSession* session = nil;
        NSURLSessionDataTask* task = nil;
        @autoreleasepool
        {
            NSURL* url = [NSURL URLWithString:String(request.url)];
            NSString* scheme = url.scheme.lowercaseString;
            if (!url || !url.host || !([scheme isEqualToString:@"https"] || (request.allowHttp && [scheme isEqualToString:@"http"])))
            {
                Fetched result;
                result.detail = "not an https address";
                return result;
            }
            NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = request.timeoutSeconds;
            config.timeoutIntervalForResource = request.timeoutSeconds;
            config.requestCachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;
            config.URLCache = nil;
            config.HTTPCookieStorage = nil;
            config.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
            config.HTTPShouldSetCookies = NO;
            config.URLCredentialStorage = nil;
            config.waitsForConnectivity = NO;
            NSMutableURLRequest* get = [NSMutableURLRequest requestWithURL:url
                                                               cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData
                                                           timeoutInterval:request.timeoutSeconds];
            get.HTTPMethod = @"GET";
            get.HTTPShouldHandleCookies = NO;
            [get setValue:String(request.userAgent) forHTTPHeaderField:@"User-Agent"];
            // CFNetwork would add the system's languages ("en-US,en;q=0.9"):
            // a fixed one says nothing about the player (curl sends none).
            [get setValue:@"en" forHTTPHeaderField:@"Accept-Language"];
            for (const auto& [name, value] : request.headers)
                [get setValue:String(value) forHTTPHeaderField:String(name)];

            const size_t maxBytes = request.maxBytes;
            session = [[NSURLSession sessionWithConfiguration:config] retain];
            task = [[session dataTaskWithRequest:get
                               completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                                   Fetched result;
                                   if (error)
                                   {
                                       result.transport = FromError(error);
                                       result.detail = std::format("{} {}", error.domain.UTF8String ?: "", int64_t(error.code));
                                   }
                                   else if (![response isKindOfClass:[NSHTTPURLResponse class]])
                                   {
                                       result.transport = Transport::Garbled;
                                       result.detail = "not an HTTP answer";
                                   }
                                   else if (data.length > maxBytes)
                                   {
                                       result.transport = Transport::TooLarge;
                                       result.detail = std::format("{} bytes", uint64_t(data.length));
                                   }
                                   else
                                   {
                                       NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
                                       result.transport = Transport::Ok;
                                       result.response.status = int(http.statusCode);
                                       for (NSString* key in http.allHeaderFields)
                                       {
                                           id value = http.allHeaderFields[key];
                                           if (![key isKindOfClass:[NSString class]] || ![value isKindOfClass:[NSString class]])
                                               continue;
                                           std::string name = key.lowercaseString.UTF8String ?: "";
                                           result.response.headers.emplace_back(std::move(name), [(NSString*)value UTF8String] ?: "");
                                       }
                                       if (data.length)
                                           result.response.body.assign(static_cast<const char*>(data.bytes), data.length);
                                   }
                                   std::lock_guard lock(pending->mutex);
                                   pending->result = std::move(result);
                                   pending->finished = true;
                                   pending->done.notify_all();
                               }] retain];
            [task resume];
        }

        Fetched result;
        bool finished;
        {
            std::unique_lock lock(pending->mutex);
            finished = pending->done.wait_for(lock, std::chrono::duration<double>(request.timeoutSeconds + 2.0), [&] { return pending->finished; });
            if (finished)
                result = std::move(pending->result);
        }
        if (finished)
            [session finishTasksAndInvalidate];
        else
        {
            [task cancel];
            [session invalidateAndCancel];
            result.transport = Transport::Timeout;
            result.detail = "no answer by the deadline";
        }
        [task release];
        [session release];
        return result;
    }
}
