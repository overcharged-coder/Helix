#include "network/fetcher.h"
#include <windows.h>
#include <wininet.h>
#include <cctype>
#include <cstring>
#include <mutex>
#pragma comment(lib, "wininet.lib")

static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static std::string PercentDecode(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = HexValue(input[i + 1]);
            int lo = HexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (input[i] == '+') ? ' ' : input[i];
    }
    return out;
}

static int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    if (c >= '0' && c <= '9') return 52 + c - '0';
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string Base64Decode(const std::string& input) {
    std::string out;
    int val = 0;
    int bits = -8;
    for (char c : input) {
        if (std::isspace((unsigned char)c)) continue;
        if (c == '=') break;
        int b = Base64Value(c);
        if (b < 0) continue;
        val = (val << 6) | b;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    return out;
}

static bool StartsWithNoCase(const std::string& value, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= value.size()) return false;
        if (std::tolower((unsigned char)value[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

static HINTERNET SharedInternetSession() {
    static std::once_flag init;
    static HINTERNET session = nullptr;
    std::call_once(init, []() {
        session = InternetOpenA(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Helix/0.1 (+https://github.com/helix-browser)",
            INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (session) {
            DWORD decode = TRUE;
            InternetSetOptionA(session, INTERNET_OPTION_HTTP_DECODING, &decode, sizeof(decode));
        }
    });
    return session;
}

FetchResult FetchUrl(const std::string& url, size_t maxResponseBytes) {
    FetchResult r;

    if (StartsWithNoCase(url, "data:")) {
        size_t comma = url.find(',');
        if (comma == std::string::npos) {
            r.error = "Malformed data URL";
            return r;
        }
        std::string meta = url.substr(5, comma - 5);
        std::string payload = url.substr(comma + 1);
        bool base64 = false;
        r.contentType = "text/plain";

        size_t start = 0;
        while (start <= meta.size()) {
            size_t semi = meta.find(';', start);
            std::string part = meta.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            if (!part.empty()) {
                std::string low;
                for (char c : part) low += (char)std::tolower((unsigned char)c);
                if (low == "base64") base64 = true;
                else if (part.find('/') != std::string::npos) r.contentType = part;
            }
            if (semi == std::string::npos) break;
            start = semi + 1;
        }

        std::string decoded = PercentDecode(payload);
        r.body = base64 ? Base64Decode(decoded) : decoded;
        r.finalUrl = url;
        r.success = true;
        return r;
    }

    // Keep one WinINet session alive. Besides avoiding setup churn, this keeps
    // session cookies available to subsequent page, stylesheet, and image requests.
    HINTERNET hNet = SharedInternetSession();
    if (!hNet) { r.error = "InternetOpen failed"; return r; }

    DWORD flags =
        INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_IGNORE_CERT_DATE_INVALID |
        INTERNET_FLAG_IGNORE_CERT_CN_INVALID;  // tolerate self-signed in v0.1

    static constexpr char kHeaders[] =
        "Accept-Encoding: gzip, deflate\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n";
    HINTERNET hReq = InternetOpenUrlA(hNet, url.c_str(), kHeaders, -1L, flags, 0);
    if (!hReq) {
        r.error = "InternetOpenUrl failed for: " + url;
        return r;
    }

    // INTERNET_OPTION_URL reports the resolved request URL after any redirects.
    DWORD finalUrlLength = 0;
    InternetQueryOptionA(hReq, INTERNET_OPTION_URL, nullptr, &finalUrlLength);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && finalUrlLength > 1) {
        std::string finalUrl(finalUrlLength, '\0');
        if (InternetQueryOptionA(hReq, INTERNET_OPTION_URL, finalUrl.data(), &finalUrlLength)) {
            finalUrl.resize(std::strlen(finalUrl.c_str()));
            r.finalUrl = std::move(finalUrl);
        }
    }
    if (r.finalUrl.empty()) r.finalUrl = url;

    // HTTP status code: don't treat 4xx/5xx error pages as a successful body
    // (otherwise a 429 rate-limit page gets handed to the image decoder).
    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &statusLen, nullptr);

    // Content-Type header
    char ct[256] = {};
    DWORD len = (DWORD)sizeof(ct);
    HttpQueryInfoA(hReq, HTTP_QUERY_CONTENT_TYPE, ct, &len, nullptr);
    r.contentType = ct;

    // Read body
    char buf[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf), &bytesRead) && bytesRead) {
        if (r.body.size() + bytesRead > maxResponseBytes) {
            r.body.clear();
            r.error = "Response exceeds 12 MiB limit";
            InternetCloseHandle(hReq);
            return r;
        }
        r.body.append(buf, bytesRead);
    }

    InternetCloseHandle(hReq);
    r.status = (int)status;
    if (status >= 400) {
        r.success = false;
        r.error = "HTTP " + std::to_string(status);
        r.body.clear();
    } else {
        r.success = true;
    }
    return r;
}
