/* xbox_update_http_get on Windows: WinHTTP (part of the system; the system's
 * proxy settings and certificate store apply). */
#include "xbox_update.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xbox_update_available(void) { return 1; }

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void why(DWORD e, char *err, size_t errn)
{
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT:                snprintf(err, errn, "timed out"); break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CONNECTION_ERROR:       snprintf(err, errn, "no connection"); break;
    case ERROR_WINHTTP_SECURE_FAILURE:         snprintf(err, errn, "secure connection failed"); break;
    default:                                   snprintf(err, errn, "network error %lu", (unsigned long)e);
    }
}

int xbox_update_http_get(const char *url, const char *user_agent, int timeout_ms,
                         char **body, char *err, size_t errn)
{
    int status = 0;
    wchar_t *wurl = widen(url), *wua = widen(user_agent), *host = NULL, *path = NULL;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    URL_COMPONENTS uc;
    char *buf = NULL;
    size_t len = 0, cap = 0;

    *body = NULL;
    err[0] = 0;
    if (!wurl || !wua) {
        snprintf(err, errn, "out of memory");
        goto out;
    }
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        snprintf(err, errn, "bad address");
        goto out;
    }
    host = (wchar_t *)calloc(uc.dwHostNameLength + 1, sizeof *host);
    path = (wchar_t *)calloc(uc.dwUrlPathLength + uc.dwExtraInfoLength + 1, sizeof *path);
    if (!host || !path) {
        snprintf(err, errn, "out of memory");
        goto out;
    }
    memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof *host);
    memcpy(path, uc.lpszUrlPath, (uc.dwUrlPathLength + uc.dwExtraInfoLength) * sizeof *path);

    ses = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { why(GetLastError(), err, errn); goto out; }
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) { why(GetLastError(), err, errn); goto out; }
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) { why(GetLastError(), err, errn); goto out; }
    WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json", (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(req, NULL)) {
        why(GetLastError(), err, errn);
        goto out;
    }
    {
        DWORD code = 0, sz = sizeof code;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX)) {
            why(GetLastError(), err, errn);
            goto out;
        }
        status = (int)code;
    }
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail)
            break;
        if (len + avail > (1u << 20))
            avail = (DWORD)((1u << 20) - len);
        if (!avail)
            break;
        if (len + avail + 1 > cap) {
            char *nb = (char *)realloc(buf, len + avail + 1);
            if (!nb)
                break;
            buf = nb;
            cap = len + avail + 1;
        }
        if (!WinHttpReadData(req, buf + len, avail, &got) || !got)
            break;
        len += got;
    }
    if (buf) {
        buf[len] = 0;
        *body = buf;
        buf = NULL;
    }
out:
    free(buf);
    free(wurl); free(wua); free(host); free(path);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return status;
}
