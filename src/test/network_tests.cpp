#include "test/fixture.h"

#include "network/fetcher.h"
#include "network/url.h"

TestResult RunNetworkTests() {
    TestResult result;

    {
        auto res = FetchUrl("data:text/plain,Hello%20World%21");
        std::string actual = (res.success ? "success " : "failure ")
            + res.contentType + " " + res.body + "\n";
        ExpectEqual("network/data-url/plain", actual,
            "success text/plain Hello World!\n", result);
    }

    {
        auto res = FetchUrl("data:image/png;base64,QUJDRA%3D%3D");
        std::string actual = (res.success ? "success " : "failure ")
            + res.contentType + " "
            + std::to_string(res.body.size()) + " " + res.body + "\n";
        ExpectEqual("network/data-url/base64", actual,
            "success image/png 4 ABCD\n", result);
    }

    {
        std::string actual;
        actual += ResolveUrlAgainstBase("data:text/css,.picture%7Bbackground%3Anone%7D",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("/files/acid2/reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        actual += ResolveUrlAgainstBase("reference.html",
            "https://www.webstandards.org/files/acid2/test.html") + "\n";
        ExpectEqual("network/resolve-url/scheme-and-relative",
            actual,
            "data:text/css,.picture%7Bbackground%3Anone%7D\n"
            "https://www.webstandards.org/files/acid2/reference.html\n"
            "https://www.webstandards.org/files/acid2/reference.html\n",
            result);
    }

    return result;
}
