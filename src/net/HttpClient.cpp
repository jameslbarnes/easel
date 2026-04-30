#include "net/HttpClient.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#ifdef HAS_CURL
#include <curl/curl.h>
#endif
#endif

#include <algorithm>
#include <sstream>

namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = url.substr(0, schemeEnd);
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string authority = (pathStart == std::string::npos)
        ? url.substr(hostStart)
        : url.substr(hostStart, pathStart - hostStart);
    out.path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        out.host = authority;
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    return !out.host.empty();
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring(s.begin(), s.end());
    std::wstring out((size_t)len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}
#endif

#ifndef _WIN32
#ifdef HAS_CURL
struct CurlWriteCtx {
    std::string data;
    static size_t callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        size_t total = size * nmemb;
        static_cast<CurlWriteCtx*>(userdata)->data.append(ptr, total);
        return total;
    }
};
#endif
#endif

} // namespace

HttpResponse HttpClient::get(const std::string& url,
                             const std::vector<HttpHeader>& headers,
                             long timeoutSeconds) {
    HttpResponse response;

#ifdef _WIN32
    ParsedUrl parsed;
    if (!parseUrl(url, parsed)) {
        response.error = "invalid URL";
        return response;
    }

    HINTERNET session = WinHttpOpen(L"Easel/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        response.error = "WinHttpOpen failed";
        return response;
    }

    HINTERNET connect = WinHttpConnect(session, widen(parsed.host).c_str(),
                                       (INTERNET_PORT)parsed.port, 0);
    if (!connect) {
        response.error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD flags = (parsed.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", widen(parsed.path).c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        response.error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD timeoutMs = (DWORD)std::max(1L, timeoutSeconds) * 1000;
    WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring headerText;
    for (const auto& h : headers) {
        headerText += widen(h.name + ": " + h.value + "\r\n");
    }

    BOOL ok = WinHttpSendRequest(request,
                                 headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                 headerText.empty() ? 0 : (DWORD)-1L,
                                 WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
        response.error = "WinHTTP request failed";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.status = (long)status;
    }

    DWORD bytesAvail = 0;
    while (WinHttpQueryDataAvailable(request, &bytesAvail) && bytesAvail > 0) {
        std::string buf(bytesAvail, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buf.data(), bytesAvail, &bytesRead)) break;
        response.body.append(buf.data(), bytesRead);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;

#else
#ifdef HAS_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "curl_easy_init failed";
        return response;
    }

    CurlWriteCtx writeCtx;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCtx::callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(3L, timeoutSeconds));

    struct curl_slist* curlHeaders = nullptr;
    for (const auto& h : headers) {
        curlHeaders = curl_slist_append(curlHeaders, (h.name + ": " + h.value).c_str());
    }
    if (curlHeaders) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);

    CURLcode res = curl_easy_perform(curl);
    if (curlHeaders) curl_slist_free_all(curlHeaders);

    if (res != CURLE_OK) {
        response.error = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return response;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.body = std::move(writeCtx.data);
    curl_easy_cleanup(curl);
    return response;
#else
    (void)url;
    (void)headers;
    (void)timeoutSeconds;
    response.error = "libcurl not available";
    return response;
#endif
#endif
}
