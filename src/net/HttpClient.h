#pragma once

#include <string>
#include <vector>

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    static HttpResponse get(const std::string& url,
                            const std::vector<HttpHeader>& headers = {},
                            long timeoutSeconds = 6);
};
