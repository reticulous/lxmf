/* lxmproxy_wire — the LXMF-proxy shared wire format. See lxmproxy_wire.h for
 * the frame ladder and what each one means. Deliberately self-contained: its
 * own minimal msgpack (fixarray, fixmap, bin, str, uint, nil — the subset these
 * frames use) rather than a dependency on lxmf.cpp's file-local codec, and no
 * crypto at all. */

#include "lxmproxy_wire.h"

#include <algorithm>
#include <cstring>

/* ─────────────── minimal msgpack ─────────────── */

static void mpArr(std::vector<uint8_t>& o, size_t n)
{
    if (n < 16) { o.push_back((uint8_t)(0x90 | (n & 0x0F))); return; }
    o.push_back(0xDC); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)(n & 0xFF));
}

static void mpMap(std::vector<uint8_t>& o, size_t n)
{
    if (n < 16) { o.push_back((uint8_t)(0x80 | (n & 0x0F))); return; }
    o.push_back(0xDE); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)(n & 0xFF));
}

static void mpNil(std::vector<uint8_t>& o) { o.push_back(0xC0); }

static void mpUint(std::vector<uint8_t>& o, uint32_t v)
{
    if (v < 0x80)         { o.push_back((uint8_t)v); }
    else if (v <= 0xFF)   { o.push_back(0xCC); o.push_back((uint8_t)v); }
    else if (v <= 0xFFFF) { o.push_back(0xCD); o.push_back((uint8_t)(v >> 8));
                            o.push_back((uint8_t)(v & 0xFF)); }
    else                  { o.push_back(0xCE); o.push_back((uint8_t)(v >> 24));
                            o.push_back((uint8_t)((v >> 16) & 0xFF));
                            o.push_back((uint8_t)((v >> 8) & 0xFF));
                            o.push_back((uint8_t)(v & 0xFF)); }
}

static void mpBin(std::vector<uint8_t>& o, const uint8_t* p, size_t n)
{
    if (n <= 0xFF)        { o.push_back(0xC4); o.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { o.push_back(0xC5); o.push_back((uint8_t)(n >> 8));
                            o.push_back((uint8_t)(n & 0xFF)); }
    else                  { o.push_back(0xC6); o.push_back((uint8_t)((n >> 24) & 0xFF));
                            o.push_back((uint8_t)((n >> 16) & 0xFF));
                            o.push_back((uint8_t)((n >> 8) & 0xFF));
                            o.push_back((uint8_t)(n & 0xFF)); }
    o.insert(o.end(), p, p + n);
}

/* Text goes on the wire as BIN, like everything else here: the only consumers
 * are the two ends of this protocol, and one representation is one thing to get
 * wrong. */
static void mpStr(std::vector<uint8_t>& o, const std::string& s)
{
    mpBin(o, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static void mpStrC(std::vector<uint8_t>& o, const char* s)
{
    size_t n = s ? std::strlen(s) : 0;
    mpBin(o, reinterpret_cast<const uint8_t*>(s), n);
}

/* Optional fixed-width bin: null pointer packs nil. */
static void mpBinOrNil(std::vector<uint8_t>& o, const uint8_t* p, size_t n)
{
    if (p) mpBin(o, p, n); else mpNil(o);
}

/* Walking parser state. */
struct MpIn { const uint8_t* p; size_t n; size_t i; };

static bool mpInUint(MpIn& s, uint32_t& v)
{
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b < 0x80) { v = b; return true; }
    auto need = [&](size_t k) { return s.i + k <= s.n; };
    if (b == 0xCC) { if (!need(1)) return false; v = s.p[s.i]; s.i += 1; return true; }
    if (b == 0xCD) { if (!need(2)) return false;
                     v = ((uint32_t)s.p[s.i] << 8) | s.p[s.i + 1]; s.i += 2; return true; }
    if (b == 0xCE) { if (!need(4)) return false;
                     v = ((uint32_t)s.p[s.i] << 24) | ((uint32_t)s.p[s.i + 1] << 16) |
                         ((uint32_t)s.p[s.i + 2] << 8) | s.p[s.i + 3]; s.i += 4; return true; }
    return false;
}

/* nil → true with len 0 and *is_nil set, so an optional element reads the same
 * way a present one does. */
static bool mpInBin(MpIn& s, const uint8_t*& p, size_t& len, bool* is_nil = nullptr)
{
    if (is_nil) *is_nil = false;
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b == 0xC0) { p = nullptr; len = 0; if (is_nil) *is_nil = true; return true; }
    size_t l;
    if      (b == 0xC4) { if (s.i + 1 > s.n) return false; l = s.p[s.i]; s.i += 1; }
    else if (b == 0xC5) { if (s.i + 2 > s.n) return false;
                          l = ((size_t)s.p[s.i] << 8) | s.p[s.i + 1]; s.i += 2; }
    else if (b == 0xC6) { if (s.i + 4 > s.n) return false;
                          l = ((size_t)s.p[s.i] << 24) | ((size_t)s.p[s.i + 1] << 16) |
                              ((size_t)s.p[s.i + 2] << 8) | s.p[s.i + 3]; s.i += 4; }
    else if (b >= 0xA0 && b <= 0xBF) { l = b & 0x1F; }                              /* fixstr */
    else if (b == 0xD9) { if (s.i + 1 > s.n) return false; l = s.p[s.i]; s.i += 1; } /* str8 */
    else if (b == 0xDA) { if (s.i + 2 > s.n) return false;
                          l = ((size_t)s.p[s.i] << 8) | s.p[s.i + 1]; s.i += 2; }    /* str16 */
    else if (b == 0xDB) { if (s.i + 4 > s.n) return false;
                          l = ((size_t)s.p[s.i] << 24) | ((size_t)s.p[s.i + 1] << 16) |
                              ((size_t)s.p[s.i + 2] << 8) | s.p[s.i + 3]; s.i += 4; } /* str32 */
    else return false;
    if (s.i + l > s.n) return false;
    p = s.p + s.i; len = l; s.i += l;
    return true;
}

static bool mpInStr(MpIn& s, std::string& out)
{
    const uint8_t* p; size_t l; bool nil = false;
    if (!mpInBin(s, p, l, &nil)) return false;
    out.assign(p ? (const char*)p : "", l);
    return true;
}

/* Fixed-width bin into a fixed buffer. A nil (or a wrong length) leaves the
 * buffer alone and clears *have — an optional element, read the same way. */
static bool mpInBinN(MpIn& s, uint8_t* out, size_t want, bool* have)
{
    const uint8_t* p; size_t l; bool nil = false;
    if (!mpInBin(s, p, l, &nil)) return false;
    if (nil || l != want) { if (have) *have = false; return true; }
    std::memcpy(out, p, want);
    if (have) *have = true;
    return true;
}

static bool mpInArr(MpIn& s, size_t& cnt)
{
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b >= 0x90 && b <= 0x9F) { cnt = b & 0x0F; return true; }
    if (b == 0xDC) { if (s.i + 2 > s.n) return false;
                     cnt = ((size_t)s.p[s.i] << 8) | s.p[s.i + 1]; s.i += 2; return true; }
    return false;
}

static bool mpInMap(MpIn& s, size_t& cnt)
{
    if (s.i >= s.n) return false;
    uint8_t b = s.p[s.i++];
    if (b >= 0x80 && b <= 0x8F) { cnt = b & 0x0F; return true; }
    if (b == 0xDE) { if (s.i + 2 > s.n) return false;
                     cnt = ((size_t)s.p[s.i] << 8) | s.p[s.i + 1]; s.i += 2; return true; }
    return false;
}

/* ─────────────── builders ─────────────── */

std::vector<uint8_t> lxmproxyBuildHello(const char* label, uint32_t quota_kb,
                                        uint32_t max_envelope_kb,
                                        uint32_t retain_days, bool serving,
                                        const char* reason)
{
    std::vector<uint8_t> o;
    mpArr(o, 7);
    mpUint(o, LXMPROXY_FR_HELLO);
    mpStrC(o, label);
    mpUint(o, quota_kb);
    mpUint(o, max_envelope_kb);
    mpUint(o, retain_days);
    mpUint(o, serving ? 1 : 0);
    mpStrC(o, reason);
    return o;
}

std::vector<uint8_t> lxmproxyBuildHandover(const uint8_t privkey[64],
                                           const char* display_name,
                                           const std::string& ratchets)
{
    std::vector<uint8_t> o;
    mpArr(o, 4);
    mpUint(o, LXMPROXY_FR_HANDOVER);
    mpBin(o, privkey, 64);
    mpStrC(o, display_name);
    mpStr(o, ratchets);
    return o;
}

std::vector<uint8_t> lxmproxyBuildServing(bool ok, const char* reason, bool hold)
{
    std::vector<uint8_t> o;
    mpArr(o, 4);
    mpUint(o, LXMPROXY_FR_SERVING);
    mpUint(o, ok ? 1 : 0);
    mpStrC(o, reason);
    mpUint(o, hold ? 1 : 0);
    return o;
}

std::vector<uint8_t> lxmproxyBuildMsg(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                      const uint8_t peer[LXMPROXY_DEST_LEN],
                                      const char* peer_name, uint32_t ts,
                                      const std::string& title, uint32_t size,
                                      const std::string* body,
                                      const uint8_t* reply_to,
                                      const std::string& reply_quote)
{
    std::vector<uint8_t> o;
    mpArr(o, 10);
    mpUint(o, LXMPROXY_FR_MSG);
    mpBin(o, msg_id, LXMPROXY_MID_LEN);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    mpStrC(o, peer_name);
    mpUint(o, ts);
    mpStr(o, title);
    mpUint(o, size);
    if (body) mpStr(o, *body); else mpNil(o);
    mpBinOrNil(o, reply_to, LXMPROXY_MID_LEN);
    mpStr(o, reply_quote);
    return o;
}

std::vector<uint8_t> lxmproxyBuildFetch(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                        const uint8_t peer[LXMPROXY_DEST_LEN])
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, LXMPROXY_FR_FETCH);
    mpBin(o, msg_id, LXMPROXY_MID_LEN);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    return o;
}

std::vector<uint8_t> lxmproxyBuildBody(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                       const uint8_t peer[LXMPROXY_DEST_LEN],
                                       const std::string& content,
                                       const uint8_t* reply_to,
                                       const std::string& reply_quote)
{
    std::vector<uint8_t> o;
    mpArr(o, 6);
    mpUint(o, LXMPROXY_FR_BODY);
    mpBin(o, msg_id, LXMPROXY_MID_LEN);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    mpStr(o, content);
    mpBinOrNil(o, reply_to, LXMPROXY_MID_LEN);
    mpStr(o, reply_quote);
    return o;
}

std::vector<uint8_t> lxmproxyBuildHanded(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                         const uint8_t peer[LXMPROXY_DEST_LEN])
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, LXMPROXY_FR_HANDED);
    mpBin(o, msg_id, LXMPROXY_MID_LEN);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    return o;
}

std::vector<uint8_t> lxmproxyBuildSend(const std::string& local_key,
                                       const uint8_t peer[LXMPROXY_DEST_LEN],
                                       uint32_t ts, const std::string& title,
                                       const std::string& content,
                                       const uint8_t* reply_to,
                                       const std::string& reply_quote,
                                       const char* method, const uint8_t* pn)
{
    std::vector<uint8_t> o;
    mpArr(o, 10);
    mpUint(o, LXMPROXY_FR_SEND);
    mpStr(o, local_key);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    mpUint(o, ts);
    mpStr(o, title);
    mpStr(o, content);
    mpBinOrNil(o, reply_to, LXMPROXY_MID_LEN);
    mpStr(o, reply_quote);
    mpStrC(o, method);
    mpBinOrNil(o, pn, LXMPROXY_DEST_LEN);
    return o;
}

std::vector<uint8_t> lxmproxyBuildStatus(const std::string& key,
                                         const uint8_t peer[LXMPROXY_DEST_LEN],
                                         uint32_t status,
                                         uint32_t ts, const uint8_t* message_id)
{
    std::vector<uint8_t> o;
    mpArr(o, 6);
    mpUint(o, LXMPROXY_FR_STATUS);
    mpStr(o, key);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    mpUint(o, status);
    mpUint(o, ts);
    mpBinOrNil(o, message_id, LXMPROXY_MID_LEN);
    return o;
}

/* DROP and RETRY name a record and nothing else — the body is already on both
 * ends, and the whole point of each is to act on it without moving it. */
static std::vector<uint8_t> buildKeyPeerFrame(uint8_t type, const std::string& key,
                                              const uint8_t peer[LXMPROXY_DEST_LEN])
{
    std::vector<uint8_t> o;
    mpArr(o, 3);
    mpUint(o, type);
    mpStr(o, key);
    mpBin(o, peer, LXMPROXY_DEST_LEN);
    return o;
}

std::vector<uint8_t> lxmproxyBuildDrop(const std::string& key,
                                       const uint8_t peer[LXMPROXY_DEST_LEN])
{
    return buildKeyPeerFrame(LXMPROXY_FR_DROP, key, peer);
}

std::vector<uint8_t> lxmproxyBuildRetry(const std::string& key,
                                        const uint8_t peer[LXMPROXY_DEST_LEN])
{
    return buildKeyPeerFrame(LXMPROXY_FR_RETRY, key, peer);
}

static std::vector<uint8_t> buildMapFrame(
        uint8_t type, const std::vector<std::pair<std::string, std::string>>& map)
{
    std::vector<uint8_t> o;
    mpArr(o, 2);
    mpUint(o, type);
    mpMap(o, map.size());
    for (const auto& kv : map) { mpStr(o, kv.first); mpStr(o, kv.second); }
    return o;
}

std::vector<uint8_t> lxmproxyBuildConfig(
        const std::vector<std::pair<std::string, std::string>>& map)
{
    return buildMapFrame(LXMPROXY_FR_CONFIG, map);
}

std::vector<uint8_t> lxmproxyBuildState(
        const std::vector<std::pair<std::string, std::string>>& map)
{
    return buildMapFrame(LXMPROXY_FR_STATE, map);
}

std::vector<uint8_t> lxmproxyBuildRelease(void)
{
    std::vector<uint8_t> o;
    mpArr(o, 1);
    mpUint(o, LXMPROXY_FR_RELEASE);
    return o;
}

std::vector<uint8_t> lxmproxyBuildRatchets(const std::string& record)
{
    std::vector<uint8_t> o;
    mpArr(o, 2);
    mpUint(o, LXMPROXY_FR_RATCHETS);
    mpStr(o, record);
    return o;
}

/* ─────────────── parse ─────────────── */

bool lxmproxyParse(const uint8_t* p, size_t n, LxmproxyFrame& out)
{
    out = LxmproxyFrame{};
    if (!p || n == 0) return false;
    MpIn s{p, n, 0};
    size_t cnt = 0;
    if (!mpInArr(s, cnt) || cnt < 1) return false;
    uint32_t type = 0;
    if (!mpInUint(s, type)) return false;
    out.type = (uint8_t)type;

    switch (out.type) {
        case LXMPROXY_FR_HELLO: {
            if (cnt < 7) return false;
            if (!mpInStr(s, out.label))            return false;
            if (!mpInUint(s, out.quota_kb))        return false;
            if (!mpInUint(s, out.max_envelope_kb)) return false;
            if (!mpInUint(s, out.retain_days))     return false;
            if (!mpInUint(s, out.serving))         return false;
            if (!mpInStr(s, out.reason))           return false;
            return true;
        }
        case LXMPROXY_FR_HANDOVER: {
            if (cnt < 4) return false;
            if (!mpInBinN(s, out.privkey, 64, &out.have_privkey)) return false;
            if (!out.have_privkey)                 return false;
            if (!mpInStr(s, out.display_name))     return false;
            if (!mpInStr(s, out.ratchets))         return false;
            return true;
        }
        case LXMPROXY_FR_SERVING: {
            if (cnt < 4) return false;
            if (!mpInUint(s, out.ok))     return false;
            if (!mpInStr(s, out.reason))  return false;
            if (!mpInUint(s, out.hold))   return false;
            return true;
        }
        case LXMPROXY_FR_MSG: {
            if (cnt < 10) return false;
            if (!mpInBinN(s, out.msg_id, LXMPROXY_MID_LEN, &out.have_msg_id)) return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer))    return false;
            if (!out.have_msg_id || !out.have_peer) return false;
            if (!mpInStr(s, out.peer_name)) return false;
            if (!mpInUint(s, out.ts))       return false;
            if (!mpInStr(s, out.title))     return false;
            if (!mpInUint(s, out.size))     return false;
            {
                const uint8_t* bp; size_t bl; bool nil = false;
                if (!mpInBin(s, bp, bl, &nil)) return false;
                if (!nil) { out.content.assign(bp ? (const char*)bp : "", bl);
                            out.have_body = true; }
            }
            if (!mpInBinN(s, out.reply_to, LXMPROXY_MID_LEN, &out.have_reply_to)) return false;
            if (!mpInStr(s, out.reply_quote)) return false;
            return true;
        }
        case LXMPROXY_FR_FETCH:
        case LXMPROXY_FR_HANDED: {
            if (cnt < 3) return false;
            if (!mpInBinN(s, out.msg_id, LXMPROXY_MID_LEN, &out.have_msg_id)) return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer))    return false;
            return out.have_msg_id && out.have_peer;
        }
        case LXMPROXY_FR_BODY: {
            if (cnt < 6) return false;
            if (!mpInBinN(s, out.msg_id, LXMPROXY_MID_LEN, &out.have_msg_id)) return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer))    return false;
            if (!out.have_msg_id || !out.have_peer) return false;
            if (!mpInStr(s, out.content)) return false;
            out.have_body = true;
            if (!mpInBinN(s, out.reply_to, LXMPROXY_MID_LEN, &out.have_reply_to)) return false;
            if (!mpInStr(s, out.reply_quote)) return false;
            return true;
        }
        case LXMPROXY_FR_SEND: {
            if (cnt < 10) return false;
            if (!mpInStr(s, out.key))                                      return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer)) return false;
            if (!out.have_peer)                                            return false;
            if (!mpInUint(s, out.ts))                                      return false;
            if (!mpInStr(s, out.title))                                    return false;
            if (!mpInStr(s, out.content))                                  return false;
            if (!mpInBinN(s, out.reply_to, LXMPROXY_MID_LEN, &out.have_reply_to)) return false;
            if (!mpInStr(s, out.reply_quote))                              return false;
            if (!mpInStr(s, out.method))                                   return false;
            if (!mpInBinN(s, out.pn, LXMPROXY_DEST_LEN, &out.have_pn))     return false;
            out.have_body = true;
            return true;
        }
        case LXMPROXY_FR_STATUS: {
            if (cnt < 6) return false;
            if (!mpInStr(s, out.key))                                      return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer)) return false;
            if (!out.have_peer)                                            return false;
            if (!mpInUint(s, out.status))  return false;
            if (!mpInUint(s, out.ts))      return false;
            if (!mpInBinN(s, out.msg_id, LXMPROXY_MID_LEN, &out.have_msg_id)) return false;
            return true;
        }
        case LXMPROXY_FR_DROP:
        case LXMPROXY_FR_RETRY: {
            if (cnt < 3) return false;
            if (!mpInStr(s, out.key))                                      return false;
            if (!mpInBinN(s, out.peer, LXMPROXY_DEST_LEN, &out.have_peer)) return false;
            return out.have_peer;
        }
        case LXMPROXY_FR_CONFIG:
        case LXMPROXY_FR_STATE: {
            if (cnt < 2) return false;
            size_t m = 0;
            if (!mpInMap(s, m)) return false;
            out.map.reserve(m);
            for (size_t k = 0; k < m; ++k) {
                std::string key, val;
                if (!mpInStr(s, key)) return false;
                if (!mpInStr(s, val)) return false;
                out.map.emplace_back(std::move(key), std::move(val));
            }
            return true;
        }
        case LXMPROXY_FR_RELEASE:
            return true;
        case LXMPROXY_FR_RATCHETS: {
            if (cnt < 2) return false;
            return mpInStr(s, out.ratchets);
        }
        default:
            return false;
    }
}

const char* lxmproxyFrameName(uint8_t type)
{
    switch (type) {
        case LXMPROXY_FR_HELLO:    return "HELLO";
        case LXMPROXY_FR_HANDOVER: return "HANDOVER";
        case LXMPROXY_FR_SERVING:  return "SERVING";
        case LXMPROXY_FR_MSG:      return "MSG";
        case LXMPROXY_FR_FETCH:    return "FETCH";
        case LXMPROXY_FR_BODY:     return "BODY";
        case LXMPROXY_FR_HANDED:   return "HANDED";
        case LXMPROXY_FR_SEND:     return "SEND";
        case LXMPROXY_FR_STATUS:   return "STATUS";
        case LXMPROXY_FR_DROP:     return "DROP";
        case LXMPROXY_FR_RETRY:    return "RETRY";
        case LXMPROXY_FR_CONFIG:   return "CONFIG";
        case LXMPROXY_FR_STATE:    return "STATE";
        case LXMPROXY_FR_RELEASE:  return "RELEASE";
        case LXMPROXY_FR_RATCHETS: return "RATCHETS";
        default:                   return "?";
    }
}

/* ─────────────── announce app_data ─────────────── */

std::vector<uint8_t> lxmproxyBuildAnnounce(const char* label)
{
    std::vector<uint8_t> o;
    mpArr(o, 1);
    mpStrC(o, label);
    return o;
}

bool lxmproxyParseAnnounce(const uint8_t* p, size_t n, std::string& label_out)
{
    label_out.clear();
    if (!p || n == 0) return false;
    MpIn s{p, n, 0};
    size_t cnt = 0;
    if (!mpInArr(s, cnt) || cnt < 1) return false;
    return mpInStr(s, label_out);
}

/* ─────────────── the inline-body threshold ─────────────── */

uint32_t lxmproxyInlineThreshold(uint32_t rtt_ms, uint32_t override_bytes)
{
    if (override_bytes) return override_bytes;
    /* 8 KB is comfortable on a link that answers in a quarter second; a slower
     * link earns proportionally less, because the push is what the user is
     * waiting behind. The clamp is what keeps a first-measurement outlier — or
     * a link that has not been measured at all — from either withholding
     * everything or pushing a body over a radio for minutes. */
    static constexpr uint32_t kRefBytes = 8192;
    static constexpr uint32_t kRefRttMs = 250;
    static constexpr uint32_t kFloor    = 256;
    static constexpr uint32_t kCeiling  = 16384;
    if (rtt_ms == 0) return kFloor;
    uint64_t v = ((uint64_t)kRefBytes * kRefRttMs) / rtt_ms;
    return (uint32_t)std::min<uint64_t>(std::max<uint64_t>(v, kFloor), kCeiling);
}
