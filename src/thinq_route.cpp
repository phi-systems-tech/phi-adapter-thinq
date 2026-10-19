#include "thinq_route.h"

#include "thinq_json.h"

#include <cctype>
#include <cstdlib>

namespace phicore::thinq::ipc {

namespace {

constexpr std::string_view kDevicePrefix = "/device/";
constexpr std::string_view kDeviceSuffix = "/certificate";

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string okResult(const Json &result)
{
    Json out = Json::object();
    out["resultCode"] = "0000";
    out["result"] = result;
    return dump(out);
}

HttpResponse errorResponse(int status, const char *code, const std::string &message)
{
    Json out = Json::object();
    out["resultCode"] = code;
    out["result"] = message;
    return {status, "application/json", dump(out)};
}

const char *reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    default:
        return "Error";
    }
}

} // namespace

std::optional<HttpRequest> parseHttpRequest(std::string_view raw)
{
    const std::size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string_view::npos)
        return std::nullopt;
    HttpRequest out;
    std::string_view head = raw.substr(0, headEnd);

    const std::size_t lineEnd = head.find("\r\n");
    std::string_view requestLine = head.substr(0, lineEnd);
    const std::size_t sp1 = requestLine.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos)
        return out; // not HTTP; empty method says so
    out.method = std::string(requestLine.substr(0, sp1));
    std::string_view target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = target.find('?');
    out.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos)
        out.query = std::string(target.substr(q + 1));

    std::string_view rest = lineEnd == std::string_view::npos ? std::string_view() : head.substr(lineEnd + 2);
    while (!rest.empty()) {
        const std::size_t eol = rest.find("\r\n");
        std::string_view line = rest.substr(0, eol);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos)
            out.headers[lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
        if (eol == std::string_view::npos)
            break;
        rest = rest.substr(eol + 2);
    }

    std::size_t length = 0;
    if (const auto it = out.headers.find("content-length"); it != out.headers.end())
        length = static_cast<std::size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    std::string_view body = raw.substr(headEnd + 4);
    if (body.size() < length)
        return std::nullopt;
    out.body = std::string(body.substr(0, length));
    return out;
}

std::string serializeHttpResponse(const HttpResponse &response)
{
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + reasonPhrase(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += response.body;
    return out;
}

std::string deviceUuidFromPath(std::string_view path)
{
    if (path.size() <= kDevicePrefix.size() + kDeviceSuffix.size() || path.substr(0, kDevicePrefix.size()) != kDevicePrefix
        || path.substr(path.size() - kDeviceSuffix.size()) != kDeviceSuffix)
        return {};
    std::string_view uuid = path.substr(kDevicePrefix.size(), path.size() - kDevicePrefix.size() - kDeviceSuffix.size());
    if (uuid.empty() || uuid.find('/') != std::string_view::npos)
        return {};
    return std::string(uuid);
}

std::string queryValue(std::string_view query, std::string_view key)
{
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        const std::size_t eq = pair.find('=');
        if (pair.substr(0, eq) == key)
            return eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1));
        if (amp == std::string_view::npos)
            break;
        query = query.substr(amp + 1);
    }
    return {};
}

RouteService::RouteService(RouteConfig config, KeyAndCert ca)
    : m_config(std::move(config))
    , m_ca(std::move(ca))
{
}

HttpResponse RouteService::handle(const HttpRequest &request) const
{
    if (request.path == "/route") {
        if (request.method != "GET" && request.method != "POST")
            return errorResponse(405, "9999", "method not allowed");
        return route();
    }
    if (request.path == "/route/certificate") {
        if (request.method != "GET" && request.method != "POST")
            return errorResponse(405, "9999", "method not allowed");
        const std::string name = queryValue(request.query, "name");
        return name.empty() ? certificateNames() : certificate(name);
    }
    const std::string uuid = deviceUuidFromPath(request.path);
    if (!uuid.empty()) {
        if (request.method != "POST" && request.method != "PUT")
            return errorResponse(405, "9999", "method not allowed");
        return deviceCertificate(uuid, request.body);
    }
    return errorResponse(404, "9999", "no such route");
}

HttpResponse RouteService::route() const
{
    Json result = Json::object();
    result["apiServer"] = "https://" + m_config.host + ":" + std::to_string(m_config.apiPort);
    result["mqttServer"] = "ssl://" + m_config.host + ":" + std::to_string(m_config.mqttPort);
    return {200, "application/json", okResult(result)};
}

HttpResponse RouteService::certificateNames() const
{
    return {200, "application/json", okResult(Json::array({"common-server", "aws-iot"}))};
}

HttpResponse RouteService::certificate(const std::string &name) const
{
    // One CA for both anchors (§3.2).
    if (name != "common-server" && name != "aws-iot")
        return errorResponse(404, "9999", "no such certificate");
    Json result = Json::object();
    result["certificatePem"] = m_ca.certPem;
    return {200, "application/json", okResult(result)};
}

HttpResponse RouteService::deviceCertificate(const std::string &uuid, const std::string &body) const
{
    const std::string csr = extractPemBlock(body, "CERTIFICATE REQUEST");
    if (csr.empty())
        return errorResponse(400, "9999", "no CSR in the body");
    std::string error;
    std::string subject;
    const std::optional<std::string> cert = signCsr(m_ca, csr, kDeviceDays, &error, &subject);
    if (!cert)
        return errorResponse(400, "9999", error);
    if (m_onIssued)
        m_onIssued(uuid, *cert, subject);
    Json result = Json::object();
    result["certificatePem"] = *cert;
    return {200, "application/json", okResult(result)};
}

} // namespace phicore::thinq::ipc
