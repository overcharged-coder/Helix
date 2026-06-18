#pragma once
#include <string>

struct FetchResult {
    bool        success = false;
    std::string finalUrl;
    std::string body;
    std::string contentType;
    std::string error;
};

// Fetch a URL over HTTP or HTTPS using WinINet.
// Follows redirects automatically.
FetchResult FetchUrl(const std::string& url);
