/* rlpg_wire — the RLPG protocol's shared wire formats.
 *
 * RLPG (Reticulous LXMF Proper Gation) is a personal mailbox node bound to
 * a single LXMF address. This header defines everything both sides speak:
 * the owner-signed certificate, the link frames (deposit / pickup / owner
 * session), and the service-message status codec. It lives in the lxmf
 * straddle so the LXMF client can use it without depending on the rlpg
 * straddle (rlpg depends on lxmf, not vice versa). Self-contained: own
 * minimal msgpack, crypto via rnsd.h only.
 *
 * Destination: one aspect, RLPG_ASPECT ("rlpg.mailbox"), for depositors
 * and owner alike. The first packet on any inbound link is a HELLO
 * carrying the certificate (or nil while uncertified) and a nonce; a
 * depositor verifies the cert and deposits anonymously. The owner answers
 * the HELLO with an AUTH frame (pubkey + signature over the nonce) whose
 * pubkey hashes to the served address, opening a pickup session. The
 * mailbox is a blind blob store — it signals nothing to anyone; delivery
 * confirmation is sourced end-to-end by the recipient (a delivery-confirm
 * LXMF field to the sender), not by the mailbox.
 *
 * The envelope `blob` is always an mR Identity token (rnsdEncryptFor)
 * for the final recipient — opaque to the mailbox. Its SHA-256 is the
 * transient id, the handle all bookkeeping uses; the mailbox
 * never sees inner LXMF message ids.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#define RLPG_ASPECT        "rlpg.mailbox"
#define RLPG_WIRE_VERSION  1

/* ── Certificate ─────────────────────────────────────────────────────
 * msgpack [version, owner_pubkey b64, node_id b16, service_dest b16,
 *          issued_at, expires_at, sig b64]; sig is Ed25519 by the owner
 * identity over the packed 6-element prefix array. Self-certifying: the
 * served LXMF address is derived from owner_pubkey, never carried. */

struct RlpgCert {
    uint8_t  version;
    uint8_t  owner_pubkey[64];   /* owner RNS identity public key blob */
    uint8_t  node_id[16];        /* mailbox node RNS identity hash */
    uint8_t  service_dest[16];   /* the node's own dest — the mailbox runs no
                                  * service identity; retained for wire-compat,
                                  * not used for trust (relay status rides the
                                  * pickup link, delivery is recipient-sourced) */
    uint32_t issued_at;          /* unix s */
    uint32_t expires_at;         /* unix s */
    uint8_t  sig[64];
};

/* Build + sign a certificate with the owner identity at storage path
 * `owner_identity_key` (fills c.owner_pubkey and c.sig). */
bool rlpgCertPack(RlpgCert& c, const char* owner_identity_key,
                  std::vector<uint8_t>& out);

/* Parse structure only — no signature check. */
bool rlpgCertParse(const uint8_t* p, size_t n, RlpgCert& out);

/* Verify the signature against the embedded owner pubkey and derive the
 * served lxmf.delivery destination hash from it. Expiry is NOT checked
 * here (clock policy is the caller's). */
bool rlpgCertVerify(const RlpgCert& c, uint8_t out_served_dest[16]);

/* ── Link frames ─────────────────────────────────────────────────────
 * Every link packet (and every resource body) is one msgpack array whose
 * element [0] is the frame type, so a single parser dispatches. */

enum RlpgFrameType : uint8_t {
    RLPG_FR_HELLO        = 1,  /* node→peer  [1, cert bin|nil, nonce b16, retain_days,
                                *             service_dest b16]
                                * service_dest rides outside the cert so the owner can
                                * build the FIRST cert (cert still nil then). It now
                                * carries the node's own dest (no service identity). */
    RLPG_FR_AUTH         = 2,  /* owner→node [2, pubkey b64, sig b64]
                                * sig over "RLPGAUTH" || nonce || node_id.
                                * The pubkey must hash to the served address —
                                * this is how the owner opens a pickup session;
                                * any other (or no) identify stays a depositor. */
    RLPG_FR_DEPOSIT      = 3,  /* peer→node  [3, blob bin, stamp bin|nil]
                                * fully anonymous — no receipt is ever sent. */
    RLPG_FR_DEPOSIT_ACK  = 4,  /* node→peer  [4, transient_id b32, code, reason] */
    RLPG_FR_PICKUP       = 5,  /* node→owner [5, transient_id b32, blob bin] */
    RLPG_FR_RX_PROOF     = 6,  /* owner→node [6, transient_id b32, variant] */
    RLPG_FR_OUTBOUND     = 7,  /* owner→node [7, dest b16, lxmf_hash b32, blob bin, timeout_s] */
    RLPG_FR_OUTBOUND_ACK = 8,  /* node→owner [8, lxmf_hash b32, code, reason] */
    RLPG_FR_CERT_SET     = 9,  /* owner→node [9, cert bin] */
    RLPG_FR_CERT_ACK     = 10, /* node→owner [10, ok] */
    RLPG_FR_PICKUP_DONE  = 11, /* node→owner [11, held_remaining] — initial stream drained */
    RLPG_FR_RELAY_STATUS = 12, /* node→owner [12, lxmf_hash b32, status_code uint]
                                * outbound-relay status for the owner's message,
                                * over the pickup link (status_code = LxmfStatus:
                                * REMOTE_RLPG / _FULL / _ERR / NO_RESPONSE). */
};

/* DEPOSIT_ACK / OUTBOUND_ACK codes. */
enum RlpgAckCode : uint8_t {
    RLPG_ACK_STORED    = 0,
    RLPG_ACK_DUPLICATE = 1,   /* counts as stored */
    RLPG_ACK_FULL      = 2,   /* quota — depositor maps to REMOTE_RLPG_FULL */
    RLPG_ACK_ERR       = 3,   /* see reason — depositor maps to REMOTE_RLPG_ERR */
};
enum RlpgAckReason : uint8_t {
    RLPG_RSN_NONE        = 0,
    RLPG_RSN_BAD_STAMP   = 1,
    RLPG_RSN_OVERSIZE    = 2,
    RLPG_RSN_STORE_FAIL  = 3,
    RLPG_RSN_UNCERTIFIED = 4,
    RLPG_RSN_MALFORMED   = 5,
};

/* RX_PROOF variants. */
enum RlpgRxVariant : uint8_t {
    RLPG_RX_OK      = 0,      /* decrypted + verified — delete and notify */
    RLPG_RX_DISCARD = 1,      /* garbage blob — delete, no notification */
};

/* One parsed frame. Absent optionals: zero-length vectors / all-zero. */
struct RlpgFrame {
    uint8_t              type = 0;
    std::vector<uint8_t> cert;          /* HELLO, CERT_SET */
    uint8_t              nonce[16] = {};/* HELLO */
    uint32_t             retain_days = 0;
    uint8_t              service_dest[16] = {};  /* HELLO */
    uint8_t              pubkey[64] = {};/* AUTH */
    uint8_t              sig[64] = {};   /* AUTH */
    std::vector<uint8_t> blob;          /* DEPOSIT, PICKUP, OUTBOUND */
    std::vector<uint8_t> stamp;         /* DEPOSIT */
    uint8_t              transient_id[32] = {};
    uint8_t              lxmf_hash[32] = {};
    uint8_t              dest[16] = {};  /* OUTBOUND */
    uint32_t             timeout_s = 0;  /* OUTBOUND, 0 = node default */
    uint8_t              code = 0;      /* acks / RX_PROOF variant / CERT_ACK ok */
    uint8_t              reason = 0;
    uint32_t             count = 0;     /* PICKUP_DONE held_remaining */
};

bool rlpgFrameParse(const uint8_t* p, size_t n, RlpgFrame& out);

/* Builders (each returns the packed frame). */
std::vector<uint8_t> rlpgBuildHello(const std::vector<uint8_t>& cert /* empty = uncertified */,
                                    const uint8_t nonce[16], uint32_t retain_days,
                                    const uint8_t service_dest[16]);
std::vector<uint8_t> rlpgBuildAuth(const uint8_t pubkey[64], const uint8_t sig[64]);
std::vector<uint8_t> rlpgBuildDeposit(const uint8_t* blob, size_t blob_len,
                                      const uint8_t* stamp, size_t stamp_len);
std::vector<uint8_t> rlpgBuildDepositAck(const uint8_t transient_id[32],
                                         uint8_t code, uint8_t reason);
std::vector<uint8_t> rlpgBuildPickup(const uint8_t transient_id[32],
                                     const uint8_t* blob, size_t blob_len);
std::vector<uint8_t> rlpgBuildRxProof(const uint8_t transient_id[32], uint8_t variant);
std::vector<uint8_t> rlpgBuildOutbound(const uint8_t dest[16], const uint8_t lxmf_hash[32],
                                       const uint8_t* blob, size_t blob_len,
                                       uint32_t timeout_s);
std::vector<uint8_t> rlpgBuildOutboundAck(const uint8_t lxmf_hash[32],
                                          uint8_t code, uint8_t reason);
std::vector<uint8_t> rlpgBuildCertSet(const std::vector<uint8_t>& cert);
std::vector<uint8_t> rlpgBuildCertAck(uint8_t ok);
std::vector<uint8_t> rlpgBuildPickupDone(uint32_t held_remaining);
std::vector<uint8_t> rlpgBuildRelayStatus(const uint8_t lxmf_hash[32], uint8_t status_code);

/* The AUTH signable: "RLPGAUTH" || nonce16 || node_id16. */
void rlpgAuthSignable(const uint8_t nonce[16], const uint8_t node_id[16],
                      uint8_t out[8 + 16 + 16]);

/* ── Mailbox announce app_data ───────────────────────────────────────
 * msgpack [version, served b16|nil, cert_issued_at, stamp_cost,
 * retain_days]. served nil + issued 0 = uncertified (reachability-only:
 * the node is routable so the owner can install a cert, but advertises
 * no mailbox service and clients must not store it). */

struct RlpgAnnounce {
    bool     certified;      /* served present + issued > 0 */
    uint8_t  served[16];
    uint32_t cert_issued_at;
    uint32_t stamp_cost;
    uint32_t retain_days;
};

std::vector<uint8_t> rlpgBuildMailboxAppData(const uint8_t* served /* nullptr = uncertified */,
                                             uint32_t cert_issued_at,
                                             uint32_t stamp_cost,
                                             uint32_t retain_days);
bool rlpgParseMailboxAppData(const uint8_t* p, size_t n, RlpgAnnounce& out);
