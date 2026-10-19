#include "thinq_ca.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>

namespace phicore::thinq::ipc {

namespace {

struct BioFree {
    void operator()(BIO *b) const { BIO_free(b); }
};
struct KeyFree {
    void operator()(EVP_PKEY *k) const { EVP_PKEY_free(k); }
};
struct CertFree {
    void operator()(X509 *c) const { X509_free(c); }
};
struct ReqFree {
    void operator()(X509_REQ *r) const { X509_REQ_free(r); }
};
struct NameFree {
    void operator()(X509_NAME *n) const { X509_NAME_free(n); }
};
using BioPtr = std::unique_ptr<BIO, BioFree>;
using KeyPtr = std::unique_ptr<EVP_PKEY, KeyFree>;
using CertPtr = std::unique_ptr<X509, CertFree>;
using ReqPtr = std::unique_ptr<X509_REQ, ReqFree>;

std::string lastOpenSslError()
{
    char buf[256];
    const unsigned long code = ERR_get_error();
    if (code == 0)
        return "unknown OpenSSL error";
    ERR_error_string_n(code, buf, sizeof(buf));
    ERR_clear_error();
    return buf;
}

bool fail(std::string *error, const std::string &what)
{
    if (error)
        *error = what + ": " + lastOpenSslError();
    return false;
}

KeyPtr generateRsa()
{
    return KeyPtr(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(2048)));
}

KeyPtr readKey(const std::string &pem)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return KeyPtr(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

CertPtr readCert(const std::string &pem)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    return CertPtr(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

std::string keyToPem(EVP_PKEY *key)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        return {};
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    return len > 0 ? std::string(data, static_cast<size_t>(len)) : std::string();
}

std::string certToPem(X509 *cert)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1)
        return {};
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    return len > 0 ? std::string(data, static_cast<size_t>(len)) : std::string();
}

bool randomSerial(X509 *cert)
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return false;
    bytes[0] &= 0x7F; // positive
    BIGNUM *bn = BN_bin2bn(bytes, sizeof(bytes), nullptr);
    if (!bn)
        return false;
    ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    const bool ok = BN_to_ASN1_INTEGER(bn, serial) != nullptr;
    BN_free(bn);
    return ok;
}

bool addExtension(X509 *cert, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext)
        return false;
    const bool ok = X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

/// The parts every certificate here shares: version 3, a random serial, the
/// validity from now.
bool prepare(X509 *cert, int days)
{
    return X509_set_version(cert, 2) == 1 && randomSerial(cert) && X509_gmtime_adj(X509_getm_notBefore(cert), 0)
           && X509_gmtime_adj(X509_getm_notAfter(cert), static_cast<long>(days) * 24 * 60 * 60);
}

bool setCommonName(X509_NAME *name, const std::string &cn)
{
    return X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                      reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0)
           == 1;
}

} // namespace

std::optional<KeyAndCert> mintCa(const std::string &commonName, int days, std::string *error)
{
    KeyPtr key = generateRsa();
    if (!key) {
        fail(error, "key generation failed");
        return std::nullopt;
    }
    CertPtr cert(X509_new());
    if (!cert || !prepare(cert.get(), days) || X509_set_pubkey(cert.get(), key.get()) != 1
        || !setCommonName(X509_get_subject_name(cert.get()), commonName)
        || X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get())) != 1
        || !addExtension(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE")
        || !addExtension(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign")
        || !addExtension(cert.get(), cert.get(), NID_subject_key_identifier, "hash")
        || X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) {
        fail(error, "building the CA certificate failed");
        return std::nullopt;
    }
    KeyAndCert out{keyToPem(key.get()), certToPem(cert.get())};
    if (out.keyPem.empty() || out.certPem.empty()) {
        fail(error, "writing the CA failed");
        return std::nullopt;
    }
    return out;
}

std::optional<KeyAndCert> mintServerLeaf(const KeyAndCert &ca, const std::string &dnsName, int days,
                                         std::string *error)
{
    KeyPtr caKey = readKey(ca.keyPem);
    CertPtr caCert = readCert(ca.certPem);
    if (!caKey || !caCert) {
        fail(error, "unreadable CA");
        return std::nullopt;
    }
    KeyPtr key = generateRsa();
    if (!key) {
        fail(error, "key generation failed");
        return std::nullopt;
    }
    const std::string san = "DNS:" + dnsName;
    CertPtr cert(X509_new());
    if (!cert || !prepare(cert.get(), days) || X509_set_pubkey(cert.get(), key.get()) != 1
        || !setCommonName(X509_get_subject_name(cert.get()), dnsName)
        || X509_set_issuer_name(cert.get(), X509_get_subject_name(caCert.get())) != 1
        || !addExtension(cert.get(), caCert.get(), NID_basic_constraints, "critical,CA:FALSE")
        || !addExtension(cert.get(), caCert.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment")
        || !addExtension(cert.get(), caCert.get(), NID_ext_key_usage, "serverAuth")
        || !addExtension(cert.get(), caCert.get(), NID_subject_alt_name, san.c_str())
        || !addExtension(cert.get(), caCert.get(), NID_authority_key_identifier, "keyid")
        || X509_sign(cert.get(), caKey.get(), EVP_sha256()) == 0) {
        fail(error, "building the server certificate failed");
        return std::nullopt;
    }
    KeyAndCert out{keyToPem(key.get()), certToPem(cert.get())};
    if (out.keyPem.empty() || out.certPem.empty()) {
        fail(error, "writing the server certificate failed");
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> signCsr(const KeyAndCert &ca, const std::string &csrPem, int days, std::string *error,
                                   std::string *subjectOut)
{
    KeyPtr caKey = readKey(ca.keyPem);
    CertPtr caCert = readCert(ca.certPem);
    if (!caKey || !caCert) {
        fail(error, "unreadable CA");
        return std::nullopt;
    }
    BioPtr bio(BIO_new_mem_buf(csrPem.data(), static_cast<int>(csrPem.size())));
    ReqPtr req(bio ? PEM_read_bio_X509_REQ(bio.get(), nullptr, nullptr, nullptr) : nullptr);
    if (!req) {
        fail(error, "unreadable CSR");
        return std::nullopt;
    }
    KeyPtr reqKey(X509_REQ_get_pubkey(req.get()));
    if (!reqKey || X509_REQ_verify(req.get(), reqKey.get()) != 1) {
        fail(error, "the CSR's signature does not verify");
        return std::nullopt;
    }

    CertPtr cert(X509_new());
    if (!cert || !prepare(cert.get(), days) || X509_set_pubkey(cert.get(), reqKey.get()) != 1
        || X509_set_subject_name(cert.get(), X509_REQ_get_subject_name(req.get())) != 1
        || X509_set_issuer_name(cert.get(), X509_get_subject_name(caCert.get())) != 1
        || !addExtension(cert.get(), caCert.get(), NID_basic_constraints, "critical,CA:FALSE")
        || !addExtension(cert.get(), caCert.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment")
        || !addExtension(cert.get(), caCert.get(), NID_ext_key_usage, "clientAuth")
        || !addExtension(cert.get(), caCert.get(), NID_authority_key_identifier, "keyid")
        || X509_sign(cert.get(), caKey.get(), EVP_sha256()) == 0) {
        fail(error, "signing the CSR failed");
        return std::nullopt;
    }
    if (subjectOut) {
        char *line = X509_NAME_oneline(X509_get_subject_name(cert.get()), nullptr, 0);
        *subjectOut = line ? line : "";
        OPENSSL_free(line);
    }
    std::string pem = certToPem(cert.get());
    if (pem.empty()) {
        fail(error, "writing the device certificate failed");
        return std::nullopt;
    }
    return pem;
}

std::string extractPemBlock(std::string_view text, std::string_view label)
{
    const std::string begin = "-----BEGIN " + std::string(label) + "-----";
    const std::string end = "-----END " + std::string(label) + "-----";
    const std::size_t from = text.find(begin);
    if (from == std::string_view::npos)
        return {};
    const std::size_t to = text.find(end, from);
    if (to == std::string_view::npos)
        return {};
    std::string_view raw = text.substr(from, to + end.size() - from);

    // Undo JSON string escapes; a raw PEM has none and passes through.
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            const char next = raw[i + 1];
            if (next == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (next == 'r') {
                ++i;
                continue;
            }
            if (next == '/' || next == '\\' || next == '"') {
                out.push_back(next);
                ++i;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    if (out.back() != '\n')
        out.push_back('\n');
    return out;
}

std::string certificateFingerprint(const std::string &certPem)
{
    CertPtr cert = readCert(certPem);
    if (!cert)
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &len) != 1)
        return {};
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kDigits[digest[i] >> 4]);
        out.push_back(kDigits[digest[i] & 0x0F]);
    }
    return out;
}

} // namespace phicore::thinq::ipc
