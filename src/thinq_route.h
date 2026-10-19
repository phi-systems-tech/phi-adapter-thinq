#pragma once

// The HTTP half of the cloud we stand in for: `/route`, `/route/certificate`
// and the CSR signing of docs/thinq-protocol.md §3. Pure request → response;
// the TLS listener that carries it is phi-thinq-routed.

#include "thinq_ca.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
};

/**
 * @brief Parses one request from `raw`.
 *
 * nullopt while the head is incomplete or the body is shorter than
 * Content-Length says; the caller reads more and tries again. A head that
 * is not HTTP at all gives a request with an empty method.
 */
std::optional<HttpRequest> parseHttpRequest(std::string_view raw);

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

std::string serializeHttpResponse(const HttpResponse &response);

/// Where the module is to go next; both are DNS names, not addresses (§3.1).
struct RouteConfig {
    std::string host;
    int apiPort = 443;
    int mqttPort = 8883;
};

class RouteService
{
public:
    /// Called with a freshly signed device certificate so it can be recorded.
    using IssuedFn = std::function<void(const std::string &uuid, const std::string &certPem, const std::string &subject)>;

    RouteService(RouteConfig config, KeyAndCert ca);

    void onIssued(IssuedFn fn) { m_onIssued = std::move(fn); }

    HttpResponse handle(const HttpRequest &request) const;

    [[nodiscard]] const RouteConfig &config() const { return m_config; }

private:
    HttpResponse route() const;
    HttpResponse certificateNames() const;
    HttpResponse certificate(const std::string &name) const;
    HttpResponse deviceCertificate(const std::string &uuid, const std::string &body) const;

    RouteConfig m_config;
    KeyAndCert m_ca;
    IssuedFn m_onIssued;
};

/// `/device/<uuid>/certificate` → `<uuid>`, else empty.
std::string deviceUuidFromPath(std::string_view path);

/// `name=aws-iot` → `aws-iot`; the one query parameter the service reads.
std::string queryValue(std::string_view query, std::string_view key);

} // namespace phicore::thinq::ipc
