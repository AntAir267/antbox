/* xbox_update_http_get on macOS: NSURLSession (Foundation is linked for Metal
 * already). An ephemeral session: no cache, no cookies, nothing kept. */
#include "xbox_update.h"

#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

int xbox_update_available(void) { return 1; }

int xbox_update_http_get(const char *url, const char *user_agent, int timeout_ms,
                         char **body, char *err, size_t errn)
{
    @autoreleasepool {
        *body = NULL;
        err[0] = 0;
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!u) {
            snprintf(err, errn, "bad address");
            return 0;
        }
        NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = timeout_ms / 1000.0;
        cfg.timeoutIntervalForResource = timeout_ms / 1000.0;
        cfg.HTTPCookieStorage = nil;
        cfg.URLCache = nil;
        cfg.waitsForConnectivity = NO;
        NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg];
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:timeout_ms / 1000.0];
        [req setValue:[NSString stringWithUTF8String:user_agent] forHTTPHeaderField:@"User-Agent"];
        [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];

        __block int status = 0;
        __block NSData *data = nil;
        __block NSError *error = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask *task = [session dataTaskWithRequest:req
            completionHandler:^(NSData *d, NSURLResponse *resp, NSError *e) {
                data = d;
                error = e;
                if ([resp isKindOfClass:[NSHTTPURLResponse class]])
                    status = (int)((NSHTTPURLResponse *)resp).statusCode;
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                                         (int64_t)(timeout_ms + 2000) * NSEC_PER_MSEC))) {
            [task cancel];
            [session invalidateAndCancel];
            snprintf(err, errn, "timed out");
            return 0;
        }
        [session finishTasksAndInvalidate];
        if (!status) {
            switch (error ? error.code : 0) {
            case NSURLErrorTimedOut:            snprintf(err, errn, "timed out"); break;
            case NSURLErrorNotConnectedToInternet:
            case NSURLErrorCannotFindHost:
            case NSURLErrorCannotConnectToHost:
            case NSURLErrorNetworkConnectionLost:
            case NSURLErrorDNSLookupFailed:     snprintf(err, errn, "no connection"); break;
            case NSURLErrorSecureConnectionFailed:
            case NSURLErrorServerCertificateUntrusted:
                                                snprintf(err, errn, "secure connection failed"); break;
            default:                            snprintf(err, errn, "network error %ld", (long)(error ? error.code : 0));
            }
            return 0;
        }
        NSUInteger len = data ? data.length : 0;
        if (len > (1u << 20))
            len = 1u << 20;
        char *b = (char *)malloc(len + 1);
        if (b) {
            if (len)
                memcpy(b, data.bytes, len);
            b[len] = 0;
            *body = b;
        }
        return status;
    }
}
