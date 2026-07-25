/* rlpg_wire — RLPG shared wire formats: certificate, link frames, status
 * codec. See rlpg_wire.h for the protocol overview. Deliberately
 * self-contained: its own minimal msgpack (the subset these frames use —
 * fixarray, bin8/16, uint, nil) rather than a dependency on lxmf.cpp's
 * file-local codec, and crypto only through rnsd.h. */

#include "rlpg_wire.h"
#include "rnsd.h"

#include <cstring>
#include <cstdio>

/* ─────────────── minimal msgpack ─────────────── */

static void mpArr(std::vector<uint8_t>& o, size_t n)
{
    /* All RLPG frames are < 16 elements. */
    o.push_back((uint8_t)(0x90 | (n & 0x0F)));
}

static void mpNil(std::vector<uint8_t>& o) { o.push_back(0xC0); }

static void mpUint(std::vector<uint8_t>& o, uint32_t v)
{
    if (v < 0x80)            { o.push_back((uint8_t)v); }
    else if (v <= 0xFF)      { o.push_back(0xCC); o.push_back((uint8_t)v); }
    else if (v <= 0xFFFF)    { o.push_back(0xCD); o.push_back(v >> 8); o.push_back(v & 0xFF); }
    else                     { o.push_back(0xCE); o.push_back(v >> 24); o.push_back((v >> 16) & 0xFF);
                               o.push_back((v >> 8) & 0xFF); o.push_back(v & 0xFF); }
}

static void mpBin(std::vector<uint8_t>& o, const uint8_t* p, size_t n)
{
    if (n <= 0xFF)      { o.push_back(0xC4); o.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { o.push_back(0xC5); o.push_back(n >> 8); o.push_back(n & 0xFF); }
    else                { o.push_back(0xC6); o.push_back((n >> 24) & 0xFF); o.push_back((n >> 16) & 0xFF);
                          o.push_back((n >> 8) & 0xFF); o.push_back(n & 0xFF); }
    o.insert(o.end(), p, p + n);
}

/* Walking parser state. */
struct MpIn { const uint8_t* p; size_t n; size_t i; };

static bool mpInUint(MpIn& s, uint32_t& v)
{
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b < 0x80)      { v = b; return true; }
    auto need = [&](size_t k) { return s.i + k <= s.n; };
    if (b == 0xCC) { if (!need(1)) return false; v = s.p[s.i]; s.i += 1; return true; }
    if (b == 0xCD) { if (!need(2)) return false; v = ((uint32_t)s.p[s.i] << 8) | s.p[s.i+1]; s.i += 2; return true; }
    if (b == 0xCE) { if (!need(4)) return false;
                     v = ((uint32_t)s.p[s.i] << 24) | ((uint32_t)s.p[s.i+1] << 16) |
                         ((uint32_t)s.p[s.i+2] << 8) | s.p[s.i+3]; s.i += 4; return true; }
    return false;
}

/* nil → ok=true with len 0 and *is_nil set. */
static bool mpInBin(MpIn& s, const uint8_t*& p, size_t& len, bool* is_nil = nullptr)
{
    if (is_nil) *is_nil = false;
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b == 0xC0) { p = nullptr; len = 0; if (is_nil) *is_nil = true; return true; }
    size_t l;
    if      (b == 0xC4) { if (s.i + 1 > s.n) return false; l = s.p[s.i]; s.i += 1; }
    else if (b == 0xC5) { if (s.i + 2 > s.n) return false; l = ((size_t)s.p[s.i] << 8) | s.p[s.i+1]; s.i += 2; }
    else if (b == 0xC6) { if (s.i + 4 > s.n) return false;
                          l = ((size_t)s.p[s.i] << 24) | ((size_t)s.p[s.i+1] << 16) |
                              ((size_t)s.p[s.i+2] << 8) | s.p[s.i+3]; s.i += 4; }
    else if (b >= 0xA0 && b <= 0xBF) { l = b & 0x1F; }                     /* fixstr */
    else if (b == 0xD9) { if (s.i + 1 > s.n) return false; l = s.p[s.i]; s.i += 1; } /* str8 */
    else return false;
    if (s.i + l > s.n) return false;
    p = s.p + s.i; len = l; s.i += l;
    return true;
}

static bool mpInArr(MpIn& s, size_t& cnt)
{
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b >= 0x90 && b <= 0x9F) { cnt = b & 0x0F; return true; }
    if (b == 0xDC) { if (s.i + 2 > s.n) return false;
                     cnt = ((size_t)s.p[s.i] << 8) | s.p[s.i+1]; s.i += 2; return true; }
    return false;
}

/* Fixed-size bin element into a fixed buffer; wrong length = fail. */
static bool mpInBinN(MpIn& s, uint8_t* out, size_t want)
{
    const uint8_t* p; size_t l;
    if (!mpInBin(s, p, l) || l != want) return false;
    std::memcpy(out, p, want);
    return true;
}

/* ─────────────── certificate ─────────────── */

/* Pack the signable 6-element prefix. */
static void certPackPrefix(const RlpgCert& c, std::vector<uint8_t>& o, size_t elems)
{
    mpArr(o, elems);
    mpUint(o, c.version);
    mpBin(o, c.owner_pubkey, 64);
    mpBin(o, c.node_id, 16);
    mpBin(o, c.service_dest, 16);
    mpUint(o, c.issued_at);
    mpUint(o, c.expires_at);
}

bool rlpgCertPack(RlpgCert& c, const char* owner_identity_key,
                  std::vector<uint8_t>& out)
{
    c.version = RLPG_WIRE_VERSION;
    if (!rnsdIdentityPubkey(owner_identity_key, c.owner_pubkey))
        return false;
    std::vector<uint8_t> signable;
    certPackPrefix(c, signable, 6);
    if (!rnsdSign(owner_identity_key, signable.data(), signable.size(), c.sig))
        return false;
    out.clear();
    certPackPrefix(c, out, 7);
    mpBin(out, c.sig, 64);
    return true;
}

bool rlpgCertParse(const uint8_t* p, size_t n, RlpgCert& out)
{
    MpIn s{p, n, 0};
    size_t cnt; uint32_t v;
    if (!mpInArr(s, cnt) || cnt < 7) return false;
    if (!mpInUint(s, v)) return false;
    out.version = (uint8_t)v;
    if (out.version != RLPG_WIRE_VERSION) return false;
    if (!mpInBinN(s, out.owner_pubkey, 64)) return false;
    if (!mpInBinN(s, out.node_id, 16)) return false;
    if (!mpInBinN(s, out.service_dest, 16)) return false;
    if (!mpInUint(s, out.issued_at)) return false;
    if (!mpInUint(s, out.expires_at)) return false;
    if (!mpInBinN(s, out.sig, 64)) return false;
    return true;
}

bool rlpgCertVerify(const RlpgCert& c, uint8_t out_served_dest[16])
{
    std::vector<uint8_t> signable;
    certPackPrefix(c, signable, 6);
    if (!rnsdVerify(c.owner_pubkey, signable.data(), signable.size(), c.sig))
        return false;
    return rnsdDestinationHashFromPubkey(c.owner_pubkey, "lxmf", "delivery",
                                         out_served_dest);
}

/* ─────────────── frames ─────────────── */

std::vector<uint8_t> rlpgBuildHello(const std::vector<uint8_t>& cert,
                                    const uint8_t nonce[16], uint32_t retain_days,
                                    const uint8_t service_dest[16])
{
    std::vector<uint8_t> o;
    mpArr(o, 5);
    mpUint(o, RLPG_FR_HELLO);
    if (cert.empty()) mpNil(o);
    else              mpBin(o, cert.data(), cert.size());
    mpBin(o, nonce, 16);
    mpUint(o, retain_days);
    mpBin(o, service_dest, 16);
    return o;
}

std::vector<uint8_t> rlpgBuildAuth(const uint8_t pubkey[64], const uint8_t sig[64])
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, RLPG_FR_AUTH);
    mpBin(o, pubkey, 64);
    mpBin(o, sig, 64);
    return o;
}

std::vector<uint8_t> rlpgBuildDeposit(const uint8_t* blob, size_t blob_len,
                                      const uint8_t* stamp, size_t stamp_len)
{
    std::vector<uint8_t> o;
    o.reserve(blob_len + stamp_len + 40);
    mpArr(o, 3);
    mpUint(o, RLPG_FR_DEPOSIT);
    mpBin(o, blob, blob_len);
    if (stamp && stamp_len) mpBin(o, stamp, stamp_len); else mpNil(o);
    return o;
}

std::vector<uint8_t> rlpgBuildDepositAck(const uint8_t transient_id[32],
                                         uint8_t code, uint8_t reason)
{
    std::vector<uint8_t> o;
    mpArr(o, 4);
    mpUint(o, RLPG_FR_DEPOSIT_ACK);
    mpBin(o, transient_id, 32);
    mpUint(o, code);
    mpUint(o, reason);
    return o;
}

std::vector<uint8_t> rlpgBuildPickup(const uint8_t transient_id[32],
                                     const uint8_t* blob, size_t blob_len)
{
    std::vector<uint8_t> o;
    o.reserve(blob_len + 48);
    mpArr(o, 3);
    mpUint(o, RLPG_FR_PICKUP);
    mpBin(o, transient_id, 32);
    mpBin(o, blob, blob_len);
    return o;
}

std::vector<uint8_t> rlpgBuildRxProof(const uint8_t transient_id[32], uint8_t variant)
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, RLPG_FR_RX_PROOF);
    mpBin(o, transient_id, 32);
    mpUint(o, variant);
    return o;
}

std::vector<uint8_t> rlpgBuildOutbound(const uint8_t dest[16], const uint8_t lxmf_hash[32],
                                       const uint8_t* blob, size_t blob_len,
                                       uint32_t timeout_s)
{
    std::vector<uint8_t> o;
    o.reserve(blob_len + 72);
    mpArr(o, 5);
    mpUint(o, RLPG_FR_OUTBOUND);
    mpBin(o, dest, 16);
    mpBin(o, lxmf_hash, 32);
    mpBin(o, blob, blob_len);
    mpUint(o, timeout_s);
    return o;
}

std::vector<uint8_t> rlpgBuildOutboundAck(const uint8_t lxmf_hash[32],
                                          uint8_t code, uint8_t reason)
{
    std::vector<uint8_t> o;
    mpArr(o, 4);
    mpUint(o, RLPG_FR_OUTBOUND_ACK);
    mpBin(o, lxmf_hash, 32);
    mpUint(o, code);
    mpUint(o, reason);
    return o;
}

std::vector<uint8_t> rlpgBuildCertSet(const std::vector<uint8_t>& cert)
{
    std::vector<uint8_t> o;
    mpArr(o, 2);
    mpUint(o, RLPG_FR_CERT_SET);
    mpBin(o, cert.data(), cert.size());
    return o;
}

std::vector<uint8_t> rlpgBuildCertAck(uint8_t ok)
{
    std::vector<uint8_t> o;
    mpArr(o, 2);
    mpUint(o, RLPG_FR_CERT_ACK);
    mpUint(o, ok);
    return o;
}

std::vector<uint8_t> rlpgBuildPickupDone(uint32_t held_remaining)
{
    std::vector<uint8_t> o;
    mpArr(o, 2);
    mpUint(o, RLPG_FR_PICKUP_DONE);
    mpUint(o, held_remaining);
    return o;
}

std::vector<uint8_t> rlpgBuildRelayStatus(const uint8_t lxmf_hash[32], uint8_t status_code)
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, RLPG_FR_RELAY_STATUS);
    mpBin(o, lxmf_hash, 32);
    mpUint(o, status_code);
    return o;
}

void rlpgAuthSignable(const uint8_t nonce[16], const uint8_t node_id[16],
                      uint8_t out[8 + 16 + 16])
{
    std::memcpy(out, "RLPGAUTH", 8);
    std::memcpy(out + 8, nonce, 16);
    std::memcpy(out + 24, node_id, 16);
}

bool rlpgFrameParse(const uint8_t* p, size_t n, RlpgFrame& out)
{
    MpIn s{p, n, 0};
    size_t cnt; uint32_t v;
    const uint8_t* bp; size_t bl; bool nil;
    if (!mpInArr(s, cnt) || cnt < 2) return false;
    if (!mpInUint(s, v)) return false;
    out.type = (uint8_t)v;

    switch (out.type) {
    case RLPG_FR_HELLO:
        if (cnt < 5) return false;
        if (!mpInBin(s, bp, bl, &nil)) return false;
        if (!nil) out.cert.assign(bp, bp + bl);
        if (!mpInBinN(s, out.nonce, 16)) return false;
        if (!mpInUint(s, out.retain_days)) return false;
        if (!mpInBinN(s, out.service_dest, 16)) return false;
        return true;
    case RLPG_FR_AUTH:
        if (cnt < 3) return false;
        if (!mpInBinN(s, out.pubkey, 64)) return false;
        if (!mpInBinN(s, out.sig, 64)) return false;
        return true;
    case RLPG_FR_DEPOSIT:
        if (cnt < 3) return false;
        if (!mpInBin(s, bp, bl) || bl == 0) return false;
        out.blob.assign(bp, bp + bl);
        if (!mpInBin(s, bp, bl, &nil)) return false;
        if (!nil && bl) out.stamp.assign(bp, bp + bl);
        return true;
    case RLPG_FR_DEPOSIT_ACK:
        if (cnt < 4) return false;
        if (!mpInBinN(s, out.transient_id, 32)) return false;
        if (!mpInUint(s, v)) return false;
        out.code = (uint8_t)v;
        if (!mpInUint(s, v)) return false;
        out.reason = (uint8_t)v;
        return true;
    case RLPG_FR_PICKUP:
        if (cnt < 3) return false;
        if (!mpInBinN(s, out.transient_id, 32)) return false;
        if (!mpInBin(s, bp, bl) || bl == 0) return false;
        out.blob.assign(bp, bp + bl);
        return true;
    case RLPG_FR_RX_PROOF:
        if (cnt < 3) return false;
        if (!mpInBinN(s, out.transient_id, 32)) return false;
        if (!mpInUint(s, v)) return false;
        out.code = (uint8_t)v;
        return true;
    case RLPG_FR_OUTBOUND:
        if (cnt < 5) return false;
        if (!mpInBinN(s, out.dest, 16)) return false;
        if (!mpInBinN(s, out.lxmf_hash, 32)) return false;
        if (!mpInBin(s, bp, bl) || bl == 0) return false;
        out.blob.assign(bp, bp + bl);
        if (!mpInUint(s, out.timeout_s)) return false;
        return true;
    case RLPG_FR_OUTBOUND_ACK:
        if (cnt < 4) return false;
        if (!mpInBinN(s, out.lxmf_hash, 32)) return false;
        if (!mpInUint(s, v)) return false;
        out.code = (uint8_t)v;
        if (!mpInUint(s, v)) return false;
        out.reason = (uint8_t)v;
        return true;
    case RLPG_FR_CERT_SET:
        if (!mpInBin(s, bp, bl) || bl == 0) return false;
        out.cert.assign(bp, bp + bl);
        return true;
    case RLPG_FR_CERT_ACK:
        if (!mpInUint(s, v)) return false;
        out.code = (uint8_t)v;
        return true;
    case RLPG_FR_PICKUP_DONE:
        if (!mpInUint(s, out.count)) return false;
        return true;
    case RLPG_FR_RELAY_STATUS:
        if (cnt < 3) return false;
        if (!mpInBinN(s, out.lxmf_hash, 32)) return false;
        if (!mpInUint(s, v)) return false;
        out.code = (uint8_t)v;
        return true;
    default:
        return false;   /* unknown frame — caller drops it */
    }
}

/* ─────────────── mailbox announce app_data ─────────────── */

std::vector<uint8_t> rlpgBuildMailboxAppData(const uint8_t* served,
                                             uint32_t cert_issued_at,
                                             uint32_t stamp_cost,
                                             uint32_t retain_days)
{
    std::vector<uint8_t> o;
    mpArr(o, 5);
    mpUint(o, RLPG_WIRE_VERSION);
    if (served) mpBin(o, served, 16); else mpNil(o);
    mpUint(o, served ? cert_issued_at : 0);
    mpUint(o, stamp_cost);
    mpUint(o, retain_days);
    return o;
}

bool rlpgParseMailboxAppData(const uint8_t* p, size_t n, RlpgAnnounce& out)
{
    out = RlpgAnnounce{};
    MpIn s{p, n, 0};
    size_t cnt; uint32_t v;
    if (!mpInArr(s, cnt) || cnt < 5) return false;
    if (!mpInUint(s, v) || v != RLPG_WIRE_VERSION) return false;
    const uint8_t* bp; size_t bl; bool nil;
    if (!mpInBin(s, bp, bl, &nil)) return false;
    bool have_served = !nil && bl == 16;
    if (have_served) std::memcpy(out.served, bp, 16);
    if (!mpInUint(s, out.cert_issued_at)) return false;
    if (!mpInUint(s, out.stamp_cost)) return false;
    if (!mpInUint(s, out.retain_days)) return false;
    out.certified = have_served && out.cert_issued_at > 0;
    return true;
}
