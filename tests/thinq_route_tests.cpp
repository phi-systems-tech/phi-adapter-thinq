// The CA and the route service: minting, signing a CSR, the four endpoints.

#include <phi/adapter/testing/check.h>

#include "thinq_ca.h"
#include "thinq_json.h"
#include "thinq_route.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>

using namespace phicore::thinq::ipc;

namespace {

const char *const kUuid = "6b0e3a52-1111-4222-8333-444455556666";

struct Free {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
    void operator()(X509_REQ *r) const { X509_REQ_free(r); }
    void operator()(X509 *x) const { X509_free(x); }
    void operator()(BIO *b) const { BIO_free(b); }
};
template <typename T>
using Owned = std::unique_ptr<T, Free>;

/// A PEM CSR with CN `cn`, made the way a module would: RSA 2048, SHA-256.
std::string makeCsr(const std::string &cn)
{
    Owned<EVP_PKEY> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(2048)));
    Owned<X509_REQ> req(X509_REQ_new());
    if (!key || !req)
        return {};
    X509_REQ_set_version(req.get(), 0);
    X509_NAME *name = X509_REQ_get_subject_name(req.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(cn.c_str()), -1,
                               -1, 0);
    X509_REQ_set_pubkey(req.get(), key.get());
    if (X509_REQ_sign(req.get(), key.get(), EVP_sha256()) <= 0)
        return {};
    Owned<BIO> bio(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509_REQ(bio.get(), req.get());
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(size));
}

/// The CSR as a JSON string value: newlines escaped, as in §3.3's wrapped form.
std::string jsonWrapped(const std::string &pem)
{
    return Json{{"csr", pem}}.dump();
}

/// The certificate in a PEM, for reading its subject and issuer back.
Owned<X509> parseCert(const std::string &pem)
{
    Owned<BIO> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return Owned<X509>(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

std::string nameLine(X509_NAME *name)
{
    char *line = X509_NAME_oneline(name, nullptr, 0);
    const std::string out = line ? line : "";
    OPENSSL_free(line);
    return out;
}

const KeyAndCert &testCa()
{
    static const KeyAndCert ca = [] {
        std::optional<KeyAndCert> minted = mintCa("phi test CA", kCaDays);
        return minted ? *minted : KeyAndCert{};
    }();
    return ca;
}

HttpRequest get(const std::string &path, const std::string &query = {})
{
    HttpRequest r;
    r.method = "GET";
    r.path = path;
    r.query = query;
    return r;
}

RouteService makeService()
{
    return RouteService(RouteConfig{"phi-box.lan", 443, 8883}, testCa());
}

// ---- CA --------------------------------------------------------------------

void testMintCa()
{
    std::string error;
    const std::optional<KeyAndCert> ca = mintCa("phi test CA", kCaDays, &error);
    PHI_CHECK_MSG(ca.has_value(), "%s", error.c_str());
    if (!ca)
        return;
    PHI_CHECK(ca->certPem.find("-----BEGIN CERTIFICATE-----") != std::string::npos);
    PHI_CHECK(ca->keyPem.find("PRIVATE KEY-----") != std::string::npos);

    // Self-signed, with the name asked for.
    const Owned<X509> cert = parseCert(ca->certPem);
    PHI_CHECK(cert != nullptr);
    if (!cert)
        return;
    PHI_CHECK(nameLine(X509_get_subject_name(cert.get())).find("phi test CA") != std::string::npos);
    PHI_CHECK(X509_NAME_cmp(X509_get_subject_name(cert.get()), X509_get_issuer_name(cert.get())) == 0);
    // The module trusts this certificate as an anchor, so it has to say CA.
    PHI_CHECK((X509_get_extension_flags(cert.get()) & EXFLAG_CA) != 0);
}

void testCertificateFingerprint()
{
    const std::string fp = certificateFingerprint(testCa().certPem);
    PHI_CHECK(fp.size() == 64);
    PHI_CHECK(std::all_of(fp.begin(), fp.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); }));
    PHI_CHECK(fp == certificateFingerprint(testCa().certPem));

    // Another CA has another fingerprint; garbage has none.
    const std::optional<KeyAndCert> other = mintCa("other CA", 30);
    PHI_CHECK(other && certificateFingerprint(other->certPem) != fp);
    PHI_CHECK(certificateFingerprint("not a pem").empty());
    PHI_CHECK(certificateFingerprint("").empty());
}

void testMintServerLeaf()
{
    std::string error;
    const std::optional<KeyAndCert> leaf = mintServerLeaf(testCa(), "phi-box.lan", kLeafDays, &error);
    PHI_CHECK_MSG(leaf.has_value(), "%s", error.c_str());
    if (!leaf)
        return;
    const Owned<X509> cert = parseCert(leaf->certPem);
    const Owned<X509> ca = parseCert(testCa().certPem);
    PHI_CHECK(cert != nullptr && ca != nullptr);
    if (!cert || !ca)
        return;
    PHI_CHECK(nameLine(X509_get_subject_name(cert.get())).find("phi-box.lan") != std::string::npos);
    // The module checks the broker certificate against the DNS name from
    // /route, so the name must be a SAN as well as the CN.
    PHI_CHECK(X509_check_host(cert.get(), "phi-box.lan", 0, 0, nullptr) == 1);
    PHI_CHECK(X509_check_host(cert.get(), "other.lan", 0, 0, nullptr) != 1);
    // Signed by the CA.
    PHI_CHECK(X509_NAME_cmp(X509_get_issuer_name(cert.get()), X509_get_subject_name(ca.get())) == 0);
    PHI_CHECK(X509_verify(cert.get(), X509_get0_pubkey(ca.get())) == 1);
    PHI_CHECK(leaf->keyPem.find("PRIVATE KEY-----") != std::string::npos);
}

void testSignCsr()
{
    const std::string csr = makeCsr(kUuid);
    PHI_CHECK(!csr.empty());
    std::string error;
    std::string subject;
    const std::optional<std::string> pem = signCsr(testCa(), csr, kDeviceDays, &error, &subject);
    PHI_CHECK_MSG(pem.has_value(), "%s", error.c_str());
    if (!pem)
        return;
    PHI_CHECK(subject.find(kUuid) != std::string::npos);

    const Owned<X509> cert = parseCert(*pem);
    const Owned<X509> ca = parseCert(testCa().certPem);
    PHI_CHECK(cert != nullptr && ca != nullptr);
    if (!cert || !ca)
        return;
    PHI_CHECK(nameLine(X509_get_subject_name(cert.get())).find(kUuid) != std::string::npos);
    PHI_CHECK(X509_verify(cert.get(), X509_get0_pubkey(ca.get())) == 1);
}

void testSignCsrRejectsGarbage()
{
    std::string error;
    PHI_CHECK(!signCsr(testCa(), "not a csr", 30, &error).has_value());
    PHI_CHECK(!error.empty());
    PHI_CHECK(!signCsr(testCa(), "", 30).has_value());
}

void testExtractPemBlockRaw()
{
    const std::string pem = makeCsr(kUuid);
    // The block comes back whole and newline-terminated, which is what the
    // PEM reader of the signing step expects; a raw PEM already ends that way.
    PHI_CHECK(extractPemBlock(pem, "CERTIFICATE REQUEST") == pem);
    // Surrounding text is left out, only the first block is returned.
    const std::string noisy = "header\n" + pem + "trailer";
    const std::string block = extractPemBlock(noisy, "CERTIFICATE REQUEST");
    PHI_CHECK(block.rfind("-----BEGIN CERTIFICATE REQUEST-----", 0) == 0);
    PHI_CHECK(block.find("trailer") == std::string::npos);
    PHI_CHECK(extractPemBlock(pem, "CERTIFICATE").empty());
    PHI_CHECK(extractPemBlock("", "CERTIFICATE REQUEST").empty());
}

void testExtractPemBlockJsonEscaped()
{
    const std::string wrapped =
        "{\"csr\":\"-----BEGIN CERTIFICATE REQUEST-----\\nMIIB\\nabcd\\n-----END CERTIFICATE REQUEST-----\"}";
    const std::string block = extractPemBlock(wrapped, "CERTIFICATE REQUEST");
    // The escapes are undone and the block is newline-terminated.
    PHI_CHECK(block == "-----BEGIN CERTIFICATE REQUEST-----\nMIIB\nabcd\n-----END CERTIFICATE REQUEST-----\n");

    // An escaped slash is part of base64 and must come back as a slash.
    const std::string slash = "-----BEGIN CERTIFICATE REQUEST-----\\nAB\\/CD\\n-----END CERTIFICATE REQUEST-----";
    PHI_CHECK(extractPemBlock(slash, "CERTIFICATE REQUEST").find("AB/CD") != std::string::npos);

    // A real CSR survives the round trip through a JSON string.
    const std::string csr = makeCsr(kUuid);
    const std::string unwrapped = extractPemBlock(jsonWrapped(csr), "CERTIFICATE REQUEST");
    PHI_CHECK(!unwrapped.empty());
    PHI_CHECK(unwrapped.find('\\') == std::string::npos);
    PHI_CHECK(signCsr(testCa(), unwrapped, 30).has_value());
}

// ---- HTTP ------------------------------------------------------------------

void testParseGetWithQuery()
{
    const std::optional<HttpRequest> r = parseHttpRequest(
        "GET /route/certificate?name=aws-iot HTTP/1.1\r\nHost: phi-box.lan\r\nUser-Agent: lg\r\n\r\n");
    PHI_CHECK(r.has_value());
    if (!r)
        return;
    PHI_CHECK(r->method == "GET");
    PHI_CHECK(r->path == "/route/certificate");
    PHI_CHECK(r->query == "name=aws-iot");
    PHI_CHECK(r->headers.at("host") == "phi-box.lan");
    PHI_CHECK(r->headers.at("user-agent") == "lg");
    PHI_CHECK(r->body.empty());
}

void testParsePostWithBody()
{
    const std::optional<HttpRequest> r = parseHttpRequest(
        "POST /device/abc/certificate HTTP/1.1\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello");
    PHI_CHECK(r.has_value());
    if (!r)
        return;
    PHI_CHECK(r->method == "POST");
    PHI_CHECK(r->path == "/device/abc/certificate");
    PHI_CHECK(r->query.empty());
    // Header names are lower-cased so lookups do not depend on the module's spelling.
    PHI_CHECK(r->headers.at("content-length") == "5");
    PHI_CHECK(r->body == "hello");

    // Bytes beyond Content-Length are not part of the body.
    const std::optional<HttpRequest> extra = parseHttpRequest("POST /x HTTP/1.1\r\nContent-Length: 2\r\n\r\nabcdef");
    PHI_CHECK(extra && extra->body == "ab");
}

void testParseIncompleteInput()
{
    // The caller reads more and tries again: no head end, or a short body.
    PHI_CHECK(!parseHttpRequest("GET /route HTTP/1.1\r\nHost: x\r\n").has_value());
    PHI_CHECK(!parseHttpRequest("").has_value());
    PHI_CHECK(!parseHttpRequest("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc").has_value());
}

void testParseGarbage()
{
    const std::optional<HttpRequest> r = parseHttpRequest("garbage\r\n\r\n");
    PHI_CHECK(r.has_value());
    PHI_CHECK(r && r->method.empty());
    const std::optional<HttpRequest> spaces = parseHttpRequest("GET\r\n\r\n");
    PHI_CHECK(spaces && spaces->method.empty());
}

void testSerializeResponse()
{
    HttpResponse response;
    response.status = 404;
    response.body = "{\"a\":1}";
    const std::string text = serializeHttpResponse(response);
    PHI_CHECK(text.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    PHI_CHECK(text.find("Content-Type: application/json\r\n") != std::string::npos);
    PHI_CHECK(text.find("Content-Length: 7\r\n") != std::string::npos);
    PHI_CHECK(text.find("Connection: close\r\n") != std::string::npos);
    PHI_CHECK(text.size() >= 7 && text.substr(text.size() - 7) == "{\"a\":1}");
    PHI_CHECK(text.find("\r\n\r\n{\"a\":1}") != std::string::npos);

    PHI_CHECK(serializeHttpResponse(HttpResponse{}).rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
}

// ---- helpers ---------------------------------------------------------------

void testDeviceUuidFromPath()
{
    PHI_CHECK(deviceUuidFromPath("/device/abc-123/certificate") == "abc-123");
    PHI_CHECK(deviceUuidFromPath("/device//certificate").empty());
    PHI_CHECK(deviceUuidFromPath("/device/certificate").empty());
    PHI_CHECK(deviceUuidFromPath("/device/a/b/certificate").empty());
    PHI_CHECK(deviceUuidFromPath("/device/abc").empty());
    PHI_CHECK(deviceUuidFromPath("/devices/abc/certificate").empty());
    PHI_CHECK(deviceUuidFromPath("/route").empty());
    PHI_CHECK(deviceUuidFromPath("").empty());
}

void testQueryValue()
{
    PHI_CHECK(queryValue("name=aws-iot", "name") == "aws-iot");
    PHI_CHECK(queryValue("a=1&name=x&b=2", "name") == "x");
    PHI_CHECK(queryValue("a=1&b=2", "name").empty());
    PHI_CHECK(queryValue("", "name").empty());
    PHI_CHECK(queryValue("name", "name").empty());
    PHI_CHECK(queryValue("name=", "name").empty());
    // A longer key with the same prefix is another key.
    PHI_CHECK(queryValue("names=x", "name").empty());
    PHI_CHECK(queryValue("a=1&", "a") == "1");
}

// ---- service ---------------------------------------------------------------

void testRoute()
{
    const RouteService service = makeService();
    const HttpResponse r = service.handle(get("/route"));
    PHI_CHECK(r.status == 200);
    const Json body = parseObject(r.body);
    PHI_CHECK(body["resultCode"] == "0000");
    PHI_CHECK(body["result"]["apiServer"] == "https://phi-box.lan:443");
    PHI_CHECK(body["result"]["mqttServer"] == "ssl://phi-box.lan:8883");
}

void testCertificateNames()
{
    const RouteService service = makeService();
    const HttpResponse r = service.handle(get("/route/certificate"));
    PHI_CHECK(r.status == 200);
    const Json body = parseObject(r.body);
    PHI_CHECK(body["resultCode"] == "0000");
    PHI_CHECK(body["result"] == Json::array({"common-server", "aws-iot"}));
}

void testCertificateByName()
{
    const RouteService service = makeService();
    for (const char *name : {"aws-iot", "common-server"}) {
        const HttpResponse r = service.handle(get("/route/certificate", std::string("name=") + name));
        PHI_CHECK_MSG(r.status == 200, "%s", name);
        // One CA serves as the anchor for both servers (§3.2).
        PHI_CHECK_MSG(parseObject(r.body)["result"]["certificatePem"] == testCa().certPem, "%s", name);
    }
    const HttpResponse other = service.handle(get("/route/certificate", "name=other"));
    PHI_CHECK(other.status == 404);
    PHI_CHECK(parseObject(other.body)["resultCode"] != "0000");
}

void testDeviceCertificateRawAndWrapped()
{
    int issued = 0;
    std::string issuedUuid;
    std::string issuedSubject;
    std::string issuedPem;
    RouteService service = makeService();
    service.onIssued([&](const std::string &uuid, const std::string &certPem, const std::string &subject) {
        ++issued;
        issuedUuid = uuid;
        issuedPem = certPem;
        issuedSubject = subject;
    });

    const std::string csr = makeCsr(kUuid);
    const std::string path = std::string("/device/") + kUuid + "/certificate";

    for (const std::string &body : {csr, jsonWrapped(csr)}) {
        HttpRequest request;
        request.method = "POST";
        request.path = path;
        request.body = body;
        const HttpResponse r = service.handle(request);
        PHI_CHECK(r.status == 200);
        const Json j = parseObject(r.body);
        PHI_CHECK(j["resultCode"] == "0000");
        const std::string pem = j["result"].value("certificatePem", "");
        PHI_CHECK(pem.find("-----BEGIN CERTIFICATE-----") != std::string::npos);
        PHI_CHECK(pem == issuedPem);
        PHI_CHECK(issuedUuid == kUuid);
        PHI_CHECK(issuedSubject.find(kUuid) != std::string::npos);
    }
    PHI_CHECK(issued == 2);
}

void testDeviceCertificateErrors()
{
    int issued = 0;
    RouteService service = makeService();
    service.onIssued([&](const std::string &, const std::string &, const std::string &) { ++issued; });
    const std::string path = std::string("/device/") + kUuid + "/certificate";

    HttpRequest empty;
    empty.method = "POST";
    empty.path = path;
    PHI_CHECK(service.handle(empty).status == 400);

    HttpRequest broken = empty;
    broken.body = "-----BEGIN CERTIFICATE REQUEST-----\nAAAA\n-----END CERTIFICATE REQUEST-----";
    PHI_CHECK(service.handle(broken).status == 400);
    PHI_CHECK(issued == 0);

    // A CSR may only be posted; a GET on the same path is the wrong method.
    PHI_CHECK(service.handle(get(path)).status == 405);
}

void testUnknownPathAndMethods()
{
    const RouteService service = makeService();
    PHI_CHECK(service.handle(get("/nothing")).status == 404);
    PHI_CHECK(service.handle(get("/")).status == 404);
    HttpRequest del;
    del.method = "DELETE";
    del.path = "/route";
    PHI_CHECK(service.handle(del).status == 405);
    PHI_CHECK(service.config().host == "phi-box.lan");
}

} // namespace

int main()
{
    PHI_CHECK(!testCa().certPem.empty());

    testMintCa();
    testCertificateFingerprint();
    testMintServerLeaf();
    testSignCsr();
    testSignCsrRejectsGarbage();
    testExtractPemBlockRaw();
    testExtractPemBlockJsonEscaped();

    testParseGetWithQuery();
    testParsePostWithBody();
    testParseIncompleteInput();
    testParseGarbage();
    testSerializeResponse();

    testDeviceUuidFromPath();
    testQueryValue();

    testRoute();
    testCertificateNames();
    testCertificateByName();
    testDeviceCertificateRawAndWrapped();
    testDeviceCertificateErrors();
    testUnknownPathAndMethods();

    return phi::testing::report("thinq_route_tests");
}
