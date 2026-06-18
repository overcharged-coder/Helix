#include "network/fetcher.h"
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

FetchResult FetchUrl(const std::string& url) {
    FetchResult r;

    HINTERNET hNet = InternetOpenA(
        "Felix/0.1",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hNet) { r.error = "InternetOpen failed"; return r; }

    DWORD flags =
        INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_IGNORE_CERT_DATE_INVALID |
        INTERNET_FLAG_IGNORE_CERT_CN_INVALID;  // tolerate self-signed in v0.1

    HINTERNET hReq = InternetOpenUrlA(hNet, url.c_str(), nullptr, 0, flags, 0);
    if (!hReq) {
        r.error = "InternetOpenUrl failed for: " + url;
        InternetCloseHandle(hNet);
        return r;
    }

    // Resolve final URL (after redirects)
    char finalUrl[2048] = {};
    DWORD len = (DWORD)sizeof(finalUrl);
    HttpQueryInfoA(hReq, HTTP_QUERY_LOCATION, finalUrl, &len, nullptr);
    r.finalUrl = *finalUrl ? std::string(finalUrl) : url;

    // Content-Type header
    char ct[256] = {};
    len = (DWORD)sizeof(ct);
    HttpQueryInfoA(hReq, HTTP_QUERY_CONTENT_TYPE, ct, &len, nullptr);
    r.contentType = ct;

    // Read body
    char buf[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf), &bytesRead) && bytesRead)
        r.body.append(buf, bytesRead);

    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);
    r.success = true;
    return r;
}
