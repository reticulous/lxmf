/**
 * lxmf — LXMF messaging protocol task (Phase 4a).
 *
 * Architecture: docs/plans/lxmf.md. Storage is the API; the firmware
 * subscribes to its own keys and reacts. Wire format is LXMF 0.9.8:
 *
 *   destination_hash(16) || source_hash(16) || Ed25519 sig(64) ||
 *   msgpack([timestamp, title, content, fields, stamp?])
 *
 * Signature scope is `dest||src||packed||SHA-256(dest||src||packed)` —
 * the SHA-256 of the data is signed alongside it. message_id =
 * SHA-256(dest||src||packed). transient_id (used by propagation stores)
 * is distinct and not used in Phase 4a.
 */
#include "lxmf.h"
#include "diptych.h"
#include "ports.h"
#include "rnsd.h"     /* SHA-256, sign/verify, dest-hash, recall, request_path */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>
#include <array>

static const char* TAG = "lxmf";

/* ─────────────── constants ─────────────── */

#define LXMF_VERSION 1

/* docs/plans/lxmf.md §2.1 */
constexpr size_t LXMF_DEST_HASH_LEN = 16;
constexpr size_t LXMF_SIG_LEN       = 64;
constexpr size_t LXMF_OVERHEAD      = LXMF_DEST_HASH_LEN * 2 + LXMF_SIG_LEN;  /* 112 */
constexpr size_t LXMF_OPP_CONTENT_BUDGET = 311;  /* single RNS packet plaintext */

/* Phase 4a: one identity for now. Schema is an array (id.<n>) but
 * multi-identity features land in Phase 8. */
#define LXMF_MAX_IDENTITIES 4

/* In-RAM dedup hashlist depth — last N inbound message_ids. Cheap
 * defence against accidental duplicates from path-flapping repeats.
 * Storage existence is the authoritative dedup. */
#define LXMF_DEDUP_RING 64

/* LXMF field registry keys (msgpack int keys in `fields` map). See
 * docs/plans/lxmf.md §2.5. */
enum : int {
    LXMF_FIELD_EMBEDDED_LXMS    = 0x01,
    LXMF_FIELD_TELEMETRY        = 0x02,
    LXMF_FIELD_TELEMETRY_STREAM = 0x03,
    LXMF_FIELD_ICON_APPEARANCE  = 0x04,
    LXMF_FIELD_FILE_ATTACHMENTS = 0x05,
    LXMF_FIELD_IMAGE            = 0x06,
    LXMF_FIELD_AUDIO            = 0x07,
    LXMF_FIELD_THREAD           = 0x08,
    LXMF_FIELD_COMMANDS         = 0x09,
    LXMF_FIELD_RESULTS          = 0x0A,
    LXMF_FIELD_GROUP            = 0x0B,
    LXMF_FIELD_TICKET           = 0x0C,
    LXMF_FIELD_EVENT            = 0x0D,
    LXMF_FIELD_RNR_REFS         = 0x0E,
    LXMF_FIELD_RENDERER         = 0x0F,
};

/* ─────────────── state ─────────────── */

struct outbound_t {
    bool        used;
    uint16_t    send_id;
    std::string msg_key;        /* "o_<...>" — the local outbound key under id.<n>.msgs */
};

struct lxmf_id_t {
    bool          used;
    int         index;                              /* 0..LXMF_MAX_IDENTITIES-1 */
    int         handle;                             /* RNSD_PORT_DEST handle, -1 = closed */
    std::string identity_key;                       /* storage path: "secrets.lxmf.id.<n>.privkey" */
    uint8_t     dest_hash[RNSD_DEST_HASH_LEN];      /* 16-byte LXMF delivery destination hash */
    uint16_t    next_send_id;
    outbound_t  outboxes[8];                        /* in-flight send_id → message-key tracking */

    /* Stats (mirrored to lxmf.id.<n>.stats.* at 1 Hz). */
    uint32_t      sent;
    uint32_t      received;
    uint32_t      pending;
    uint32_t      failed;

    /* Monotonic tick of last successful announce. 0 = never announced
     * this session. Used to schedule periodic re-announces. */
    TickType_t    last_announce_tick;
};

static TaskHandle_t s_task = nullptr;
static lxmf_id_t    s_ids[LXMF_MAX_IDENTITIES];

/* Inbound dedup ring (recent message_ids, hex64). */
static std::string s_dedup_ring[LXMF_DEDUP_RING];
static int         s_dedup_head = 0;

/* Forward declarations for per-identity cmd-subscription helpers (defined
 * below alongside the rest of the cmd-handler glue). Called from the
 * identity lifecycle. */
static void subscribePerIdCmds(int n);
static void unsubscribePerIdCmds(int n);

/* ─────────────── small helpers ─────────────── */

static std::string idPath(int n, const char* tail)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "s.lxmf.id.%d%s%s", n, *tail ? "." : "", tail);
    return buf;
}

static std::string idEphPath(int n, const char* tail)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "lxmf.id.%d%s%s", n, *tail ? "." : "", tail);
    return buf;
}

static std::string secretsPath(int n, const char* tail)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "secrets.lxmf.id.%d%s%s", n, *tail ? "." : "", tail);
    return buf;
}

static std::string msgPath(int n, const std::string& mid, const char* field)
{
    char buf[120];
    snprintf(buf, sizeof(buf), "s.lxmf.id.%d.msgs.%s.%s", n, mid.c_str(), field);
    return buf;
}

static std::string contactPath(int n, const std::string& peer_hex, const char* field)
{
    char buf[120];
    snprintf(buf, sizeof(buf), "s.lxmf.id.%d.contacts.%s.%s", n, peer_hex.c_str(), field);
    return buf;
}

static uint64_t nowUnixMs(void)
{
    /* Monotonic since boot. lxmf doesn't care about wall clock except
     * for the user-facing `ts` / `last_announce_s` fields, where
     * uptime is still a useful coarse "since when" indicator without
     * needing NTP. */
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint32_t cheapRand(void)
{
    /* RNG seed has been initialised by the platform by the time lxmf
     * starts; xorshift over rand() is fine for message-key salt. */
    return (uint32_t)esp_random();
}

/* ── byte / hex helpers (replace RNS::Bytes::toHex / assignHex) ── */

static std::string bytesToHex(const uint8_t* data, size_t n)
{
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i)
        std::snprintf(&out[2*i], 3, "%02x", data[i]);
    return out;
}

static bool hexToBytes(const char* hex, size_t hex_len,
                       uint8_t* out, size_t out_len)
{
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned x = 0;
        if (std::sscanf(hex + 2*i, "%2x", &x) != 1) return false;
        out[i] = (uint8_t)x;
    }
    return true;
}

/* Strip C0 controls (0x00–0x1F) and DEL (0x7F) from network-supplied
 * strings before logging them. C0 bytes never appear as UTF-8
 * continuation bytes, so this leaves valid multibyte UTF-8 intact.
 * Without it an embedded 0x1B in e.g. an announce display_name is fed
 * verbatim into xterm.js on the browser side and parses as a CSI
 * escape sequence, hosing the log window. */
static std::string sanitizeForLog(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (uint8_t b : s)
        out += (b < 0x20 || b == 0x7F) ? '.' : (char)b;
    return out;
}

/* Case-insensitive substring match. ASCII fold only (A-Z ↔ a-z); UTF-8
 * multibyte sequences compare byte-for-byte, so `é` matches `é` but
 * `é` doesn't match `É` — true Unicode case folding would need a
 * table, not worth it at Phase 4a scale. Empty needle matches
 * anything. */
static bool nameContainsCI(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto lower = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
    };
    for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
        size_t k = 0;
        for (; k < needle.size(); k++)
            if (lower((unsigned char)haystack[i+k]) !=
                lower((unsigned char)needle[k])) break;
        if (k == needle.size()) return true;
    }
    return false;
}

/* ─────────────── msgpack: minimal pack/unpack ─────────────── */

/* We pack a fixed-shape LXMF payload: fixarray of [uint64 ts, str title,
 * str/bin content, map<int,*> fields]. Decoding accepts the same shape
 * plus an optional stamp element at index 4. */

static void mpPackUint64(std::vector<uint8_t>& out, uint64_t v)
{
    out.push_back(0xCF);
    for (int i = 7; i >= 0; --i) out.push_back((uint8_t)((v >> (8*i)) & 0xFF));
}

static void mpPackInt(std::vector<uint8_t>& out, int v)
{
    if (v >= 0 && v <= 0x7F) { out.push_back((uint8_t)v); return; }
    if (v < 0 && v >= -32)   { out.push_back((uint8_t)(0xE0 | (v & 0x1F))); return; }
    if (v >= 0 && v <= 0xFF) { out.push_back(0xCC); out.push_back((uint8_t)v); return; }
    if (v >= 0 && v <= 0xFFFF) {
        out.push_back(0xCD);
        out.push_back((uint8_t)((v >> 8) & 0xFF));
        out.push_back((uint8_t)( v       & 0xFF));
        return;
    }
    /* uint32 / int32 catch-all. */
    if (v >= 0) {
        out.push_back(0xCE);
        for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((v >> (8*i)) & 0xFF));
    } else {
        out.push_back(0xD2);
        uint32_t u = (uint32_t)v;
        for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((u >> (8*i)) & 0xFF));
    }
}

static void mpPackStrHeader(std::vector<uint8_t>& out, size_t len)
{
    if (len <= 31)    { out.push_back((uint8_t)(0xA0 | len)); return; }
    if (len <= 0xFF)  { out.push_back(0xD9); out.push_back((uint8_t)len); return; }
    if (len <= 0xFFFF){ out.push_back(0xDA);
                        out.push_back((uint8_t)((len >> 8) & 0xFF));
                        out.push_back((uint8_t)( len       & 0xFF)); return; }
    out.push_back(0xDB);
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((len >> (8*i)) & 0xFF));
}

static void mpPackBinHeader(std::vector<uint8_t>& out, size_t len)
{
    if (len <= 0xFF)   { out.push_back(0xC4); out.push_back((uint8_t)len); return; }
    if (len <= 0xFFFF) { out.push_back(0xC5);
                         out.push_back((uint8_t)((len >> 8) & 0xFF));
                         out.push_back((uint8_t)( len       & 0xFF)); return; }
    out.push_back(0xC6);
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((len >> (8*i)) & 0xFF));
}

static void mpPackStr(std::vector<uint8_t>& out, std::string_view s)
{
    mpPackStrHeader(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

static void mpPackBin(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
{
    mpPackBinHeader(out, n);
    out.insert(out.end(), p, p + n);
}

static void mpPackArrayHeader(std::vector<uint8_t>& out, size_t n)
{
    if (n <= 15)      { out.push_back((uint8_t)(0x90 | n)); return; }
    if (n <= 0xFFFF)  { out.push_back(0xDC);
                        out.push_back((uint8_t)((n >> 8) & 0xFF));
                        out.push_back((uint8_t)( n       & 0xFF)); return; }
    out.push_back(0xDD);
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((n >> (8*i)) & 0xFF));
}

static void mpPackMapHeader(std::vector<uint8_t>& out, size_t n)
{
    if (n <= 15)      { out.push_back((uint8_t)(0x80 | n)); return; }
    if (n <= 0xFFFF)  { out.push_back(0xDE);
                        out.push_back((uint8_t)((n >> 8) & 0xFF));
                        out.push_back((uint8_t)( n       & 0xFF)); return; }
    out.push_back(0xDF);
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((n >> (8*i)) & 0xFF));
}

/* Minimal msgpack walker — advances `i` past one element, leaves the
 * raw bytes for the caller to inspect/copy. Returns false on malformed
 * input. Used to walk the top-level array and extract elements by
 * position. */
struct mpScan {
    const uint8_t* p;
    size_t         n;
    size_t         i;
    /* Filled by scanNext: span of the most-recent element including
     * its type byte(s). For containers the span covers the whole
     * subtree. */
    size_t         elem_start;
    size_t         elem_len;
};

static bool mpReadBe(mpScan& s, int nbytes, uint64_t& v)
{
    if (s.i + (size_t)nbytes > s.n) return false;
    v = 0;
    for (int j = 0; j < nbytes; ++j) v = (v << 8) | s.p[s.i + j];
    s.i += nbytes;
    return true;
}

static bool mpScanNext(mpScan& s);  /* fwd */

static bool mpSkipN(mpScan& s, size_t n)
{
    for (size_t k = 0; k < n; ++k) if (!mpScanNext(s)) return false;
    return true;
}

static bool mpScanNext(mpScan& s)
{
    if (s.i >= s.n) return false;
    size_t start = s.i;
    uint8_t b = s.p[s.i++];
    uint64_t v;

    if (b <= 0x7F || b >= 0xE0) { /* fixint */
        s.elem_start = start; s.elem_len = s.i - start; return true;
    }
    if (b >= 0xA0 && b <= 0xBF) { /* fixstr */
        size_t L = b & 0x1F;
        if (s.i + L > s.n) return false;
        s.i += L; s.elem_start = start; s.elem_len = s.i - start; return true;
    }
    if (b >= 0x90 && b <= 0x9F) { /* fixarray */
        size_t cnt = b & 0x0F;
        if (!mpSkipN(s, cnt)) return false;
        s.elem_start = start; s.elem_len = s.i - start; return true;
    }
    if (b >= 0x80 && b <= 0x8F) { /* fixmap */
        size_t cnt = b & 0x0F;
        if (!mpSkipN(s, cnt * 2)) return false;
        s.elem_start = start; s.elem_len = s.i - start; return true;
    }
    switch (b) {
        case 0xC0: case 0xC2: case 0xC3:  /* nil/false/true */
            s.elem_start = start; s.elem_len = s.i - start; return true;

        case 0xC4: if (!mpReadBe(s,1,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;
        case 0xC5: if (!mpReadBe(s,2,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;
        case 0xC6: if (!mpReadBe(s,4,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;

        case 0xCC: if (s.i + 1 > s.n) return false; s.i += 1; break;
        case 0xCD: if (s.i + 2 > s.n) return false; s.i += 2; break;
        case 0xCE: if (s.i + 4 > s.n) return false; s.i += 4; break;
        case 0xCF: if (s.i + 8 > s.n) return false; s.i += 8; break;
        case 0xD0: if (s.i + 1 > s.n) return false; s.i += 1; break;
        case 0xD1: if (s.i + 2 > s.n) return false; s.i += 2; break;
        case 0xD2: if (s.i + 4 > s.n) return false; s.i += 4; break;
        case 0xD3: if (s.i + 8 > s.n) return false; s.i += 8; break;

        case 0xCA: if (s.i + 4 > s.n) return false; s.i += 4; break;
        case 0xCB: if (s.i + 8 > s.n) return false; s.i += 8; break;

        case 0xD9: if (!mpReadBe(s,1,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;
        case 0xDA: if (!mpReadBe(s,2,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;
        case 0xDB: if (!mpReadBe(s,4,v)) return false; if (s.i + v > s.n) return false; s.i += v; break;

        case 0xDC: { if (!mpReadBe(s,2,v)) return false; if (!mpSkipN(s, (size_t)v)) return false; break; }
        case 0xDD: { if (!mpReadBe(s,4,v)) return false; if (!mpSkipN(s, (size_t)v)) return false; break; }
        case 0xDE: { if (!mpReadBe(s,2,v)) return false; if (!mpSkipN(s, (size_t)v*2)) return false; break; }
        case 0xDF: { if (!mpReadBe(s,4,v)) return false; if (!mpSkipN(s, (size_t)v*2)) return false; break; }

        /* fixext / ext — bail; LXMF doesn't use them. */
        default: return false;
    }
    s.elem_start = start;
    s.elem_len   = s.i - start;
    return true;
}

/* Read the next element as a uint64. Returns false if not int-like. */
static bool mpReadUint(mpScan& s, uint64_t& out)
{
    size_t snap = s.i;
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b <= 0x7F) { out = b; return true; }
    if (b >= 0xE0) { out = (uint64_t)(int8_t)b; return true; }
    uint64_t v;
    switch (b) {
        case 0xCC: if (!mpReadBe(s,1,v)) { s.i = snap; return false; } out = v; return true;
        case 0xCD: if (!mpReadBe(s,2,v)) { s.i = snap; return false; } out = v; return true;
        case 0xCE: if (!mpReadBe(s,4,v)) { s.i = snap; return false; } out = v; return true;
        case 0xCF: if (!mpReadBe(s,8,v)) { s.i = snap; return false; } out = v; return true;
    }
    s.i = snap;
    return false;
}

/* Read the next element as a string/bin/raw. Returns false if not. */
static bool mpReadStrOrBin(mpScan& s, std::string& out)
{
    size_t snap = s.i;
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    uint64_t L = 0;
    if (b >= 0xA0 && b <= 0xBF) { L = b & 0x1F; }
    else if (b == 0xD9) { if (!mpReadBe(s,1,L)) { s.i = snap; return false; } }
    else if (b == 0xDA) { if (!mpReadBe(s,2,L)) { s.i = snap; return false; } }
    else if (b == 0xDB) { if (!mpReadBe(s,4,L)) { s.i = snap; return false; } }
    else if (b == 0xC4) { if (!mpReadBe(s,1,L)) { s.i = snap; return false; } }
    else if (b == 0xC5) { if (!mpReadBe(s,2,L)) { s.i = snap; return false; } }
    else if (b == 0xC6) { if (!mpReadBe(s,4,L)) { s.i = snap; return false; } }
    else { s.i = snap; return false; }
    if (s.i + L > s.n) { s.i = snap; return false; }
    out.assign((const char*)(s.p + s.i), (size_t)L);
    s.i += L;
    return true;
}

/* ─────────────── LXMF announce app_data parser ─────────────── */

struct LxmfAnnounceInfo {
    std::string name;          /* utf-8 display name, possibly empty */
    int         stamp_cost;    /* -1 = unknown */
    std::string ratchet_hex;   /* empty if not present */
};

/* LXMF announce app_data shapes seen in the wild (LXMF reference 0.9.8):
 *
 *   [a] 32B ratchet || msgpack([display_name_bytes_or_nil, stamp_cost])
 *   [b] msgpack([display_name_bytes_or_nil, stamp_cost])   (older clients)
 *   [c] msgpack([display_name_bytes_or_nil])               (no cost yet)
 *   [d] 32B ratchet || raw_utf8_name                       (very old)
 *   [e] raw_utf8_name                                      (very old)
 *
 * Try strict-msgpack forms first; fall back to raw-bytes name. */
static LxmfAnnounceInfo parseLxmfAnnounce(const uint8_t* p, size_t n)
{
    LxmfAnnounceInfo info;
    info.stamp_cost = -1;

    if (!p || n == 0) return info;

    auto tryArrayAt = [&](size_t start) -> bool {
        if (start >= n) return false;
        mpScan s{p, n, start, 0, 0};
        uint8_t b = p[s.i++];
        size_t cnt;
        uint64_t v;
        if (b >= 0x90 && b <= 0x9F) cnt = b & 0x0F;
        else if (b == 0xDC) { if (!mpReadBe(s, 2, v)) return false; cnt = (size_t)v; }
        else return false;
        if (cnt < 1) return false;

        std::string nm;
        /* First element may be nil — accept and leave name empty. */
        size_t snap = s.i;
        if (s.i < s.n && p[s.i] == 0xC0) { ++s.i; }
        else if (!mpReadStrOrBin(s, nm)) { s.i = snap; return false; }
        info.name = nm;

        if (cnt >= 2) {
            uint64_t cost = 0;
            if (mpReadUint(s, cost)) info.stamp_cost = (int)cost;
        }
        return true;
    };

    /* [a] ratchet + msgpack */
    if (n >= 32 + 2 && tryArrayAt(32)) {
        info.ratchet_hex = bytesToHex(p, 32);
        return info;
    }
    /* [b]/[c] msgpack at start */
    if (tryArrayAt(0)) return info;

    /* [d]/[e] raw UTF-8 name. Heuristic: if the first 32 bytes look
     * like random ratchet material (any control char in 0x00..0x1F
     * other than tab/cr/lf), assume ratchet prefix is present. */
    auto plausibleText = [&](size_t off, size_t len) {
        if (off >= n || len == 0) return false;
        for (size_t k = 0; k < len && off + k < n; ++k) {
            uint8_t b = p[off + k];
            if (b == 0x7F) return false;
            if (b < 0x20 && b != '\t' && b != '\n' && b != '\r') return false;
        }
        return true;
    };
    if (n > 32 && plausibleText(32, n - 32)) {
        info.name.assign((const char*)(p + 32), n - 32);
        info.ratchet_hex = bytesToHex(p, 32);
        return info;
    }
    if (plausibleText(0, n)) {
        info.name.assign((const char*)p, n);
    }
    return info;
}

/* ─────────────── LXM pack / parse ─────────────── */

struct LxmFields {
    std::string thread;        /* hex64 root message_id, empty if none */
    /* Future: telemetry, attachments, ticket, etc. */
};

/* Pack the msgpack payload alone (no dest/src/sig). title/content may
 * be UTF-8 strings. fields can carry FIELD_THREAD as raw 32 bytes. */
static std::vector<uint8_t> lxmPackPayload(uint64_t ts_ms, std::string_view title,
                                            std::string_view content, const LxmFields& fields)
{
    std::vector<uint8_t> out;
    out.reserve(8 + title.size() + content.size() + 64);

    /* Top-level fixarray of 4. */
    mpPackArrayHeader(out, 4);

    /* [0] timestamp (uint64, ms-precision unix). The reference impl uses
     * float seconds, but uint64 ms decodes identically on the wire (just
     * a different msgpack type) and reads cleanly on both sides. */
    mpPackUint64(out, ts_ms);

    /* [1] title (str), [2] content (str). */
    mpPackStr(out, title);
    mpPackStr(out, content);

    /* [3] fields — map of int → value. Only emit non-empty entries. */
    size_t field_count = 0;
    if (!fields.thread.empty()) field_count++;
    mpPackMapHeader(out, field_count);
    if (!fields.thread.empty()) {
        mpPackInt(out, LXMF_FIELD_THREAD);
        /* Thread roots are 32-byte SHA-256 message_ids. The plan stores
         * them as hex64; convert back to raw 32 B for the wire. */
        uint8_t raw[32] = {};
        if (fields.thread.size() == 64) {
            for (int k = 0; k < 32; ++k) {
                unsigned x = 0;
                std::sscanf(fields.thread.c_str() + 2*k, "%2x", &x);
                raw[k] = (uint8_t)x;
            }
        }
        mpPackBin(out, raw, sizeof(raw));
    }
    return out;
}

/* Parse an inbound LXM payload (msgpack). Returns true iff the top-level
 * structure is recognizable. */
static bool lxmParsePayload(const uint8_t* p, size_t n,
                            uint64_t* ts_out, std::string* title_out,
                            std::string* content_out, LxmFields* fields_out)
{
    mpScan s{p, n, 0, 0, 0};
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    size_t arr_len;
    uint64_t v;
    if (b >= 0x90 && b <= 0x9F)       arr_len = b & 0x0F;
    else if (b == 0xDC) { if (!mpReadBe(s,2,v)) return false; arr_len = (size_t)v; }
    else if (b == 0xDD) { if (!mpReadBe(s,4,v)) return false; arr_len = (size_t)v; }
    else return false;
    if (arr_len < 4) return false;  /* need at least ts/title/content/fields */

    uint64_t ts = 0;
    if (!mpReadUint(s, ts)) {
        /* Tolerate float timestamps from reference impl. */
        if (s.i >= s.n) return false;
        uint8_t tb = s.p[s.i];
        if (tb == 0xCA) {
            ++s.i;
            if (s.i + 4 > s.n) return false;
            uint32_t bits = 0;
            for (int k = 0; k < 4; ++k) bits = (bits << 8) | s.p[s.i + k];
            float f; std::memcpy(&f, &bits, 4);
            ts = (uint64_t)(f * 1000.0f);
            s.i += 4;
        } else if (tb == 0xCB) {
            ++s.i;
            if (s.i + 8 > s.n) return false;
            uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits = (bits << 8) | s.p[s.i + k];
            double f; std::memcpy(&f, &bits, 8);
            ts = (uint64_t)(f * 1000.0);
            s.i += 8;
        } else return false;
    }
    if (ts_out) *ts_out = ts;

    std::string title, content;
    if (!mpReadStrOrBin(s, title))   return false;
    if (!mpReadStrOrBin(s, content)) return false;
    if (title_out)   *title_out   = std::move(title);
    if (content_out) *content_out = std::move(content);

    /* [3] fields — map. */
    size_t map_len = 0;
    if (s.i >= s.n) return false;
    uint8_t mb = s.p[s.i++];
    if (mb >= 0x80 && mb <= 0x8F) map_len = mb & 0x0F;
    else if (mb == 0xDE) { if (!mpReadBe(s,2,v)) return false; map_len = (size_t)v; }
    else if (mb == 0xDF) { if (!mpReadBe(s,4,v)) return false; map_len = (size_t)v; }
    else return false;

    if (fields_out) *fields_out = LxmFields{};
    for (size_t k = 0; k < map_len; ++k) {
        uint64_t key = 0;
        if (!mpReadUint(s, key)) {
            /* Unrecognized key type: skip value. */
            if (!mpScanNext(s)) return false;
            continue;
        }
        if (key == LXMF_FIELD_THREAD) {
            std::string raw;
            if (!mpReadStrOrBin(s, raw)) { if (!mpScanNext(s)) return false; continue; }
            if (raw.size() == 32 && fields_out) {
                char hex[65];
                for (int j = 0; j < 32; ++j)
                    std::snprintf(hex + 2*j, 3, "%02x", (uint8_t)raw[j]);
                fields_out->thread.assign(hex, 64);
            }
        } else {
            /* Phase 4a: skip everything else but stay parseable. */
            if (!mpScanNext(s)) return false;
        }
    }
    /* [4] optional stamp — Phase 4b. */
    return true;
}

/* Build the full LXM wire bytes: dest || src || sig || msgpack.
 * `identity_key` is the storage path of the sender's private key (used
 * by rnsdSign + rnsdIdentityHash). Returns empty on failure. */
static std::vector<uint8_t> lxmPackWire(const char* identity_key,
                                         const uint8_t dest_hash[LXMF_DEST_HASH_LEN],
                                         uint64_t ts_ms,
                                         std::string_view title,
                                         std::string_view content,
                                         const LxmFields& fields)
{
    std::vector<uint8_t> packed = lxmPackPayload(ts_ms, title, content, fields);

    uint8_t src_hash[RNSD_IDENT_HASH_LEN];
    if (!rnsdIdentityHash(identity_key, src_hash)) return {};

    /* signable = dest || src || packed || SHA-256(dest || src || packed) */
    std::vector<uint8_t> signable;
    signable.reserve(LXMF_DEST_HASH_LEN * 2 + packed.size() + RNSD_HASH_LEN);
    signable.insert(signable.end(), dest_hash, dest_hash + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), src_hash,  src_hash  + RNSD_IDENT_HASH_LEN);
    signable.insert(signable.end(), packed.begin(), packed.end());
    uint8_t hash[RNSD_HASH_LEN];
    rnsdSha256(signable.data(), signable.size(), hash);
    signable.insert(signable.end(), hash, hash + RNSD_HASH_LEN);

    uint8_t sig[RNSD_SIG_LEN];
    if (!rnsdSign(identity_key, signable.data(), signable.size(), sig))
        return {};

    std::vector<uint8_t> wire;
    wire.reserve(LXMF_OVERHEAD + packed.size());
    wire.insert(wire.end(), dest_hash, dest_hash + LXMF_DEST_HASH_LEN);
    wire.insert(wire.end(), src_hash,  src_hash  + RNSD_IDENT_HASH_LEN);
    wire.insert(wire.end(), sig,       sig       + RNSD_SIG_LEN);
    wire.insert(wire.end(), packed.begin(), packed.end());
    return wire;
}

/* Compute message_id = SHA-256(dest || src || packed). 32 B → 64-hex. */
static std::string lxmMessageIdHex(const uint8_t* wire, size_t n)
{
    if (n < LXMF_OVERHEAD) return "";
    /* The wire is already laid out as dest(16)||src(16)||sig(64)||packed.
     * Skip the sig (64 B) when computing the message_id input. */
    std::vector<uint8_t> mid_input;
    mid_input.reserve(LXMF_DEST_HASH_LEN * 2 + (n - LXMF_OVERHEAD));
    mid_input.insert(mid_input.end(), wire, wire + LXMF_DEST_HASH_LEN * 2);
    mid_input.insert(mid_input.end(),
                     wire + LXMF_OVERHEAD, wire + n);
    uint8_t hash[RNSD_HASH_LEN];
    rnsdSha256(mid_input.data(), mid_input.size(), hash);
    return bytesToHex(hash, RNSD_HASH_LEN);
}

/* ─────────────── identity table ─────────────── */

static lxmf_id_t* idAt(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return nullptr;
    return &s_ids[n];
}

static lxmf_id_t* idForHandle(int handle)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n)
        if (s_ids[n].used && s_ids[n].handle == handle) return &s_ids[n];
    return nullptr;
}

static int idAllocSlot(void)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) if (!s_ids[n].used) return n;
    return -1;
}

static outbound_t* outboundFindBySendId(lxmf_id_t& id, uint16_t send_id)
{
    for (auto& o : id.outboxes)
        if (o.used && o.send_id == send_id) return &o;
    return nullptr;
}

static outbound_t* outboundAlloc(lxmf_id_t& id)
{
    for (auto& o : id.outboxes) if (!o.used) return &o;
    return nullptr;
}

/* ─────────────── directory of seen LXMF mailboxes ─────────────── */

/* Cross-identity, ephemeral catalogue of every `lxmf.delivery` announce
 * we've heard recently. Populated by onAnnounceFromRnsd — fired on the
 * lxmf task when rnsd dispatches an event over our RNSD_PORT_ANNOUNCES
 * subscription. All storage writes happen here, on our task — the rnsd
 * task only memcpys the announce into one ITS packet and forwards.
 *
 * Path: `lxmf.directory.<dest_hex>.{hash,display_name,stamp_cost,ratchet,
 *        hops,last_announce_s}`. Pruning is left to a future cron job —
 * for Phase 4a entries simply accumulate. */

static bool isOwnDest(const uint8_t dh[LXMF_DEST_HASH_LEN])
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_ids[n].used &&
            std::memcmp(s_ids[n].dest_hash, dh, LXMF_DEST_HASH_LEN) == 0)
            return true;
    }
    return false;
}

static int s_announce_sub_handle = -1;

/* RNSD_PORT_ANNOUNCES frame: hops(1) | dest_hash(16) | identity_hash(16) | app_data(N) */
constexpr size_t LXMF_ANNOUNCE_HDR = 1 + 16 + 16;

/* ── lxmf.directory.<hex> packed-value format ──
 *
 *   <last_s>|<cost>|<hops>|<ratchet>|<name>
 *
 *   last_s   decimal unix-secish (from nowUnixMs()/1000, monotonic)
 *   cost     decimal int, -1 if unknown
 *   hops     decimal int, -1 if unknown
 *   ratchet  64-hex or empty
 *   name     utf-8, may contain '|' (no escaping — name is last)
 *
 * Replaces the previous per-field subtree (6 leaves per entry). One
 * cJSON_String + the parent's child pointer per entry; ~3× fewer
 * heap blocks attributed to lxmf as the directory grows.
 *
 * For eviction we only need last_s; that's the first field, so
 * atoi(value) is enough — stops at the first '|'. */

struct DirEntry {
    int         last_s;
    int         cost;
    int         hops;
    std::string ratchet;
    std::string name;
};

static std::string buildDirValue(const DirEntry& e)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d|%d|%d|%s|",
                  e.last_s, e.cost, e.hops, e.ratchet.c_str());
    std::string out = buf;
    out += e.name;   /* name may be arbitrary utf-8; append last so embedded
                        '|' in display names doesn't break parsing. */
    return out;
}

static bool parseDirValue(const char* val, DirEntry* out)
{
    if (!val) return false;
    /* Find the first 4 pipes. Anything after pipe #4 is the name. */
    const char* pos[4] = {};
    int cnt = 0;
    for (const char* p = val; *p && cnt < 4; ++p)
        if (*p == '|') pos[cnt++] = p;
    if (cnt < 4) return false;
    out->last_s = std::atoi(val);
    out->cost   = std::atoi(pos[0] + 1);
    out->hops   = std::atoi(pos[1] + 1);
    out->ratchet.assign(pos[2] + 1, pos[3] - pos[2] - 1);
    out->name   = pos[3] + 1;
    return true;
}

/* ── directory size cap + LRU-by-announce-time eviction ──
 *
 * Walk `lxmf.directory.` and find the oldest entry. Used when the
 * directory hits `s.lxmf.max_dir_size` (default 2048) so a new
 * insert evicts one. Walk is O(N) under CFG_LOCK; at 2048 entries
 * ≈ 12 ms. Eviction only fires for brand-new destinations after
 * the cap is reached — re-announces from existing entries just
 * update in place. */

struct DirOldestCtx {
    int         count;
    int         oldest_s;
    std::string oldest_hex;
};
static DirOldestCtx* s_dir_oldest_ctx = nullptr;

static void dirOldestLeaf(const char* key, const char* val)
{
    if (!s_dir_oldest_ctx || !key || !val) return;
    const char* tail = key + sizeof("lxmf.directory.") - 1;
    /* Bare `lxmf.directory.<hex>` (no nested key). */
    if (std::strchr(tail, '.')) return;
    s_dir_oldest_ctx->count++;
    int ls = std::atoi(val);
    if (s_dir_oldest_ctx->oldest_hex.empty() ||
        ls < s_dir_oldest_ctx->oldest_s) {
        s_dir_oldest_ctx->oldest_s   = ls;
        s_dir_oldest_ctx->oldest_hex = tail;
    }
}

/* Find (count, oldest entry). Returns count. If max_entries is non-
 * zero and count >= max_entries, sets oldest_hex_out so the caller
 * can evict. */
static int dirCountAndMaybeOldest(int max_entries, std::string* oldest_hex_out)
{
    DirOldestCtx ctx{};
    s_dir_oldest_ctx = &ctx;
    storageForEach("lxmf.directory.", dirOldestLeaf);
    s_dir_oldest_ctx = nullptr;
    if (max_entries > 0 && ctx.count >= max_entries &&
        !ctx.oldest_hex.empty() && oldest_hex_out)
        *oldest_hex_out = ctx.oldest_hex;
    return ctx.count;
}

static void onAnnounceFromRnsd(int handle, size_t /*bytesAvail*/)
{
    if (handle != s_announce_sub_handle) return;
    static uint8_t buf[LXMF_ANNOUNCE_HDR + 1024];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n < LXMF_ANNOUNCE_HDR) {
        if (n > 0) warn("announce sub: short frame %zu B", n);
        return;
    }

    int            hops     = buf[0];
    const uint8_t* dh       = buf + 1;
    /* buf + 17 is the announce identity hash — unused in Phase 4a but
     * available if a consumer ever wants it. */
    const uint8_t* app_data = buf + LXMF_ANNOUNCE_HDR;
    size_t         app_len  = n - LXMF_ANNOUNCE_HDR;

    /* rnsd already aspect-filtered for us, so dh is always a real
     * lxmf.delivery destination. Still filter out our own identities. */
    if (isOwnDest(dh)) return;

    LxmfAnnounceInfo info = parseLxmfAnnounce(app_data, app_len);
    std::string dh_hex = bytesToHex(dh, LXMF_DEST_HASH_LEN);

    char key[64];
    std::snprintf(key, sizeof(key), "lxmf.directory.%s", dh_hex.c_str());

    /* If this is a brand-new destination (no existing entry) and we
     * would exceed the cap, evict the oldest entry by last_s. Re-
     * announces from existing entries skip this scan. */
    bool is_new = !storageExists(key);
    if (is_new) {
        int max_dir = storageGetInt("s.lxmf.max_dir_size", 2048);
        if (max_dir > 0) {
            std::string oldest;
            int cur = dirCountAndMaybeOldest(max_dir, &oldest);
            if (cur >= max_dir && !oldest.empty()) {
                char old_key[64];
                std::snprintf(old_key, sizeof(old_key),
                              "lxmf.directory.%s", oldest.c_str());
                storageUnset(old_key);
                dbg("directory: evicted oldest %s (cap=%d)",
                    oldest.c_str(), max_dir);
            }
        }
    }

    DirEntry e;
    e.last_s  = (int)(nowUnixMs() / 1000);
    e.cost    = info.stamp_cost;   /* -1 if unknown */
    e.hops    = hops;
    e.ratchet = info.ratchet_hex;  /* empty if unknown */
    e.name    = info.name;         /* may be empty */
    storageSet(key, buildDirValue(e).c_str());

    dbg("directory: %s name=\"%s\" cost=%d hops=%d",
        dh_hex.c_str(), sanitizeForLog(info.name).c_str(),
        info.stamp_cost, hops);
}

static void onAnnounceSubDisconnect(int /*handle*/)
{
    warn("announce sub: disconnected from rnsd");
    s_announce_sub_handle = -1;
    /* Reconnect attempted on the next 1 Hz publish tick. */
}

static bool connectAnnounceSub(void)
{
    if (s_announce_sub_handle >= 0) return true;
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, "lxmf.delivery", sizeof(req.aspect));
    int h = itsConnect("rnsd", RNSD_PORT_ANNOUNCES,
                       &req, sizeof(req), pdMS_TO_TICKS(2000),
                       /*ref*/ 0, onAnnounceFromRnsd, onAnnounceSubDisconnect);
    if (h < 0) {
        warn("announce sub: connect failed");
        return false;
    }
    s_announce_sub_handle = h;
    info("announce sub: connected (handle=%d aspect=lxmf.delivery)", h);
    return true;
}

/* ─────────────── connect to rnsd mailbox ─────────────── */

/* Forward decl — onIts callbacks live below. */
static void onMailboxRecv(int handle, size_t bytesAvail);
static void onMailboxDisconnect(int handle);

static bool connectMailbox(lxmf_id_t& id)
{
    if (id.handle >= 0) return true;

    rnsd_mailbox_connect_t req = {};
    safeStrncpy(req.aspect, "lxmf.delivery", sizeof(req.aspect));
    std::string ikey = secretsPath(id.index, "privkey");
    safeStrncpy(req.identity_key, ikey.c_str(), sizeof(req.identity_key));
    req.dest_type = 0;   /* SINGLE */

    int h = itsConnect("rnsd", RNSD_PORT_DEST,
                       &req, sizeof(req), pdMS_TO_TICKS(2000),
                       /*ref*/ id.index, onMailboxRecv, onMailboxDisconnect);
    if (h < 0) {
        err("id %d: mailbox connect failed", id.index);
        return false;
    }
    id.handle = h;
    info("id %d: mailbox connected (handle=%d)", id.index, h);

    storageSet(idEphPath(id.index, "up").c_str(), 1);
    storageSet(idEphPath(id.index, "dest_hash").c_str(),
               bytesToHex(id.dest_hash, LXMF_DEST_HASH_LEN).c_str());
    return true;
}

/* ─────────────── identity bootstrap ─────────────── */

/* Generate a fresh identity for slot n with the given display name,
 * persist the private key, and stub the per-identity config. The
 * display name is what gets broadcast in `lxmf.delivery` announces;
 * `label` is a local UI hint and tracks the display name unless the
 * user overrides it later via storage. Returns true iff a usable
 * identity is loaded into s_ids[n]. */
static bool createIdentityForSlot(int n, const std::string& display_name)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return false;
    lxmf_id_t& slot = s_ids[n];
    if (slot.used) return false;
    if (display_name.empty()) {
        err("id %d: refusing to create with empty display_name", n);
        return false;
    }

    std::string ikey = secretsPath(n, "privkey");
    /* Wipe any stale key under this slot, then generate fresh. */
    rnsdIdentityErase(ikey.c_str());
    if (!rnsdIdentityGenerate(ikey.c_str())) {
        err("id %d: identity generate failed", n);
        return false;
    }

    slot.used         = true;
    slot.index        = n;
    slot.handle       = -1;
    slot.identity_key = ikey;
    if (!rnsdDestinationHash(ikey.c_str(), "lxmf", "delivery", slot.dest_hash)) {
        err("id %d: dest hash compute failed", n);
        slot = lxmf_id_t{};
        rnsdIdentityErase(ikey.c_str());
        return false;
    }
    slot.next_send_id = 1;
    for (auto& o : slot.outboxes) o.used = false;
    slot.sent = slot.received = slot.pending = slot.failed = 0;

    storageBegin();
    storageSet    (idPath(n, "label").c_str(),        display_name.c_str());
    storageSet    (idPath(n, "display_name").c_str(), display_name.c_str());
    storageDefault(idPath(n, "enabled").c_str(),      1);
    storageDefault(idPath(n, "stamp_cost").c_str(),   16);
    storageEnd();

    subscribePerIdCmds(n);

    uint8_t id_hash[RNSD_IDENT_HASH_LEN] = {};
    rnsdIdentityHash(ikey.c_str(), id_hash);
    info("id %d: created identity \"%s\" %s dest=%s",
         n, display_name.c_str(),
         bytesToHex(id_hash, RNSD_IDENT_HASH_LEN).c_str(),
         bytesToHex(slot.dest_hash, LXMF_DEST_HASH_LEN).c_str());
    return true;
}

/* Load an existing identity from secrets storage. */
static bool loadIdentityForSlot(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return false;
    lxmf_id_t& slot = s_ids[n];
    if (slot.used) return true;

    std::string ikey = secretsPath(n, "privkey");
    if (!rnsdIdentityExists(ikey.c_str())) return false;

    slot.used         = true;
    slot.index        = n;
    slot.handle       = -1;
    slot.identity_key = ikey;
    if (!rnsdDestinationHash(ikey.c_str(), "lxmf", "delivery", slot.dest_hash)) {
        err("id %d: dest hash compute failed for stored key", n);
        slot = lxmf_id_t{};
        return false;
    }
    slot.next_send_id = 1;
    for (auto& o : slot.outboxes) o.used = false;
    slot.sent = slot.received = slot.pending = slot.failed = 0;

    subscribePerIdCmds(n);

    uint8_t id_hash[RNSD_IDENT_HASH_LEN] = {};
    rnsdIdentityHash(ikey.c_str(), id_hash);
    info("id %d: loaded identity %s dest=%s", n,
         bytesToHex(id_hash, RNSD_IDENT_HASH_LEN).c_str(),
         bytesToHex(slot.dest_hash, LXMF_DEST_HASH_LEN).c_str());
    return true;
}

/* Destroy slot n: wipe secrets, wipe inbox + contacts + tickets, close
 * the mailbox. Used by lxmf.cmd.identity_destroy. */
static void destroyIdentity(int n)
{
    lxmf_id_t* slot = idAt(n);
    if (!slot || !slot->used) return;
    unsubscribePerIdCmds(n);
    if (slot->handle >= 0) {
        itsDisconnect(slot->handle);
        slot->handle = -1;
    }
    /* secrets.lxmf.id.<n>.privkey via rnsd's identity API, plus any
     * other secrets under the subtree. */
    rnsdIdentityErase(secretsPath(n, "privkey").c_str());
    storageDeleteTree(secretsPath(n, "").c_str());
    storageDeleteTree(idPath(n, "").c_str());
    storageDeleteTree(idEphPath(n, "").c_str());
    *slot = lxmf_id_t{};
    info("id %d: destroyed", n);
}

/* ─────────────── outbound: pack + send ─────────────── */

/* Hex-decode a 16-byte (32-hex) destination hash. Returns true iff well-formed. */
static bool hexToDestHash(const std::string& s, uint8_t out[16])
{
    if (s.size() != 32) return false;
    for (int k = 0; k < 16; ++k) {
        unsigned x = 0;
        if (std::sscanf(s.c_str() + 2*k, "%2x", &x) != 1) return false;
        out[k] = (uint8_t)x;
    }
    return true;
}

/* Push a frame to rnsd. Returns false if the buffer is full. */
static bool sendFrame(lxmf_id_t& id, const uint8_t* frame, size_t n)
{
    if (id.handle < 0) return false;
    if (itsSend(id.handle, frame, n, pdMS_TO_TICKS(200)) == 0) {
        warn("id %d: frame send dropped (%zu B)", id.index, n);
        return false;
    }
    return true;
}

/* ── announce ── */

/* Build LXMF announce app_data as msgpack `[display_name_bytes, stamp_cost]`
 * (the [b] shape parseLxmfAnnounce accepts). Modern clients also prefix
 * a 32-byte ratchet pubkey; that's reserved for Phase 4b alongside the
 * stamps machinery, which is when ratchet rotation becomes meaningful. */
static std::vector<uint8_t> buildAnnounceAppData(int id_n)
{
    std::string name = storageGetStr(idPath(id_n, "display_name").c_str(), "");
    int cost = storageGetInt(idPath(id_n, "stamp_cost").c_str(), 16);

    std::vector<uint8_t> out;
    mpPackArrayHeader(out, 2);
    if (name.empty()) out.push_back(0xC0 /* nil */);
    else              mpPackStr(out, name);
    mpPackInt(out, cost);
    return out;
}

static void sendAnnounce(lxmf_id_t& id)
{
    if (id.handle < 0) {
        warn("id %d: announce skipped (no mailbox handle)", id.index);
        return;
    }
    std::vector<uint8_t> app_data = buildAnnounceAppData(id.index);
    std::vector<uint8_t> frame;
    frame.reserve(1 + app_data.size());
    frame.push_back(RNSD_DEST_ANNOUNCE);
    frame.insert(frame.end(), app_data.begin(), app_data.end());
    if (!sendFrame(id, frame.data(), frame.size())) {
        warn("id %d: announce frame send dropped", id.index);
        return;
    }
    storageSet(idEphPath(id.index, "last_announce_s").c_str(),
               (int)(nowUnixMs() / 1000));
    id.last_announce_tick = xTaskGetTickCount();
    if (id.last_announce_tick == 0) id.last_announce_tick = 1;  /* 0 means "never" */
    info("id %d: announce sent (%zu B app_data)", id.index, app_data.size());
}

/* Move a draft from `ready` into the packed/sending pipeline. */
static void processReady(lxmf_id_t& id, const std::string& mid)
{
    std::string peer_hex = storageGetStr(msgPath(id.index, mid, "peer").c_str(), "");
    if (peer_hex.size() != 32) {
        warn("id %d: msg %s ready but peer is malformed (\"%s\")",
             id.index, mid.c_str(), peer_hex.c_str());
        storageSet(msgPath(id.index, mid, "stage").c_str(), "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "bad peer");
        return;
    }
    uint8_t dh[16];
    if (!hexToDestHash(peer_hex, dh)) {
        warn("id %d: msg %s peer hex invalid", id.index, mid.c_str());
        storageSet(msgPath(id.index, mid, "stage").c_str(), "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "bad peer hex");
        return;
    }

    std::string title   = storageGetStr(msgPath(id.index, mid, "title").c_str(),   "");
    std::string content = storageGetStr(msgPath(id.index, mid, "content").c_str(), "");
    std::string thread  = storageGetStr(msgPath(id.index, mid, "thread").c_str(),  "");

    /* 4a: refuse drafts whose packed payload alone wouldn't fit the
     * opportunistic single-packet budget. Plan §10. We approximate the
     * encoded size with the raw byte counts plus a small msgpack
     * overhead allowance. */
    if (title.size() + content.size() + 32 > LXMF_OPP_CONTENT_BUDGET) {
        warn("id %d: msg %s exceeds opportunistic budget (%zu B)",
             id.index, mid.c_str(), title.size() + content.size());
        storageSet(msgPath(id.index, mid, "stage").c_str(), "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "too large for opportunistic");
        return;
    }

    LxmFields fields;
    fields.thread = thread;

    uint64_t ts_ms = nowUnixMs();
    std::vector<uint8_t> wire = lxmPackWire(id.identity_key.c_str(),
                                             dh, ts_ms, title, content, fields);
    if (wire.empty()) {
        err("id %d: msg %s pack/sign failed", id.index, mid.c_str());
        storageSet(msgPath(id.index, mid, "stage").c_str(), "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "pack/sign failed");
        return;
    }

    std::string msg_id_hex = lxmMessageIdHex(wire.data(), wire.size());

    /* Reserve an outbox slot. */
    outbound_t* o = outboundAlloc(id);
    if (!o) {
        warn("id %d: outbox full", id.index);
        storageSet(msgPath(id.index, mid, "stage").c_str(),      "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "outbox full");
        return;
    }
    o->used    = true;
    o->send_id = id.next_send_id++;
    if (id.next_send_id == 0) id.next_send_id = 1;
    o->msg_key = mid;

    /* Persist firmware-owned fields. */
    storageBegin();
    storageSet(msgPath(id.index, mid, "wire").c_str(),       bytesToHex(wire.data(), wire.size()).c_str());
    storageSet(msgPath(id.index, mid, "message_id").c_str(), msg_id_hex.c_str());
    storageSet(msgPath(id.index, mid, "ts").c_str(),         (int)(ts_ms / 1000));
    storageSet(msgPath(id.index, mid, "stage").c_str(),      "queued");
    storageSet(msgPath(id.index, mid, "last_error").c_str(), "");
    storageEnd();

    /* OUT_PACKET frame: op | send_id(2) | lxm_wire_bytes */
    std::vector<uint8_t> frame;
    frame.reserve(3 + wire.size());
    frame.push_back(RNSD_DEST_OUT_PACKET);
    frame.push_back((uint8_t)(o->send_id >> 8));
    frame.push_back((uint8_t)(o->send_id & 0xFF));
    frame.insert(frame.end(), wire.begin(), wire.end());

    if (!sendFrame(id, frame.data(), frame.size())) {
        o->used = false;
        storageSet(msgPath(id.index, mid, "stage").c_str(),      "failed");
        storageSet(msgPath(id.index, mid, "last_error").c_str(), "OUT_PACKET send dropped");
        return;
    }
    storageSet(msgPath(id.index, mid, "stage").c_str(),      "sending");
    storageSet(msgPath(id.index, mid, "last_error").c_str(), "");
    id.pending++;
    info("id %d: send mid=%s peer=%s send_id=%u wire=%zuB",
         id.index, mid.c_str(), peer_hex.c_str(),
         (unsigned)o->send_id, wire.size());
}

/* ─────────────── inbound: verify + dedup + store ─────────────── */

static bool dedupSeen(const std::string& mid_hex)
{
    for (int k = 0; k < LXMF_DEDUP_RING; ++k)
        if (s_dedup_ring[k] == mid_hex) return true;
    return false;
}

static void dedupAdd(const std::string& mid_hex)
{
    s_dedup_ring[s_dedup_head] = mid_hex;
    s_dedup_head = (s_dedup_head + 1) % LXMF_DEDUP_RING;
}

static void onInboundLxm(lxmf_id_t& id, const uint8_t* wire, size_t n)
{
    if (n < LXMF_OVERHEAD) {
        warn("id %d: inbound LXM too short (%zu B)", id.index, n);
        return;
    }

    /* Layout: dest(16) | src(16) | sig(64) | packed(...). dest must
     * equal our delivery destination hash — it'll only be different if
     * rnsd routed something weird. */
    const uint8_t* dh     = wire;
    const uint8_t* sh     = wire + LXMF_DEST_HASH_LEN;
    const uint8_t* sig    = wire + 2 * LXMF_DEST_HASH_LEN;
    const uint8_t* packed = wire + LXMF_OVERHEAD;
    size_t         packed_n = n - LXMF_OVERHEAD;

    if (std::memcmp(dh, id.dest_hash, LXMF_DEST_HASH_LEN) != 0) {
        warn("id %d: inbound LXM dest mismatch (got=%s want=%s)",
             id.index,
             bytesToHex(dh, LXMF_DEST_HASH_LEN).c_str(),
             bytesToHex(id.dest_hash, LXMF_DEST_HASH_LEN).c_str());
        return;
    }

    /* Recall sender pubkey from rnsd's cache (populated by their
     * announces). If absent, request a path and drop for now — Phase 4a
     * doesn't buffer pending verifications. */
    uint8_t sender_pubkey[RNSD_PUBKEY_LEN];
    if (!rnsdRecallPubkey(sh, sender_pubkey)) {
        warn("id %d: inbound LXM from unknown sender %s — issuing path request",
             id.index, bytesToHex(sh, LXMF_DEST_HASH_LEN).c_str());
        rnsdRequestPath(sh);
        return;
    }

    /* Signature over dest || src || packed || SHA-256(...). */
    std::vector<uint8_t> signable;
    signable.reserve(LXMF_DEST_HASH_LEN * 2 + packed_n + RNSD_HASH_LEN);
    signable.insert(signable.end(), dh,     dh     + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), sh,     sh     + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), packed, packed + packed_n);
    uint8_t sig_hash[RNSD_HASH_LEN];
    rnsdSha256(signable.data(), signable.size(), sig_hash);
    signable.insert(signable.end(), sig_hash, sig_hash + RNSD_HASH_LEN);

    if (!rnsdVerify(sender_pubkey, signable.data(), signable.size(), sig)) {
        warn("id %d: inbound LXM signature invalid (from=%s)",
             id.index, bytesToHex(sh, LXMF_DEST_HASH_LEN).c_str());
        return;
    }

    /* message_id = SHA-256(dest || src || packed). Plan §2.1. */
    std::vector<uint8_t> mid_input;
    mid_input.reserve(LXMF_DEST_HASH_LEN * 2 + packed_n);
    mid_input.insert(mid_input.end(), dh,     dh     + LXMF_DEST_HASH_LEN);
    mid_input.insert(mid_input.end(), sh,     sh     + LXMF_DEST_HASH_LEN);
    mid_input.insert(mid_input.end(), packed, packed + packed_n);
    uint8_t mid_hash[RNSD_HASH_LEN];
    rnsdSha256(mid_input.data(), mid_input.size(), mid_hash);
    std::string mid_hex = bytesToHex(mid_hash, RNSD_HASH_LEN);

    if (dedupSeen(mid_hex)) {
        verb("id %d: inbound LXM dup (mid=%s)", id.index, mid_hex.c_str());
        return;
    }
    dedupAdd(mid_hex);

    /* Storage existence is the authoritative dedup. */
    std::string stage_key = msgPath(id.index, mid_hex, "stage");
    if (storageExists(stage_key.c_str())) {
        verb("id %d: inbound LXM already stored (mid=%s)", id.index, mid_hex.c_str());
        return;
    }

    /* Parse payload. */
    uint64_t    ts = 0;
    std::string title, content;
    LxmFields   fields;
    if (!lxmParsePayload(packed, packed_n,
                         &ts, &title, &content, &fields)) {
        warn("id %d: inbound LXM payload malformed", id.index);
        return;
    }

    /* Persist. */
    std::string sh_hex = bytesToHex(sh, LXMF_DEST_HASH_LEN);
    storageBegin();
    storageSet(stage_key.c_str(),                                "received");
    storageSet(msgPath(id.index, mid_hex, "dir").c_str(),        "in");
    storageSet(msgPath(id.index, mid_hex, "peer").c_str(),       sh_hex.c_str());
    storageSet(msgPath(id.index, mid_hex, "title").c_str(),      title.c_str());
    storageSet(msgPath(id.index, mid_hex, "content").c_str(),    content.c_str());
    if (!fields.thread.empty())
        storageSet(msgPath(id.index, mid_hex, "thread").c_str(), fields.thread.c_str());
    storageSet(msgPath(id.index, mid_hex, "ts").c_str(),         (int)(ts / 1000));
    storageSet(msgPath(id.index, mid_hex, "read").c_str(),       0);
    storageSet(msgPath(id.index, mid_hex, "message_id").c_str(), mid_hex.c_str());
    /* Stub contact if new — copy display_name across from the cross-
     * identity directory if we've heard them announce. */
    if (!storageExists(contactPath(id.index, sh_hex, "hash").c_str())) {
        storageSet(contactPath(id.index, sh_hex, "hash").c_str(),  sh_hex.c_str());
        storageSet(contactPath(id.index, sh_hex, "trust").c_str(), 0);
        char dirKey[120];
        std::snprintf(dirKey, sizeof(dirKey), "lxmf.directory.%s.display_name", sh_hex.c_str());
        std::string peer_name = storageGetStr(dirKey, "");
        if (!peer_name.empty())
            storageSet(contactPath(id.index, sh_hex, "display_name").c_str(), peer_name.c_str());
    }
    storageSet(contactPath(id.index, sh_hex, "last_seen").c_str(),
               (int)(nowUnixMs() / 1000));
    storageEnd();

    id.received++;
    info("id %d: recv mid=%s from=%s len=%zuB title=\"%s\"",
         id.index, mid_hex.c_str(), sh_hex.c_str(), n,
         sanitizeForLog(title).c_str());
}

/* ─────────────── mailbox frame handlers ─────────────── */

static void applyOutResult(lxmf_id_t& id, uint16_t send_id, uint8_t status,
                           uint32_t /*rtt_ms*/, uint8_t /*hops*/)
{
    outbound_t* o = outboundFindBySendId(id, send_id);
    if (!o) {
        verb("id %d: OUT_RESULT for unknown send_id=%u", id.index, (unsigned)send_id);
        return;
    }
    std::string mid = o->msg_key;
    o->used = false;
    if (id.pending > 0) id.pending--;

    const char* next_stage = "failed";
    const char* err_msg    = "";
    switch (status) {
        case RNSD_DEST_STATUS_SENT:      next_stage = "sent";      id.sent++;   break;
        case RNSD_DEST_STATUS_DELIVERED: next_stage = "delivered"; id.sent++;   break;
        case RNSD_DEST_STATUS_CANCELLED: next_stage = "cancelled"; id.failed++; err_msg = "cancelled"; break;
        case RNSD_DEST_STATUS_EVICTED:   next_stage = "failed";    id.failed++; err_msg = "evicted (resource limit)"; break;
        default:                         next_stage = "failed";    id.failed++; err_msg = "unknown status"; break;
    }
    storageBegin();
    storageSet(msgPath(id.index, mid, "stage").c_str(),      next_stage);
    storageSet(msgPath(id.index, mid, "last_error").c_str(), err_msg);
    storageEnd();
    dbg("id %d: msg %s → %s%s%s%s",
        id.index, mid.c_str(), next_stage,
        *err_msg ? " (" : "", err_msg, *err_msg ? ")" : "");
}

static void applyOutStatus(lxmf_id_t& id, uint16_t send_id, uint8_t type,
                           const uint8_t* tail, size_t tail_n)
{
    outbound_t* o = outboundFindBySendId(id, send_id);
    if (!o) return;
    const char* note = nullptr;
    switch (type) {
        case RNSD_DEST_AUX_REQUESTING_PATH: note = "requesting path"; break;
        case RNSD_DEST_AUX_PATH_KNOWN:      note = "path known";      break;
        case RNSD_DEST_AUX_EGRESS_QUEUED:   note = "egress queued";   break;
        case RNSD_DEST_AUX_LINK_ESTABLISHING: note = "establishing link"; break;
        case RNSD_DEST_AUX_PATH_LOST:       note = "path lost";       break;
        case RNSD_DEST_AUX_RETRY:
            if (tail_n >= 2) {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "retry %u (reason 0x%02x)",
                              (unsigned)tail[0], (unsigned)tail[1]);
                storageSet(msgPath(id.index, o->msg_key, "last_error").c_str(), buf);
                storageSet(msgPath(id.index, o->msg_key, "attempts").c_str(),
                           (int)tail[0]);
                return;
            }
            note = "retry";
            break;
        default: note = nullptr; break;
    }
    if (note)
        storageSet(msgPath(id.index, o->msg_key, "last_error").c_str(), note);
}

static void onMailboxRecv(int handle, size_t /*bytesAvail*/)
{
    lxmf_id_t* id = idForHandle(handle);
    if (!id) return;

    static uint8_t buf[2048];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n == 0) return;

    switch (buf[0]) {
        case RNSD_DEST_IN_PACKET: {
            onInboundLxm(*id, buf + 1, n - 1);
            break;
        }
        case RNSD_DEST_OUT_RESULT: {
            if (n < 9) { warn("OUT_RESULT short (%zu)", n); break; }
            uint16_t send_id = ((uint16_t)buf[1] << 8) | (uint16_t)buf[2];
            uint8_t status  = buf[3];
            uint32_t rtt_ms = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                            | ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
            uint8_t hops    = buf[8];
            applyOutResult(*id, send_id, status, rtt_ms, hops);
            break;
        }
        case RNSD_DEST_OUT_STATUS: {
            if (n < 4) { warn("OUT_STATUS short (%zu)", n); break; }
            uint16_t send_id = ((uint16_t)buf[1] << 8) | (uint16_t)buf[2];
            uint8_t type     = buf[3];
            applyOutStatus(*id, send_id, type, buf + 4, n - 4);
            break;
        }
        default:
            warn("id %d: unknown mailbox opcode 0x%02x", id->index, (unsigned)buf[0]);
            break;
    }
}

static void onMailboxDisconnect(int handle)
{
    lxmf_id_t* id = idForHandle(handle);
    if (!id) return;
    warn("id %d: mailbox disconnected (handle=%d)", id->index, handle);
    id->handle = -1;
    storageSet(idEphPath(id->index, "up").c_str(), 0);
    /* Reconnect attempted on the next 1 Hz publish tick. */
}

/* ─────────────── command processing (self-clearing keys) ─────────────── */

/* Storage subscriptions are narrow by design: `lxmf.cmd.` for identity-
 * level commands (one sub, installed in lxmfInit) and `lxmf.id.<n>.cmd.`
 * for per-identity commands (one sub per allocated slot, added on
 * createIdentityForSlot/loadIdentityForSlot, removed on destroyIdentity).
 * Firmware-side state writes (`s.lxmf.id.<n>.msgs.*`, ephemeral stats,
 * etc.) are NOT in any subscribed scope, so no self-notify churn. */

/* `processReady` from the pre-sentinel design now triggered via cmd.send.
 * Renamed processSend; body unchanged. Defined above; see processReady. */

static void processSend(lxmf_id_t& id, const std::string& mid)
{
    processReady(id, mid);
}

/* cmd.cancel — abort an in-flight send. If we have an outbox slot for
 * the message, push OUT_CANCEL; rnsd will reply with OUT_RESULT status=2
 * which applyOutResult maps to stage=cancelled. Otherwise the message is
 * already terminal — just stamp the stage. */
static void processCancel(lxmf_id_t& id, const std::string& mid)
{
    outbound_t* o = nullptr;
    for (auto& slot : id.outboxes)
        if (slot.used && slot.msg_key == mid) { o = &slot; break; }

    if (o && id.handle >= 0) {
        uint8_t f[3] = {
            RNSD_DEST_OUT_CANCEL,
            (uint8_t)(o->send_id >> 8),
            (uint8_t)(o->send_id & 0xFF),
        };
        if (itsSend(id.handle, f, sizeof(f), pdMS_TO_TICKS(200)) == 0)
            warn("id %d: cancel frame send dropped (mid=%s)",
                 id.index, mid.c_str());
        dbg("id %d: cancel requested for send_id=%u (mid=%s)",
            id.index, (unsigned)o->send_id, mid.c_str());
        /* applyOutResult will mark stage=cancelled when the OUT_RESULT
         * arrives. */
        return;
    }

    /* Not in flight — mark cancelled directly. */
    storageSet(msgPath(id.index, mid, "stage").c_str(), "cancelled");
}

/* cmd.delete — wipe the message record entirely. Frees `wire` storage. */
static void processDelete(lxmf_id_t& id, const std::string& mid)
{
    char prefix[120];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.%s.",
                  id.index, mid.c_str());
    storageDeleteTree(prefix);
    info("id %d: deleted msg %s", id.index, mid.c_str());
}

/* ── lxmf.cmd.* — identity-level commands ──
 *
 * The handler is fired by the storage subscription pump. Our own
 * storageUnset(key) at the end ALSO fires the subscription on the
 * next dispatch with val="" — that re-entry is filtered by the
 * empty-val guard at the top. The unset happens *last* so a sync
 * waiter polling `storageExists(sentinel)` sees "still present"
 * until the work is done, then "gone" exactly once the work
 * completes. */
static void onIdentityLevelCmd(const char* key, const char* val)
{
    if (!key) return;
    if (!val || !*val) return;   /* self-unset re-fire — ignore */
    const char* tail = key + sizeof("lxmf.cmd.") - 1;
    dbg("cmd.%s val=\"%s\"", tail, val);

    try {
        if (std::strcmp(tail, "identity_new") == 0) {
            int n = idAllocSlot();
            if (n < 0) {
                err("identity_new: no free slot");
            } else if (!createIdentityForSlot(n, val)) {
                err("identity_new: createIdentityForSlot failed");
            } else {
                connectMailbox(s_ids[n]);
            }
        }
        else if (std::strcmp(tail, "identity_import") == 0) {
            std::string hex = val;
            if (hex.size() != 128) {
                err("identity_import: expect 128 hex chars");
            } else {
                int n = idAllocSlot();
                if (n < 0) {
                    err("identity_import: no free slot");
                } else {
                    storageSet(secretsPath(n, "privkey").c_str(), hex.c_str());
                    if (!loadIdentityForSlot(n)) {
                        err("identity_import: load failed; cleaning up");
                        storageDeleteTree(secretsPath(n, "").c_str());
                    } else {
                        storageBegin();
                        storageDefault(idPath(n, "label").c_str(),        "imported");
                        storageDefault(idPath(n, "enabled").c_str(),      1);
                        storageDefault(idPath(n, "display_name").c_str(), "");
                        storageDefault(idPath(n, "stamp_cost").c_str(),   16);
                        storageEnd();
                        connectMailbox(s_ids[n]);
                    }
                }
            }
        }
        else if (std::strcmp(tail, "identity_destroy") == 0) {
            char* end = nullptr;
            long n = std::strtol(val, &end, 10);
            if (!end || *end != '\0' || n < 0 || n >= LXMF_MAX_IDENTITIES) {
                warn("identity_destroy: bad slot \"%s\"", val);
            } else {
                destroyIdentity((int)n);
            }
        }
        else {
            warn("unknown lxmf.cmd: %s", tail);
        }
    } catch (const std::exception& e) {
        err("cmd.%s threw: %s", tail, e.what());
    }

    storageUnset(key);   /* last step — sync waiter sees "processed" now */
}

/* ── lxmf.id.<n>.cmd.* — per-identity commands ──
 *
 * Same shape as onIdentityLevelCmd: ignore the self-unset re-entry
 * (val=""), do the work, unset the sentinel last. */
static void handleIdCmd(int n, const char* key, const char* val)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return;
    if (!val || !*val) return;   /* self-unset re-fire — ignore */
    lxmf_id_t& id = s_ids[n];

    /* key is "lxmf.id.<n>.cmd.<verb>". Find the verb tail. */
    const char* p = std::strstr(key, ".cmd.");
    if (!p) return;
    const char* verb = p + 5;
    dbg("id %d: cmd.%s val=\"%s\"", n, verb, val);

    std::string mid = val;   /* used by send/cancel/delete; ignored by announce */

    try {
        if (std::strcmp(verb, "announce") == 0) {
            if (id.used) sendAnnounce(id);
            else         warn("id %d: announce requested but slot is empty", n);
        }
        else if (!id.used) {
            warn("id %d: cmd.%s for absent identity", n, verb);
        }
        else if (std::strcmp(verb, "send") == 0) {
            processSend(id, mid);
        }
        else if (std::strcmp(verb, "cancel") == 0) {
            processCancel(id, mid);
        }
        else if (std::strcmp(verb, "delete") == 0) {
            processDelete(id, mid);
        }
        else {
            warn("id %d: unknown cmd %s", n, verb);
        }
    } catch (const std::exception& e) {
        err("id %d: cmd.%s threw: %s", n, verb, e.what());
    }

    storageUnset(key);   /* last step — sync waiter sees "processed" now */
}

/* Per-identity static stubs — slot index captured at compile time so the
 * callback signature stays (key, val). */
static void onIdCmd0(const char* key, const char* val) { handleIdCmd(0, key, val); }
static void onIdCmd1(const char* key, const char* val) { handleIdCmd(1, key, val); }
static void onIdCmd2(const char* key, const char* val) { handleIdCmd(2, key, val); }
static void onIdCmd3(const char* key, const char* val) { handleIdCmd(3, key, val); }
static_assert(LXMF_MAX_IDENTITIES == 4,
              "per-identity cmd stubs must match LXMF_MAX_IDENTITIES");
static storage_change_cb_t s_id_cmd_stubs[LXMF_MAX_IDENTITIES] = {
    onIdCmd0, onIdCmd1, onIdCmd2, onIdCmd3,
};

static std::string idCmdScope(int n)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "lxmf.id.%d.cmd.", n);
    return buf;
}

static void subscribePerIdCmds(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return;
    storageSubscribeChanges(idCmdScope(n).c_str(), s_id_cmd_stubs[n]);
}

static void unsubscribePerIdCmds(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return;
    storageUnsubscribe(idCmdScope(n).c_str());
}

/* ─────────────── periodic publish ─────────────── */

static TickType_t s_lastPublishTick = 0;
static TickType_t s_lastSummaryTick = 0;
#define LXMF_PUBLISH_INTERVAL_MS 1000

/* When non-zero, the absolute tick at which any newly-armed announce
 * should fire. Armed once at task startup (covers the case where rnsd
 * brought interfaces up before we subscribed) and re-armed on every
 * `rnsd.iface_event_seq` change. Each re-arm extends the deadline by
 * the full debounce window, which naturally rate-limits a burst of
 * iface-up events (announce fires 10 s after the LAST iface came up,
 * not after each one). */
static TickType_t s_announce_due_tick = 0;
#define LXMF_ANNOUNCE_DEBOUNCE_MS 10000

static void onRnsdIfaceEvent(const char* /*key*/, const char* /*val*/)
{
    s_announce_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LXMF_ANNOUNCE_DEBOUNCE_MS);
}

static void publishStats(void)
{
    storageBegin();
    storageSet("lxmf.up", 1);
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        lxmf_id_t& id = s_ids[n];
        if (!id.used) continue;
        storageSet(idEphPath(n, "stats.sent").c_str(),     (int)id.sent);
        storageSet(idEphPath(n, "stats.received").c_str(), (int)id.received);
        storageSet(idEphPath(n, "stats.pending").c_str(),  (int)id.pending);
        storageSet(idEphPath(n, "stats.failed").c_str(),   (int)id.failed);
    }
    storageEnd();
}

/* collectTokens is defined later (in the CLI section). */
/* Count entries under `lxmf.directory.` without allocating per-entry —
 * each entry is one packed leaf, so we just count leaves. Replaces the
 * old `collectTokens` call which built a std::vector<std::string> per
 * summary tick. */
static int s_dir_count_tmp = 0;
static void dirCountLeaf(const char* key, const char* /*val*/)
{
    if (!key) return;
    const char* tail = key + sizeof("lxmf.directory.") - 1;
    if (std::strchr(tail, '.')) return;
    s_dir_count_tmp++;
}
static int dirCount(void)
{
    s_dir_count_tmp = 0;
    storageForEach("lxmf.directory.", dirCountLeaf);
    return s_dir_count_tmp;
}

/* One-line info() snapshot of LXMF state. Gated on s.lxmf.log_summary
 * (seconds; 0 disables). Aggregates across all identities. */
static void logSummary(void)
{
    int n_used = 0, n_up = 0;
    uint32_t sent = 0, recv = 0, pending = 0, failed = 0;
    for (auto& id : s_ids) {
        if (!id.used) continue;
        n_used++;
        if (id.handle >= 0) n_up++;
        sent    += id.sent;
        recv    += id.received;
        pending += id.pending;
        failed  += id.failed;
    }
    info("ids=%d/%d dir=%d sent=%u recv=%u pending=%u failed=%u%s",
         n_up, n_used, dirCount(),
         (unsigned)sent, (unsigned)recv, (unsigned)pending, (unsigned)failed,
         s_announce_sub_handle < 0 ? " (announce_sub=down)" : "");
}

/* ─────────────── bootstrap ─────────────── */

/* Load every persisted identity slot. No auto-creation — a transport-
 * only node has no identity and is perfectly legitimate; users opt in
 * via `lxmfCreateIdentity(...)` / CLI `lxmf create <name>`. */
static void loadAllIdentities(void)
{
    int loaded = 0;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (loadIdentityForSlot(n)) loaded++;
    }
    if (loaded == 0)
        info("no LXMF identities present — transport-only mode "
             "(use `lxmf create <name>` to add one)");
}

/* ─────────────── CLI ─────────────── */

/* Numbered-list state for the CLI. CLI commands all run on the same
 * (single-threaded) cli task, so plain statics are race-free here. */
static std::vector<std::string> s_peer_list;          /* hex dest hashes */
static std::vector<std::string> s_msgs_list;          /* message_id segments */
static const char*              s_peer_list_label = "";  /* "contacts" / "directory" */

static int selectedId(void)
{
    int n = storageGetInt("s.lxmf.cli.selected_id", 0);
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) n = 0;
    return n;
}

/* Generic walker: collect every distinct `<token>` matching keys of the
 * form `<prefix><token>.<anything>` under storage. Sorted alphabetically
 * by token. */
struct CollectTokensCtx {
    std::string              prefix;
    std::vector<std::string> tokens;
};
static CollectTokensCtx* s_collect_ctx = nullptr;

static void collectTokenLeaf(const char* key, const char* /*val*/)
{
    if (!s_collect_ctx || !key) return;
    if (std::strncmp(key, s_collect_ctx->prefix.c_str(),
                     s_collect_ctx->prefix.size()) != 0) return;
    const char* tail = key + s_collect_ctx->prefix.size();
    const char* dot = std::strchr(tail, '.');
    if (!dot) return;
    std::string t(tail, dot - tail);
    for (const auto& e : s_collect_ctx->tokens) if (e == t) return;
    s_collect_ctx->tokens.push_back(std::move(t));
}

static std::vector<std::string> collectTokens(const std::string& prefix)
{
    CollectTokensCtx ctx{prefix, {}};
    s_collect_ctx = &ctx;
    storageForEach(prefix.c_str(), collectTokenLeaf);
    s_collect_ctx = nullptr;
    return std::move(ctx.tokens);
}

/* Build the "send a message" record + post the cmd.send sentinel. The
 * sentinel fires our per-id cmd subscription on the lxmf task. */
static void cliEnqueueSend(int id_n, const std::string& peer_hex,
                            const std::string& text)
{
    char key[160];
    uint32_t r = cheapRand();
    std::snprintf(key, sizeof(key), "o_%lld_%04x",
                  (long long)(nowUnixMs()), (unsigned)(r & 0xFFFF));
    std::string mid = key;
    storageBegin();
    storageSet(msgPath(id_n, mid, "dir").c_str(),     "out");
    storageSet(msgPath(id_n, mid, "peer").c_str(),    peer_hex.c_str());
    storageSet(msgPath(id_n, mid, "title").c_str(),   "");
    storageSet(msgPath(id_n, mid, "content").c_str(), text.c_str());
    storageSet(msgPath(id_n, mid, "method").c_str(),  "opp");
    storageSet(msgPath(id_n, mid, "stage").c_str(),   "draft");
    /* cmd.send fires *last* so the record is fully present when the
     * lxmf task sees the sentinel. */
    char send_key[40];
    std::snprintf(send_key, sizeof(send_key), "lxmf.id.%d.cmd.send", id_n);
    storageSet(send_key, mid.c_str());
    storageEnd();
    cliPrintf("queued %s → %s\n", mid.c_str(), peer_hex.c_str());
}

/* Resolve a CLI `<peer>` argument. Accepts:
 *   - 32-char hex (the LXMF destination hash) → returned directly
 *   - a positive integer N → the Nth entry from the most recent
 *     `lxmf contacts` / `lxmf directory` listing
 *   - any other text → case-insensitive substring match against
 *     `lxmf.directory.<hex>.display_name`; exactly one match returns
 *     the hash, multiple prints a disambiguation list (name + hash),
 *     zero prints an error.
 * Returns empty string on failure (and prints an explanation). */
struct PeerNameMatch {
    std::string hash;
    std::string name;
};
struct PeerNameLookupCtx {
    std::string query;
    std::vector<PeerNameMatch> matches;
};
static PeerNameLookupCtx* s_peer_name_ctx = nullptr;

static void peerNameLookupLeaf(const char* key, const char* val)
{
    if (!s_peer_name_ctx || !key || !val) return;
    /* key = "lxmf.directory.<hex>" — one packed leaf per dest. */
    const char* tail = key + sizeof("lxmf.directory.") - 1;
    if (std::strchr(tail, '.')) return;   /* skip stray nested keys */
    DirEntry e;
    if (!parseDirValue(val, &e)) return;
    if (!nameContainsCI(e.name, s_peer_name_ctx->query)) return;
    s_peer_name_ctx->matches.push_back({std::string(tail), e.name});
}

static std::string cliResolvePeer(const std::string& arg)
{
    if (arg.size() == 32) {
        uint8_t dh[16];
        if (hexToDestHash(arg, dh)) return arg;
    }
    char* end = nullptr;
    long n = std::strtol(arg.c_str(), &end, 10);
    if (end && *end == '\0' && n >= 1) {
        if (s_peer_list.empty()) {
            cliPrintf("no peer list — run `lxmf contacts` or `lxmf directory` first\n");
            return "";
        }
        if ((size_t)n > s_peer_list.size()) {
            cliPrintf("peer index %ld out of range (last %s had %zu entries)\n",
                      n, s_peer_list_label, s_peer_list.size());
            return "";
        }
        return s_peer_list[(size_t)n - 1];
    }

    /* Treat the remainder as a case-insensitive display-name substring
     * lookup against the cross-identity directory. */
    PeerNameLookupCtx ctx{arg, {}};
    s_peer_name_ctx = &ctx;
    storageForEach("lxmf.directory.", peerNameLookupLeaf);
    s_peer_name_ctx = nullptr;

    if (ctx.matches.empty()) {
        cliPrintf("no peer matches \"%s\"\n", arg.c_str());
        return "";
    }
    if (ctx.matches.size() == 1) return ctx.matches[0].hash;

    /* Multiple matches — refuse the send, but populate the peer list
     * so the user can pick by line number on a retry:
     *   lxmf send 2 "hi"   */
    s_peer_list.clear();
    s_peer_list_label = "directory match";
    cliPrintf("ambiguous \"%s\" — %zu matches:\n",
              arg.c_str(), ctx.matches.size());
    cliPrintf("  %-3s %-32s %s\n", "#", "destination", "name");
    int row = 1;
    for (const auto& m : ctx.matches) {
        s_peer_list.push_back(m.hash);
        cliPrintf("  %-3d %-32s %s\n",
                  row++, m.hash.c_str(), sanitizeForLog(m.name).c_str());
    }
    cliPrintf("(retry with `lxmf send <#> <text>`, a longer substring, or the 32-hex hash)\n");
    return "";
}

/* ── `lxmf id` ── */

static void cliId(const char* rest)
{
    /* No arg: print all. */
    while (*rest == ' ') rest++;
    if (!*rest) {
        int sel = selectedId();
        cliPrintf("  %-3s %-12s %s\n", "id", "label", "destination");
        for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
            lxmf_id_t& id = s_ids[n];
            if (!id.used) continue;
            std::string lbl = storageGetStr(idPath(n, "label").c_str(), "");
            cliPrintf("%s %-3d %-12s %s\n",
                      n == sel ? "*" : " ", n, lbl.c_str(),
                      bytesToHex(id.dest_hash, LXMF_DEST_HASH_LEN).c_str());
        }
        return;
    }
    /* With arg: switch. */
    int n = std::atoi(rest);
    lxmf_id_t* id = idAt(n);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", n); return; }
    storageSet("s.lxmf.cli.selected_id", n);
    cliPrintf("selected id %d (%s)\n", n,
              bytesToHex(id->dest_hash, LXMF_DEST_HASH_LEN).c_str());
}

/* ── `lxmf msgs [<stage>]` ── */

struct MsgRow {
    std::string mid;
    std::string dir;
    std::string stage;
    std::string peer;
    std::string title;
    int         ts;
    int         read;
};

static void cliMsgs(const char* rest)
{
    while (*rest == ' ') rest++;
    std::string filter = rest;
    int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.", sel);

    auto mids = collectTokens(prefix);
    std::vector<MsgRow> rows;
    rows.reserve(mids.size());
    for (const auto& mid : mids) {
        MsgRow r;
        r.mid   = mid;
        r.dir   = storageGetStr(msgPath(sel, mid, "dir").c_str(),   "");
        r.stage = storageGetStr(msgPath(sel, mid, "stage").c_str(), "");
        r.peer  = storageGetStr(msgPath(sel, mid, "peer").c_str(),  "");
        r.title = storageGetStr(msgPath(sel, mid, "title").c_str(), "");
        r.ts    = storageGetInt(msgPath(sel, mid, "ts").c_str(),    0);
        r.read  = storageGetInt(msgPath(sel, mid, "read").c_str(),  0);
        if (!filter.empty() && r.stage != filter) continue;
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const MsgRow& a, const MsgRow& b) { return a.ts > b.ts; });

    s_msgs_list.clear();
    cliPrintf("id %d  %zu message(s)%s%s\n", sel, rows.size(),
              filter.empty() ? "" : " filtered to stage=",
              filter.empty() ? "" : filter.c_str());
    if (rows.empty()) return;
    cliPrintf("  %-3s %-3s %-9s %-16s %-7s %s\n",
              "#", "dir", "stage", "peer", "unread", "title");
    int n = 1;
    for (const auto& r : rows) {
        s_msgs_list.push_back(r.mid);
        std::string peer16 = r.peer.size() >= 16 ? r.peer.substr(0, 16) : r.peer;
        cliPrintf("  %-3d %-3s %-9s %-16s %-7s %s\n",
                  n++,
                  r.dir.c_str(), r.stage.c_str(), peer16.c_str(),
                  (r.dir == "in" && !r.read) ? "yes" : "",
                  r.title.c_str());
    }
}

/* ── `lxmf read <n>` ── */

static void cliRead(const char* rest)
{
    while (*rest == ' ') rest++;
    int n = std::atoi(rest);
    if (n < 1 || (size_t)n > s_msgs_list.size()) {
        cliPrintf("read: index out of range (run `lxmf msgs` first)\n");
        return;
    }
    int sel = selectedId();
    const std::string& mid = s_msgs_list[(size_t)n - 1];

    std::string dir     = storageGetStr(msgPath(sel, mid, "dir").c_str(),     "");
    std::string stage   = storageGetStr(msgPath(sel, mid, "stage").c_str(),   "");
    std::string peer    = storageGetStr(msgPath(sel, mid, "peer").c_str(),    "");
    std::string title   = storageGetStr(msgPath(sel, mid, "title").c_str(),   "");
    std::string content = storageGetStr(msgPath(sel, mid, "content").c_str(), "");
    std::string thread  = storageGetStr(msgPath(sel, mid, "thread").c_str(),  "");
    std::string err_msg = storageGetStr(msgPath(sel, mid, "last_error").c_str(), "");
    int ts              = storageGetInt(msgPath(sel, mid, "ts").c_str(),       0);

    cliPrintf("─── id %d  msg #%d  %s ───\n", sel, n, mid.c_str());
    cliPrintf("dir:    %s\n", dir.c_str());
    cliPrintf("stage:  %s%s%s\n",
              stage.c_str(),
              err_msg.empty() ? "" : "  (",
              err_msg.empty() ? "" : (err_msg + ")").c_str());
    cliPrintf("peer:   %s\n", peer.c_str());
    cliPrintf("ts:     %d\n", ts);
    if (!thread.empty()) cliPrintf("thread: %s\n", thread.c_str());
    if (!title.empty())  cliPrintf("title:  %s\n", title.c_str());
    cliPrintf("\n%s\n", content.c_str());

    if (dir == "in" &&
        storageGetInt(msgPath(sel, mid, "read").c_str(), 0) == 0) {
        storageSet(msgPath(sel, mid, "read").c_str(), 1);
    }
}

/* ── `lxmf contacts` ── */

struct ContactRow {
    std::string hash;
    std::string nick;
    std::string display_name;
    int trust;
    int last_seen;
};

static void cliContacts(void)
{
    int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.contacts.", sel);
    auto hashes = collectTokens(prefix);

    std::vector<ContactRow> rows;
    rows.reserve(hashes.size());
    for (const auto& h : hashes) {
        ContactRow r;
        r.hash         = h;
        r.nick         = storageGetStr(contactPath(sel, h, "nick").c_str(),         "");
        r.display_name = storageGetStr(contactPath(sel, h, "display_name").c_str(), "");
        if (r.display_name.empty()) {
            /* Fall back to the cross-identity directory entry. */
            char dirKey[120];
            std::snprintf(dirKey, sizeof(dirKey),
                          "lxmf.directory.%s.display_name", h.c_str());
            r.display_name = storageGetStr(dirKey, "");
        }
        r.trust     = storageGetInt(contactPath(sel, h, "trust").c_str(),     0);
        r.last_seen = storageGetInt(contactPath(sel, h, "last_seen").c_str(), 0);
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const ContactRow& a, const ContactRow& b) {
                  return a.last_seen > b.last_seen;
              });

    s_peer_list.clear();
    s_peer_list_label = "contacts";

    cliPrintf("id %d  %zu contact(s)\n", sel, rows.size());
    if (rows.empty()) return;
    cliPrintf("  %-3s %-32s %-5s %-12s %s\n",
              "#", "destination", "trust", "nick", "display_name");
    int n = 1;
    for (const auto& r : rows) {
        s_peer_list.push_back(r.hash);
        cliPrintf("  %-3d %-32s %-5d %-12s %s\n",
                  n++, r.hash.c_str(), r.trust,
                  r.nick.c_str(), r.display_name.c_str());
    }
}

/* ── `lxmf directory [<hash>|<name substring>]` ──
 *
 * Three modes:
 *   - no arg               → stream the entire directory
 *   - 32-hex destination   → direct storage path lookup, one row,
 *                            instant (no walk)
 *   - anything else        → stream with display_name substring
 *                            filter (case-insensitive). Non-matching
 *                            rows are short-circuited as soon as their
 *                            display_name is read, since cJSON
 *                            insertion order puts `display_name` near
 *                            the head of each entry.
 *
 * Streaming relies on walkLeaves visiting all leaves under one
 * `<hex>` contiguously before moving to the next sibling (see
 * storage.cpp walkLeavesImpl). We track the current hex; on
 * transition, emit the accumulated row.
 *
 * storageForEach holds CFG_LOCK for the duration; our cliPrintf-in-
 * callback inherits that lock. For a few-thousand-row directory
 * that's ~tens of ms of held lock; lxmf's own storage writes during
 * that window queue at the announce-fanout ITS recv buffer, no data
 * loss. */

/* Streaming directory walker for `lxmf directory` (with optional
 * substring filter). Each `lxmf.directory.<hex>` is a single packed
 * leaf; one row per leaf. */
struct DirStreamState {
    int row_num;
    int now_s;
};
static DirStreamState s_dir_stream;
static std::string    s_dir_filter;        /* empty = no filter; not 32-hex */

static void dirEmitRow(const char* hex, const DirEntry& e)
{
    if (!nameContainsCI(e.name, s_dir_filter)) return;
    int age = (e.last_s > 0) ? (s_dir_stream.now_s - e.last_s) : -1;
    s_dir_stream.row_num++;
    s_peer_list.push_back(hex);
    cliPrintf("  %-3d %-32s %-5d %-5d %-7d %s\n",
              s_dir_stream.row_num, hex,
              e.hops, e.cost, age,
              sanitizeForLog(e.name).c_str());
}

static void dirStreamLeaf(const char* key, const char* val)
{
    if (!key || !val) return;
    const char* tail = key + sizeof("lxmf.directory.") - 1;
    if (std::strchr(tail, '.')) return;   /* skip stray nested keys */
    DirEntry e;
    if (!parseDirValue(val, &e)) return;
    dirEmitRow(tail, e);
}

/* Direct lookup — `lxmf directory <32-hex>`. One row, instant. */
static void cliDirectoryByHash(const std::string& hex)
{
    int now_s = (int)(nowUnixMs() / 1000);
    char k[64];
    std::snprintf(k, sizeof(k), "lxmf.directory.%s", hex.c_str());
    std::string val = storageGetStr(k, "");
    if (val.empty()) {
        cliPrintf("(no directory entry for %s)\n", hex.c_str());
        return;
    }
    DirEntry e;
    if (!parseDirValue(val.c_str(), &e)) {
        cliPrintf("(malformed directory entry for %s)\n", hex.c_str());
        return;
    }
    int age = e.last_s > 0 ? (now_s - e.last_s) : -1;
    cliPrintf("  %-3s %-32s %-5s %-5s %-7s %s\n",
              "#", "destination", "hops", "cost", "age(s)", "name");
    s_peer_list.clear();
    s_peer_list_label = "directory";
    s_peer_list.push_back(hex);
    cliPrintf("  %-3d %-32s %-5d %-5d %-7d %s\n",
              1, hex.c_str(), e.hops, e.cost, age,
              sanitizeForLog(e.name).c_str());
}

static void cliDirectory(const char* rest)
{
    while (rest && *rest == ' ') rest++;
    std::string arg = (rest && *rest) ? std::string(rest) : "";

    /* 32-hex → direct lookup. */
    if (arg.size() == 32) {
        uint8_t dh[16];
        if (hexToDestHash(arg, dh)) { cliDirectoryByHash(arg); return; }
        /* not valid hex despite the length — fall through to substring */
    }

    s_peer_list.clear();
    s_peer_list_label = "directory";

    s_dir_stream       = DirStreamState{};
    s_dir_stream.now_s = (int)(nowUnixMs() / 1000);
    s_dir_filter       = arg;   /* empty = no filter */

    cliPrintf("  %-3s %-32s %-5s %-5s %-7s %s\n",
              "#", "destination", "hops", "cost", "age(s)", "name");

    storageForEach("lxmf.directory.", dirStreamLeaf);

    if (s_dir_stream.row_num == 0) {
        if (arg.empty()) cliPrintf("(no mailboxes heard yet)\n");
        else             cliPrintf("(no match for \"%s\")\n", arg.c_str());
    } else if (arg.empty()) {
        cliPrintf("%d mailbox(es)\n", s_dir_stream.row_num);
    } else {
        cliPrintf("%d match(es) for \"%s\"\n", s_dir_stream.row_num, arg.c_str());
    }
}

/* ── `lxmf send <peer> <text>` ── */

static void cliSend(const char* rest)
{
    while (*rest == ' ') rest++;
    /* First token = peer (digit or 32-hex), remainder = text. */
    const char* sp = std::strchr(rest, ' ');
    if (!sp || sp == rest) {
        cliPrintf("usage: lxmf send <peer> <text>\n");
        cliPrintf("  <peer> = 32-hex destination, or a number from `contacts`/`directory`\n");
        return;
    }
    std::string peer_arg(rest, sp - rest);
    while (*sp == ' ') sp++;
    if (!*sp) { cliPrintf("send: empty message\n"); return; }
    std::string text = sp;

    std::string peer_hex = cliResolvePeer(peer_arg);
    if (peer_hex.empty()) return;

    int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    cliEnqueueSend(sel, peer_hex, text);
}

/* ── dispatch ── */

static void cliLxmf(const char* args)
{
    if (!args) args = "";
    /* Skip leading spaces. */
    while (*args == ' ') args++;

    if (!*args || std::strcmp(args, "help") == 0) {
        cliPrintf("usage:\n");
        cliPrintf("  lxmf create <name>      generate a new identity with display_name=<name>\n");
        cliPrintf("  lxmf destroy <n>        wipe identity at slot <n> (secrets + storage)\n");
        cliPrintf("  lxmf id                 list identities (* = selected)\n");
        cliPrintf("  lxmf id <n>             switch selected identity\n");
        cliPrintf("  lxmf msgs [<stage>]     list messages for selected id (numbered)\n");
        cliPrintf("  lxmf read <n>           print msg n from last listing; marks read\n");
        cliPrintf("  lxmf contacts           list contacts for selected id (numbered)\n");
        cliPrintf("  lxmf directory [<arg>]  every lxmf.delivery announce we've heard;\n");
        cliPrintf("                          arg = 32-hex (instant lookup) or name substring\n");
        cliPrintf("  lxmf send <peer> <msg>  send msg; <peer> = 32-hex, list-#, or display name\n");
        cliPrintf("  lxmf announce           emit a delivery announce for selected id\n");
        return;
    }

    /* Split into verb + rest. */
    const char* sp = std::strchr(args, ' ');
    std::string verb = sp ? std::string(args, sp - args) : std::string(args);
    const char* rest = sp ? sp + 1 : "";

    if (verb == "create") {
        while (*rest == ' ') rest++;
        if (!*rest) {
            cliPrintf("usage: lxmf create <display_name>\n");
            return;
        }
        int slot = lxmfCreateIdentity(rest, /*sync=*/true);
        if (slot < 0) {
            cliPrintf("create failed (see log)\n");
        } else {
            lxmf_id_t* id = idAt(slot);
            cliPrintf("created \"%s\" at slot %d (%s)\n",
                      rest, slot,
                      id ? bytesToHex(id->dest_hash, LXMF_DEST_HASH_LEN).c_str() : "?");
        }
        return;
    }
    if (verb == "destroy") {
        while (*rest == ' ') rest++;
        if (!*rest) {
            cliPrintf("usage: lxmf destroy <slot>\n");
            return;
        }
        char* end = nullptr;
        long n = std::strtol(rest, &end, 10);
        if (!end || *end != '\0' || n < 0 || n >= LXMF_MAX_IDENTITIES) {
            cliPrintf("destroy: bad slot \"%s\"\n", rest);
            return;
        }
        lxmf_id_t* id = idAt((int)n);
        if (!id || !id->used) {
            cliPrintf("destroy: no identity at slot %ld\n", n);
            return;
        }
        if (!lxmfDestroyIdentity((int)n, /*sync=*/true)) {
            cliPrintf("destroy failed (see log)\n");
        } else {
            cliPrintf("destroyed slot %ld\n", n);
        }
        return;
    }
    if (verb == "id")        { cliId(rest); return; }
    if (verb == "msgs")      { cliMsgs(rest); return; }
    if (verb == "read")      { cliRead(rest); return; }
    if (verb == "contacts")  { cliContacts(); return; }
    if (verb == "directory") { cliDirectory(rest); return; }
    if (verb == "send")      { cliSend(rest); return; }
    if (verb == "announce") {
        int sel = selectedId();
        lxmf_id_t* id = idAt(sel);
        if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
        /* Sentinel — actual send runs on the lxmf task (which owns the
         * mailbox handle). Storage subscription wakes it. */
        storageSet(idEphPath(sel, "cmd.announce").c_str(), 1);
        cliPrintf("announce requested for id %d (%s)\n",
                  sel, bytesToHex(id->dest_hash, LXMF_DEST_HASH_LEN).c_str());
        return;
    }

    cliPrintf("unknown subcommand `%s`. try `lxmf help`.\n", verb.c_str());
}

/* ─────────────── task ─────────────── */

static TickType_t nextDeadline(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t due = s_lastPublishTick + pdMS_TO_TICKS(LXMF_PUBLISH_INTERVAL_MS);
    if (due <= now) return 0;
    return due - now;
}

static void lxmfTaskMain(void*)
{
    info("[%s] task up", TAG);

    /* itsClient initialisation — one connection per identity plus the
     * shared announce-fanout subscription. */
    itsClientInit(LXMF_MAX_IDENTITIES + 1);

    /* Identity-level commands (clients write `lxmf.cmd.identity_*`). All
     * per-identity command subs are added by createIdentityForSlot /
     * loadIdentityForSlot via subscribePerIdCmds. */
    storageSubscribeChanges("lxmf.cmd.", onIdentityLevelCmd);

    /* Re-arm the 10 s announce-debounce window whenever rnsd brings an
     * iface up (rnsd writes rnsd.iface_event_seq on every iface-up
     * transition — narrow single-key subscription, no filtering). */
    storageSubscribeChanges("rnsd.iface_event_seq", onRnsdIfaceEvent);

    /* Bootstrap. rnsd should be up by the time the first events arrive;
     * we'll just block in itsConnect if it's not. loadAllIdentities
     * calls loadIdentityForSlot which installs per-id cmd subs. We do
     * NOT announce here — that's driven by iface_event_seq + the
     * periodic schedule, both checked from the 1 Hz tick. */
    loadAllIdentities();
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_ids[n].used) connectMailbox(s_ids[n]);
    }

    /* Subscribe to lxmf.delivery announces via rnsd's fan-out port. All
     * directory writes happen on this task — rnsd's task only memcpys
     * the announce into one ITS packet. */
    connectAnnounceSub();

    /* Arm the initial announce debounce window. Covers the case where
     * rnsd brought ifaces up before our subscription landed; either
     * way, the first announce fires 10 s from now (or later if more
     * ifaces come up in the meantime). */
    s_announce_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LXMF_ANNOUNCE_DEBOUNCE_MS);

    s_lastPublishTick = xTaskGetTickCount();
    publishStats();

    for (;;) {
        itsPoll(nextDeadline());

        TickType_t now = xTaskGetTickCount();
        if (now - s_lastPublishTick >= pdMS_TO_TICKS(LXMF_PUBLISH_INTERVAL_MS)) {
            publishStats();
            /* Reconnect anything that dropped since the last tick. */
            for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                lxmf_id_t& id = s_ids[n];
                if (!id.used || id.handle >= 0) continue;
                connectMailbox(id);
            }
            if (s_announce_sub_handle < 0) connectAnnounceSub();

            /* Debounced announce: fires once 10 s after the last iface
             * came up (per onRnsdIfaceEvent). Re-armed by each
             * iface-up; this branch runs at most once per debounce
             * window. */
            if (s_announce_due_tick != 0 &&
                (int32_t)(now - s_announce_due_tick) >= 0) {
                s_announce_due_tick = 0;
                for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                    lxmf_id_t& id = s_ids[n];
                    if (id.used && id.handle >= 0) sendAnnounce(id);
                }
            }

            /* Periodic re-announce per identity. Interval read live
             * from storage; 0 disables periodic. Identities whose
             * mailbox is currently down are skipped (they'll
             * re-announce on the next iface_event_seq or once the
             * mailbox comes back). */
            int announce_s = storageGetInt("s.lxmf.announce_interval_s", 1800);
            if (announce_s > 0) {
                TickType_t threshold = pdMS_TO_TICKS(announce_s * 1000);
                for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                    lxmf_id_t& id = s_ids[n];
                    if (!id.used || id.handle < 0) continue;
                    if (id.last_announce_tick == 0) continue;   /* never — wait for trigger */
                    if (now - id.last_announce_tick >= threshold)
                        sendAnnounce(id);
                }
            }

            /* Periodic info() summary line. Interval is read live from
             * storage; 0 disables. */
            int summary_s = storageGetInt("s.lxmf.log_summary", 60);
            if (summary_s > 0 &&
                now - s_lastSummaryTick >= pdMS_TO_TICKS(summary_s * 1000)) {
                logSummary();
                s_lastSummaryTick = now;
            }

            s_lastPublishTick = now;
        }
    }
}

void lxmfInit(void)
{
    /* Storage defaults gated on version. */
    if (storageGetInt("s.lxmf.version", 0) < LXMF_VERSION) {
        storageBegin();
        storageDefault("s.lxmf.enable",                 1);
        storageDefault("s.lxmf.enforce_stamps",         0);
        storageDefault("s.lxmf.auto_ticket",            1);
        storageDefault("s.lxmf.log_summary",            60);    /* seconds; 0 disables */
        storageDefault("s.lxmf.announce_interval_s",    1800);  /* periodic re-announce; 0 disables */
        storageDefault("s.lxmf.max_dir_size",           2048);  /* directory cap; 0 disables eviction */
        storageSet("s.lxmf.version", LXMF_VERSION);
        storageEnd();
    }

    cliRegisterCmd("lxmf", cliLxmf);

    /* Core 1, prio 1, 8 KB PSRAM stack per the plan. */
    s_task = spawnTask(lxmfTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
}

/* ─────────────── public API (lxmf.h) ─────────────── */

/* Block (via vTaskDelay) until `sentinel_key` no longer exists in
 * storage, or `timeout` elapses. The lxmf task always `storageUnset`s
 * the sentinel as the first step of processing a cmd, so disappearance
 * means "lxmf observed it." Independent of ITS / aux delivery — works
 * from any task. */
static bool waitForCmdProcessed(const char* sentinel_key, TickType_t timeout)
{
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (storageExists(sentinel_key)) {
        if ((int)(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

int lxmfCreateIdentity(const char* display_name, bool sync)
{
    if (!display_name || !*display_name) {
        warn("lxmfCreateIdentity: missing display name");
        return -1;
    }
    storageSet("lxmf.cmd.identity_new", display_name);
    if (!sync) return 0;

    if (!waitForCmdProcessed("lxmf.cmd.identity_new", pdMS_TO_TICKS(5000))) {
        warn("lxmfCreateIdentity: timeout waiting for lxmf task");
        return -1;
    }

    /* Sentinel cleared — find the slot whose display_name matches our
     * request. The lxmf task seeds `s.lxmf.id.<n>.display_name` during
     * createIdentityForSlot; if no slot matches, the create failed
     * (e.g. no free slot, invalid name) and the reason is in [lxmf]
     * err() output. */
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        std::string cur = storageGetStr(idPath(n, "display_name").c_str(), "");
        if (cur == display_name) return n;
    }
    return -1;
}

bool lxmfDestroyIdentity(int n, bool sync)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) {
        warn("lxmfDestroyIdentity: bad slot %d", n);
        return false;
    }
    storageSet("lxmf.cmd.identity_destroy", n);
    if (!sync) return true;

    if (!waitForCmdProcessed("lxmf.cmd.identity_destroy", pdMS_TO_TICKS(5000))) {
        warn("lxmfDestroyIdentity: timeout waiting for lxmf task");
        return false;
    }

    /* destroyIdentity wipes secrets.lxmf.id.<n>.privkey. If the key is
     * still there, the destroy failed (validation, etc.) — see [lxmf]
     * warn() output. */
    if (storageExists(secretsPath(n, "privkey").c_str())) {
        warn("lxmfDestroyIdentity: sentinel cleared but secrets still present");
        return false;
    }
    return true;
}
