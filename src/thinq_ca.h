#pragma once

// The box's certificate authority for its appliances (docs/thinq-protocol.md
// §3, §6): minted once, it signs the leaf the HTTPS service and the broker
// present under the box's DNS name, and every module's CSR. A module trusts
// exactly this CA from bootstrap on and has no way to learn another short of
// a re-setup, which is why the validities are long.
//
// RSA 2048 throughout: the module families in question predate anything
// else, and a key type a module cannot verify is a module that never
// connects.

#include <optional>
#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

inline constexpr int kCaDays = 365 * 20;
inline constexpr int kLeafDays = 365 * 10;
inline constexpr int kDeviceDays = 365 * 20;

struct KeyAndCert {
    std::string keyPem;
    std::string certPem;
};

/// A self-signed CA with CN `commonName`, basicConstraints CA:TRUE.
std::optional<KeyAndCert> mintCa(const std::string &commonName, int days, std::string *error = nullptr);

/// A server leaf for `dnsName` (CN and SAN), signed by `ca`. One certificate
/// serves 443 and 8883.
std::optional<KeyAndCert> mintServerLeaf(const KeyAndCert &ca, const std::string &dnsName, int days,
                                         std::string *error = nullptr);

/// Signs a PEM CSR with `ca`, keeping the CSR's subject. `subjectOut`, when
/// given, receives the subject in one-line form.
std::optional<std::string> signCsr(const KeyAndCert &ca, const std::string &csrPem, int days,
                                   std::string *error = nullptr, std::string *subjectOut = nullptr);

/// The first `-----BEGIN <label>-----` … `-----END <label>-----` block in
/// `text`, with JSON escapes (`\n`, `\/`) undone - the CSR may arrive raw or
/// inside a JSON string (§3.3). Empty when there is none.
std::string extractPemBlock(std::string_view text, std::string_view label);

/// Lower-case hex SHA-256 of the DER form of a PEM certificate; empty on a
/// malformed PEM.
std::string certificateFingerprint(const std::string &certPem);

} // namespace phicore::thinq::ipc
