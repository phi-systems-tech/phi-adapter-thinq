// phi-thinq-routed: the HTTPS half of the cloud the appliances are pointed
// at (docs/thinq-protocol.md §3, §6). It answers `/route`, hands out the
// box CA as both trust anchors, and signs each module's CSR. The CA is the
// box's, minted by phi-mosquitto (phi-mosquitto-tls) under /var/lib/phi/tls
// and shared with the broker's 8883 listener; only the leaf this service
// presents on 443 is minted here, into the state directory.
//
// Small and sequential on purpose: one appliance enrols at a time, each
// request is a few hundred bytes, and a blocking accept loop with socket
// timeouts is the whole of what that needs. It runs as the `phi` user with
// the capability to bind 443, see debian/phi-thinq-routed.service.

#include "thinq_ca.h"
#include "thinq_route.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace phicore::thinq::ipc;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int)
{
    g_stop = 1;
}

void logLine(const char *level, const std::string &text)
{
    std::fprintf(stderr, "[%s] %s\n", level, text.c_str());
    std::fflush(stderr);
}

struct Options {
    std::string host;
    std::string stateDir = "/var/lib/phi/thinq";
    std::string caDir = "/var/lib/phi/tls";
    int port = 443;
    int apiPort = 443;
    int mqttPort = 8883;
};

void usage()
{
    std::fprintf(stderr,
                 "usage: phi-thinq-routed --host <dns-name> [--state-dir <dir>] [--ca-dir <dir>] "
                 "[--port 443] [--api-port 443] [--mqtt-port 8883]\n"
                 "  --host      the DNS name the appliances resolve to this box; it is the CN of the\n"
                 "              server certificate and what /route answers\n"
                 "  --state-dir where the server leaf and the issued device certificates live\n"
                 "  --ca-dir    where the box CA is (ca.key, ca.crt; phi-mosquitto-tls mints it)\n"
                 "  --api-port  what /route advertises as apiServer (usually --port)\n");
}

std::optional<Options> parseOptions(int argc, char **argv)
{
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--host" && hasValue) {
            out.host = argv[++i];
        } else if (arg == "--state-dir" && hasValue) {
            out.stateDir = argv[++i];
        } else if (arg == "--ca-dir" && hasValue) {
            out.caDir = argv[++i];
        } else if (arg == "--port" && hasValue) {
            out.port = std::atoi(argv[++i]);
            out.apiPort = out.port;
        } else if (arg == "--api-port" && hasValue) {
            out.apiPort = std::atoi(argv[++i]);
        } else if (arg == "--mqtt-port" && hasValue) {
            out.mqttPort = std::atoi(argv[++i]);
        } else {
            usage();
            return std::nullopt;
        }
    }
    if (const char *env = std::getenv("PHI_THINQ_HOST"); out.host.empty() && env)
        out.host = env;
    if (const char *env = std::getenv("PHI_THINQ_STATE_DIR"); env && std::string(env) != "")
        out.stateDir = env;
    if (const char *env = std::getenv("PHI_TLS_DIR"); env && std::string(env) != "")
        out.caDir = env;
    if (out.port <= 0 || out.apiPort <= 0 || out.mqttPort <= 0) {
        usage();
        return std::nullopt;
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

bool writeFile(const std::string &path, const std::string &data, mode_t mode)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    ::fchmod(fd, mode);
    ::close(fd);
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

/// The box CA, which this service does not own: phi-mosquitto mints it before
/// the broker starts, so that the 8883 listener and this service vouch for
/// the same anchor. Missing is a configuration error, not something to paper
/// over with a second CA the broker would never trust.
std::optional<KeyAndCert> loadCa(const Options &o)
{
    KeyAndCert ca{readFile(o.caDir + "/ca.key"), readFile(o.caDir + "/ca.crt")};
    if (ca.keyPem.empty() || ca.certPem.empty()) {
        logLine("error", "no box CA in " + o.caDir + " (ca.key, ca.crt): phi-mosquitto's phi-mosquitto-tls mints it, "
                             "and the key must be readable by this user");
        return std::nullopt;
    }
    logLine("info", "using the box CA, fingerprint " + certificateFingerprint(ca.certPem));
    return ca;
}

/// The leaf this service presents: minted whenever the host name or the CA
/// behind it changed.
std::optional<KeyAndCert> loadOrMintLeaf(const Options &o, const KeyAndCert &ca)
{
    KeyAndCert leaf{readFile(o.stateDir + "/server.key"), readFile(o.stateDir + "/server.crt")};
    const std::string host = readFile(o.stateDir + "/server.host");
    const std::string issuer = readFile(o.stateDir + "/server.ca");
    const std::string caFingerprint = certificateFingerprint(ca.certPem);
    if (!leaf.keyPem.empty() && !leaf.certPem.empty() && host == o.host && issuer == caFingerprint)
        return leaf;
    std::string error;
    const std::optional<KeyAndCert> minted = mintServerLeaf(ca, o.host, kLeafDays, &error);
    if (!minted) {
        logLine("error", "cannot mint the server certificate: " + error);
        return std::nullopt;
    }
    if (!writeFile(o.stateDir + "/server.key", minted->keyPem, 0600) || !writeFile(o.stateDir + "/server.crt", minted->certPem, 0644)
        || !writeFile(o.stateDir + "/server.host", o.host, 0644) || !writeFile(o.stateDir + "/server.ca", caFingerprint, 0644)) {
        logLine("error", "cannot write the server certificate into " + o.stateDir);
        return std::nullopt;
    }
    logLine("info", "minted the server certificate for " + o.host);
    return minted;
}

struct CtxFree {
    void operator()(SSL_CTX *c) const { SSL_CTX_free(c); }
};
struct SslFree {
    void operator()(SSL *s) const { SSL_free(s); }
};

/// The profile of §6: TLS 1.2 only, the legacy suites the modules offer, and
/// the renegotiation leniency the RTL8720cm needs.
std::unique_ptr<SSL_CTX, CtxFree> makeContext(const KeyAndCert &leaf, const KeyAndCert &ca)
{
    std::unique_ptr<SSL_CTX, CtxFree> ctx(SSL_CTX_new(TLS_server_method()));
    if (!ctx)
        return nullptr;
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_security_level(ctx.get(), 0);
    SSL_CTX_set_options(ctx.get(), SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION | SSL_OP_LEGACY_SERVER_CONNECT
                                       | SSL_OP_NO_TICKET);
    SSL_CTX_set_cipher_list(ctx.get(), "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:ECDHE-RSA-AES128-GCM-SHA256:"
                                       "ECDHE-RSA-AES256-GCM-SHA384:AES128-SHA:AES256-SHA:AES128-GCM-SHA256:"
                                       "AES256-GCM-SHA384:@SECLEVEL=0");

    BIO *certBio = BIO_new_mem_buf(leaf.certPem.data(), static_cast<int>(leaf.certPem.size()));
    X509 *cert = certBio ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(certBio);
    BIO *keyBio = BIO_new_mem_buf(leaf.keyPem.data(), static_cast<int>(leaf.keyPem.size()));
    EVP_PKEY *key = keyBio ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(keyBio);
    BIO *caBio = BIO_new_mem_buf(ca.certPem.data(), static_cast<int>(ca.certPem.size()));
    X509 *caCert = caBio ? PEM_read_bio_X509(caBio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(caBio);

    bool ok = cert && key && caCert && SSL_CTX_use_certificate(ctx.get(), cert) == 1
              && SSL_CTX_use_PrivateKey(ctx.get(), key) == 1 && SSL_CTX_check_private_key(ctx.get()) == 1;
    // The chain, for a module that wants the anchor on the wire too.
    if (ok && SSL_CTX_add1_chain_cert(ctx.get(), caCert) != 1)
        ok = false;
    X509_free(cert);
    EVP_PKEY_free(key);
    X509_free(caCert);
    return ok ? std::move(ctx) : nullptr;
}

int listenOn(int port)
{
    const int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    int zero = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string peerName(const sockaddr_storage &addr)
{
    char buf[INET6_ADDRSTRLEN] = {0};
    if (addr.ss_family == AF_INET6) {
        const auto *a6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr))
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], buf, sizeof(buf));
        else
            inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
    } else if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(&addr)->sin_addr, buf, sizeof(buf));
    }
    return buf;
}

void serve(int client, SSL_CTX *ctx, const RouteService &service, const std::string &peer)
{
    timeval tv{};
    tv.tv_sec = 10;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::unique_ptr<SSL, SslFree> ssl(SSL_new(ctx));
    if (!ssl || SSL_set_fd(ssl.get(), client) != 1)
        return;
    if (SSL_accept(ssl.get()) != 1) {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        ERR_clear_error();
        logLine("warn", peer + ": TLS handshake failed: " + buf);
        return;
    }
    const char *version = SSL_get_version(ssl.get());
    const char *cipher = SSL_get_cipher_name(ssl.get());

    std::string raw;
    std::optional<HttpRequest> request;
    while (!request && raw.size() < 65536) {
        char buf[4096];
        const int n = SSL_read(ssl.get(), buf, sizeof(buf));
        if (n <= 0)
            break;
        raw.append(buf, static_cast<std::size_t>(n));
        request = parseHttpRequest(raw);
    }
    if (!request || request->method.empty()) {
        logLine("warn", peer + ": no HTTP request (" + std::to_string(raw.size()) + " bytes)");
        return;
    }
    const HttpResponse response = service.handle(*request);
    logLine("info", peer + ": " + request->method + " " + request->path + (request->query.empty() ? "" : "?" + request->query)
                        + " -> " + std::to_string(response.status) + " (" + version + ", " + cipher + ")");
    const std::string wire = serializeHttpResponse(response);
    std::size_t done = 0;
    while (done < wire.size()) {
        const int n = SSL_write(ssl.get(), wire.data() + done, static_cast<int>(wire.size() - done));
        if (n <= 0)
            break;
        done += static_cast<std::size_t>(n);
    }
    SSL_shutdown(ssl.get());
}

} // namespace

int main(int argc, char **argv)
{
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options)
        return 2;
    const Options &o = *options;
    // Not configured is not a failure: the unit would be restarted every five
    // seconds for as long as nobody has set the name. It is said once and the
    // process ends; `phi-cli backend restart thinq@main` after setting it.
    if (o.host.empty()) {
        logLine("info", "no host name configured (PHI_THINQ_HOST in /etc/default/phi-adapter-thinq); "
                        "nothing to serve until it is set");
        return 0;
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    ::mkdir(o.stateDir.c_str(), 0750);
    ::mkdir((o.stateDir + "/devices").c_str(), 0750);

    const std::optional<KeyAndCert> ca = loadCa(o);
    if (!ca)
        return 1;
    const std::optional<KeyAndCert> leaf = loadOrMintLeaf(o, *ca);
    if (!leaf)
        return 1;
    const auto ctx = makeContext(*leaf, *ca);
    if (!ctx) {
        logLine("error", "cannot build the TLS context");
        return 1;
    }

    RouteService service(RouteConfig{o.host, o.apiPort, o.mqttPort}, *ca);
    service.onIssued([&o](const std::string &uuid, const std::string &certPem, const std::string &subject) {
        // A uuid in a path is untrusted: keep it to a file name.
        std::string safe;
        for (const char c : uuid)
            safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
        const std::string path = o.stateDir + "/devices/" + safe + ".crt";
        if (!writeFile(path, certPem, 0644))
            logLine("warn", "cannot record the certificate of " + uuid + " at " + path);
        logLine("info", "issued a certificate to " + uuid + " (" + subject + ")");
    });

    const int listener = listenOn(o.port);
    if (listener < 0) {
        logLine("error", "cannot listen on port " + std::to_string(o.port) + ": " + std::strerror(errno));
        return 1;
    }
    logLine("info", "serving /route for " + o.host + " on port " + std::to_string(o.port) + "; apiServer :"
                        + std::to_string(o.apiPort) + ", mqttServer :" + std::to_string(o.mqttPort));

    while (!g_stop) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        const int client = ::accept4(listener, reinterpret_cast<sockaddr *>(&addr), &len, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            logLine("warn", std::string("accept failed: ") + std::strerror(errno));
            continue;
        }
        serve(client, ctx.get(), service, peerName(addr));
        ::close(client);
    }
    ::close(listener);
    logLine("info", "stopped");
    return 0;
}
