#include "network/url.h"

#include <cctype>

bool HasUrlScheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    size_t stop = url.find_first_of("/?#");
    if (stop != std::string::npos && stop < colon) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = url[i];
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return true;
}

std::string ResolveUrlAgainstBase(const std::string& href, const std::string& base) {
    if (href.empty()) return {};
    if (HasUrlScheme(href)) return href;
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') return "https:" + href;
    if (href[0] == '/') {
        size_t p = base.find("://");
        if (p == std::string::npos) return href;
        size_t slash = base.find('/', p + 3);
        return (slash == std::string::npos ? base : base.substr(0, slash)) + href;
    }
    size_t last = base.rfind('/');
    return (last == std::string::npos ? base + "/" : base.substr(0, last + 1)) + href;
}
