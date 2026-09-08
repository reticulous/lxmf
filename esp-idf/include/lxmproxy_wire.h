/* lxmproxy_wire — the LXMF-proxy protocol's shared wire format.
 *
 *   C → S   CHANNEL (rnsd identifies with the account identity)
 *   S → C   HELLO    [label, limits, serving?]        as soon as the identify is read
 *   C → S   HANDOVER [privkey, display_name, ratchets]
 *   S → C   SERVING  [ok, reason]                     the account is on the air
 *   S → C   MSG      [msg_id, peer, name, ts, title, size, body?]   pushed live
 *   C → S   FETCH    [msg_id, peer]                   a withheld body
 *   S → C   BODY     [msg_id, peer, content, reply_to]  Resource when large
 *   C → S   HANDED   [msg_id, peer]                   after persist
 *   C → S   SEND     [local_key, peer, ts, title, content, reply_to, method, pn]
 *   S → C   STATUS   [key, peer, status, ts, message_id?]  live status
 *   C → S   SETTLED  [key, peer]                      terminal status seen
 *   C ↔ S   CONFIG   [map]                            account settings
 *   S → C   STATE    [map]                            what only the server knows
 *   C → S   RELEASE  []                               deprovision
 *   S → C   RATCHETS [record]                         release hands the set back
 *
 * One device — the **server** — holds an LXMF account's keys, registers and
 * announces its `lxmf.delivery`, and does all the sending and receiving. The
 * other — the **client** — holds the same keys, registers nothing, and
 * exchanges messages with the server over a permanently-held RNS Channel. The
 * rest of the network sees an ordinary always-online LXMF node.
 *
 * The header lives in the lxmf straddle so the client half needs no dependency
 * on the lxmproxy straddle (lxmproxy depends on lxmf, not the other way round).
 * Self-contained: its own minimal msgpack — the subset these frames use
 * (fixarray, fixmap, bin, str, uint, nil) — and no crypto of its own.
 *
 * Every frame is one msgpack array whose element [0] is the frame type, so a
 * single parser dispatches. A frame rides one Channel message when it fits the
 * channel MDU and a Resource on the Channel's Link when it does not; the bytes
 * are identical either way, so the parse does not care which arrived.
 *
 * Every frame that names a message also carries the PEER, because a message
 * lives at `msgs.<peer>.<key>` on both ends and neither side may depend on a
 * RAM map surviving a reboot to find it again.
 *
 * There is no version field. Everything flashes together; the wire changes
 * outright.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/** The server's destination aspect. Named for the protocol rather than for
 *  this project, so another implementation can speak it. */
#define LXMPROXY_ASPECT      "lxmproxy.server"

/** Destination-hash and message-id widths, matching LXMF's own. */
#define LXMPROXY_DEST_LEN    16
#define LXMPROXY_MID_LEN     32

/** ITS port on the SERVER task where rnsd back-connects each accepted inbound
 *  Channel (rnsdDestListenChannels). The Resource lifecycle for those Channels
 *  arrives on RNSD_LINK_RESOURCE_AUX_PORT, the port every link consumer shares
 *  (ports.h) — there is no per-consumer one. */
#define LXMPROXY_CHAN_PORT   150

enum LxmproxyFrameType : uint8_t {
    /* S→C  [1, label str, quota_kb u32, max_envelope_kb u32, retain_days u32,
     *          serving u32, reason str]
     * The first frame on every Channel, sent once the server has read the
     * initiator's identity. `serving` is 1 when this account is already
     * provisioned here, 0 while the operator has not approved it (reason says
     * which). A client that is not yet provisioned answers with HANDOVER; one
     * that is simply resumes. */
    LXMPROXY_FR_HELLO    = 1,

    /* C→S  [2, privkey b64, display_name str, ratchets str]
     * `privkey` is the account's raw 64-byte Ed25519+X25519 private key.
     * `ratchets` is rnsd's whole retained ratchet record for the account's
     * delivery destination, verbatim — peers encrypt to the ratchet in the last
     * announce they heard, so a server that started fresh could not read
     * anything sent before its own announce reached each peer. */
    LXMPROXY_FR_HANDOVER = 2,

    /* S→C  [3, ok u32, reason str, hold u32]
     * The account is registered and announced (or `ok` 0 and why not). Only on
     * ok does the client unregister its own destination and become a client.
     *
     * `hold` says what a refusal means. 1 = keep the Channel and stay
     * provisioning: the answer is not no, it is not yet — the operator has not
     * approved this account — and the server will send a fresh HELLO the moment
     * they do. Without it the client would give up and the user would have to
     * ask to be proxied a second time, after the approval, for no reason they
     * could see. 0 = a real refusal; stop asking. */
    LXMPROXY_FR_SERVING  = 3,

    /* S→C  [4, msg_id b32, peer b16, peer_name str, ts u32, title str,
     *          size u32, body str|nil]
     * One inbound message the server holds and the client does not. `size` is
     * the body length whether or not the body rides along; a nil body is a
     * withheld one the client fetches on demand. `peer_name` is an
     * opportunistic hint for a peer the client has never heard announce. */
    LXMPROXY_FR_MSG      = 4,

    /* C→S  [5, msg_id b32, peer b16] — send me the body you withheld. */
    LXMPROXY_FR_FETCH    = 5,

    /* S→C  [6, msg_id b32, peer b16, content str, reply_to b32|nil]
     * The withheld body. Rides a Resource on the Channel's Link when it does
     * not fit one Channel message. */
    LXMPROXY_FR_BODY     = 6,

    /* C→S  [7, msg_id b32, peer b16]
     * The record — body included — is in the client's storage. This is the only
     * thing that makes the server stop owing it; rnsd's own packet proof and a
     * Resource's conclusion are both pre-persist and neither is a handover ack. */
    LXMPROXY_FR_HANDED   = 7,

    /* C→S  [8, local_key str, peer b16, ts u32, title str, content str,
     *          reply_to b32|nil, method str, pn b16|nil]
     * Reproduce this draft on the server and send it. `local_key` is the
     * client's own `o_<unix_ms>_<rand4>`, and it is the idempotency key: a SEND
     * repeated after a reconnect meets the same record and gets the same
     * STATUS. `ts` is the client's timestamp, so the message_id both ends
     * derive is the same one. */
    LXMPROXY_FR_SEND     = 8,

    /* S→C  [9, key str, peer b16, status u32, ts u32, message_id b32|nil]
     * `key` is the client's local key for an outbound, or the message id for an
     * inbound. The first STATUS for an outbound carries `message_id`, which is
     * what maps the client's local key onto the server's record. The status is
     * the server's real LxmfStatus, relayed verbatim — the client shows the
     * true error rather than a proxy-flavoured one. */
    LXMPROXY_FR_STATUS   = 9,

    /* C→S  [10, key str, peer b16] — a terminal outbound status is persisted
     * here. */
    LXMPROXY_FR_SETTLED  = 10,

    /* C↔S  [11, map str→str]
     * Account-scoped settings. Edited only on the client, because it is the
     * only end with a UI; held only on the server, because it is the end that
     * faces the world. Pushed on change and reconciled on connect. */
    LXMPROXY_FR_CONFIG   = 11,

    /* S→C  [12, map str→str]
     * What only the server knows — real announce state, quota use, propagation
     * results — so the client displays truth rather than intent. */
    LXMPROXY_FR_STATE    = 12,

    /* C→S  [13] — deprovision. The client stays proxied and fully working
     * until RATCHETS comes back; it never enters a state where neither end
     * is registered. */
    LXMPROXY_FR_RELEASE  = 13,

    /* S→C  [14, ratchets str] — the retained ratchet record, handed back.
     * The server has deregistered by the time this is sent. */
    LXMPROXY_FR_RATCHETS = 14,
};

/** Every field any frame can carry, filled by lxmproxyParse. Unset fields keep
 *  their zero value; `type` says which of them mean anything. */
struct LxmproxyFrame {
    uint8_t     type = 0;

    /* HELLO */
    std::string label;
    uint32_t    quota_kb = 0;
    uint32_t    max_envelope_kb = 0;
    uint32_t    retain_days = 0;
    uint32_t    serving = 0;
    std::string reason;

    /* HANDOVER */
    uint8_t     privkey[64] = {};
    bool        have_privkey = false;
    std::string display_name;
    std::string ratchets;

    /* SERVING */
    uint32_t    ok = 0;
    uint32_t    hold = 0;       /* a refusal to wait out, not to give up on */

    /* MSG / FETCH / BODY / HANDED / STATUS / SETTLED */
    uint8_t     msg_id[LXMPROXY_MID_LEN] = {};
    bool        have_msg_id = false;
    uint8_t     peer[LXMPROXY_DEST_LEN] = {};
    bool        have_peer = false;
    std::string peer_name;
    uint32_t    ts = 0;
    std::string title;
    uint32_t    size = 0;
    std::string content;
    bool        have_body = false;      /* MSG carried its body inline */
    uint8_t     reply_to[LXMPROXY_MID_LEN] = {};
    bool        have_reply_to = false;

    /* SEND */
    std::string key;                    /* the client's local key, or a message id */
    std::string method;
    uint8_t     pn[LXMPROXY_DEST_LEN] = {};
    bool        have_pn = false;

    /* STATUS */
    uint32_t    status = 0;

    /* CONFIG / STATE */
    std::vector<std::pair<std::string, std::string>> map;
};

/* ── builders ──
 * Each returns the complete frame bytes, ready to hand to the Channel (or to a
 * Resource on its Link when they do not fit one message). */

std::vector<uint8_t> lxmproxyBuildHello(const char* label, uint32_t quota_kb,
                                        uint32_t max_envelope_kb,
                                        uint32_t retain_days, bool serving,
                                        const char* reason);
std::vector<uint8_t> lxmproxyBuildHandover(const uint8_t privkey[64],
                                           const char* display_name,
                                           const std::string& ratchets);
std::vector<uint8_t> lxmproxyBuildServing(bool ok, const char* reason, bool hold = false);
std::vector<uint8_t> lxmproxyBuildMsg(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                      const uint8_t peer[LXMPROXY_DEST_LEN],
                                      const char* peer_name, uint32_t ts,
                                      const std::string& title, uint32_t size,
                                      const std::string* body);
std::vector<uint8_t> lxmproxyBuildFetch(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                        const uint8_t peer[LXMPROXY_DEST_LEN]);
std::vector<uint8_t> lxmproxyBuildBody(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                       const uint8_t peer[LXMPROXY_DEST_LEN],
                                       const std::string& content,
                                       const uint8_t* reply_to);
std::vector<uint8_t> lxmproxyBuildHanded(const uint8_t msg_id[LXMPROXY_MID_LEN],
                                         const uint8_t peer[LXMPROXY_DEST_LEN]);
std::vector<uint8_t> lxmproxyBuildSend(const std::string& local_key,
                                       const uint8_t peer[LXMPROXY_DEST_LEN],
                                       uint32_t ts, const std::string& title,
                                       const std::string& content,
                                       const uint8_t* reply_to,
                                       const char* method, const uint8_t* pn);
std::vector<uint8_t> lxmproxyBuildStatus(const std::string& key,
                                         const uint8_t peer[LXMPROXY_DEST_LEN],
                                         uint32_t status,
                                         uint32_t ts, const uint8_t* message_id);
std::vector<uint8_t> lxmproxyBuildSettled(const std::string& key,
                                          const uint8_t peer[LXMPROXY_DEST_LEN]);
std::vector<uint8_t> lxmproxyBuildConfig(
        const std::vector<std::pair<std::string, std::string>>& map);
std::vector<uint8_t> lxmproxyBuildState(
        const std::vector<std::pair<std::string, std::string>>& map);
std::vector<uint8_t> lxmproxyBuildRelease(void);
std::vector<uint8_t> lxmproxyBuildRatchets(const std::string& record);

/** Parse one frame. Returns false on anything that is not a well-formed frame
 *  of a known type; a frame carrying more elements than this build knows about
 *  still parses, since the trailing elements are simply not read. */
bool lxmproxyParse(const uint8_t* p, size_t n, LxmproxyFrame& out);

/** The frame type in words, for logs. Never null. */
const char* lxmproxyFrameName(uint8_t type);

/* ── announce app_data ──
 * msgpack [label str]. It carries what the client needs before it links — the
 * operator's label — and nothing that identifies any account behind the
 * destination: every account on one box links to the same hash, and the
 * announce must not also say whose. */

std::vector<uint8_t> lxmproxyBuildAnnounce(const char* label);

/** Read a server's announce app_data. False when it is not one of ours. */
bool lxmproxyParseAnnounce(const uint8_t* p, size_t n, std::string& label_out);

/* ── the inline-body threshold ──
 *
 * A body larger than this is withheld: the client renders a download
 * affordance and fetches it when the user asks. The bound is what a link can
 * carry without the push feeling like a stall, so it comes from the link's own
 * measured round trip (`rnsd.chan.<tag>.rtt_ms`, re-measured continuously on a
 * held link) rather than from a guess about the medium.
 *
 * `override_bytes` non-zero is used verbatim — the operator's answer beats the
 * derivation. `rtt_ms` 0 means not yet measured, and yields the floor. */
uint32_t lxmproxyInlineThreshold(uint32_t rtt_ms, uint32_t override_bytes);
