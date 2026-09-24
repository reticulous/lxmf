/**
 * lxmf — LXMF messaging protocol task.
 *
 * Storage is the API; the firmware subscribes to its own keys and
 * reacts. Wire format is LXMF 0.9.8:
 *
 *   destination_hash(16) || source_hash(16) || Ed25519 sig(64) ||
 *   msgpack([timestamp, title, content, fields, stamp?])
 *
 * Signature scope is `dest||src||packed||SHA-256(dest||src||packed)` —
 * the SHA-256 of the data is signed alongside it. message_id =
 * SHA-256(dest||src||packed). transient_id (used by propagation stores)
 * is distinct and not used here.
 */
#include "lxmf.h"
#include "lxmf_stamp.h"
#include "lxmproxy_wire.h"
#include "mem.h"
#include "storage.h"
#include "storage_db.h"
#include "spangap.h"
#include "ports.h"
#include "rnsd.h"     /* SHA-256, sign/verify, dest-hash, recall, request_path */
#if CONFIG_STRADDLE_AUDIO
#include "audio.h"    /* audioPlayWav — message-notification sound (optional dep) */
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <sys/time.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <unordered_map>



static const char* TAG = "lxmf";

/* Cached `s.lxmf.debug.only_local` — when true, the per-announce
 * catalogue dbg() lines (everyone else's announces, not us) are
 * demoted to verb() so they only show at verbose. dbg-level then
 * surfaces just traffic that affects this node directly (sends,
 * receives, cmd processing, etc.). Live-mirrored from storage via
 * subscription, so writes take effect immediately. */
static bool s_dbg_only_local = false;

#define DBG_REMOTE(...) do { if (s_dbg_only_local) verb(__VA_ARGS__); else dbg(__VA_ARGS__); } while (0)

/* ─────────────── constants ─────────────── */

#define LXMF_VERSION 1

constexpr size_t LXMF_DEST_HASH_LEN = 16;
constexpr size_t LXMF_SIG_LEN       = 64;
constexpr size_t LXMF_OVERHEAD      = LXMF_DEST_HASH_LEN * 2 + LXMF_SIG_LEN;  /* 112 */
/* Largest plaintext that fits a single encrypted RNS packet (RNS
 * ENCRYPTED_MDU). The opportunistic LXM payload — the packed wire minus
 * the stripped 16-byte dest hash that rnsd re-derives — must not exceed
 * this, or Packet::pack() throws on the MTU check (surfacing as a
 * spurious "evicted"). Derived from MTU=500: ((464-48-32)/16)*16-1. */
constexpr size_t LXMF_OPP_PAYLOAD_MAX = 383;

/* Max concurrent LXMF identities. Schema is an array (id.<n>). */
#define LXMF_MAX_IDENTITIES 4

/* In-RAM dedup hashlist depth — last N inbound message_ids. Cheap
 * defence against accidental duplicates from path-flapping repeats.
 * Storage existence is the authoritative dedup. */
#define LXMF_DEDUP_RING 64

/* Inbound LXMs from a sender whose identity we don't have yet are held
 * here (per identity) until their lxmf.delivery announce arrives, then
 * re-verified. Bounds: at most N buffered, oldest dropped on overflow;
 * an entry that never resolves is TTL-evicted so the queue can't leak. */
#define LXMF_MAX_PENDING_VERIFY     25
#define LXMF_PENDING_VERIFY_TTL_MS  (30u * 60u * 1000u)

/* LXMF field registry keys (msgpack int keys in `fields` map). */
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
    LXMF_FIELD_REPLY_TO         = 0x30,   /* Bytes, full LXMessage.hash of the replied-to message */
    LXMF_FIELD_REPLY_QUOTE      = 0x31,   /* Bytes, quoted content (UTF-8) */
};

/* ─────────────── state ─────────────── */

struct outbound_t {
    bool        used;
    uint16_t    send_id;
    std::string peer;           /* 32-hex destination — the conversation subtree */
    std::string msg_key;        /* "o_<...>" — the local outbound key under id.<n>.msgs.<peer> */

    /* DIRECT: when this send went out over a Link instead of
     * opportunistic. link_handle is the RNSD_PORT_LINK ITS handle;
     * link_tag keys rnsd.links.<tag>.* which the 1 Hz tick polls for
     * the active/failed transition (rnsd has no OUT_RESULT on links). */
    bool        direct;
    int         link_handle;
    std::string link_tag;
    uint32_t    direct_deadline_s;  /* unix s; fail if not active by then */

    /* This DIRECT send is a Resource (wire > one Link packet).
     * Settled by the RNSD_LINK_RESOURCE_OUTBOUND_DONE aux (or
     * rnsd.links.<tag>.resource.state) — NOT by link "active", because
     * the Link must stay up for the whole transfer. */
    bool        is_resource;

    /* Delivery-proof phase: stage "sent" has been written (egress
     * accepted) and the slot is held open awaiting cryptographic proof.
     *   - opportunistic: rnsd emits a second OUT_RESULT
     *     (DELIVERED / PROOF_TIMEOUT) for the same send_id;
     *   - DIRECT packet: rnsd bumps rnsd.links.<tag>.tx_proven /
     *     proof_timeouts — we baseline both at send time and watch for
     *     increments (sends are serialized per link, so unambiguous).
     * Holding the slot keeps convBusy() true until the proof settles,
     * which is what serializes the link and makes the counters safe.
     * proof_deadline_s is the local backstop: settle as "no delivery
     * proof" (stage stays "sent" — NOT failed, the message may well have
     * arrived) and free the slot. */
    bool        awaiting_proof;
    uint32_t    proof_deadline_s;       /* unix s */
    int         proof_base_proven;      /* tx_proven at send time */
    int         proof_base_timeouts;    /* proof_timeouts at send time */
    /* DIRECT packet: the wire as sent, for the one re-send over the same
     * link when rnsd's receipt times out (resolveDirectSends), and whether
     * that re-send has been made. */
    std::vector<uint8_t> link_wire;
    bool        link_resent;

    /* REQUESTING_PATH events for this send with no PATH_KNOWN in between.
     * rnsd emits one per park (initial no-path, and a re-park when the
     * found path lacks a recallable identity); its path-retry ladder
     * signals RETRY auxes instead. */
    uint8_t     path_reqs;

    /* Unix s by which rnsd must have found a path for a parked opportunistic
     * send, else the 1 Hz pass cancels it rnsd-side and returns the message to
     * the delivery queue (LXMF_PATH_GRACE_S). 0 = not path-parked. */
    uint32_t    path_deadline_s;

    /* Unix s the slot went in flight. */
    uint32_t    started_s;

    /* Unix s rnsd reported the opportunistic packet on the air (SENT): what
     * the next re-send of the same packet is spaced from. */
    uint32_t    sent_s;
};

/* A received LXM we can't verify yet because the sender's identity
 * (public key) isn't in rnsd's cache — we never heard their announce.
 * onInboundLxm issues a path request (which makes them re-announce) and
 * parks the raw wire here; drainPendingVerify replays it once the
 * announce lands. Without this, opportunistic single-packet messages
 * from a not-yet-known sender are lost — LXMF has no retransmission. */
struct pending_verify_t {
    uint8_t              sender[LXMF_DEST_HASH_LEN];
    std::vector<uint8_t> wire;
    uint64_t             enqueued_ms;
};

/* An in-flight Ping — the contact page's reachability probe. One per identity:
 * the button is a single measurement the user watches, so a second press
 * supersedes the first rather than queueing behind it. It rides the same
 * our-dest connection and send_id space as messages, so OUT_RESULT dispatch
 * checks this slot before the outbox table. */
struct ping_t {
    bool        used = false;
    uint16_t    send_id = 0;
    std::string peer;               /* 32-hex destination being probed */
    uint32_t    deadline_s = 0;     /* unix s; settle "timeout" past it whatever rnsd says */
    /* A probe that IS a link establishment. Used where this device has no
     * destination registration of its own to send a packet from — a proxied
     * account, whose registrant is the server's device — and where there is no
     * conversation link to the peer already open. Dialling one and timing the
     * answer measures the same round trip a probe packet would: the request
     * goes out, the far end proves it by accepting, and the link comes up. It
     * is then dropped again, because the question was the round trip and not
     * the session. */
    bool        by_link = false;
    std::string link_tag;
    int         link_handle = -1;   /* >= 0 only for a link this probe dialled,
                                     * which is also what says the probe owns it
                                     * and must take it down again */
    uint32_t    started_ms = 0;     /* the press: what `answer_ms` is measured from */
    /* Probing over a link that was already open: the link's delivery-proof
     * counters as they stood before the probe, so the first increment after it
     * is this probe's answer. The link is serialized, so nothing else can move
     * them in between. */
    int         proof_base_proven = 0;
    int         proof_base_timeouts = 0;
};

struct lxmf_id_t {
    bool          used;
    int         index;                              /* 0..LXMF_MAX_IDENTITIES-1 */
    int         handle;                             /* RNSD_PORT_DEST handle, -1 = closed */
    std::string identity_key;                       /* storage path: "secrets.lxmf.id.<n>.privkey" */
    uint8_t     dest_hash[RNSD_DEST_HASH_LEN];      /* 16-byte LXMF delivery destination hash */
    uint16_t    next_send_id;
    outbound_t  outboxes[8];                        /* in-flight send_id → message-key tracking */
    ping_t      ping;                               /* the contact page's Ping, at most one */
    uint8_t     ping_seq;                           /* rotates the probe link tag (see pingStart) */

    /* Stats (mirrored to lxmf.id.<n>.stats.* at 1 Hz). */
    uint32_t      sent;
    uint32_t      received;
    uint32_t      pending;
    uint32_t      failed;

    /* Inbound messages waiting on their sender's identity. Touched only
     * on the lxmf task (onInboundLxm / onAnnounceFromRnsd), no locking. */
    std::vector<pending_verify_t> pending_verify;
};

static TaskHandle_t s_task = nullptr;
static volatile bool s_stop = false;   /* rns stop → break the work loop and park */
static volatile bool s_parked = false; /* true while parked (stopped); lxmfStop waits on it */
PSRAM_BSS static lxmf_id_t    s_ids[LXMF_MAX_IDENTITIES];

/* Inbound dedup ring (recent message_ids, hex64). */
PSRAM_BSS static std::string s_dedup_ring[LXMF_DEDUP_RING];
static int         s_dedup_head = 0;

/* Forward declarations for per-identity cmd-subscription helpers (defined
 * below alongside the rest of the cmd-handler glue). Called from the
 * identity lifecycle. */
static void subscribePerIdCmds(int n);
static void unsubscribePerIdCmds(int n);

/* Inbound pipeline + the pending-verification drain. onAnnounceFromRnsd
 * (defined above onInboundLxm) calls the drain; both run on the lxmf
 * task only. */
/* `via_link` marks an LXM that arrived on a conversation link of ours rather
 * than through the destination registration — which, while proxied, is held by
 * the server and not by this device. Defaulted, so only the link's own receive
 * path has to say so. */
static void onInboundLxm(lxmf_id_t& id, const uint8_t* wire, size_t n,
                         bool via_link = false);
static void drainPendingVerify(lxmf_id_t& id, const uint8_t* sender_hash);
static void drainAllPendingVerify(lxmf_id_t& id);

/* Proxy client (its own section below the inbound-DIRECT handlers). A proxied
 * identity registers nothing here and sends nothing itself: the delivery queue
 * and the announce path both ask these before doing anything of their own. */
static bool proxyIsClient(int n);   /* this account's mail belongs to a server */
static bool proxyReady(int n);      /* …and the Channel to it is up and serving */
static bool proxySendFrame(int n, const std::vector<uint8_t>& frame,
                           uint32_t* opaque_out = nullptr);
static void proxySend(lxmf_id_t& id, const std::string& peer_hex,
                      const std::string& mid);

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

/* Summed LoRa LBT drop counters across the radio slots
 * (lora.<n>.stats.tx_dropped, absent = 0; the lora task mirrors each
 * increment at drop time). Snapshot at send setup, compared at settle. */
static uint32_t loraTxDroppedSum(void)
{
    uint32_t s = 0;
    for (int i = 0; i < 4; ++i) {
        char k[40];
        snprintf(k, sizeof(k), "lora.%d.stats.tx_dropped", i);
        s += (uint32_t)storageGetInt(k, 0);
    }
    return s;
}

/* DELIVERY_TIMEOUT vs RADIO_BUSY: when the local LoRa radio shed frames to
 * channel contention while the message was waiting, the failure names the
 * jammed channel rather than the peer — the own transmitter never got the
 * message out, which is a different fault from a silent recipient. `base` is
 * the counter as it stood when the message went on the queue. */
static uint8_t radioBusyOr(uint32_t base, uint8_t fallback)
{
    return loraTxDroppedSum() > base ? (uint8_t)LXMF_ST_RADIO_BUSY : fallback;
}

/* Write an int key only when the value actually changes. storageSet fires change
 * subscriptions on every write, so rewriting an unchanged value churns
 * subscribers for nothing — notably the on-device LXMF UI, which rebuilds itself
 * on any `lxmf.id.*` / `s.lxmf.id.*` change. */
static void setIntIfChanged(const std::string& key, int v)
{
    if (storageGetInt(key.c_str(), INT32_MIN) != v) storageSet(key.c_str(), v);
}

/* String analogue of setIntIfChanged for the same churn reason. */
static void setStrIfChanged(const std::string& key, const char* v)
{
    if (storageGetStr(key.c_str(), "").compare(v) != 0) storageSet(key.c_str(), v);
}

/* Canonicalize a delivery-method name to one of the four current values,
 * mapping the legacy names and coercing anything unknown to the default.
 *   link-always            always a Reticulum Link.
 *   link-if-one-exists     ride a warm link to this peer if one exists,
 *                          else opportunistic (oversize still opens a link).
 *   link-if-big            opportunistic when it fits one packet, a Link
 *                          only when the wire is oversize.
 *   opportunistic-or-fail  never a Link; oversize hard-fails.
 *
 * `link-always` is the default, and `auto` — "choose for me" — resolves to it.
 * A Link costs a request, a proof, a round-trip measurement and an identify
 * (about 500 B of air) before the body goes out, where a packet that fits
 * costs one packet; but the packet carries no sender key, so a recipient
 * beyond every gateway's radius cannot verify it and never shows it, and its
 * proof is a single packet that a long route loses. The Link identifies the
 * sender and carries any size. Legacy: direct→link-always,
 * opportunistic→opportunistic-or-fail. */
static std::string canonMethod(const std::string& m)
{
    if (m == "direct")        return "link-always";
    if (m == "opportunistic") return "opportunistic-or-fail";
    if (m == "link-always" || m == "link-if-one-exists" ||
        m == "link-if-big"  || m == "opportunistic-or-fail")
        return m;
    return "link-always";
}

/* Per-contact message store: s.lxmf.id.<n>.msgs.<peer>.<key>.<field>.
 * `peer` is a 32-hex destination (the conversation subtree); `key` is
 * the real message_id (inbound) or a local o_<ms>_<rand> (outbound). */
static std::string msgPath(int n, const std::string& peer,
                           const std::string& key, const char* field)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "s.lxmf.id.%d.msgs.%s.%s.%s",
             n, peer.c_str(), key.c_str(), field);
    return buf;
}

/* The method a message is sent by: its own override, else its identity's
 * default, else the global default. Empty and `auto` at the first two levels
 * mean "no choice here" and fall through; `auto` is what a new identity is
 * created with. */
static std::string resolveMethod(int n, const std::string& peer, const std::string& mid)
{
    std::string m = storageGetStr(msgPath(n, peer, mid, "method").c_str(), "");
    if (m.empty() || m == "auto")
        m = storageGetStr(idPath(n, "default_method").c_str(), "");
    if (m.empty() || m == "auto")
        m = storageGetStr("s.lxmf.default_method", "link-always");
    return canonMethod(m);
}

/* RAM-only outbound wire cache ("the outbox"): the packed + signed + stamped LXM
 * bytes for a message awaiting delivery, keyed "<peer>/<mid>". Deliberately NOT
 * in the record store — it's transient (dead after delivery), it would double
 * each outbound record on disk, and re-deriving it on every rnsd-busy resend
 * would re-run the multi-second proof-of-work stamp. Dropped on the terminal
 * transition (sent/delivered/failed/cancelled). Lost on reboot: a still-queued
 * message is then re-packed from its stored title/content on the next attempt. */
struct OutboxWire { std::vector<uint8_t> wire; std::string msg_id_hex; uint64_t ts_ms; };
static std::unordered_map<std::string, OutboxWire> g_wireOutbox;
static std::string outboxKey(const std::string& peer, const std::string& mid) { return peer + "/" + mid; }

static uint64_t nowUnixMs();                                     /* fwd */
static void queueRemove(int n, const std::string& peer, const std::string& mid);   /* fwd */
static void queueKickConversation(int n, const std::string& peer);                 /* fwd */

/* Set a message's unified status (u8 record field), overwritten in place. The
 * cached wire is kept while the message is in play so every attempt resends
 * the identical bytes — same message_id (the recipient still dedups), no
 * re-pack, no re-stamp. It is dropped only once the message reaches a settled
 * outcome (DELIVERED / CANCELLED here; terminal failure via msgFail), after
 * which there is nothing left to resend — and the message leaves the delivery
 * queue with it. */
static void msgSetStatus(int n, const std::string& peer, const std::string& mid,
                         uint8_t status)
{
    if (status == LXMF_ST_DELIVERED) {
        storageBegin();
        storageSet(msgPath(n, peer, mid, "status").c_str(), (int)status);
        storageSet(msgPath(n, peer, mid, "delivered_ts").c_str(),
                   (int)(nowUnixMs() / 1000));
        storageEnd();
    } else {
        storageSet(msgPath(n, peer, mid, "status").c_str(), (int)status);
    }
    if (status == LXMF_ST_DELIVERED || status == LXMF_ST_CANCELLED) {
        g_wireOutbox.erase(outboxKey(peer, mid));
        queueRemove(n, peer, mid);
    }
    /* A delivery is proof the peer is reachable RIGHT NOW, and — on the Link a
     * retry uses — that the Link is up and just went idle. Anything else queued
     * for this conversation goes next, instead of waiting out an interval sized
     * for a peer that may be gone for hours and finding the Link closed when it
     * gets there. Every settle path funnels through here, so this is the one
     * place it needs saying. */
    if (status == LXMF_ST_DELIVERED) queueKickConversation(n, peer);
}

/* Terminal failure: status = the gave-up reason, tries = 255 (the one definitive
 * terminal marker). The two u8 keys commit as one op-list so frontends see a
 * single atomic transition. */
static void msgFail(int n, const std::string& peer, const std::string& mid,
                    uint8_t reason)
{
    storageBegin();
    storageSet(msgPath(n, peer, mid, "status").c_str(), (int)reason);
    storageSet(msgPath(n, peer, mid, "tries").c_str(),  (int)LXMF_TRIES_GAVEUP);
    storageEnd();
    g_wireOutbox.erase(outboxKey(peer, mid));
    queueRemove(n, peer, mid);
}

/* Prefix of one conversation's subtree (key empty) or one message's
 * subtree — for storageForEach / storageDeleteTree. */
static std::string msgPrefix(int n, const std::string& peer,
                             const std::string& key = "")
{
    char buf[160];
    if (key.empty())
        snprintf(buf, sizeof(buf), "s.lxmf.id.%d.msgs.%s.", n, peer.c_str());
    else
        snprintf(buf, sizeof(buf), "s.lxmf.id.%d.msgs.%s.%s.",
                 n, peer.c_str(), key.c_str());
    return buf;
}

/* Conversations are backed by structured record stores (storageStructuredDB,
 * pattern "s.lxmf.id.$.msgs.$"), created lazily on the first field write — no
 * per-conversation registration is needed here any more. Kept as a no-op so the
 * call sites document where a conversation first materialises. */
static void ensureConvFile(int, const std::string&) {}

/* The LXMF message record schema (see storage_db.h). Mutable fields are
 * fixed-width so a stage transition overwrites in place; title/content are
 * immutable text. The packed outbound `wire` is NOT stored — it's transient
 * (dead after delivery) and lives in the RAM outbox (g_wireOutbox); redundant
 * addressing (`peer`) isn't stored either (the routing layer drops it). Built
 * once; the registration keeps the pointer.
 *
 * schema_ver 2 dropped the old `wire` text field. schema_ver 3 replaced the
 * fixstr `stage`(12) + `last_error`(48) + `attempts`(1) with a single u8 `status`
 * (LxmfStatus in lxmf.h) + u8 `tries` — 61 bytes of status per message down to 2,
 * still overwritten in place. `tries == 255` is the terminal marker. schema_ver 4
 * moved `message_id` from hex text to a raw 32-byte DATA field and replaced the
 * hex text `thread` with a raw 32-byte DATA `reply_to` (FIELD_REPLY_TO on the
 * wire; all-zero = not a reply). Each hdr_size change trips the store's header
 * check, so older files load empty; lxmfMigrateMsgs() rewrites them first
 * (data-preserving). */
static const sdb_schema& lxmfMsgSchema()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 1;
        x.schema_ver = 4;
        x.u8("tries").u8("status")
         .u8("body_absent")   /* 1 = the content is not here yet: a proxy server
                               * withheld a body over its inline threshold and
                               * the client fetches it on demand. Absence cannot
                               * be inferred from empty content — an empty
                               * message is a legitimate one — and the decision
                               * has to survive a reboot, so it is a field. */
         .u8("handed")        /* SERVER side: 1 = the client has this record in
                               * its own storage. What is left is what is owed,
                               * which is what makes deletion a policy and not a
                               * wire rule. Meaningless on a client. */
         .u8("via_link")      /* 1 = this message went out, or came in, over a
                               * conversation link to the peer rather than
                               * through the proxy that holds our address. Only
                               * interesting while proxied — unproxied it is the
                               * ordinary case and the frontends do not draw it
                               * — but recorded either way, because what carried
                               * a message is a fact about the message and not
                               * about how the account happens to be set up
                               * today. */
         .u8("proxy_status")  /* CLIENT side: the server's own LxmfStatus behind
                               * an OUR_PROXY_GAVE_UP, verbatim — which failure
                               * it stopped on. The five proxy states are what a
                               * conversation shows; this is what its detail
                               * page can say underneath, and the distinction
                               * that matters is that it is the PROXY's verdict
                               * on reaching the recipient and not this device's
                               * trouble reaching the proxy. 0 = nothing said. */
         .u8("offered")       /* SERVER side: 1 = this inbound has been offered
                               * to the client (a MSG frame went out for it).
                               * Said once: the Channel already resends what
                               * goes unproved, so repeating the offer above it
                               * was never what delivered anything — it only
                               * meant a message the client held but could not
                               * yet acknowledge, a withheld body waiting on a
                               * download, was announced again on a timer for as
                               * long as both ends stayed up. Meaningless on a
                               * client. */
         .u8("told")          /* SERVER side: the status last relayed to the
                               * client, +1 so that 0 means "nothing told yet"
                               * and DRAFT (0) is tellable. A status is relayed
                               * when it differs from this, so the two being
                               * equal IS "the client knows the latest" — no
                               * separate flag to keep in step with the status
                               * it describes. Persisted rather than held per
                               * Channel: a reconnect used to re-announce every
                               * record the box still held, one frame each, for
                               * a client that already knew all of them.
                               * Meaningless on a client. */
         .u32("body_size")    /* content length in bytes, whether or not the
                               * content is here — what a download affordance
                               * shows before it downloads anything */
         .u32("recv_ts")   /* monotonic (never-decreasing) receive time — the stable
                            * anchor for date separators; `ts` is the sender's clock */
         .fixstr("dir", 4).fixstr("method", 16)
         .u32("ts")
         .u32("delivered_ts") /* unix s the delivery proof settled DELIVERED;
                               * 0 = not delivered */
         .data("message_id", 32).data("reply_to", 32)
         .text("title").text("content")
         .text("reply_quote");  /* the fragment of the replied-to message this
                                 * one quotes (FIELD_REPLY_QUOTE); empty when the
                                 * reply quotes the message as a whole and each
                                 * end draws the preview from its own copy */
        return x;
    }();
    return s;
}

/* Conversation directory store (schema 2): one record per peer, one file per
 * identity slot ("lxmf/contacts/$1.db.gz"). Fields mirror the leaves written via
 * contactPath(). The mutable scalars (counters, watermarks, last_seen, trust)
 * are fixed-width so a new message / read / announce updates them in place; the
 * text fields (names, preview) rebuild the record when they change. `hash` stores
 * the peer hash redundantly with the record key — it is the "contact exists"
 * sentinel the seed/first-contact paths test via storageExists.
 *
 * schema_ver 2 moved `hash` from hex text to a raw 16-byte DATA field. Older
 * files upgrade automatically — the generic auto-migrator decodes them via the
 * registered legacy hint layouts and re-packs them in this one, dropping values
 * for fields this layout no longer carries.
 *
 * A contact's public key is here because verifying a message from that contact
 * needs the key and nothing else — no route, no path, no announce. rnsd's
 * directory holds the same key beside the route and is the fast path every
 * reader takes, but it is a cache: its arena is sized from free PSRAM at boot
 * and its image is discarded whole on any mismatch, so a key that lived only
 * there can be gone while the contact is still in the address book, and every
 * message from that contact then waits on a path request it never needed. The
 * address book is the durable, bounded list of destinations this node cares
 * about, so the key rides with it and is handed back to the directory at boot
 * (lxmfSyncContactKey). */
static const sdb_schema& lxmfContactSchema()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 2;
        x.schema_ver = 2;
        x.u32("count").u32("last_ts").u32("unread").u32("read_ts").u32("last_seen")
         .u8("trust")
         .u8("preview_mine")    /* Which way the ONE preview went: 1 = we sent
                                * it, so the list prefixes "You: ". There is no
                                * second preview — `preview` is the last message
                                * whoever wrote it, and this byte is the only
                                * thing needed to label it. */
         .data("hash", 16)
         .data("pubkey", 64)   /* X25519(32) ‖ Ed25519(32), as rnsd hands it
                                * out; unset until an announce is heard or a
                                * message from them verifies */
         .data("pn", 16)       /* the contact's preferred classic
                                * lxmf.propagation node, client-set from the
                                * contact details page; all-zero = none */
         .text("display_name").text("nick").text("preview");
        return x;
    }();
    return s;
}

/* Heard-announce catalogue (schema 3): one global RAM-only store, capped and
 * self-evicting (STORAGE_DB_DROP). `last` (announce time) and `hops` mutate in
 * place on every re-announce; `cost` is a fixstr so the -1 "unknown" sentinel
 * survives; `name` is the announced display name. schema_ver 2 moved `ratchet`
 * from hex text to a raw 32-byte DATA field. This store is RAM-only, so the
 * schema change needs no file migration. */
static const sdb_schema& lxmfAnnounceSchema()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 3;
        x.schema_ver = 2;
        x.u32("last").u8("hops").fixstr("cost", 6).data("ratchet", 32)
         .text("name");
        return x;
    }();
    return s;
}

static std::string contactPath(int n, const std::string& peer_hex, const char* field)
{
    char buf[120];
    snprintf(buf, sizeof(buf), "s.lxmf.id.%d.contacts.%s.%s", n, peer_hex.c_str(), field);
    return buf;
}

static bool hexToBytes(const char* hex, size_t hex_len,
                       uint8_t* out, size_t out_len);   /* fwd */

static std::string bytesToHex(const uint8_t* data, size_t n);   /* fwd */
static uint64_t    nowUnixMs();                                  /* fwd */

/* Claim a contact in rnsd's directory. The claim is what keeps a contact's
 * identity — and its route — from being evicted by the announce traffic of a
 * busy public network: eviction ranks claimed records above unclaimed ones, and
 * PERSIST above ephemeral. It is a preference, not a guarantee; rnsd may still
 * break it under real pressure, in which case the next announce or path
 * response restores the record.
 *
 * Safe to assert repeatedly (it restamps), and safe to assert for a contact we
 * have never heard from — the claim waits on an otherwise empty record for the
 * first announce to land on. The address book bounds the population, which is
 * the condition that makes a long-lived claim legitimate at all.
 *
 * DIR, not DIR_BLOB: we need to know who a contact is, not to answer path
 * requests on their behalf. */
static void lxmfClaimContact(const std::string& peer_hex)
{
    uint8_t dh[16];
    if (!hexToBytes(peer_hex.c_str(), peer_hex.size(), dh, 16)) return;
    rnsdClaim(dh, RNSD_CLAIM_LXMF, RNSD_CLAIM_PERSIST, RNSD_CLAIM_LAYER_DIR, 0);
}

/* Keep a contact's public key with the contact (see lxmfContactSchema). Both
 * sources are authenticated: an announce carries a signature over the key it
 * advertises, and a message that verifies proves the key that verified it.
 *
 * Written only when it differs from what is stored: a peer re-announces on its
 * own beat, and an unchanged 64-byte write would rewrite the record and notify
 * every subscriber for news nobody had. */
static void contactSetPubkey(int n, const std::string& peer_hex,
                             const uint8_t pk[RNSD_PUBKEY_LEN])
{
    std::string key = contactPath(n, peer_hex, "pubkey");
    std::string hex = bytesToHex(pk, RNSD_PUBKEY_LEN);
    if (storageGetStr(key.c_str(), "") == hex) return;
    storageSet(key.c_str(), hex.c_str());
}

/* The stored key for a contact, if one was ever heard. */
static bool contactGetPubkey(int n, const std::string& peer_hex,
                             uint8_t out[RNSD_PUBKEY_LEN])
{
    std::string hex = storageGetStr(contactPath(n, peer_hex, "pubkey").c_str(), "");
    if (hex.size() != RNSD_PUBKEY_LEN * 2) return false;
    return hexToBytes(hex.c_str(), hex.size(), out, RNSD_PUBKEY_LEN);
}

/* Reconcile one contact's key between the address book and rnsd's directory:
 * whichever side holds it hands it to the other. Both directions matter at
 * boot — a contact stored before its key was is filled in from a directory
 * image that survived, and a contact whose key we hold gets it back into the
 * directory the image lost, which is where every consumer looks it up. */
static void lxmfSyncContactKey(int n, const std::string& peer_hex)
{
    uint8_t dh[LXMF_DEST_HASH_LEN], pk[RNSD_PUBKEY_LEN];
    if (!hexToBytes(peer_hex.c_str(), peer_hex.size(), dh, LXMF_DEST_HASH_LEN)) return;
    if (contactGetPubkey(n, peer_hex, pk)) { rnsdSeedPubkey(dh, pk); return; }
    if (rnsdRecallPubkey(dh, pk)) contactSetPubkey(n, peer_hex, pk);
}

/* Per-conversation read watermark: the ts (seconds) up to and including which
 * the conversation is read; unread = inbound messages with a later ts. One
 * scalar per conversation replaces a `read` flag on every message — marking a
 * conversation read used to be an O(messages) write burst (and O(messages^2) to
 * apply on the flat msgs tree, the LCD/browser liveness break). Kept under
 * contacts (small, stays in cJSON) so it survives messages moving to a record
 * store. */
static int convReadTs(int n, const std::string& peer)
{
    return storageGetInt(contactPath(n, peer, "read_ts").c_str(), 0);
}
static void convMarkRead(int n, const std::string& peer, int upto_ts)
{
    if (upto_ts > convReadTs(n, peer))
        storageSet(contactPath(n, peer, "read_ts").c_str(), upto_ts);
    /* Directory unread counter — maintained, not derived: opening a conversation
     * clears it (the watermark above still records the exact read point). */
    if (storageGetInt(contactPath(n, peer, "unread").c_str(), 0) != 0)
        storageSet(contactPath(n, peer, "unread").c_str(), 0);
}

/* The maintained conversation directory (stays in cJSON, so it rides the browser
 * config mirror and the LCD contact list without loading any message store):
 * per-conversation last_ts / preview / count / unread under contacts.<peer>.
 * Bumped as messages are written so "what conversations exist, newest first,
 * with a preview and unread badge" costs O(conversations), never O(messages).
 * Call inside the message-write bracket. `inbound` bumps the unread counter.
 * Returns the message's monotonic recv_ts: the sender's `ts_s` clamped so it never
 * goes below the newest already seen (last_ts is that running max), so date
 * separators anchored to recv_ts can never jump backward on a weird timestamp. */
static int bumpConvDirectory(int n, const std::string& peer, int ts_s,
                             const std::string& preview, bool inbound)
{
    int count = storageGetInt(contactPath(n, peer, "count").c_str(), 0);
    storageSet(contactPath(n, peer, "count").c_str(),   count + 1);
    int prev = storageGetInt(contactPath(n, peer, "last_ts").c_str(), 0);
    int recv = ts_s > prev ? ts_s : prev;               /* refuse to go back */
    storageSet(contactPath(n, peer, "last_ts").c_str(), recv);
    /* Bounded, single-line preview (control chars folded) — the list shows a
     * snippet, never the whole body. The preview is the LAST message either
     * way; `preview_mine` says which way, so the list can put "You:" in front of
     * ours and nothing in front of theirs. */
    std::string p;
    for (char c : preview) {
        if (p.size() >= 80) break;
        p += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    storageSet(contactPath(n, peer, "preview").c_str(), p.c_str());
    storageSet(contactPath(n, peer, "preview_mine").c_str(), inbound ? 0 : 1);
    if (inbound)
        storageSet(contactPath(n, peer, "unread").c_str(),
                   storageGetInt(contactPath(n, peer, "unread").c_str(), 0) + 1);
    return recv;
}

/* One-time directory backfill. Migration moved message bodies into the record
 * stores but did not seed the maintained directory (count/last_ts/preview/unread)
 * for conversations that predate it — so a migrated device would show only
 * conversations touched since. Runs once (guarded by s.lxmf.dir_seeded2), after
 * registration + migration: for every contact whose directory count is still 0,
 * aggregate its stored messages and seed the summary. Cheap on later boots (the
 * guard skips the whole pass).
 *
 * The per-peer skip tests count > 0, NOT storageExists(count): count is a
 * fixed-width record scalar, present (reading 0) in every contact record the
 * moment the record exists, so storageExists would report every migrated contact
 * as already seeded and skip the whole backfill. A real conversation always has
 * count > 0; an announce-only contact has 0 and is correctly left unseeded. */
static std::vector<std::string> s_seedPeers;
static void seedCollectPeer(const char* key, const char*) {
    const char* c = strstr(key, ".contacts.");
    if (!c) return;
    c += sizeof(".contacts.") - 1;
    const char* dot = strchr(c, '.');
    if (!dot) return;
    std::string peer(c, dot - c);
    for (auto& p : s_seedPeers) if (p == peer) return;
    s_seedPeers.push_back(peer);
}
static int         s_aggCount, s_aggUnread;
static long        s_aggLastTs, s_aggReadTs;
static std::string s_aggPreview;
static bool        s_aggPreviewMine;
static std::string s_aggMid, s_aggMidStage, s_aggMidContent;   /* message being accumulated */
static long        s_aggMidTs;
static bool        s_aggMidIn;
static void seedFlushMsg() {
    if (!s_aggMid.empty() && atoi(s_aggMidStage.c_str()) != LXMF_ST_DRAFT) {
        s_aggCount++;
        s_aggLastTs     = s_aggMidTs;
        s_aggPreview    = s_aggMidContent;
        s_aggPreviewMine = !s_aggMidIn;
        if (s_aggMidIn && s_aggMidTs > s_aggReadTs) s_aggUnread++;
    }
    s_aggMid.clear(); s_aggMidTs = 0; s_aggMidIn = false;
    s_aggMidStage.clear(); s_aggMidContent.clear();
}
static void seedMsgField(const char* key, const char* val) {
    /* key = s.lxmf.id.N.msgs.<peer>.<mid>.<field> — take the last two segments
     * (mid has no dots; records arrive grouped by mid in arena order). */
    const char* d2 = strrchr(key, '.');
    if (!d2) return;
    std::string field(d2 + 1);
    std::string head(key, d2 - key);
    size_t p = head.rfind('.');
    if (p == std::string::npos) return;
    std::string mid = head.substr(p + 1);
    if (mid != s_aggMid) { seedFlushMsg(); s_aggMid = mid; }
    if      (field == "dir")     s_aggMidIn = (val && !strcmp(val, "in"));
    else if (field == "ts")      s_aggMidTs = val ? atol(val) : 0;
    else if (field == "status")   s_aggMidStage = val ? val : "";
    else if (field == "content") s_aggMidContent = val ? val : "";
}
static void lxmfSeedDirectory() {
    if (storageGetInt("s.lxmf.dir_seeded2", 0) != 0) return;
    int seeded = 0;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; n++) {
        s_seedPeers.clear();
        std::string cpre = "s.lxmf.id." + std::to_string(n) + ".contacts";
        storageForEach(cpre.c_str(), seedCollectPeer);
        for (auto& peer : s_seedPeers) {
            if (storageGetInt(contactPath(n, peer, "count").c_str(), 0) > 0) continue;
            s_aggCount = 0; s_aggUnread = 0; s_aggLastTs = 0; s_aggPreview.clear();
            s_aggPreviewMine = false;
            s_aggReadTs = convReadTs(n, peer);
            seedFlushMsg();   /* reset per-message accumulators */
            std::string mp = "s.lxmf.id." + std::to_string(n) + ".msgs." + peer;
            storageForEach(mp.c_str(), seedMsgField);
            seedFlushMsg();   /* finalize the last message */
            if (s_aggCount <= 0) continue;
            std::string pv;
            for (char c : s_aggPreview) { if (pv.size() >= 80) break;
                pv += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c; }
            storageBegin();
            storageSet(contactPath(n, peer, "count").c_str(),   s_aggCount);
            storageSet(contactPath(n, peer, "last_ts").c_str(), (int)s_aggLastTs);
            storageSet(contactPath(n, peer, "preview").c_str(), pv.c_str());
            storageSet(contactPath(n, peer, "preview_mine").c_str(), s_aggPreviewMine ? 1 : 0);
            storageSet(contactPath(n, peer, "unread").c_str(),  s_aggUnread);
            storageEnd();
            seeded++;
        }
    }
    storageSet("s.lxmf.dir_seeded2", 1);
    if (seeded) info("lxmf: seeded directory for %d conversation(s)", seeded);
}

static uint64_t nowUnixMs(void)
{
    /* Monotonic since boot. Use this for deadline/retry math and the
     * coarse user-facing "since when" fields — it never steps when the
     * platform clock is synced, so durations stay correct. NOT for the
     * LXMF wire timestamp: that must be real wall-clock time (see
     * wallUnixMs). */
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t wallUnixMs(void)
{
    /* Real wall-clock Unix ms for the LXMF message timestamp (payload
     * field [0]). The platform syncs the system clock via SNTP /
     * sys.time.set (spangap-core ntp.cpp); once valid, gettimeofday()
     * returns true Unix time. Before the clock is ever set the ESP
     * system clock counts up from the 1970 epoch, so an offline,
     * never-synced device still produces near-epoch stamps — that's a
     * missing time source, not something lxmf can paper over. */
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
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

[[maybe_unused]] static bool hexToBytes(const char* hex, size_t hex_len,
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
 * table, not worth it at this scale. Empty needle matches
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

static void mpPackFloat64(std::vector<uint8_t>& out, double d)
{
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    out.push_back(0xCB);
    for (int i = 7; i >= 0; --i) out.push_back((uint8_t)((bits >> (8*i)) & 0xFF));
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


static void mpPackBinHeader(std::vector<uint8_t>& out, size_t len)
{
    if (len <= 0xFF)   { out.push_back(0xC4); out.push_back((uint8_t)len); return; }
    if (len <= 0xFFFF) { out.push_back(0xC5);
                         out.push_back((uint8_t)((len >> 8) & 0xFF));
                         out.push_back((uint8_t)( len       & 0xFF)); return; }
    out.push_back(0xC6);
    for (int i = 3; i >= 0; --i) out.push_back((uint8_t)((len >> (8*i)) & 0xFF));
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
    std::string ratchet_hex;   /* announce field, filled by the caller from the
                                * fan-out frame; empty if the peer advertises
                                * none */
};

/* LXMF announce app_data shapes seen in the wild (LXMF reference 0.9.8).
 * A ratchet is never among them: it is an RNS announce field, delivered as
 * its own field on the fan-out frame.
 *
 *   [a] msgpack([display_name_bytes_or_nil, stamp_cost])
 *   [b] msgpack([display_name_bytes_or_nil])               (no cost yet)
 *   [c] raw_utf8_name                                      (very old)
 *
 * Try strict-msgpack forms first; fall back to raw-bytes name — and once a
 * name has parsed from the array, later malformed elements never demote to
 * that fallback. Elements past [1] are skipped rather than read: other
 * implementations may extend the array positionally, and an extension nothing
 * here acts on must not be able to cost us the name in front of it. */
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
        /* From here on the name is banked — every path returns true, so a
         * malformed tail can never demote the announce to the raw-name
         * heuristic (which would lose the parsed name). */
        return true;
    };

    /* [a]/[b]/[d] msgpack array */
    if (tryArrayAt(0)) return info;

    /* [c] raw UTF-8 name */
    auto plausibleText = [&](size_t off, size_t len) {
        if (off >= n || len == 0) return false;
        for (size_t k = 0; k < len && off + k < n; ++k) {
            uint8_t b = p[off + k];
            if (b == 0x7F) return false;
            if (b < 0x20 && b != '\t' && b != '\n' && b != '\r') return false;
        }
        return true;
    };
    if (plausibleText(0, n)) {
        info.name.assign((const char*)p, n);
    }
    return info;
}

/* ─────────────── LXM pack / parse ─────────────── */

struct LxmFields {
    std::string reply_to;      /* hex64 hash of the replied-to message, empty if not a reply */
    /* The fragment of the replied-to message the sender quoted, UTF-8
     * (FIELD_REPLY_QUOTE). Sent only when the user picked a fragment by
     * selecting it — a plain reply carries reply_to alone and each end draws the
     * quote from the message it already holds. A receiver shows this text only
     * where it really occurs in the message reply_to names, so a quote cannot
     * put words in anyone's mouth. */
    std::string reply_quote;
    std::string ticket;        /* raw msgpack value of FIELD_TICKET, empty if none */
    /* Future: telemetry, attachments, etc. */
};

/* Pack the msgpack payload alone (no dest/src/sig). title/content may
 * be UTF-8 strings. fields can carry FIELD_REPLY_TO as raw 32 bytes. */
static std::vector<uint8_t> lxmPackPayload(uint64_t ts_ms, std::string_view title,
                                            std::string_view content, const LxmFields& fields)
{
    std::vector<uint8_t> out;
    out.reserve(8 + title.size() + content.size() + 64);

    /* Top-level fixarray of 4. */
    mpPackArrayHeader(out, 4);

    /* [0] timestamp — msgpack float64 POSIX SECONDS, exactly like the
     * reference (LXMessage: `self.timestamp = time.time()`, repacked
     * as-is). It does NO unit conversion on receive, so packing uint64
     * ms here makes our messages land ~1000x in the future on every
     * real upstream peer / echo server and sort to the bottom of any
     * shared timeline. ts_ms stays ms internally; we divide here. Our
     * own inbound parser already accepts float64 and *=1000 → ms. */
    mpPackFloat64(out, (double)ts_ms / 1000.0);

    /* [1] title, [2] content — packed as msgpack BIN, not str. The
     * reference impl UTF-8-encodes both to Python `bytes` before
     * msgpack.packb (LXMessage.set_{title,content}_from_string), so
     * they go on the wire as bin. A real upstream peer that gets str
     * decodes them to a Python str; its echo/display paths then call
     * `.decode("utf-8")` on a str and the content arrives empty. Our
     * own inbound path uses mpReadStrOrBin, so this round-trips. */
    mpPackBin(out, reinterpret_cast<const uint8_t*>(title.data()),   title.size());
    mpPackBin(out, reinterpret_cast<const uint8_t*>(content.data()), content.size());

    /* [3] fields — map of int → value. Only emit non-empty entries, so a
     * normal (non-reply) message carries no reply field at all. */
    size_t field_count = 0;
    if (!fields.reply_to.empty())      field_count++;
    /* A quote without the hash it belongs to is not addressable, so the pair
     * travels together or not at all. */
    bool quote = !fields.reply_to.empty() && !fields.reply_quote.empty();
    if (quote)                         field_count++;
    mpPackMapHeader(out, field_count);
    if (!fields.reply_to.empty()) {
        mpPackInt(out, LXMF_FIELD_REPLY_TO);
        /* FIELD_REPLY_TO is the raw 32-byte replied-to message hash. Stored
         * as hex64; convert back to raw 32 B for the wire. */
        uint8_t raw[32] = {};
        if (fields.reply_to.size() == 64) {
            for (int k = 0; k < 32; ++k) {
                unsigned x = 0;
                std::sscanf(fields.reply_to.c_str() + 2*k, "%2x", &x);
                raw[k] = (uint8_t)x;
            }
        }
        mpPackBin(out, raw, sizeof(raw));
    }
    if (quote) {
        mpPackInt(out, LXMF_FIELD_REPLY_QUOTE);
        /* UTF-8 bytes, packed BIN like title/content — the reference impl's
         * fields are bytes, and a str lands as a Python str a client then tries
         * to .decode(). */
        mpPackBin(out, reinterpret_cast<const uint8_t*>(fields.reply_quote.data()),
                  fields.reply_quote.size());
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
        if (key == LXMF_FIELD_REPLY_TO) {
            std::string raw;
            if (!mpReadStrOrBin(s, raw)) { if (!mpScanNext(s)) return false; continue; }
            if (raw.size() == 32 && fields_out) {
                char hex[65];
                for (int j = 0; j < 32; ++j)
                    std::snprintf(hex + 2*j, 3, "%02x", (uint8_t)raw[j]);
                fields_out->reply_to.assign(hex, 64);
            }
        } else if (key == LXMF_FIELD_REPLY_QUOTE) {
            std::string q;
            if (!mpReadStrOrBin(s, q)) { if (!mpScanNext(s)) return false; continue; }
            if (fields_out && q.size() <= LXMF_REPLY_QUOTE_MAX)
                fields_out->reply_quote = std::move(q);
        } else if (key == LXMF_FIELD_TICKET) {
            /* Capture the raw msgpack value span regardless of its shape
             * (str/bin/array) so it can be logged. We don't yet cache or
             * use tickets — see onInboundLxm. */
            size_t v0 = s.i;
            if (!mpScanNext(s)) return false;
            if (fields_out)
                fields_out->ticket.assign((const char*)s.p + v0, s.i - v0);
        } else {
            /* Skip everything else but stay parseable. */
            if (!mpScanNext(s)) return false;
        }
    }
    /* [4] optional stamp — not parsed here. */
    return true;
}

/* Yield hook handed to lxmfStampGenerate: stamp generation runs ~4 s of
 * tight CPU on the lxmf task (mostly the workblock build), so let
 * lower-priority tasks (and the idle task / WDT) breathe periodically. */
static void stampYield(void) { vTaskDelay(1); }

/* Build the full LXM wire bytes: dest || src || sig || msgpack.
 *
 * `src_hash` is the sender's *delivery destination hash* (not the
 * identity hash). The recipient does `Identity.recall(src_hash)` to
 * look up the sender for signature verification, and that map is
 * keyed by destination hash — passing the identity hash would make
 * the recipient see SOURCE_UNKNOWN and reject our signature.
 *
 * `identity_key` is the storage path of the sender's private key used
 * by rnsdSign. `out_message_id` (if non-null) receives the 32-byte
 * message_id = SHA-256(dest||src||packed-4-element) — the same value
 * regardless of any appended stamp.
 *
 * When `stamp_cost > 0` an LXMF proof-of-work stamp meeting that cost is
 * generated over the message_id and appended as payload element [4]
 * (this is AFTER the signature, exactly like the reference: the stamp is
 * not signed and not part of message_id). Returns empty on failure. */
static std::vector<uint8_t> lxmPackWire(const char* identity_key,
                                         const uint8_t src_hash[LXMF_DEST_HASH_LEN],
                                         const uint8_t dest_hash[LXMF_DEST_HASH_LEN],
                                         uint64_t ts_ms,
                                         std::string_view title,
                                         std::string_view content,
                                         const LxmFields& fields,
                                         int stamp_cost,
                                         uint8_t out_message_id[RNSD_HASH_LEN])
{
    std::vector<uint8_t> packed = lxmPackPayload(ts_ms, title, content, fields);

    /* signable = dest || src || packed || SHA-256(dest || src || packed).
     * The inner SHA-256 IS the message_id. */
    std::vector<uint8_t> signable;
    signable.reserve(LXMF_DEST_HASH_LEN * 2 + packed.size() + RNSD_HASH_LEN);
    signable.insert(signable.end(), dest_hash, dest_hash + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), src_hash,  src_hash  + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), packed.begin(), packed.end());
    uint8_t hash[RNSD_HASH_LEN];
    rnsdSha256(signable.data(), signable.size(), hash);
    if (out_message_id) std::memcpy(out_message_id, hash, RNSD_HASH_LEN);
    signable.insert(signable.end(), hash, hash + RNSD_HASH_LEN);

    uint8_t sig[RNSD_SIG_LEN];
    if (!rnsdSign(identity_key, signable.data(), signable.size(), sig))
        return {};

    std::vector<uint8_t> wire;
    wire.reserve(LXMF_OVERHEAD + packed.size() + 2 + LXMF_STAMP_LEN);
    wire.insert(wire.end(), dest_hash, dest_hash + LXMF_DEST_HASH_LEN);
    wire.insert(wire.end(), src_hash,  src_hash  + LXMF_DEST_HASH_LEN);
    wire.insert(wire.end(), sig,       sig       + RNSD_SIG_LEN);
    wire.insert(wire.end(), packed.begin(), packed.end());

    /* Append the proof-of-work stamp as a 5th payload element. The
     * 4-element payload always starts with a fixarray header (0x94, since
     * 4 ≤ 15); bump it to 0x95 and append the stamp as msgpack bin. The
     * recipient strips it back to 4 elements to re-derive message_id and
     * verify the signature. */
    if (stamp_cost > 0 && wire[LXMF_OVERHEAD] == 0x94) {
        uint8_t stamp[LXMF_STAMP_LEN];
        uint64_t t0 = nowUnixMs();
        if (lxmfStampGenerate(hash, stamp_cost, stamp, stampYield, nowUnixMs)) {
            wire[LXMF_OVERHEAD] = 0x95;
            mpPackBin(wire, stamp, sizeof stamp);
            info("stamp generated cost=%d in %llu ms",
                 stamp_cost, (unsigned long long)(nowUnixMs() - t0));
        } else {
            warn("stamp generation failed (cost=%d) — sending unstamped", stamp_cost);
        }
    }
    return wire;
}

/* Split an on-wire packed payload into the bytes that were hashed/signed
 * (the 4-element payload) and, if present, the appended PoW stamp.
 *
 * For a 4-element payload `*hashed_ptr` aliases `packed` directly. For a
 * ≥5-element payload the fixarray header is rewritten to 4 elements into
 * `scratch` (elements 0..3 are byte-identical to what the sender signed)
 * and the stamp (element [4]) is decoded into `stamp_out`. Reference
 * peers always use a single-byte fixarray header, so a non-fixarray
 * header is treated as "no stamp" and left untouched. */
static void lxmSplitStamp(const uint8_t* packed, size_t packed_n,
                          std::vector<uint8_t>& scratch,
                          const uint8_t** hashed_ptr, size_t* hashed_n,
                          std::vector<uint8_t>& stamp_out)
{
    *hashed_ptr = packed;
    *hashed_n   = packed_n;
    stamp_out.clear();
    if (packed_n < 1) return;
    uint8_t b = packed[0];
    if (b < 0x90 || b > 0x9F) return;     /* not a fixarray → leave as-is */
    if ((b & 0x0F) <= 4) return;          /* no stamp element */

    mpScan s{packed, packed_n, 1, 0, 0};
    for (int k = 0; k < 4; ++k) if (!mpScanNext(s)) return;  /* malformed */
    size_t stamp_start = s.i;

    scratch.clear();
    scratch.reserve(stamp_start);
    scratch.push_back(0x94);              /* 4-element fixarray header */
    scratch.insert(scratch.end(), packed + 1, packed + stamp_start);
    *hashed_ptr = scratch.data();
    *hashed_n   = scratch.size();

    std::string st;
    if (mpReadStrOrBin(s, st)) stamp_out.assign(st.begin(), st.end());
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

/* ─────────────── announces of seen LXMF destinations ─────────────── */

/* Cross-identity, ephemeral catalogue of every `lxmf.delivery` announce
 * we've heard recently. Populated by onAnnounceFromRnsd — fired on the
 * lxmf task when rnsd dispatches an event over our RNSD_PORT_ANNOUNCES
 * subscription. All storage writes happen here, on our task — the rnsd
 * task only memcpys the announce into one ITS packet and forwards.
 *
 * One record per dest in the RAM-only "lxmf_announces" store, addressed as
 * `lxmf.announces.<dest_hex>.<field>` (schema 3: last/hops/cost/name). Bounded
 * by `s.lxmf.max_announces` — the store self-evicts the oldest-inserted record
 * (STORAGE_DB_DROP) when a brand-new dest would exceed the cap. */

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

/* RNSD_PORT_ANNOUNCES frame:
 *   hops(1) | dest_hash(16) | identity_hash(16) | pubkey(64) | ratchet(32) |
 *   app_data(N)
 * The public key rides along so a subscriber can act on an announce without
 * calling back into rnsd for the identity. The ratchet is an announce field of
 * its own — all-zero when the peer advertises none — and is NOT part of
 * app_data, whatever the byte order on the wire may suggest. */
constexpr size_t LXMF_ANNOUNCE_PUBKEY_OFF  = 1 + 16 + 16;
constexpr size_t LXMF_ANNOUNCE_RATCHET_OFF = 1 + 16 + 16 + 64;
constexpr size_t LXMF_ANNOUNCE_HDR = 1 + 16 + 16 + 64 + 32;

/* ── lxmf.announces.<hex> record fields (store schema 3) ──
 *
 *   last    u32     announce time, nowUnixMs()/1000 (monotonic); mutates in place
 *   hops    u8      hop count from the frame; mutates in place
 *   cost    fixstr  decimal stamp cost, "-1" if unknown (fixstr so -1 round-trips)
 *   ratchet text    64-hex ratchet or empty (the browser ContactCard shows it)
 *   name    text    announced utf-8 display name (may be empty)
 *
 * AnnounceEntry is the in-RAM read shape for the device's own consumers (which
 * don't need the ratchet — it's carried only for the browser mirror). */

struct AnnounceEntry {
    int         last_s = 0;
    int         cost   = -1;
    int         hops   = -1;
    std::string name;
};

/* Read one announce record's fields. Announces live in the RAM-only record
 * store as `lxmf.announces.<hex>.<field>` (schema 3). Returns false if there is
 * no such record. `cost` is a fixstr so it round-trips the -1 "unknown" sentinel. */
static bool readAnnounce(const std::string& hex, AnnounceEntry* out)
{
    std::string base = "lxmf.announces." + hex;
    if (!storageExists(base.c_str())) return false;   /* 1-seg tail → sdbHasRecord */
    out->last_s = storageGetInt((base + ".last").c_str(), 0);
    out->cost   = storageGetInt((base + ".cost").c_str(), -1);
    out->hops   = storageGetInt((base + ".hops").c_str(), -1);
    out->name   = storageGetStr((base + ".name").c_str(), "");
    return true;
}

/* Display name last heard for `peer_hex` in the cross-identity announce
 * catalogue, or "" if we've never heard them announce (or the announce carried
 * no name). Used to seed `contacts.<peer>.display_name` on both first-contact
 * paths, and as the CLI's contact-name fallback. */
static std::string announceName(const std::string& peer_hex)
{
    return storageGetStr(("lxmf.announces." + peer_hex + ".name").c_str(), "");
}

/* Walk every announce record oldest-first, invoking cb(hex, entry) once per
 * record. storageForEach yields a store's fields grouped per record (arena
 * order), so we accumulate on the current hex and flush on the boundary — and
 * once more after the walk for the final record. File-scope state because the
 * storageForEach callback carries no ctx pointer.
 *
 * The catalogue is no longer size-capped here: the RAM-only store enforces its
 * own cap (STORAGE_DB_DROP) at write time, dropping the oldest-inserted record
 * when a brand-new dest would exceed s.lxmf.max_announces. */
static AnnounceEntry s_annAcc;
static std::string   s_annAccHex;
static bool          s_annAccHave = false;
static void        (*s_annRecordCb)(const std::string&, const AnnounceEntry&) = nullptr;

static void annAccLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("lxmf.announces.") - 1;
    const char* dot  = std::strchr(tail, '.');
    if (!dot) return;
    std::string hex(tail, dot - tail);
    const char* field = dot + 1;
    if (std::strchr(field, '.')) return;   /* leaf field only */
    if (s_annAccHave && hex != s_annAccHex) {
        s_annRecordCb(s_annAccHex, s_annAcc);
        s_annAcc = AnnounceEntry{};
    }
    s_annAccHex = hex; s_annAccHave = true;
    if      (!std::strcmp(field, "last")) s_annAcc.last_s = val ? std::atoi(val) : 0;
    else if (!std::strcmp(field, "cost")) s_annAcc.cost   = val ? std::atoi(val) : -1;
    else if (!std::strcmp(field, "hops")) s_annAcc.hops   = val ? std::atoi(val) : -1;
    else if (!std::strcmp(field, "name")) s_annAcc.name   = val ? val : "";
}

static void forEachAnnounce(void (*cb)(const std::string&, const AnnounceEntry&))
{
    s_annAccHave = false; s_annAcc = AnnounceEntry{}; s_annAccHex.clear();
    s_annRecordCb = cb;
    storageForEach("lxmf.announces.", annAccLeaf);
    if (s_annAccHave) cb(s_annAccHex, s_annAcc);
    s_annRecordCb = nullptr;
}

static void onAnnounceFromRnsd(int handle, size_t /*bytesAvail*/)
{
    if (handle != s_announce_sub_handle) return;
    PSRAM_BSS static uint8_t buf[LXMF_ANNOUNCE_HDR + 1024];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n < LXMF_ANNOUNCE_HDR) {
        if (n > 0) warn("announce sub: short frame %zu B", n);
        return;
    }

    int            hops     = buf[0];
    const uint8_t* dh       = buf + 1;
    /* buf + 17 is the announce identity hash — unused but
     * available if a consumer ever wants it. */
    const uint8_t* pubkey   = buf + LXMF_ANNOUNCE_PUBKEY_OFF;
    const uint8_t* ratchet  = buf + LXMF_ANNOUNCE_RATCHET_OFF;
    const uint8_t* app_data = buf + LXMF_ANNOUNCE_HDR;
    size_t         app_len  = n - LXMF_ANNOUNCE_HDR;

    /* rnsd already aspect-filtered for us, so dh is always a real
     * lxmf.delivery destination. Still filter out our own identities. */
    if (isOwnDest(dh)) return;

    LxmfAnnounceInfo info = parseLxmfAnnounce(app_data, app_len);
    bool has_ratchet = false;
    for (size_t k = 0; k < 32; ++k) if (ratchet[k]) { has_ratchet = true; break; }
    if (has_ratchet) info.ratchet_hex = bytesToHex(ratchet, 32);
    std::string dh_hex = bytesToHex(dh, LXMF_DEST_HASH_LEN);

    /* When the array parse yields no name/cost the raw bytes are the only
     * way to tell an emit-side shape change from a receive-side mangle —
     * dump them so a "name went blank" hunt has ground truth. */
    if (info.name.empty() && info.stamp_cost < 0)
        dbg("announces: %s unparsed app_data (%zu B) %s", dh_hex.c_str(),
            app_len, bytesToHex(app_data, app_len).c_str());

    std::string base = "lxmf.announces." + dh_hex;

    /* Write the record's fields. The RAM-only store self-caps
     * (STORAGE_DB_DROP): a brand-new dest past s.lxmf.max_announces drops the
     * oldest-inserted record; a re-announce from an existing dest mutates
     * last/hops/cost in place — no new record, no eviction, no scan. `cost` is a
     * fixstr so the -1 "unknown" sentinel round-trips. */
    char cbuf[8];
    std::snprintf(cbuf, sizeof(cbuf), "%d", info.stamp_cost);
    storageBegin();
    storageSet((base + ".last").c_str(), (int)(nowUnixMs() / 1000));
    storageSet((base + ".hops").c_str(), hops);
    storageSet((base + ".cost").c_str(), cbuf);
    /* Written even when empty, unlike the fields below: a peer that stops
     * advertising a ratchet must stop showing one. */
    storageSet((base + ".ratchet").c_str(), info.ratchet_hex.c_str());
    if (!info.name.empty())        storageSet((base + ".name").c_str(),    info.name.c_str());
    storageEnd();

    DBG_REMOTE("announces: %s name=\"%s\" cost=%d hops=%d ratchet=%s",
        dh_hex.c_str(), sanitizeForLog(info.name).c_str(),
        info.stamp_cost, hops,
        info.ratchet_hex.empty() ? "-" : info.ratchet_hex.substr(0, 16).c_str());

    /* This sender's pubkey is now cached. Replay anything we buffered
     * waiting on it (event-driven — the path request we issued on the
     * unknown-sender drop is what triggered this re-announce). Also land
     * the announced name in every slot's contact record: the announce is
     * authoritative for display_name, and the catalogue copy above is
     * RAM-only, so this write is what makes the name reboot-durable. */
    for (auto& id : s_ids) {
        if (!id.used) continue;
        drainPendingVerify(id, dh);
        if (storageExists(contactPath(id.index, dh_hex, "hash").c_str())) {
            /* A stored name is never cleared: an empty parse (nameless
             * announce, or one we failed to decode) keeps the last known
             * name rather than demoting the contact to a hex hash. */
            if (!info.name.empty())
                storageSet(contactPath(id.index, dh_hex, "display_name").c_str(),
                           info.name.c_str());
            /* The key the announce is signed with, kept with the contact so it
             * outlives rnsd's directory image. */
            contactSetPubkey(id.index, dh_hex, pubkey);
            /* Restamp the claim while we are here: an announce is evidence
             * this contact is live, and claim recency is what orders eviction
             * among claimed records. */
            lxmfClaimContact(dh_hex);
        }
    }

    /* Announces arrive in bursts (a path request makes a whole neighbourhood
     * re-announce), and each one lands on this core-0 task. Yield between them
     * so the idle task (WDT) and equal-priority core-0 peers get a slice
     * through the burst. */
    stampYield();
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

/* Publish the two addresses of a slot: the `lxmf.delivery` DESTINATION peers
 * write to, and the IDENTITY hash underneath it — what a Reticulum node sees
 * when this account identifies on a link, and what an operator matches against
 * an allow list or against the same key's address in another app. Both derive
 * from the private key and nothing else, so they are publishable the moment
 * the slot loads, long before its mailbox is up. */
static void publishIdentityAddresses(int n, const std::string& ikey,
                                     const uint8_t dest_hash[LXMF_DEST_HASH_LEN])
{
    uint8_t id_hash[RNSD_IDENT_HASH_LEN] = {};
    storageBegin();
    storageSet(idEphPath(n, "dest_hash").c_str(),
               bytesToHex(dest_hash, LXMF_DEST_HASH_LEN).c_str());
    if (rnsdIdentityHash(ikey.c_str(), id_hash))
        storageSet(idEphPath(n, "identity_hash").c_str(),
                   bytesToHex(id_hash, RNSD_IDENT_HASH_LEN).c_str());
    storageEnd();

    /* Seed rnsd's directory with this identity's own (dest → pubkey). A node
     * never hears its own announces, so nothing else would ever put it there —
     * and without it a message from one identity on this device to another
     * cannot be VERIFIED by the recipient, which is a delivery that never
     * leaves the box. This is exactly the off-network case rnsdSeedPubkey
     * exists for: the key did not come from an announce, and no key is better
     * authenticated than one we hold the private half of. */
    uint8_t pub[RNSD_PUBKEY_LEN];
    if (rnsdIdentityPubkey(ikey.c_str(), pub)) rnsdSeedPubkey(dest_hash, pub);
}

/* The one key a frontend gates its composer and its window on: this slot can
 * take a message. Two ways to be able to: our own delivery destination is
 * registered here (`up`), or the account is proxied — a client registers
 * nothing, hands each send to the Channel, and queues locally while that is
 * down. `up` alone is a lie for a proxied slot, which is permanently and
 * correctly not up; gating a UI on it strands a working account behind
 * "waiting for initialization". Written by every edge that can move either
 * input, plus the 1 Hz proxyPublish so a role flip lands without one. */
static void publishReady(int n)
{
    bool ready = proxyIsClient(n) ||
                 storageGetInt(idEphPath(n, "up").c_str(), 0) == 1;
    setIntIfChanged(idEphPath(n, "ready"), ready ? 1 : 0);
}

/* ─────────────── connect to our hosted rnsd destination ─────────────── */

/* Forward decl — onIts callbacks live below. */
static void onOurDestRecv(int handle, size_t bytesAvail);
static void onOurDestDisconnect(int handle);

static bool connectOurDest(lxmf_id_t& id)
{
    if (id.handle >= 0) return true;
    /* A proxied identity registers nothing — never zero registrants, never
     * two, and while this one is a client the server is the registrant. Its
     * addresses are still published (they derive from the key alone), so the
     * UI shows the account's real address while somebody else answers on it. */
    if (proxyIsClient(id.index)) {
        publishIdentityAddresses(id.index, id.identity_key, id.dest_hash);
        return false;
    }

    std::string ikey = secretsPath(id.index, "privkey");
    int h = rnsdDestOpen("lxmf.delivery", ikey.c_str(), /*SINGLE*/ 0,
                         /*ref*/ id.index,
                         onOurDestRecv, onOurDestDisconnect);
    if (h < 0) {
        err("id %d: our-dest connect failed", id.index);
        return false;
    }
    id.handle = h;
    info("id %d: our-dest connected (handle=%d)", id.index, h);

    /* Accept inbound DIRECT Links to this delivery dest. rnsd
     * flips accepts_links(true) and back-connects to LXMF_LINK_INBOX_PORT
     * with an rnsd_link_incoming_t per accepted Link. Without this,
     * real-world LXMF peers (default DIRECT) can't deliver to us. */
    if (!rnsdDestListenLinks(h, LXMF_LINK_INBOX_PORT))
        warn("id %d: rnsdDestListenLinks failed", id.index);

    storageSet(idEphPath(id.index, "up").c_str(), 1);
    publishReady(id.index);
    publishIdentityAddresses(id.index, id.identity_key, id.dest_hash);
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
    slot.pending_verify.clear();
    slot.sent = slot.received = slot.pending = slot.failed = 0;

    storageBegin();
    storageSet    (idPath(n, "label").c_str(),        display_name.c_str());
    storageSet    (idPath(n, "display_name").c_str(), display_name.c_str());
    storageDefault(idPath(n, "enabled").c_str(),      1);
    storageDefault(idPath(n, "default_method").c_str(), "auto");
    storageEnd();

    subscribePerIdCmds(n);
    publishIdentityAddresses(n, ikey, slot.dest_hash);

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
    slot.pending_verify.clear();
    slot.sent = slot.received = slot.pending = slot.failed = 0;

    subscribePerIdCmds(n);

    /* Publish the addresses now. Pure local crypto — no rnsd, no clock, no
     * network — so the UI can show this identity and its stored history the
     * instant we boot, long before connectOurDest brings the mailbox up. `up`
     * stays unset until then; `ready` is what gates sending, and it is already
     * true here for a proxied slot, which never comes up at all. */
    publishIdentityAddresses(n, ikey, slot.dest_hash);
    publishReady(n);

    uint8_t id_hash[RNSD_IDENT_HASH_LEN] = {};
    rnsdIdentityHash(ikey.c_str(), id_hash);
    info("id %d: loaded identity %s dest=%s", n,
         bytesToHex(id_hash, RNSD_IDENT_HASH_LEN).c_str(),
         bytesToHex(slot.dest_hash, LXMF_DEST_HASH_LEN).c_str());
    return true;
}

/* Destroy slot n: wipe secrets, wipe inbox + contacts + tickets, close
 * the our-dest. Used by lxmf.cmd.identity_destroy. */
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
    /* The conversations and the contact directory are record-store instances,
     * and only a key that names an instance exactly routes to one — deleting
     * the identity's config subtree does not touch them. Drop each by name, or
     * they outlive the identity on disk and the next identity created in this
     * slot inherits its messages and contacts. */
    std::string cpre = "s.lxmf.id." + std::to_string(n) + ".contacts";
    s_seedPeers.clear();
    storageForEach(cpre.c_str(), seedCollectPeer);
    storageBegin();
    for (auto& peer : s_seedPeers)
        storageDeleteTree(("s.lxmf.id." + std::to_string(n) + ".msgs." + peer).c_str());
    storageDeleteTree(cpre.c_str());
    storageDeleteTree(secretsPath(n, "").c_str());
    storageDeleteTree(idPath(n, "").c_str());
    storageDeleteTree(idEphPath(n, "").c_str());
    storageEnd();
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

/* Best display name we can find for `peer_hex` right now, "" if none. Tries the
 * RAM announce catalogue first, then rnsd's identity cache: it keeps the raw
 * app_data of the last announce it heard, which the catalogue may never have
 * recorded (nil-name guard) or have since evicted. The app_data is an LXMF
 * announce payload, so the same parser extracts the name. */
static std::string bestHeardName(const std::string& peer_hex)
{
    std::string nm = announceName(peer_hex);
    if (!nm.empty()) return nm;
    uint8_t dh[16];
    if (hexToDestHash(peer_hex, dh)) {
        uint8_t app[512];
        size_t  len = sizeof(app);
        if (rnsdRecallAppData(dh, app, &len)) {
            LxmfAnnounceInfo info = parseLxmfAnnounce(app, len);
            if (!info.name.empty()) return info.name;
        }
    }
    return "";
}

/* Fill display_name for any contact still showing only a hash, pulling a name
 * from any source we currently hold (bestHeardName) and persisting it. Because
 * the write lands in the browser-mirrored contact record, every frontend picks
 * it up and the recovery survives reboot. Only contacts whose display_name is
 * empty are probed; a fully-named book costs one storageForEach per identity. */
static void backfillContactNames()
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (!s_ids[n].used) continue;
        s_seedPeers.clear();
        std::string cpre = "s.lxmf.id." + std::to_string(n) + ".contacts";
        storageForEach(cpre.c_str(), seedCollectPeer);
        for (auto& peer : s_seedPeers) {
            if (!storageGetStr(contactPath(n, peer, "display_name").c_str(), "").empty())
                continue;
            std::string nm = bestHeardName(peer);
            if (!nm.empty())
                storageSet(contactPath(n, peer, "display_name").c_str(), nm.c_str());
        }
    }
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

/* Build LXMF announce app_data as msgpack `[display_name_bytes, stamp_cost]`.
 * NOTE: the ratchet is NOT part of app_data — upstream LXMF app_data is just
 * msgpack([display_name, stamp_cost, supported_functionality]). The ratchet
 * is a separate field in the RNS announce packet (public_key · name_hash ·
 * random_hash · [ratchet] · signature · app_data), its presence signalled by
 * the packet context_flag (FLAG_SET) and covered by the announce signature.
 * It is entirely rnsd's business: rnsd holds the destination, rotates its
 * ratchets and puts the current one on the air (s.rnsd.ratchets), so nothing
 * here has to ask for it or know it happened. */
static std::vector<uint8_t> buildAnnounceAppData(int id_n)
{
    std::string name = storageGetStr(idPath(id_n, "display_name").c_str(), "");
    /* Advertised stamp cost is a single global knob (0 = advertise none).
     * Whether we *require* it on inbound is the separate enforce_stamps
     * toggle. */
    int cost = storageGetInt("s.lxmf.stamp_cost", 8);

    /* Plain LXMF: `[display_name, stamp_cost]` and nothing else. We used to
     * append a capability bitfield as element [2]; nothing ever read one back
     * — not here and not in any other implementation — so it was a field
     * announced to the whole mesh on every announce and consulted by nobody. */
    std::vector<uint8_t> out;
    mpPackArrayHeader(out, 2);
    /* Display name goes out as msgpack BIN, not str: LXMF's
     * display_name_from_app_data does dn.decode("utf-8") on the unpacked
     * value, which only works when it unpacks to Python bytes. A str
     * value there throws "'str' object has no attribute 'decode'" and the
     * name silently drops from the peer's catalogue. */
    if (name.empty()) out.push_back(0xC0 /* nil */);
    else              mpPackBin(out, reinterpret_cast<const uint8_t*>(name.data()), name.size());
    mpPackInt(out, cost);
    return out;
}

/* Per-identity participation switch — `s.lxmf.id.<n>.enabled`, default
 * on (default written in createIdentityForSlot/loadIdentityForSlot). A
 * disabled identity does not announce, does not send, and drops inbound
 * LXM: it goes dark on the mesh until re-enabled. There is no global
 * lxmf enable — participation is per identity. */
static bool idEnabled(int n)
{
    return storageGetInt(idPath(n, "enabled").c_str(), 1) != 0;
}

static void sendAnnounce(lxmf_id_t& id)
{
    if (id.handle < 0) {
        warn("id %d: announce skipped (no our-dest handle)", id.index);
        return;
    }
    if (!idEnabled(id.index)) {
        dbg("id %d: announce suppressed (identity disabled)", id.index);
        return;
    }
    /* A proxied identity announces nothing: exactly one device registers and
     * announces the account's lxmf.delivery, and while `proxy_role` is client
     * that device is the server. */
    if (proxyIsClient(id.index)) {
        dbg("id %d: announce suppressed (proxied)", id.index);
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
    /* Present tense: we've handed the announce to rnsd, which decides whether it
     * actually reaches the air (it logs "announcing …" or "announce held …") —
     * so don't claim "sent" here. Pretty-print the app_data we built: name + cost
     * (the [d] shape from parseLxmfAnnounce), keeping the line self-contained. */
    std::string name = storageGetStr(idPath(id.index, "display_name").c_str(), "");
    int cost = storageGetInt("s.lxmf.stamp_cost", 8);
    info("id %d: announcing name=\"%s\" cost=%d (%zu B app_data)",
         id.index, sanitizeForLog(name).c_str(), cost, app_data.size());
}

/* Move a draft from `ready` into the packed/sending pipeline. */
/* ─────────────── persistent per-peer DIRECT links ───────────────
 *
 * Upstream LXMF keeps delivery Links open and reuses them for the whole
 * conversation; we mirror that. One Link per (identity, peer), tagged
 * "lxmf.id<n>.<peer8>", opened by the first DIRECT send and KEPT after
 * settlement (neither settle path tears down on success). rnsd ties Link
 * lifetime to our ITS handle (no parking), so this pool IS the warm-hold:
 * s.lxmf.link.idle_s reaps links idle past N seconds (default 600 = 10 min;
 * 0 = never, rely on LRU + Reticulum STALE).
 * Links are bidirectional — the peer may deliver to us over
 * this link, and those bytes feed the same onInboundLxm pipeline as the
 * inbound-Link path. Sends to one peer serialize: rnsd allows one
 * in-flight resource per link slot and the pre-active outbox holds one
 * packet, so a send while another is unfinished on the same link waits in
 * the delivery queue for the next sweep. Budget: ≤ LXMF_MAX_CONV_LINKS
 * of the device link budget (12 total; nomad's sessions hold 7);
 * LRU-evicted when full. */
#define LXMF_MAX_CONV_LINKS 4
struct convlink_t {
    bool        used;
    int         id_index;
    std::string peer_hex;
    std::string tag;
    int         handle;        /* RNSD_PORT_LINK ITS handle; -1 = conn gone */
    uint32_t    last_used_s;
    bool        identified;    /* identified to the peer (once) so it can reply over our link */
};
static convlink_t s_convlinks[LXMF_MAX_CONV_LINKS];

/* Inbound DIRECT links (peers' links into our delivery dests — accepted
 * by rnsd, forwarded to LXMF_LINK_INBOX_PORT; the handlers live in the
 * "inbound DIRECT" section below). We only ever RECEIVE on these — the
 * peer delivers LXMs to us over the link it opened. We never send over
 * one (not every client accepts that); our replies open our own
 * conversation link (see the conv pool above). */
#define LXMF_MAX_INLINKS 8
struct inlink_t {
    bool        used;
    int         handle;     /* ITS handle of the forwarded Link */
    int         id_index;   /* which s_ids[] hosts the destination */
    std::string tag;        /* rnsd-generated "in.<8hex>" — keys rnsd.links.<tag>.* */
};
static inlink_t s_inlinks[LXMF_MAX_INLINKS];

static convlink_t* convFind(int id_index, const std::string& peer_hex)
{
    for (auto& c : s_convlinks)
        if (c.used && c.id_index == id_index && c.peer_hex == peer_hex) return &c;
    return nullptr;
}

/* Closing our ITS handle tears the Link down in rnsd (onLinkDisconnect);
 * there is no separate teardown call. */
static void convDrop(convlink_t& c)
{
    if (!c.used) return;
    if (c.handle >= 0) { itsDisconnect(c.handle); c.handle = -1; }
    c.used = false;
    c.peer_hex.clear();
    c.tag.clear();
    c.identified = false;
}

/* The peer can deliver over OUR link (full LXM wire incl. dest16, exactly
 * like the inbound-Link path) — feed the shared pipeline. */
static void onConvLinkRecv(int handle, size_t /*bytesAvail*/)
{
    PSRAM_BSS static uint8_t buf[2048];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n == 0) return;
    for (auto& c : s_convlinks) {
        if (!c.used || c.handle != handle) continue;
        if (s_ids[c.id_index].used) {
            c.last_used_s = (uint32_t)(nowUnixMs() / 1000);
            onInboundLxm(s_ids[c.id_index], buf, n, /*via_link=*/true);
        }
        return;
    }
}

static void onConvLinkDisc(int ref)
{
    if (ref < 0 || ref >= LXMF_MAX_CONV_LINKS) return;
    convlink_t& c = s_convlinks[ref];
    if (!c.used) return;
    /* rnsd closed it (idle/STALE, failure, remote close): the slot + tag
     * are being reclaimed server-side — forget it; the next send reopens. */
    verb("id %d: conv link %s closed", c.id_index, c.tag.c_str());
    c.handle = -1;
    c.used = false;
    c.peer_hex.clear();
    c.tag.clear();
    c.identified = false;
}

/* True while any unfinished DIRECT outbound rides the conversation link
 * with this tag (sends serialize per link; rnsd holds one in-flight
 * resource + one receipt per slot). */
static bool linkTagBusy(const std::string& tag)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (!s_ids[n].used) continue;
        for (auto& o : s_ids[n].outboxes)
            if (o.used && o.direct && o.link_tag == tag) return true;
    }
    return false;
}

static bool convBusy(const convlink_t& c) { return linkTagBusy(c.tag); }

/* Get-or-open the conversation link for (id, peer). nullptr on failure,
 * or on a miss when open_if_missing is false (probe-only). */
static convlink_t* convGet(lxmf_id_t& id, const std::string& peer_hex,
                           const uint8_t dh[16], bool open_if_missing = true)
{
    convlink_t* c = convFind(id.index, peer_hex);
    if (c) {
        /* Liveness: a failed/closing link slot can't carry a send. */
        std::string st = storageGetStr(("rnsd.links." + c->tag + ".state").c_str(), "");
        if (c->handle >= 0 && st != "failed" && st != "closed" && st != "closing")
            return c;
        convDrop(*c);
    }
    if (!open_if_missing) return nullptr;
    /* Comms-initiate: assert the claim so the peer's directory record outlives
     * the announce churn for as long as we are talking to them. */
    lxmfClaimContact(peer_hex);
    convlink_t* slot = nullptr;
    for (auto& s : s_convlinks) if (!s.used) { slot = &s; break; }
    if (!slot) {                          /* full: evict the LRU idle one */
        convlink_t* oldest = nullptr;
        for (auto& s : s_convlinks) {
            if (convBusy(s)) continue;    /* never evict mid-send */
            if (!oldest || s.last_used_s < oldest->last_used_s) oldest = &s;
        }
        if (!oldest) return nullptr;
        convDrop(*oldest);
        slot = oldest;
    }
    char tag[24];
    std::snprintf(tag, sizeof(tag), "lxmf.id%d.%.8s", id.index, peer_hex.c_str());
    /* s.lxmf.link_timeout (seconds, 0 = let rnsd derive from interface speed)
     * overrides the establishment timeout for delivery links. */
    uint32_t link_to_ms = (uint32_t)storageGetInt("s.lxmf.link_timeout", 0) * 1000;
    int lh = rnsdLinkOpen(dh, "lxmf.delivery", id.identity_key.c_str(),
                          tag, /*path_timeout_ms=*/0, link_to_ms,
                          /*ref=*/(int)(slot - s_convlinks),
                          onConvLinkRecv, onConvLinkDisc);
    if (lh < 0) return nullptr;
    slot->used        = true;
    slot->id_index    = id.index;
    slot->peer_hex    = peer_hex;
    slot->tag         = tag;
    slot->handle      = lh;
    slot->last_used_s = (uint32_t)(nowUnixMs() / 1000);
    /* Identify on the way up, before any message rides this link. A peer
     * validates an LXM's signature against the sender identity it can recall,
     * and a peer that has never heard our announce — anything past the
     * interface's service radius — has none to recall: it drops the message
     * unread and proves nothing. The LINKIDENTIFY on this link is where that
     * peer gets our identity, so it has to arrive before the first message
     * does. Identifying only after a delivery cannot work here: the delivery
     * being waited for is the one the missing identity is preventing. rnsd
     * holds the identify until the handshake completes and runs it ahead of
     * everything queued on the link. */
    slot->identified  = rnsdLinkIdentify(tag);
    /* Dialled, not up: rnsd still has to find a path and complete the
     * handshake, and either can fail. The link is established only when
     * `rnsd.links.<tag>.state` reads "active". */
    info("id %d: conv link %s dialling %s", id.index, tag, peer_hex.c_str());
    return slot;
}

/* Post-settle bookkeeping shared by the three DIRECT settle paths
 * (resource fast-path aux, resource tick fallback, packet proof): on a
 * delivered settle keep the link warm and, if the identify queued when the
 * link was opened never reached rnsd, send it now — the peer needs our
 * identity to address replies back over OUR link (which we accept). On a
 * failed settle drop the link (it's suspect). No-proof packet settles
 * ("sent", message may have arrived) call neither — the link is kept. */
static void directLinkSettle(const std::string& tag, bool ok, uint32_t now_s)
{
    for (auto& c : s_convlinks) {
        if (!c.used || c.tag != tag) continue;
        if (!ok) { convDrop(c); return; }
        c.last_used_s = now_s;
        if (!c.identified && rnsdLinkIdentify(c.tag.c_str())) {
            c.identified = true;
            verb("id %d: conv link %s identified",
                 c.id_index, c.tag.c_str());
        }
        return;
    }
}

/* ─────────────── the delivery queue ───────────────
 *
 * Every outbound that could not be delivered YET — no path found within the
 * grace, no proof back, a conversation link that failed or is busy, an outbox
 * with no free slot, rnsd's path table full — waits here for its next attempt.
 * Nothing rnsd-side and no outbox slot is held while a message is queued, so a
 * peer that is away for an hour costs one Link attempt per sweep and nothing
 * else, however much mail is waiting for them.
 *
 *   cmd.send ──► processReady (attempt) ──ok──► outbox slot ──► DELIVERED
 *                    │  can't yet                    │ attempt failed  │
 *                    ▼                               ▼                 │
 *                 s_queue ◄──────────────────────────┘                 │
 *                    │  every s.lxmf.delivery_interval min (default 10),│
 *                    │  or at once for this peer on a delivery ◄────────┘
 *                    ▼
 *               queueSweep: queued ≥ s.lxmf.delivery_timeout min (default 60)
 *                           → DELIVERY_TIMEOUT, else ONE Link attempt per
 *                             conversation (processReady, retry = true)
 *
 * An unproven opportunistic packet waits in the queue too, but its next
 * attempts are the same packet again at a fixed spacing (oppProofMissed,
 * queueResendDue) before its conversation is kicked into a sweep.
 *
 * The sweep runs on this task from the 1 Hz tick: every piece of outbound state
 * (outbox, wire cache, conversation links) is task-local and lock-free, and a
 * task of its own would need locking on all of it for no gain. `queued_s` is
 * monotonic since boot; the boot scan (queueScanStorage) re-queues every
 * in-progress outbound it finds in storage with a fresh clock, so a reboot
 * restarts the timeout rather than failing everything that was mid-delivery.
 * `tries` counts attempts; a message settles out of the queue when its status
 * leaves the in-progress set (delivered, cancelled, deleted, handed to a proxy
 * server, timed out) — msgSetStatus/msgFail drop the entry, the sweep skips
 * the rest. */
struct queued_t {
    int id_index; std::string peer, mid;
    uint32_t queued_s;
    /* The summed LoRa contention-drop counter when this message was queued.
     * Growth by the time it gives up says the own channel was jammed, which is
     * what the give-up then names (radioBusyOr). */
    uint32_t tx_drops_base;
    /* Opportunistic re-sends of the same packet already made, and when the
     * next one is due (unix s, 0 = none waiting). See oppProofMissed. */
    uint8_t  resends;
    uint32_t resend_at_s;
    /* Link attempts for this message that failed to carry it — rnsd gave up
     * establishing, or the link closed before the send — and when the one
     * retry they earn is due (unix s, 0 = none waiting). See linkFailed. */
    uint8_t  link_fails = 0;
    uint32_t link_retry_at_s = 0;
};
static std::vector<queued_t> s_queue;
static uint32_t s_queueNextSweep_s = 0;    /* 0 = no sweep armed (queue empty) */
/* A settle asked for an immediate follow-up sweep. Read and cleared by the
 * sweep, which must not push its own arming back out to a whole interval over
 * a kick raised while it was running (a delivery that settles inline). */
static bool s_queueKicked = false;

/* Seconds rnsd may search for a path on one attempt before the message goes
 * back to the queue: covers its first two path-request retries (5 s, 30 s). A
 * search left to rnsd alone runs for hours and holds one of the four slots in
 * its per-connection table, which is how four unreachable peers stopped every
 * further send to anyone without a known path. */
static constexpr uint32_t LXMF_PATH_GRACE_S = 60;

/* Seconds we wait for a delivery-proof outcome rnsd owes us before settling the
 * send ourselves. rnsd settles its own receipt at s.rnsd.proof_timeout_s and
 * reports the outcome then, so this only has to outlast that window: a backstop
 * inside it takes the send back while rnsd is still waiting, and the real
 * outcome then arrives for a send_id whose slot is gone. Read the same knob
 * rather than assume its default, so raising the window there cannot invert the
 * two. */
static uint32_t proofBackstopS()
{
    int w = storageGetInt("s.rnsd.proof_timeout_s", 60);
    if (w < 1) w = 1;
    return (uint32_t)w + 30;
}

static uint32_t deliveryIntervalS()
{
    int m = storageGetInt("s.lxmf.delivery_interval", 10);
    return (uint32_t)(m > 0 ? m : 10) * 60u;
}
static uint32_t deliveryTimeoutS()
{
    int m = storageGetInt("s.lxmf.delivery_timeout", 60);
    return (uint32_t)(m > 0 ? m : 60) * 60u;
}

static bool statusInProgress(int st)
{
    return st == LXMF_ST_QUEUED || st == LXMF_ST_REQUESTING_PATH ||
           st == LXMF_ST_SENDING || st == LXMF_ST_AWAITING_PROOF ||
           st == LXMF_ST_RETRYING_DELIVERY || st == LXMF_ST_RETRYING_LINK ||
           /* A SEND still crossing to the server as a Resource: the transfer
            * can fail halfway, so it is ours and still moving until rnsd
            * echoes it done — unlike ON_OUR_PROXY, which is the server's to
            * move. A SEND small enough for one Channel message never sits
            * here; the Channel's own proof carries it, and it goes straight
            * to ON_OUR_PROXY. */
           st == LXMF_ST_SENDING_TO_PROXY;
}

static void queueAdd(int n, const std::string& peer, const std::string& mid)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    bool held = false;
    for (auto& e : s_queue)
        if (e.id_index == n && e.peer == peer && e.mid == mid) { held = true; break; }
    if (!held) s_queue.push_back({ n, peer, mid, now_s, loraTxDroppedSum(), 0, 0 });
    if (!s_queueNextSweep_s) s_queueNextSweep_s = now_s + deliveryIntervalS();
}

/* Bring the next sweep forward to now if the queue still holds anything for
 * this conversation. Arms nothing on an empty queue — a sweep with no work is
 * a walk over nothing, and `s_queueNextSweep_s == 0` is what says the queue is
 * idle. The sweep itself runs later in the same 1 Hz pass. */
static void queueKickConversation(int n, const std::string& peer)
{
    for (auto& e : s_queue)
        if (e.id_index == n && e.peer == peer) {
            s_queueNextSweep_s = (uint32_t)(nowUnixMs() / 1000);
            s_queueKicked      = true;
            return;
        }
}

/* Drop a message from the queue; `mid` empty = every message to `peer`. */
static void queueRemove(int n, const std::string& peer, const std::string& mid)
{
    for (auto it = s_queue.begin(); it != s_queue.end(); )
        if (it->id_index == n && it->peer == peer && (mid.empty() || it->mid == mid))
            it = s_queue.erase(it);
        else ++it;
    if (s_queue.empty()) s_queueNextSweep_s = 0;
}

/* An attempt did not deliver and the message may still be deliverable later:
 * show `status` for it, count the attempt, and queue it for the next sweep. */
static void queueRequeue(lxmf_id_t& id, const std::string& peer,
                         const std::string& mid, uint8_t status)
{
    int tries = storageGetInt(msgPath(id.index, peer, mid, "tries").c_str(), 0);
    if (tries < LXMF_TRIES_GAVEUP - 1) tries++;
    storageBegin();
    storageSet(msgPath(id.index, peer, mid, "status").c_str(), (int)status);
    storageSet(msgPath(id.index, peer, mid, "tries").c_str(),  tries);
    storageEnd();
    queueAdd(id.index, peer, mid);
    dbg("id %d: msg %s queued (%s, try %d)", id.index, mid.c_str(),
        lxmfStatusName(status), tries);
}

static bool outboxHolds(const lxmf_id_t& id, const std::string& peer, const std::string& mid)
{
    for (auto& o : id.outboxes)
        if (o.used && o.peer == peer && o.msg_key == mid) return true;
    return false;
}

/* Any attempt at all in flight to this peer. A sweep's unit is the
 * conversation, so a conversation with a send still settling has its attempt
 * already running and is left alone — sends to one peer serialize on the Link
 * anyway, and starting a second here would only bounce off linkTagBusy. */
static bool outboxHoldsPeer(const lxmf_id_t& id, const std::string& peer)
{
    for (auto& o : id.outboxes)
        if (o.used && o.peer == peer) return true;
    return false;
}

/* OUT_CANCEL for a send rnsd still holds. The CANCELLED result it answers with
 * meets a freed send_id and no-ops in applyOutResult. */
static void sendCancel(lxmf_id_t& id, uint16_t send_id)
{
    if (id.handle < 0) return;
    uint8_t f[3] = { RNSD_DEST_OUT_CANCEL, (uint8_t)(send_id >> 8), (uint8_t)(send_id & 0xFF) };
    if (itsSend(id.handle, f, sizeof(f), pdMS_TO_TICKS(200)) == 0)
        warn("id %d: cancel frame send dropped (send_id=%u)", id.index, (unsigned)send_id);
}

static void processReady(lxmf_id_t& id, const std::string& peer_hex,
                         const std::string& mid, bool retry = false);   /* fwd */

/* Re-sends of an unproven opportunistic packet before the conversation
 * escalates to a Link. */
static constexpr uint8_t LXMF_OPP_RESENDS = 2;

/* Seconds between two sends of the same opportunistic packet: no sooner than
 * a proof could have come back (rnsd's proof window) nor than a path could
 * have been found again (rnsd's path budget), since a route that moved is one
 * reason a proof goes missing. */
static uint32_t oppResendSpacingS()
{
    uint32_t s = (uint32_t)storageGetInt("s.rnsd.proof_timeout_s", 60);
    uint32_t b = (uint32_t)rnsdPathBudgetS();
    return s > b ? s : b;
}

static queued_t* queueFind(int n, const std::string& peer, const std::string& mid)
{
    for (auto& e : s_queue)
        if (e.id_index == n && e.peer == peer && e.mid == mid) return &e;
    return nullptr;
}

/* An opportunistic packet went out and no proof came back.
 *
 *   A → B   LXM packet            (sent_s)
 *   A       no proof within the proof window
 *   A → B   the same packet       sent_s + spacing, same message_id
 *   A → B   the same packet       again, spacing later
 *   A → B   Link                  the sweep's attempt, if still unproven
 *
 * A missing proof is indistinguishable from a missing message: the proof is a
 * packet on the same radio and is lost as easily. So the identical wire goes
 * out again, spaced by oppResendSpacingS — the recipient's rnsd proves every
 * copy that reaches it, and its lxmf stores the first and drops the rest on
 * message_id, so a copy that was already delivered costs one packet and
 * surfaces nothing. Only when LXMF_OPP_RESENDS re-sends have gone unanswered
 * is the conversation kicked, and the sweep's Link attempt asks whether the
 * peer is there at all. opportunistic-or-fail never escalates: it keeps
 * re-sending at the same spacing until the delivery timeout. */
static void oppProofMissed(lxmf_id_t& id, const std::string& peer,
                           const std::string& mid, uint32_t sent_s)
{
    queueRequeue(id, peer, mid, LXMF_ST_RETRYING_DELIVERY);
    queued_t* e = queueFind(id.index, peer, mid);
    if (e && (e->resends < LXMF_OPP_RESENDS ||
              resolveMethod(id.index, peer, mid) == "opportunistic-or-fail")) {
        uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
        uint32_t at = (sent_s ? sent_s : now_s) + oppResendSpacingS();
        e->resend_at_s = at > now_s ? at : now_s;
        return;
    }
    queueKickConversation(id.index, peer);
}

/* A Link attempt for this message failed to carry it: rnsd's establishment
 * ran out (after its own fresh attempts toward a peer it has heard from), or
 * the link closed before the send went out.
 *
 *   A → B   Link        failed           (link_fails 1)
 *   A → B   Link        5 min later      (the one retry)
 *   A       LINK_FAIL   if that fails too
 *
 * rnsd has already spent its retries on this attempt, so a peer that is there
 * but hard to reach gets one more try after the medium has had time to change,
 * and a message that cannot get a link says so within minutes instead of
 * sitting in the queue for the whole delivery timeout. A message waiting for a
 * PATH is not this: that one stays on the sweep. */
static constexpr uint8_t  LXMF_LINK_FAILS_MAX = 2;
static constexpr uint32_t LXMF_LINK_RETRY_S   = 5 * 60;

static void linkFailed(lxmf_id_t& id, const std::string& peer,
                       const std::string& mid)
{
    queueRequeue(id, peer, mid, LXMF_ST_RETRYING_LINK);
    queued_t* e = queueFind(id.index, peer, mid);
    if (!e) return;
    if (++e->link_fails >= LXMF_LINK_FAILS_MAX) {
        uint32_t drops_base = e->tx_drops_base;
        msgFail(id.index, peer, mid, radioBusyOr(drops_base, LXMF_ST_LINK_FAIL));
        id.failed++;
        warn("id %d: msg %s link failed twice — giving up", id.index, mid.c_str());
        return;
    }
    e->link_retry_at_s = (uint32_t)(nowUnixMs() / 1000) + LXMF_LINK_RETRY_S;
    info("id %d: msg %s link failed — one retry in %u min", id.index, mid.c_str(),
         (unsigned)(LXMF_LINK_RETRY_S / 60));
}

/* Fire the opportunistic re-sends and link retries that have come due
 * (oppProofMissed, linkFailed). From the 1 Hz tick. */
static void queueResendDue(void)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    std::vector<std::pair<queued_t, bool>> due;   /* (entry, link retry) */
    for (auto& e : s_queue) {
        if (e.link_retry_at_s && now_s >= e.link_retry_at_s) {
            e.link_retry_at_s = 0;
            due.emplace_back(e, true);
            continue;
        }
        if (!e.resend_at_s || now_s < e.resend_at_s) continue;
        e.resend_at_s = 0;
        e.resends++;
        due.emplace_back(e, false);
    }
    /* processReady may queue again, so it runs on copies, off the list. */
    for (auto& [e, link_retry] : due) {
        if (e.id_index < 0 || e.id_index >= LXMF_MAX_IDENTITIES ||
            !s_ids[e.id_index].used) continue;
        lxmf_id_t& id = s_ids[e.id_index];
        int st    = storageGetInt(msgPath(id.index, e.peer, e.mid, "status").c_str(), 0);
        int tries = storageGetInt(msgPath(id.index, e.peer, e.mid, "tries").c_str(),  0);
        if (!statusInProgress(st) || tries == LXMF_TRIES_GAVEUP) continue;
        if (outboxHolds(id, e.peer, e.mid)) continue;
        if (link_retry) {
            info("id %d: msg %s retrying its failed link", id.index, e.mid.c_str());
            processReady(id, e.peer, e.mid, /*retry=*/true);
            continue;
        }
        info("id %d: msg %s message_id=%s re-sent as the same packet (attempt %u)",
             id.index, e.mid.c_str(),
             storageGetStr(msgPath(id.index, e.peer, e.mid, "message_id").c_str(), "?").c_str(),
             (unsigned)e.resends + 1);
        processReady(id, e.peer, e.mid, /*retry=*/false);
    }
}

/* One pass over the queue. Runs from the 1 Hz tick when the interval has
 * elapsed, only while the queue holds something.
 *
 * A sweep makes **one attempt per conversation**, not one per message. The
 * unit of a retry is the Link to the peer (see processReady's `retry`), and a
 * Link is a property of the conversation: seven messages waiting for the same
 * peer are seven passengers for one attempt, not seven attempts. Sweeping
 * per-message instead put seven whole messages on the air every interval to
 * ask a question — is this peer reachable — that one Link open answers for all
 * of them. The oldest queued message goes first (queue order is insertion
 * order); the rest follow it over the Link as each delivery settles, without
 * waiting for another interval.
 *
 * The timeout check stays per-message: each carries its own `queued_s`, and a
 * message that has waited out `s.lxmf.delivery_timeout` is given up on whether
 * or not its conversation got this sweep's attempt. */
static void queueSweep(void)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    s_queueKicked  = false;
    std::vector<queued_t> q;
    q.swap(s_queue);
    /* Conversations already attempted in this pass, as (identity, peer). Small
     * and linear on purpose: it is bounded by the peers with mail waiting. */
    std::vector<std::pair<int, std::string>> attempted;
    auto claimAttempt = [&](int n, const std::string& peer) {
        for (auto& a : attempted) if (a.first == n && a.second == peer) return false;
        attempted.emplace_back(n, peer);
        return true;
    };
    for (auto& e : q) {
        if (e.id_index < 0 || e.id_index >= LXMF_MAX_IDENTITIES ||
            !s_ids[e.id_index].used) continue;
        lxmf_id_t& id = s_ids[e.id_index];
        int st    = storageGetInt(msgPath(id.index, e.peer, e.mid, "status").c_str(), 0);
        int tries = storageGetInt(msgPath(id.index, e.peer, e.mid, "tries").c_str(),  0);
        if (!statusInProgress(st) || tries == LXMF_TRIES_GAVEUP) continue;   /* settled elsewhere */
        if (outboxHolds(id, e.peer, e.mid)) { s_queue.push_back(e); continue; } /* attempt in flight */
        if (now_s - e.queued_s >= deliveryTimeoutS()) {
            msgFail(id.index, e.peer, e.mid,
                    radioBusyOr(e.tx_drops_base, LXMF_ST_DELIVERY_TIMEOUT));
            id.failed++;
            info("id %d: msg %s not delivered in %u min — giving up",
                 id.index, e.mid.c_str(), (unsigned)(deliveryTimeoutS() / 60));
            continue;
        }
        s_queue.push_back(e);          /* stays until it settles; a failed attempt finds it here */
        /* One attempt per conversation per sweep: this peer's turn is already
         * spent, and this message rides the same Link when that one settles. */
        if (!claimAttempt(e.id_index, e.peer)) continue;
        /* A re-send of the same packet, or a failed link's retry, is
         * scheduled: that is this conversation's attempt (queueResendDue). */
        if (e.resend_at_s || e.link_retry_at_s) continue;
        if (outboxHoldsPeer(id, e.peer)) continue;   /* its attempt is in flight */
        processReady(id, e.peer, e.mid, /*retry=*/true);
    }
    if (s_queue.empty())         s_queueNextSweep_s = 0;
    else if (!s_queueKicked)     s_queueNextSweep_s = now_s + deliveryIntervalS();
}

/* Boot scan: every stored outbound still in progress goes on the queue, so a
 * reboot mid-delivery resumes delivering instead of leaving the message QUEUED
 * on screen forever. Records arrive grouped by message in arena order. */
struct QueueScanCtx {
    std::string mid;
    int  status = -1, tries = 0;
    bool out = false;
    std::vector<std::string> found;
};
static QueueScanCtx* s_qscan = nullptr;
static void queueScanFlush()
{
    QueueScanCtx& c = *s_qscan;
    if (!c.mid.empty() && c.out && statusInProgress(c.status) && c.tries != LXMF_TRIES_GAVEUP)
        c.found.push_back(c.mid);
    c.mid.clear(); c.status = -1; c.tries = 0; c.out = false;
}
static void queueScanLeaf(const char* key, const char* val)
{
    const char* d2 = strrchr(key, '.');
    if (!d2) return;
    std::string field(d2 + 1);
    std::string head(key, d2 - key);
    size_t p = head.rfind('.');
    if (p == std::string::npos) return;
    std::string mid = head.substr(p + 1);
    if (mid != s_qscan->mid) { queueScanFlush(); s_qscan->mid = mid; }
    if      (field == "dir")    s_qscan->out    = (val && !strcmp(val, "out"));
    else if (field == "status") s_qscan->status = val ? atoi(val) : -1;
    else if (field == "tries")  s_qscan->tries  = val ? atoi(val) : 0;
}
static void queueScanStorage(void)
{
    int queued = 0;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (!s_ids[n].used) continue;
        s_seedPeers.clear();
        std::string cpre = "s.lxmf.id." + std::to_string(n) + ".contacts";
        storageForEach(cpre.c_str(), seedCollectPeer);
        for (auto& peer : s_seedPeers) {
            QueueScanCtx ctx;
            s_qscan = &ctx;
            std::string mp = "s.lxmf.id." + std::to_string(n) + ".msgs." + peer;
            storageForEach(mp.c_str(), queueScanLeaf);
            queueScanFlush();
            s_qscan = nullptr;
            for (auto& mid : ctx.found) { queueAdd(n, peer, mid); queued++; }
        }
    }
    s_seedPeers.clear();
    if (queued) info("[%s] delivery queue: %d message(s) resumed from storage", TAG, queued);
}

static void convReap(void)
{
    uint32_t idle_s = (uint32_t)storageGetInt("s.lxmf.link.idle_s", 0);
    if (idle_s == 0) return;                       /* 0 (default) = keep open */
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    for (auto& c : s_convlinks) {
        if (!c.used || convBusy(c)) continue;
        if (now_s - c.last_used_s >= idle_s) {
            info("id %d: conv link %s idle %us — closing",
                 c.id_index, c.tag.c_str(), (unsigned)(now_s - c.last_used_s));
            convDrop(c);
        }
    }
}

/* Resolve the packed + signed (+ delivery-stamped) wire for (peer, mid): reuse
 * the RAM outbox (a resend must not re-pack or re-pay the multi-second stamp),
 * else pack from the stored record and cache it. On the first pack the
 * firmware-owned record fields (message_id, ts, status=queued) are persisted
 * and the conversation directory bumped — a resend skips both. Returns false
 * when packing fails (the message is then already settled PACK_FAIL). */
static bool resolveOutboundWire(lxmf_id_t& id, const std::string& peer_hex,
                                const std::string& mid, const uint8_t dh[16],
                                std::vector<uint8_t>& wire, std::string& msg_id_hex)
{
    std::string obk = outboxKey(peer_hex, mid);
    auto it = g_wireOutbox.find(obk);
    if (it != g_wireOutbox.end()) {
        wire = it->second.wire;
        msg_id_hex = it->second.msg_id_hex;
        return true;
    }

    std::string title   = storageGetStr(msgPath(id.index, peer_hex, mid, "title").c_str(),   "");
    std::string content = storageGetStr(msgPath(id.index, peer_hex, mid, "content").c_str(), "");
    std::string reply_to = storageGetStr(msgPath(id.index, peer_hex, mid, "reply_to").c_str(), "");

    LxmFields fields;
    fields.reply_to    = reply_to;
    fields.reply_quote = storageGetStr(msgPath(id.index, peer_hex, mid, "reply_quote").c_str(), "");

    /* Outbound stamp: pay the recipient's advertised proof-of-work cost,
     * but only when generation is enabled and they actually advertise a
     * cost > 0. Unknown/zero cost → no stamp, no PoW delay (common case).
     * A cost above what we're willing to grind (LXMF_STAMP_MAX_COST) is
     * refused — send unstamped rather than freeze the task for minutes. */
    int stamp_cost = 0;
    if (storageGetInt("s.lxmf.generate_stamps", 1) != 0) {
        AnnounceEntry ae;
        if (readAnnounce(peer_hex, &ae) && ae.cost > 0) {
            if (ae.cost > LXMF_STAMP_MAX_COST)
                warn("id %d: peer stamp cost %d > max %d — sending unstamped",
                     id.index, ae.cost, LXMF_STAMP_MAX_COST);
            else
                stamp_cost = ae.cost;
        }
    }

    /* The message's timestamp is WHEN IT WAS WRITTEN, and it is stamped once:
     * a record that already carries one keeps it. A proxied account's draft was
     * written on the owner's device and reproduced here, possibly hours before
     * this box gets a path to send it; a composer writes the moment the user
     * pressed send. Re-stamping at pack time would date the message to the
     * transmission and hand a re-pack after a reboot a different `message_id`
     * for the same message. A record with no timestamp is stamped now, in
     * whole seconds: the record keeps seconds, and the re-pack reads them back,
     * so a fraction here would give the same message two ids. */
    uint64_t ts_ms = (uint64_t)storageGetInt(msgPath(id.index, peer_hex, mid, "ts").c_str(), 0)
                     * 1000ull;
    if (!ts_ms) ts_ms = wallUnixMs() / 1000ull * 1000ull;

    /* Pack the LXM wire so the opportunistic-vs-DIRECT decision keys off
     * the *actual* packed size, not a content estimate that under-counts
     * the signature, fields/reply_to, stamp and msgpack framing. */
    uint8_t mid_raw[RNSD_HASH_LEN];
    wire = lxmPackWire(id.identity_key.c_str(), id.dest_hash, dh,
                       ts_ms, title, content, fields, stamp_cost, mid_raw);
    if (wire.empty()) {
        err("id %d: msg %s pack/sign failed", id.index, mid.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_PACK_FAIL);
        return false;
    }
    msg_id_hex = bytesToHex(mid_raw, RNSD_HASH_LEN);
    g_wireOutbox[obk] = { wire, msg_id_hex, ts_ms };

    /* Persist firmware-owned fields — the wire itself stays in the RAM outbox,
     * never the store. The conversation directory is bumped once per message:
     * a re-pack after a reboot (the RAM outbox is empty, the record already has
     * a message_id) is the same message, not a second one. */
    bool first = storageGetStr(msgPath(id.index, peer_hex, mid, "message_id").c_str(), "").empty();
    storageBegin();
    storageSet(msgPath(id.index, peer_hex, mid, "message_id").c_str(), msg_id_hex.c_str());
    storageSet(msgPath(id.index, peer_hex, mid, "ts").c_str(),         (int)(ts_ms / 1000));
    storageSet(msgPath(id.index, peer_hex, mid, "status").c_str(), (int)LXMF_ST_QUEUED);
    if (first) {
        int recv = bumpConvDirectory(id.index, peer_hex, (int)(ts_ms / 1000),
                          storageGetStr(msgPath(id.index, peer_hex, mid, "content").c_str()),
                          /*inbound=*/false);
        storageSet(msgPath(id.index, peer_hex, mid, "recv_ts").c_str(), recv);
    }
    storageEnd();
    return true;
}

/* `retry` = this is a sweep's attempt, not the first one or an opportunistic
 * re-send. It changes exactly one thing: the message rides a Link (see the
 * method block below). */
static void processReady(lxmf_id_t& id, const std::string& peer_hex,
                         const std::string& mid, bool retry)
{
    /* peer arrives from the cmd.send sentinel (<peer>/<key>) — it *is*
     * the record's path segment, so it is authoritative, not read back
     * from storage. On a malformed peer we still write the failed stage
     * under whatever path the client used, so the client sees it. */
    uint8_t dh[16];
    if (peer_hex.size() != 32 || !hexToDestHash(peer_hex, dh)) {
        warn("id %d: msg %s ready but peer is malformed (\"%s\")",
             id.index, mid.c_str(), peer_hex.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_BAD_PEER);
        return;
    }

    /* Valid peer → back this conversation with its own file before we write
       the record (a browser-composed draft may already be in root.json; the
       next flush detaches it into this file). */
    ensureConvFile(id.index, peer_hex);

    /* Stub the contact on first outbound too, mirroring the inbound persist:
     * messaging a peer we've only heard announce (never received from) must
     * still land their name in `contacts.<peer>.display_name`, else every
     * frontend shows a bare hash for someone we just picked off the mesh. */
    if (!storageExists(contactPath(id.index, peer_hex, "hash").c_str())) {
        std::string nm = bestHeardName(peer_hex);
        storageBegin();
        storageSet(contactPath(id.index, peer_hex, "hash").c_str(),  peer_hex.c_str());
        storageSet(contactPath(id.index, peer_hex, "trust").c_str(), 0);
        if (!nm.empty())
            storageSet(contactPath(id.index, peer_hex, "display_name").c_str(), nm.c_str());
        storageEnd();
    }

    if (!idEnabled(id.index)) {
        warn("id %d: msg %s not sent (identity disabled)", id.index, mid.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_DISABLED);
        return;
    }

    /* A proxied identity has no destination of its own, so ordinarily it has
     * nothing to send FROM: the server does the sending, and the draft goes
     * over the Channel. With no Channel the message sits QUEUED locally and
     * shows no checkmark — a machine that is not mine does not have it yet.
     *
     * The exception is a conversation link that is already open to this peer.
     * A Link is dialled with the account's identity and carries its own
     * traffic; none of it goes through the registration the proxy holds. So
     * when there is one, this device can hand the message straight to the
     * recipient — fewer hops, no copy on somebody else's box, and a real
     * delivery proof of our own. Return traffic the peer does not put on that
     * same link still arrives by way of the proxy, which is the address the
     * world knows. */
    bool viaLink = false;
    if (proxyIsClient(id.index)) {
        viaLink = convGet(id, peer_hex, dh, /*open_if_missing=*/false) != nullptr;
        if (!viaLink) {
            if (proxyReady(id.index)) {
                proxySend(id, peer_hex, mid);
            } else {
                msgSetStatus(id.index, peer_hex, mid, LXMF_ST_QUEUED);
                queueAdd(id.index, peer_hex, mid);
            }
            return;
        }
    }

    /* The identity can be loaded (dest hash published, history visible) before
     * its delivery dest is connected — the post-reset window while rnsd comes
     * up. A send can't be transmitted without a live handle; fail it cleanly
     * rather than reach into rnsd with an unconnected dest. The UIs gate send
     * on `ready`, so this is a backstop for the CLI / a race.
     *
     * Not for a send riding a link: that route never touches `id.handle` —
     * the packet goes out on the link's own ITS handle and settles on the
     * link's delivery-proof counters — and a proxied identity has no handle to
     * have, permanently and by design. */
    if (id.handle < 0 && !viaLink) {
        warn("id %d: msg %s not sent (mailbox not up yet)", id.index, mid.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_MAILBOX_STARTING);
        return;
    }

    /* Resolve the packed wire: reuse the RAM outbox on a resend (no re-pack, no
     * multi-second re-stamp), else pack from the stored title/content, cache it,
     * and persist the one-time record fields + directory bump (shared with the
     * propagation-node upload path). */
    std::vector<uint8_t> wire;
    std::string msg_id_hex;
    if (!resolveOutboundWire(id, peer_hex, mid, dh, wire, msg_id_hex))
        return;

    /* A peer that is another identity on THIS device never goes near the
     * network. Reticulum keeps no path to its own destinations, so rnsd would
     * park the send on a path request for an address inside this very box and
     * it would sit REQUESTING_PATH until the delivery timeout. Hand the packed
     * wire — signed and stamped exactly as it would go on the air — to the
     * recipient slot's own inbound pipeline, which verifies, dedups and stores
     * it like any other message.
     *
     * DELIVERED here is stronger than a proof, not weaker: the message is in
     * the recipient's store on this device, which is the thing a proof is
     * evidence OF. A proxy server makes this ordinary — it hosts somebody's
     * account beside its operator's own identity, and the operator messaging
     * the person whose mail they hold is the obvious thing to try. */
    for (int k = 0; k < LXMF_MAX_IDENTITIES; ++k) {
        if (k == id.index || !s_ids[k].used) continue;
        if (memcmp(s_ids[k].dest_hash, dh, LXMF_DEST_HASH_LEN) != 0) continue;
        onInboundLxm(s_ids[k], wire.data(), wire.size());
        msgSetStatus(id.index, peer_hex, mid, LXMF_ST_DELIVERED);
        id.sent++;
        info("id %d: msg %s delivered locally to id %d (%s)",
             id.index, mid.c_str(), k, peer_hex.c_str());
        return;
    }

    /* Delivery-method selection. Resolution order is per-message override
     * → per-identity default → global default → "link-always"
     * (resolveMethod). The four methods form a spectrum of link eagerness
     * (see canonMethod): link-always always uses a Link; link-if-one-exists
     * rides a warm link to this peer if one exists (our own conversation
     * link), else opportunistic; link-if-big goes opportunistic for anything
     * that fits one packet and only opens a Link for an oversize wire;
     * opportunistic-or-fail never uses a Link. Oversize forces a Link in
     * every mode except opportunistic-or-fail, which hard-fails instead — the
     * only mode that can fail on size.
     *
     * A RETRY — a sweep's attempt — forces a Link on the same terms. An
     * unproven opportunistic packet does not come here as a retry: it is
     * re-sent as the same packet first (oppProofMissed), because a lost proof
     * looks exactly like a lost message and a packet is cheaper than a Link.
     * Once those re-sends have gone unanswered too, the sweep's Link asks
     * whether the peer is reachable ONCE and then carries every message
     * waiting for them. opportunistic-or-fail is the exception here as it is
     * for oversize: it is an explicit instruction never to open a Link, and a
     * retry does not overrule it.
     *
     * Oversize is measured on the real opportunistic payload — the wire
     * minus the dest16 that rnsd strips and re-derives — against the RNS
     * single-packet plaintext ceiling. Get this wrong on the low side and
     * an over-MTU packet reaches rnsd, where Packet::pack() throws and
     * surfaces as a spurious "evicted (resource limit)". */
    bool oversize = (wire.size() - LXMF_DEST_HASH_LEN) > LXMF_OPP_PAYLOAD_MAX;
    std::string method = resolveMethod(id.index, peer_hex, mid);

    bool use_direct;
    if (viaLink) {
        /* Proxied, and only here because a link is open. The link is the whole
         * reason this send is not going to the server, and the opportunistic
         * route is not available anyway — it goes through the registration the
         * proxy holds. So the method has nothing left to choose. */
        use_direct = true;
    } else if (method == "opportunistic-or-fail") {
        if (oversize) {
            warn("id %d: msg %s exceeds opportunistic budget (wire %zu B)",
                 id.index, mid.c_str(), wire.size());
            msgFail(id.index, peer_hex, mid, LXMF_ST_TOO_LARGE);
            return;
        }
        use_direct = false;
    } else if (oversize || retry) {
        use_direct = true;
    } else if (method == "link-always") {
        use_direct = true;
    } else if (method == "link-if-big") {
        use_direct = false;                   /* oversize is already handled above */
    } else {                                  /* link-if-one-exists */
        /* Prefer the Link when our own conversation link to this peer is
         * already warm — an active chat rides a link for every message.
         * A peer's inbound link into us never counts: not every client
         * accepts our outgoing traffic on the link it opened, so we only
         * ride links we opened ourselves. */
        use_direct = convFind(id.index, peer_hex) != nullptr;
    }

    /* Reserve an outbox slot. None free is a full moment, not a failure: the
     * message waits in the queue for the next sweep. */
    outbound_t* o = outboundAlloc(id);
    if (!o) {
        warn("id %d: outbox full — %s waits for the next sweep", id.index, mid.c_str());
        msgSetStatus(id.index, peer_hex, mid, LXMF_ST_QUEUED);
        queueAdd(id.index, peer_hex, mid);
        return;
    }
    o->used    = true;
    o->send_id = id.next_send_id++;
    if (id.next_send_id == 0) id.next_send_id = 1;
    o->peer    = peer_hex;
    o->msg_key = mid;
    o->direct  = false;            /* reset stale DIRECT state on reuse */
    o->link_handle = -1;
    o->link_tag.clear();
    o->direct_deadline_s = 0;
    o->is_resource = false;
    o->awaiting_proof      = false;
    o->proof_deadline_s    = 0;
    o->proof_base_proven   = 0;
    o->proof_base_timeouts = 0;
    o->link_wire.clear();
    o->link_resent         = false;
    o->path_reqs           = 0;
    o->path_deadline_s     = 0;
    o->started_s           = (uint32_t)(nowUnixMs() / 1000);
    o->sent_s              = 0;

    if (use_direct) {
        /* DIRECT: send the *full* LXM wire (incl. the 16-byte dest hash)
         * over the peer's conversation Link — opened on first use, then
         * kept and reused for the whole chat (see convGet above). One
         * Link packet for small wires — upstream `LXMessage.__as_packet`
         * DIRECT sends `self.packed` whole; only OPPORTUNISTIC strips
         * dest16 (LXMessage.py:630-632). rnsd's pre-active one-packet
         * outbox buffers it and flushes on establishment; the 1 Hz tick
         * watches rnsd.links.<tag>.state (RNSD_PORT_LINK has no
         * OUT_RESULT). */
        /* Always ride a conversation link WE opened — reuse the warm one
         * if it exists, else open a fresh one. We never send over a peer's
         * inbound link into us: not every client accepts that. */
        convlink_t* cl = convGet(id, peer_hex, dh);
        if (!cl) {
            /* No link to be had right now — the pool is full of busy links,
             * or rnsd refused the open. Try again at the next sweep. */
            o->used = false;
            queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
            return;
        }
        const std::string ltag    = cl->tag;
        const int         lhandle = cl->handle;
        if (linkTagBusy(ltag)) {
            /* An earlier send to this peer is still settling on the shared
             * link — wait for the next sweep; the status stays "queued". */
            o->used = false;
            queueAdd(id.index, peer_hex, mid);
            verb("id %d: msg %s queued (link %s busy)",
                 id.index, mid.c_str(), ltag.c_str());
            return;
        }
        /* One Link packet carries ~Link ENCRYPTED_MDU of content; a
         * larger LXM must ride a Resource (upstream LXMessage).
         * Conservative threshold well under Link MDU. */
        constexpr size_t LXMF_LINK_PACKET_MAX = 360;
        bool as_resource = wire.size() > LXMF_LINK_PACKET_MAX;
        if (as_resource) {
            void* rbuf = gp_alloc(wire.size());
            if (!rbuf) {
                o->used = false;
                msgFail(id.index, peer_hex, mid, LXMF_ST_RES_MALLOC);
                return;
            }
            memcpy(rbuf, wire.data(), wire.size());
            /* rnsd takes ownership of rbuf and frees it after the engine
             * copies it. opaque_id = send_id for OUTBOUND_DONE matching. */
            if (!rnsdLinkSendResource(ltag.c_str(), rbuf, wire.size(), o->send_id)) {
                convDrop(*cl);
                o->used = false;
                msgFail(id.index, peer_hex, mid, LXMF_ST_RES_SEND);
                return;
            }
        } else {
            /* Full wire, one Link packet (no strip — DIRECT keeps dest16). A
             * refused send is the link's ITS buffer backed up: drop the link
             * and try again at the next sweep. */
            if (itsSend(lhandle, wire.data(), wire.size(), 0) == 0) {
                convDrop(*cl);
                o->used = false;
                queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
                return;
            }
        }
        cl->last_used_s = (uint32_t)(nowUnixMs() / 1000);
        /* Carried by a link of our own rather than by whatever holds our
         * address. Recorded on the message, so the bubble can say so. */
        storageSet(msgPath(id.index, peer_hex, mid, "via_link").c_str(), 1);
        o->direct            = true;
        o->is_resource       = as_resource;
        o->link_handle       = lhandle;
        o->link_tag          = ltag;
        /* Outlast rnsd's own establishment budget rather than guess at it. rnsd
         * scales that budget with the next hop's interface speed and the hop
         * count — 66 s on a TCP route, 79-91 s over LoRa — and publishes it at
         * kickoff. A flat constant here is shorter than all of them on a slow
         * interface, and the send is taken back from a link that was still
         * inside its own deadline, so the establishment never gets to finish.
         * The key is absent until rnsd has processed the open, so the constant
         * stays as the floor for that window and for a link already active. */
        {
            uint32_t estab = (uint32_t)storageGetInt(
                ("rnsd.links." + ltag + ".estab_timeout_s").c_str(), 0);
            uint32_t budget = as_resource ? 120u : 45u;
            /* The link may first have to wait for a path, on rnsd's path
             * budget, before its establishment budget even starts. */
            uint32_t rnsd_budget = (uint32_t)rnsdPathBudgetS() + estab;
            if (rnsd_budget > budget) budget = rnsd_budget;
            o->direct_deadline_s = (uint32_t)(nowUnixMs() / 1000) + budget;
        }
        /* Packet-class sends: baseline the link's delivery-proof counters
         * now so resolveDirectSends can settle "delivered" on the first
         * increment after the send (the link is serialized — only this
         * send can move them). The keys may not exist yet (pre-active
         * link); they default to 0 on both sides. */
        if (!as_resource) {
            std::string base = "rnsd.links." + ltag;
            o->proof_base_proven =
                storageGetInt((base + ".tx_proven").c_str(), 0);
            o->proof_base_timeouts =
                storageGetInt((base + ".proof_timeouts").c_str(), 0);
            o->link_wire.assign(wire.begin(), wire.end());
        }
        storageBegin();
        storageSet(msgPath(id.index, peer_hex, mid, "method").c_str(),     "direct");
        storageSet(msgPath(id.index, peer_hex, mid, "status").c_str(), (int)LXMF_ST_SENDING);
        storageEnd();
        id.pending++;
        info("id %d: send DIRECT mid=%s peer=%s tag=%s wire=%zuB",
             id.index, mid.c_str(), peer_hex.c_str(), ltag.c_str(),
             wire.size());
        return;
    }

    /* OUT_PACKET frame: op | send_id(2) | lxm_wire_bytes */
    std::vector<uint8_t> frame;
    frame.reserve(3 + wire.size());
    frame.push_back(RNSD_DEST_OUT_PACKET);
    frame.push_back((uint8_t)(o->send_id >> 8));
    frame.push_back((uint8_t)(o->send_id & 0xFF));
    frame.insert(frame.end(), wire.begin(), wire.end());

    if (!sendFrame(id, frame.data(), frame.size())) {
        /* rnsd's mailbox connection is backed up — transient; next sweep. */
        o->used = false;
        queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_DELIVERY);
        return;
    }
    storageBegin();
    storageSet(msgPath(id.index, peer_hex, mid, "status").c_str(), (int)LXMF_ST_SENDING);
    storageEnd();
    id.pending++;
    info("id %d: send mid=%s peer=%s send_id=%u wire=%zuB",
         id.index, mid.c_str(), peer_hex.c_str(),
         (unsigned)o->send_id, wire.size());
}

/* ─────────────── inbound: verify + dedup + store ─────────────── */

/* Play the message-notification sound on a genuinely-new inbound message.
 * Gated on s.lxmf.sound_enabled; the path (s.lxmf.sound) defaults to the
 * bundled /fixed/lxmf/ding.wav but the user can point it elsewhere. No-op
 * unless the optional spangap/audio engine is staged. */
static void lxmfNotifySound()
{
#if CONFIG_STRADDLE_AUDIO
    if (storageGetInt("s.lxmf.sound_enabled", 1) == 0) return;
    std::string p = storageGetStr("s.lxmf.sound", FS_FIXED "/lxmf/ding.wav");
    if (p.empty()) return;
    /* Half volume when the LCD is present AND awake (the user is right there);
     * full volume otherwise — asleep, or a headless node that relies on the ding
     * to get attention. `sys.standby` exists only on a device with an LCD, and
     * the board flips it 0/1 around wake/sleep. */
    bool lcdAwake = storageExists("sys.standby") && storageGetInt("sys.standby", 0) == 0;
    audioPlayWav(p.c_str(), lcdAwake ? 50 : 100);
#endif
}

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

static void onInboundLxm(lxmf_id_t& id, const uint8_t* wire, size_t n, bool via_link)
{
    if (!idEnabled(id.index)) {
        dbg("id %d: inbound LXM dropped (identity disabled)", id.index);
        return;
    }
    /* A Ping probe: destination hash and sender hash, nothing after them. It
     * carries no message and is never meant to parse as one — rnsd has already
     * proved it on hand-off, which is the entire answer the prober wanted. Named
     * here so a peer being probed logs one legible line instead of a malformed-
     * wire warning per press. */
    if (n == 2 * LXMF_DEST_HASH_LEN) {
        verb("id %d: ping probe from %s", id.index,
             bytesToHex(wire + LXMF_DEST_HASH_LEN, LXMF_DEST_HASH_LEN).c_str());
        return;
    }
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
     * announces). A contact we have heard before answers this without the
     * network: the key is stored with the contact, and verification wants
     * nothing else. Seed it back so every other consumer finds it too.
     *
     * With no key from either, buffer the wire and request a path: the
     * path request makes the sender re-announce, and drainPendingVerify
     * replays this message once their announce lands. Opportunistic
     * LXMF has no retransmission, so dropping here loses the message. */
    uint8_t sender_pubkey[RNSD_PUBKEY_LEN];
    if (!rnsdRecallPubkey(sh, sender_pubkey)) {
        std::string peer_hex = bytesToHex(sh, LXMF_DEST_HASH_LEN);
        if (contactGetPubkey(id.index, peer_hex, sender_pubkey)) {
            rnsdSeedPubkey(sh, sender_pubkey);
            verb("id %d: inbound LXM from %s — key from the address book",
                 id.index, peer_hex.c_str());
        } else {
            warn("id %d: inbound LXM from unknown sender %s — buffering, issuing path request",
                 id.index, peer_hex.c_str());
            uint64_t now_ms = nowUnixMs();
            auto& q = id.pending_verify;
            q.erase(std::remove_if(q.begin(), q.end(),
                        [&](const pending_verify_t& e) {
                            return now_ms - e.enqueued_ms > LXMF_PENDING_VERIFY_TTL_MS;
                        }),
                    q.end());
            while (q.size() >= LXMF_MAX_PENDING_VERIFY) q.erase(q.begin());
            pending_verify_t e;
            std::memcpy(e.sender, sh, LXMF_DEST_HASH_LEN);
            e.wire.assign(wire, wire + n);
            e.enqueued_ms = now_ms;
            q.push_back(std::move(e));
            rnsdRequestPath(sh);
            return;
        }
    }

    /* A stamped message carries the PoW as payload element [4], appended
     * AFTER signing. Recover the 4-element payload the sender actually
     * hashed/signed (and the stamp, if any) and verify against that. */
    std::vector<uint8_t> hashed_scratch, stamp_bytes;
    const uint8_t* hashed   = packed;
    size_t         hashed_n = packed_n;
    lxmSplitStamp(packed, packed_n, hashed_scratch, &hashed, &hashed_n, stamp_bytes);

    /* Signature over dest || src || packed4 || SHA-256(...). The inner
     * SHA-256 IS the message_id. */
    std::vector<uint8_t> signable;
    signable.reserve(LXMF_DEST_HASH_LEN * 2 + hashed_n + RNSD_HASH_LEN);
    signable.insert(signable.end(), dh,     dh     + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), sh,     sh     + LXMF_DEST_HASH_LEN);
    signable.insert(signable.end(), hashed, hashed + hashed_n);
    uint8_t mid_hash[RNSD_HASH_LEN];
    rnsdSha256(signable.data(), signable.size(), mid_hash);
    signable.insert(signable.end(), mid_hash, mid_hash + RNSD_HASH_LEN);

    if (!rnsdVerify(sender_pubkey, signable.data(), signable.size(), sig)) {
        warn("id %d: inbound LXM signature invalid (from=%s)",
             id.index, bytesToHex(sh, LXMF_DEST_HASH_LEN).c_str());
        return;
    }

    std::string mid_hex = bytesToHex(mid_hash, RNSD_HASH_LEN);
    std::string sh_hex  = bytesToHex(sh, LXMF_DEST_HASH_LEN);  /* peer = conversation subtree */

    if (dedupSeen(mid_hex)) {
        verb("id %d: inbound LXM dup (mid=%s)", id.index, mid_hex.c_str());
        return;
    }
    dedupAdd(mid_hex);

    /* Storage existence is the authoritative dedup. */
    std::string stage_key = msgPath(id.index, sh_hex, mid_hex, "status");
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

    /* Stamp handling. When enforcing (s.lxmf.enforce_stamps), verify the
     * PoW against the cost we advertise (s.lxmf.stamp_cost), log the
     * result, and drop on failure. Validation rebuilds the 768 KB
     * workblock, so when NOT enforcing we skip that work entirely and
     * just log whether a stamp rode along. Either path runs only here,
     * for novel messages, after dedup. */
    if (storageGetInt("s.lxmf.enforce_stamps", 0) != 0) {
        int required = storageGetInt("s.lxmf.stamp_cost", 8);
        if (required > 0) {
            if (lxmfStampValid(mid_hash, required,
                               stamp_bytes.data(), stamp_bytes.size(),
                               stampYield, nowUnixMs)) {
                info("id %d: inbound stamp valid (cost>=%d, %zuB) from=%s mid=%s",
                     id.index, required, stamp_bytes.size(),
                     sh_hex.c_str(), mid_hex.c_str());
            } else {
                warn("id %d: inbound LXM dropped — stamp %s (require cost %d) from=%s mid=%s",
                     id.index, stamp_bytes.empty() ? "absent" : "below cost",
                     required, sh_hex.c_str(), mid_hex.c_str());
                return;
            }
        }
    } else if (!stamp_bytes.empty()) {
        info("id %d: inbound stamp present (%zuB, unverified) from=%s mid=%s",
             id.index, stamp_bytes.size(), sh_hex.c_str(), mid_hex.c_str());
    }

    /* Log any LXMF ticket the sender handed us. We don't cache or use it
     * yet (no stamp-exemption path), so it's visibility-only for now. */
    if (!fields.ticket.empty()) {
        size_t pfx = fields.ticket.size() < 16 ? fields.ticket.size() : 16;
        info("id %d: inbound ticket %zuB [%s%s] from=%s mid=%s (not stored)",
             id.index, fields.ticket.size(),
             bytesToHex((const uint8_t*)fields.ticket.data(), pfx).c_str(),
             fields.ticket.size() > pfx ? "…" : "",
             sh_hex.c_str(), mid_hex.c_str());
    }

    /* Persist. */
    ensureConvFile(id.index, sh_hex);
    storageBegin();
    storageSet(stage_key.c_str(), (int)LXMF_ST_RECEIVED);
    storageSet(msgPath(id.index, sh_hex, mid_hex, "dir").c_str(),        "in");
    storageSet(msgPath(id.index, sh_hex, mid_hex, "peer").c_str(),       sh_hex.c_str());
    storageSet(msgPath(id.index, sh_hex, mid_hex, "title").c_str(),      title.c_str());
    storageSet(msgPath(id.index, sh_hex, mid_hex, "content").c_str(),    content.c_str());
    /* The body's length, recorded whether or not the body is where it is being
     * read from — a proxy server hands this on so the owner's device can offer
     * a download without fetching the body first. */
    storageSet(msgPath(id.index, sh_hex, mid_hex, "body_size").c_str(), (int)content.size());
    if (!fields.reply_to.empty())
        storageSet(msgPath(id.index, sh_hex, mid_hex, "reply_to").c_str(), fields.reply_to.c_str());
    if (!fields.reply_to.empty() && !fields.reply_quote.empty())
        storageSet(msgPath(id.index, sh_hex, mid_hex, "reply_quote").c_str(),
                   fields.reply_quote.c_str());
    storageSet(msgPath(id.index, sh_hex, mid_hex, "ts").c_str(),         (int)(ts / 1000));
    storageSet(msgPath(id.index, sh_hex, mid_hex, "message_id").c_str(), mid_hex.c_str());
    /* Arrived on a conversation link rather than by way of whatever holds our
     * address. Written only when true: an inbound that came the ordinary way
     * leaves the field at its default, and the frontends draw nothing. */
    if (via_link)
        storageSet(msgPath(id.index, sh_hex, mid_hex, "via_link").c_str(), 1);
    /* Stub contact if new — copy display_name across from the cross-
     * identity announce catalogue if we've heard them announce. */
    if (!storageExists(contactPath(id.index, sh_hex, "hash").c_str())) {
        storageSet(contactPath(id.index, sh_hex, "hash").c_str(),  sh_hex.c_str());
        storageSet(contactPath(id.index, sh_hex, "trust").c_str(), 0);
        std::string peer_name = bestHeardName(sh_hex);
        if (!peer_name.empty())
            storageSet(contactPath(id.index, sh_hex, "display_name").c_str(), peer_name.c_str());
    }
    /* Floor to the whole minute and write only when it advances: contacts then
     * all age in lockstep, and a burst of messages within a minute doesn't
     * re-trigger the on-device UI rebuild. */
    setIntIfChanged(contactPath(id.index, sh_hex, "last_seen"),
                    (int)(nowUnixMs() / 60000) * 60);
    int recv = bumpConvDirectory(id.index, sh_hex, (int)(ts / 1000), content, /*inbound=*/true);
    storageSet(msgPath(id.index, sh_hex, mid_hex, "recv_ts").c_str(), recv);
    storageEnd();

    /* bumpConvDirectory has materialised the contact, so claim it: from here on
     * their directory record is protected like any other contact's — and keep
     * the key that just verified this message, which is the same key an
     * announce would have brought and may be the only copy we get. */
    lxmfClaimContact(sh_hex);
    contactSetPubkey(id.index, sh_hex, sender_pubkey);

    id.received++;
    info("id %d: recv mid=%s from=%s len=%zuB title=\"%s\"",
         id.index, mid_hex.c_str(), sh_hex.c_str(), n,
         sanitizeForLog(title).c_str());

    lxmfNotifySound();

    /* Breather: this task runs on core 0 (prio 1) alongside the rnsd transport
     * that feeds it, at the same priority. A burst of inbound messages
     * processed back-to-back monopolises the core against the idle task (WDT)
     * and its peers (rnsd, cron). Yield so they get a slice between messages. */
    stampYield();
}

/* A sender's lxmf.delivery announce just arrived (rnsd cached their
 * pubkey). Replay every buffered message from that sender through the
 * normal pipeline — now the rnsdRecallPubkey gate passes, so they
 * verify and persist. Called from onAnnounceFromRnsd, lxmf task only. */
static void drainPendingVerify(lxmf_id_t& id, const uint8_t* sender_hash)
{
    if (id.pending_verify.empty()) return;

    uint8_t pk[RNSD_PUBKEY_LEN];
    if (!rnsdRecallPubkey(sender_hash, pk)) return;  /* still unknown — keep buffered */

    std::vector<pending_verify_t> ready;
    for (auto it = id.pending_verify.begin(); it != id.pending_verify.end(); ) {
        if (std::memcmp(it->sender, sender_hash, LXMF_DEST_HASH_LEN) == 0) {
            ready.push_back(std::move(*it));
            it = id.pending_verify.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& e : ready) {
        info("id %d: sender %s now known — replaying buffered LXM (%zuB)",
             id.index, bytesToHex(sender_hash, LXMF_DEST_HASH_LEN).c_str(),
             e.wire.size());
        onInboundLxm(id, e.wire.data(), e.wire.size());
    }
}

/* Backstop sweep: try to drain every distinct buffered sender. Called
 * from the 1 Hz tick. The onAnnounceFromRnsd hook only fires drain on
 * the lxmf.delivery announce-fanout; an identity learned via a
 * path-request response (the common case here — we issue a path
 * request on the unknown-sender drop) never triggers it, so buffered
 * messages sat forever. This sweep replays them as soon as the sender
 * becomes recallable by ANY means. Bounded: <=LXMF_MAX_PENDING_VERIFY
 * entries per id, distinct-sender deduped. */
static void drainAllPendingVerify(lxmf_id_t& id)
{
    if (id.pending_verify.empty()) return;
    /* Snapshot distinct sender hashes first — drainPendingVerify mutates
     * id.pending_verify (erases drained entries). */
    std::vector<std::vector<uint8_t>> senders;
    for (auto& e : id.pending_verify) {
        std::vector<uint8_t> s(e.sender, e.sender + LXMF_DEST_HASH_LEN);
        bool dup = false;
        for (auto& x : senders) if (x == s) { dup = true; break; }
        if (!dup) senders.push_back(std::move(s));
    }
    for (auto& s : senders) drainPendingVerify(id, s.data());
}

/* ─────────────── Ping ───────────────
 *
 *   us ──probe packet (peer_dest | our_dest)──►  peer
 *   us ◄──────────── delivery proof ─────────── peer
 *
 * The contact page's reachability probe: `rnprobe lxmf.delivery <hash>` sent
 * from the identity's own our-dest connection, with our lxmf.delivery hash as
 * the plaintext so the far end sees which contact is asking. The probe measures
 * the round trip and hop count; the path loss it shows beside them is the
 * radio's own measurement of that peer, read from SUPE's per-peer publication
 * (lxmfPeerMeas) when the proof lands.
 *
 * The probe payload is the sixteen-byte source hash and nothing else. It is not
 * a valid LXM wire, so the peer's lxmf drops it after its rnsd has already
 * proved it — proving happens on hand-off, before any parsing. Sixteen bytes is
 * also as cheap as an encrypted single-destination packet gets; the envelope
 * floor dominates either way.
 *
 * Results land under `lxmf.ping.<peer>.*` (RAM, browser-mirrored by the plain
 * `lxmf.` key sync). `state` is the only field a UI must read: `probing` while
 * in flight, then a settled word. The rest are present only when measured. */

/* How long a probe waits before calling it. A probe with no link to ride first
 * needs a PATH, and that search runs on rnsd's budget (rnsdPathBudgetS) — so
 * the deadline is derived from it: a fixed one shorter than rnsd's gives up
 * while rnsd is still legitimately looking, and the first probe to any contact
 * whose path is not cached answers with nothing. The margin covers the link
 * handshake that follows the path, which is the only other thing a probe waits
 * on. */
#define LXMF_PING_HANDSHAKE_MARGIN_S 10
static uint32_t pingTimeoutS(void)
{
    int p = rnsdPathBudgetS();
    if (p < 5) p = 5;
    return (uint32_t)p + LXMF_PING_HANDSHAKE_MARGIN_S;
}

static std::string pingPath(const std::string& peer_hex, const char* field)
{
    return "lxmf.ping." + peer_hex + "." + field;
}

/* Clear every field of a peer's ping record, so a fresh probe never shows the
 * previous one's numbers next to its own `probing`. */
static void pingClear(const std::string& peer_hex)
{
    /* The probe's own findings, and nothing else. What the RADIO measured is not
     * copied here — it is read live from `lora.<n>.meas.*` wherever it is shown
     * (lxmfPingLink). A snapshot taken when the probe settled froze whatever
     * iface-lora happened to have published by then, and that publication runs
     * on a 15 s beat: a first probe settled a second after the link came up,
     * found nothing, and stored nothing, so the reading stayed missing until
     * somebody pinged again. Live, it appears the moment the beat publishes and
     * stays current afterwards. */
    static const char* kFields[] = { "state", "ts", "rtt_ms", "answer_ms", "hops" };
    storageBegin();
    for (const char* f : kFields) storageUnset(pingPath(peer_hex, f).c_str());
    storageEnd();
}

/* The radio's own measurement of a peer, from iface-lora's per-peer publication
 * `lora.<n>.meas.<slot>.*`: the record whose `tags` holds the first six hex
 * characters of the peer's destination hash.
 *
 * A direction is a path loss in dB with the reading it was measured from: the
 * frame's signal-to-noise (dB×10) and the power that frame went out at (dBm) —
 * ours for us→them, the peer's for them→us — and the unix second of the
 * reading. `loss_to` is us→them (the peer's report of how our frame landed),
 * `loss_from` is them→us, and each exists only where SUPE has measured it: a
 * loss is a level against a power the OTHER end stated, and nobody outside the
 * protocol states one. What there is for such a peer is what this radio itself
 * knows — the last level it read (`rssi`/`snr`) and the power it last sent at
 * (`txp`) — and those are always there. */
struct PeerMeas {
    bool have_to = false, have_from = false, have_sig = false, have_txp = false;
    int  loss_to = 0, snr_to = 0, txp_to = 0, to_ts = 0;
    int  loss_from = 0, snr_from = 0, peer_txp = 0, from_ts = 0;
    int  rssi = 0, snr10 = 0, txp = 0, heard_ts = 0;
};
static std::vector<std::string> collectTokens(const std::string& prefix);   /* fwd */
static bool lxmfPeerMeas(const std::string& peer_hex, PeerMeas& out)
{
    if (peer_hex.size() < 6) return false;
    std::string tag = peer_hex.substr(0, 6);
    for (int n = 0; n < 4; ++n) {
        std::string pre = "lora." + std::to_string(n) + ".meas.";
        for (const std::string& slot : collectTokens(pre)) {
            std::string base = pre + slot;
            std::string tags = storageGetStr((base + ".tags").c_str(), "");
            if (tags.find(tag) == std::string::npos) continue;
            out.have_to   = storageExists((base + ".loss_to").c_str());
            out.have_from = storageExists((base + ".loss_from").c_str());
            out.have_sig  = storageExists((base + ".rssi").c_str());
            out.loss_to   = storageGetInt((base + ".loss_to").c_str(),   0);
            out.snr_to    = storageGetInt((base + ".snr_to").c_str(),    0);
            out.txp_to    = storageGetInt((base + ".txp_to").c_str(),    0);
            out.to_ts     = storageGetInt((base + ".to_ts").c_str(),     0);
            out.loss_from = storageGetInt((base + ".loss_from").c_str(), 0);
            out.snr_from  = storageGetInt((base + ".snr_from").c_str(),  0);
            out.peer_txp  = storageGetInt((base + ".peer_txp").c_str(),  0);
            out.from_ts   = storageGetInt((base + ".from_ts").c_str(),   0);
            out.have_txp  = storageExists((base + ".txp").c_str());
            out.rssi      = storageGetInt((base + ".rssi").c_str(),      0);
            out.snr10     = storageGetInt((base + ".snr").c_str(),       0);
            out.txp       = storageGetInt((base + ".txp").c_str(),       0);
            out.heard_ts  = storageGetInt((base + ".heard_ts").c_str(),  0);
            return true;
        }
    }
    return false;
}

/* The radio's reading of the link to a peer, as a person reads it: one line per
 * direction the radio has measured, joined by `sep`. Empty when it has measured
 * nothing — no radio has heard this peer, or the probe went out over something
 * that is not a radio at all.
 *
 *     us->them 91 dB path loss, SNR 11 dB @ tx +10 dBm (10 mW)
 *     them->us 88 dB path loss, SNR 9 dB @ tx +22 dBm (158 mW)
 *
 * The path loss leads because it is the link's own property whatever either end
 * transmits at; the signal-to-noise says whether the link is weak or merely
 * quiet, and the power is what the loss was measured against — in watts beside
 * the dBm, since that is the half a reader can feel.
 *
 * **A loss needs SUPE, and without it there is no loss to print.** A path loss
 * is a level measured against the power the FAR END transmitted at, and only a
 * SUPE peer states that power. What this radio has on its own is what it read
 * and what it sent at, and that is the whole line for such a peer:
 *
 *     heard @ -95 dBm / SNR 9.5 dB @ tx +22 dBm (158 mW)
 *
 * No ages. These are the readings, and they are read LIVE from
 * `lora.<n>.meas.*` — the probe does not snapshot them. A copy taken when a
 * probe settled was stale the next time the radio heard the peer, and on a
 * first probe it was empty: iface-lora republishes on a 15 s beat, so a link
 * that came up a second ago has not been published yet, and the line stayed
 * missing until somebody pinged a second time.
 *
 * Built here rather than in each frontend so the contact page and the console
 * cannot drift into describing the same measurement two different ways. */
std::string lxmfPingLink(const std::string& peer_hex, const char* sep)
{
    PeerMeas m;
    if (!lxmfPeerMeas(peer_hex, m)) return "";
    auto dirLine = [&](const char* dir, int loss, int snr10, int txp) {
        char pw[16], b[128];
        snprintf(b, sizeof b, "%s %d dB path loss, SNR %.0f dB @ tx %+d dBm (%s)",
                 dir, loss, snr10 / 10.0, txp, fmtPower(txp, pw, sizeof pw));
        return std::string(b);
    };

    std::string out;
    if (m.have_to)
        out = dirLine("us->them", m.loss_to, m.snr_to, m.txp_to);
    if (m.have_from) {
        if (!out.empty()) out += sep;
        out += dirLine("them->us", m.loss_from, m.snr_from, m.peer_txp);
    }
    if (out.empty() && m.have_sig) {
        char b[112];
        int  n = snprintf(b, sizeof b, "heard @ %d dBm / SNR %.1f dB",
                          m.rssi, m.snr10 / 10.0);
        if (m.have_txp && n > 0 && n < (int)sizeof b) {
            char pw[16];
            snprintf(b + n, sizeof b - n, " @ tx %+d dBm (%s)",
                     m.txp, fmtPower(m.txp, pw, sizeof pw));
        }
        out = b;
    }
    return out;
}

/* Settle a ping: write the outcome word, how long the answer took, and the
 * round trip and hop count where something MEASURED one, then free the slot.
 *
 * **A round trip is a measurement, not an elapsed time.** `rtt_ms` is written
 * only from µR's own timing of the exchange the probe rode — the link
 * establishment (`rnsd.links.<tag>.rtt_ms`) or the packet proof
 * (`.tx_rtt_ms`) — and is absent when nothing measured one. What is always
 * there is `answer_ms`, the time from the press to this outcome: on a probe
 * that had to find a path first that is tens of seconds of path search, which
 * is worth showing and is not a round trip. Reported as one number, a contact
 * that merely took 30 s to find answers "rtt=30000 ms", which reads as a
 * catastrophic link rather than a cold path table. `hops` rides `rtt_ms`,
 * since it describes the same measured exchange.
 *
 * The RADIO's reading of the link is not written here. It is published
 * continuously by iface-lora and owes the probe nothing, so every surface reads
 * it live (lxmfPingLink) rather than taking a copy at this instant — a copy is
 * wrong the moment the radio hears the peer again, and on a first probe it is
 * wrong immediately, because iface-lora republishes on a 15 s beat and a link
 * that came up a second ago has not been published yet. */
static void pingSettle(lxmf_id_t& id, const char* state,
                       uint32_t rtt_ms, int hops)
{
    if (!id.ping.used) return;
    std::string peer = id.ping.peer;
    uint32_t started = id.ping.started_ms;
    id.ping.used = false;
    /* Whatever carried this probe is done with. The link a dial opened is
     * closed by the caller that knows the outcome; this only forgets it, so a
     * later settle cannot act on a handle that has already gone. */
    id.ping.by_link = false;
    id.ping.link_tag.clear();
    id.ping.link_handle = -1;
    id.ping.started_ms = 0;

    uint32_t answer_ms = started ? (uint32_t)nowUnixMs() - started : 0;
    storageBegin();
    storageSet(pingPath(peer, "state").c_str(), state);
    storageSet(pingPath(peer, "ts").c_str(),    (int)(nowUnixMs() / 1000));
    if (started) storageSet(pingPath(peer, "answer_ms").c_str(), (int)answer_ms);
    if (hops >= 0) {
        storageSet(pingPath(peer, "rtt_ms").c_str(), (int)rtt_ms);
        storageSet(pingPath(peer, "hops").c_str(),   hops);
    }
    storageEnd();
    if (hops >= 0)
        info("id %d: ping %s → %s (rtt=%u ms, answered in %u ms)", id.index,
             peer.c_str(), state, (unsigned)rtt_ms, (unsigned)answer_ms);
    else
        info("id %d: ping %s → %s (answered in %u ms, nothing measured the "
             "round trip)", id.index, peer.c_str(), state, (unsigned)answer_ms);
}

/* Abandon the probe in flight without settling it: the user has asked for a
 * newer measurement of something, and a stale result landing under the old
 * peer would overwrite the one they are waiting for.
 *
 * **The link it dialled goes with it.** A probe's link is the probe: rnsd holds
 * a Link open for exactly as long as the consumer's ITS handle, and the tag it
 * is registered under is refused to a second opener while that handle lives
 * (`onLinkConnect`, "duplicate tag … still active"). Forgetting the handle
 * instead of closing it leaves a link keepaliving on the air with nobody to
 * answer for it AND — the tag naming the peer — fails every later probe of
 * that contact at the dial, until µR lets the orphan go stale minutes later.
 * Sending the contact a message would hide that, since a probe with a
 * conversation link to ride never dials at all. */
static void pingAbandon(lxmf_id_t& id)
{
    if (!id.ping.used) return;
    if (id.ping.link_handle >= 0) itsDisconnect(id.ping.link_handle);
    info("id %d: ping %s superseded", id.index, id.ping.peer.c_str());
    /* Say so under the peer we are walking away from, or its row sits on
     * `probing` for good — nothing will settle it now. A press on the same
     * contact clears this again a line later (pingClear), so only the contact
     * actually abandoned keeps the word. */
    storageBegin();
    storageSet(pingPath(id.ping.peer, "state").c_str(), "cancelled");
    storageSet(pingPath(id.ping.peer, "ts").c_str(), (int)(nowUnixMs() / 1000));
    storageEnd();
    id.ping.used = false;
    id.ping.by_link = false;
    id.ping.link_tag.clear();
    id.ping.link_handle = -1;
    id.ping.started_ms = 0;
}

/* Start a probe to `peer_hex`, superseding any ping already in flight for this
 * identity (its result is abandoned, not settled — the user asked for a newer
 * measurement and a stale OUT_RESULT would overwrite it). */
static void pingStart(lxmf_id_t& id, const std::string& peer_hex)
{
    uint8_t dh[LXMF_DEST_HASH_LEN];
    if (peer_hex.size() != 32 || !hexToDestHash(peer_hex, dh)) {
        warn("id %d: ping bad peer \"%s\"", id.index, peer_hex.c_str());
        return;
    }
    /* Supersede: one probe per identity is in flight, and a second press ends
     * the first outright — link and all. After the hash check above, because a
     * malformed argument is not a press and must not kill a running probe. */
    pingAbandon(id);

    /* No registration with rnsd for this account — it has not come up yet, or
     * this is a proxied identity whose registrant is the server's device. The
     * 1 Hz tick retries anyway; a press is exactly when it is worth one more. */
    if (id.handle < 0) connectOurDest(id);

    /* **A ping is a link when there is no link.**
     *
     * The probe used to be a bare packet to the peer's delivery destination,
     * answered by its delivery proof. That measures a round trip and nothing
     * else — and the round trip is the half of the answer people look at least.
     * What they read is the line under it: the path loss each way, which needs
     * a power the far end STATED and a report of what it heard from us. A
     * packet stating no power and asking for nothing produces neither, so a
     * contact that has never been messaged answers a press with zeros, and the
     * same press after one message answers properly — the message having done
     * the exchange the probe did not.
     *
     * Establishing a link is that exchange. The request goes out carrying this
     * radio's power, the far end proves it by accepting and reports what it
     * heard, µR measures the round trip itself and rnsd publishes it as
     * `rtt_ms`. One question, and the whole answer comes back. The link is
     * dropped again once it has: what was wanted was the measurement, not a
     * session.
     *
     * An existing conversation link is used as it stands instead (below): it
     * has already been paid for, its establishment already produced the
     * readings, and tearing down a link somebody is talking over to measure it
     * would be a strange way to answer a button. */
    {
        convlink_t* have = convGet(id, peer_hex, dh, /*open_if_missing=*/false);
        if (!have) {
            pingClear(peer_hex);
            id.ping.used  = true;
            id.ping.peer  = peer_hex;
            id.ping.by_link = true;
            /* No outbox send belongs to this probe. 0 is never a real send_id
             * (next_send_id skips it), so the OUT_RESULT/OUT_STATUS handlers
             * cannot mistake somebody else's result for this ping's. */
            id.ping.send_id = 0;
            id.ping.deadline_s = (uint32_t)(nowUnixMs() / 1000) + pingTimeoutS();
            id.ping.started_ms = (uint32_t)nowUnixMs();
            /* A tag no earlier probe can still be holding. rnsd refuses a tag
             * whose slot is live, and it keys the state tree this probe reads
             * back — so a probe wearing the last one's name is refused at the
             * dial if that one is somehow still up, and reads its leavings if
             * it is not. pingAbandon closes the predecessor either way; the
             * counter is belt and braces, and costs two characters. Two hex
             * digits wrap harmlessly: the collision it guards against is with
             * the probe just before, never with one 256 presses ago. Stays
             * inside rnsd's 23-char tag: 9 + 1 + 1 + 8 + 1 + 2 = 22. */
            char tag[32];
            std::snprintf(tag, sizeof tag, "lxmf.ping%d.%.8s.%02x", id.index,
                          peer_hex.c_str(), (unsigned)(id.ping_seq++ & 0xff));
            id.ping.link_tag = tag;
            storageBegin();
            storageSet(pingPath(peer_hex, "state").c_str(), "probing");
            storageSet(pingPath(peer_hex, "ts").c_str(), (int)(nowUnixMs() / 1000));
            storageEnd();
            int lh = rnsdLinkOpen(dh, "lxmf.delivery", id.identity_key.c_str(),
                                  tag, /*path_timeout_ms=*/0, /*link_timeout_ms=*/0,
                                  /*ref=*/-1, nullptr, nullptr);
            if (lh < 0) {
                info("id %d: ping %s — link dial refused", id.index, peer_hex.c_str());
                pingSettle(id, "failed", 0, -1);
                return;
            }
            id.ping.link_handle = lh;
            info("id %d: ping %s by link %s", id.index, peer_hex.c_str(), tag);
            return;
        }
        /* A link is open: probe ON it. The wire is the same 32 bytes the
         * packet probe sends, and the link's own delivery proof is the answer —
         * the same counters a direct message settles on. */
        pingClear(peer_hex);
        id.ping.used       = true;
        id.ping.peer       = peer_hex;
        id.ping.by_link    = true;
        id.ping.send_id    = 0;              /* as above: no outbox send is ours */
        id.ping.link_tag   = have->tag;
        /* Not ours to close: this link belongs to the conversation, and the
         * handle field is what says which links a probe may take down. Set
         * explicitly rather than left to carry over: a superseded dial's
         * handle sitting here has the probe hang up on the conversation it is
         * riding, and read that link's `active` as its own answer. */
        id.ping.link_handle = -1;
        id.ping.started_ms = (uint32_t)nowUnixMs();
        id.ping.deadline_s = (uint32_t)(nowUnixMs() / 1000) + pingTimeoutS();
        storageBegin();
        storageSet(pingPath(peer_hex, "state").c_str(), "probing");
        storageSet(pingPath(peer_hex, "ts").c_str(), (int)(nowUnixMs() / 1000));
        storageEnd();
        uint8_t probe[2 * LXMF_DEST_HASH_LEN];
        std::memcpy(probe,                      dh,           LXMF_DEST_HASH_LEN);
        std::memcpy(probe + LXMF_DEST_HASH_LEN, id.dest_hash, LXMF_DEST_HASH_LEN);
        std::string base = "rnsd.links." + have->tag;
        id.ping.proof_base_proven   = storageGetInt((base + ".tx_proven").c_str(), 0);
        id.ping.proof_base_timeouts = storageGetInt((base + ".proof_timeouts").c_str(), 0);
        if (itsSend(have->handle, probe, sizeof probe, 0) == 0) {
            pingSettle(id, "failed", 0, -1);
            return;
        }
        info("id %d: ping %s on link %s", id.index, peer_hex.c_str(), have->tag.c_str());
        return;
    }
}

/* True when this OUT_RESULT belongs to the in-flight ping — consumed here
 * instead of being looked up in the outbox table. rnsd emits SENT first and a
 * second result when the proof lands or times out, so SENT is not an outcome.
 *
 * A probe no longer takes this route — it is a link now, and settles on the
 * link's own measurement — so `ping.send_id` is left at 0, which `next_send_id`
 * never issues. These stay because the guard is what keeps a ping and an outbox
 * send from being confused for one another, and that is worth keeping true by
 * construction rather than by nobody currently sending one. */
static bool pingApplyOutResult(lxmf_id_t& id, uint16_t send_id, uint8_t status,
                               uint32_t rtt_ms, uint8_t hops)
{
    if (!id.ping.used || id.ping.send_id != send_id) return false;
    switch (status) {
        case RNSD_DEST_STATUS_SENT:                                   return true;
        case RNSD_DEST_STATUS_DELIVERED: pingSettle(id, "ok", rtt_ms, hops); break;
        case RNSD_DEST_STATUS_PROOF_TIMEOUT: pingSettle(id, "no-proof", 0, -1); break;
        case RNSD_DEST_STATUS_FAILED:    pingSettle(id, "no-route", 0, -1); break;
        case RNSD_DEST_STATUS_TOO_LARGE: pingSettle(id, "failed",   0, -1); break;
        case RNSD_DEST_STATUS_CANCELLED: pingSettle(id, "cancelled",0, -1); break;
        default:                         pingSettle(id, "failed",   0, -1); break;
    }
    return true;
}

/* Ditto for the aux stream: the ping owns the send_id, so swallow its statuses
 * rather than letting the outbox lookup warn about an unknown one. A path search
 * is worth showing — it is the slow case the user is watching. */
static bool pingApplyOutStatus(lxmf_id_t& id, uint16_t send_id, uint8_t type)
{
    if (!id.ping.used || id.ping.send_id != send_id) return false;
    if (type == RNSD_DEST_AUX_REQUESTING_PATH) {
        storageSet(pingPath(id.ping.peer, "state").c_str(), "path");
    }
    return true;
}

/* 1 Hz backstop. rnsd settles a probe on its own in the normal cases, but a
 * send parked on a path search that never resolves produces no result at all —
 * so the deadline is what guarantees the UI stops saying `probing`. */
static void pingTick(lxmf_id_t& id)
{
    if (!id.ping.used) return;

    /* A probe that is a link. Two shapes, one deadline:
     *   - dialled for the probe: the link coming up IS the answer, and µR's own
     *     measurement of that exchange is published as `rtt_ms`. Drop the link
     *     the moment it has answered — it was a question, not a session.
     *   - sent on a link already open: the answer is the link's delivery-proof
     *     counter moving past where it stood when the probe went out. */
    if (id.ping.by_link && !id.ping.link_tag.empty()) {
        const std::string base = "rnsd.links." + id.ping.link_tag;
        {
            std::string st = storageGetStr((base + ".state").c_str(), "");
            /* A link this probe dialled is one it owns, and the handle is what
             * says so — a probe riding somebody else's conversation link holds
             * none and must leave it standing. */
            if (id.ping.link_handle >= 0) {
                if (st == "active") {
                    /* µR's own measurement of the handshake, or nothing. The
                     * wall clock here would be the press to this 1 Hz tick —
                     * path search included — which is not a round trip and
                     * must not be published as one; `answer_ms` carries it. */
                    int rtt = storageGetInt((base + ".rtt_ms").c_str(), 0);
                    itsDisconnect(id.ping.link_handle);
                    id.ping.link_handle = -1;
                    pingSettle(id, "ok", (uint32_t)(rtt > 0 ? rtt : 0),
                               rtt > 0 ? 0 : -1);
                    return;
                }
                if (st == "failed" || st == "closed") {
                    itsDisconnect(id.ping.link_handle);
                    id.ping.link_handle = -1;
                    pingSettle(id, "no-route", 0, -1);
                    return;
                }
            } else {
                int proven   = storageGetInt((base + ".tx_proven").c_str(), 0);
                int timeouts = storageGetInt((base + ".proof_timeouts").c_str(), 0);
                if (proven > id.ping.proof_base_proven) {
                    /* rnsd publishes the round trip µR timed for the packet
                     * this increment settled, in the same transaction as the
                     * counter — so the measurement is here to be read, and
                     * this tick's 1 Hz granularity never reaches the number. */
                    int rtt = storageGetInt((base + ".tx_rtt_ms").c_str(), 0);
                    pingSettle(id, "ok", (uint32_t)(rtt > 0 ? rtt : 0),
                               rtt > 0 ? 0 : -1);
                    return;
                }
                if (timeouts > id.ping.proof_base_timeouts) {
                    pingSettle(id, "no-proof", 0, -1);
                    return;
                }
            }
        }
    }

    if ((uint32_t)(nowUnixMs() / 1000) < id.ping.deadline_s) return;
    /* A dial that never answered leaves a link half-open; the probe is over
     * either way, so take it down with the answer. */
    if (id.ping.link_handle >= 0) {
        itsDisconnect(id.ping.link_handle);
        id.ping.link_handle = -1;
    }
    pingSettle(id, "timeout", 0, -1);
}

/* ─────────────── our-dest frame handlers ─────────────── */

static void applyOutResult(lxmf_id_t& id, uint16_t send_id, uint8_t status,
                           uint32_t /*rtt_ms*/, uint8_t /*hops*/)
{
    outbound_t* o = outboundFindBySendId(id, send_id);
    if (!o) {
        verb("id %d: OUT_RESULT for unknown send_id=%u", id.index, (unsigned)send_id);
        return;
    }
    std::string mid      = o->msg_key;
    std::string peer_hex = o->peer;

    /* SENT is not terminal: rnsd keeps the packet receipt and emits
     * a second OUT_RESULT (DELIVERED / PROOF_TIMEOUT) for this send_id.
     * Write "sent" (one grey check) and hold the slot for the proof.
     * pending/sent accounting settles here — the proof phase only refines
     * the stage. resolveDirectSends carries the backstop deadline in case
     * the second result is lost. */
    if (status == RNSD_DEST_STATUS_SENT) {
        if (id.pending > 0) id.pending--;
        id.sent++;
        o->awaiting_proof   = true;
        o->sent_s           = (uint32_t)(nowUnixMs() / 1000);
        o->proof_deadline_s = o->sent_s + proofBackstopS();
        storageBegin();
        storageSet(msgPath(id.index, peer_hex, mid, "status").c_str(), (int)LXMF_ST_AWAITING_PROOF);
        storageEnd();
        dbg("id %d: msg %s → sent (awaiting proof)", id.index, mid.c_str());
        return;
    }

    bool was_awaiting = o->awaiting_proof;

    o->used           = false;
    o->awaiting_proof = false;
    /* In the proof phase the pending/sent counters already settled at
     * SENT — don't move them twice. */
    if (!was_awaiting && id.pending > 0) id.pending--;

    uint8_t status_code = LXMF_ST_UNKNOWN;
    bool    gaveup      = true;    /* terminal failure → status + tries=255 via msgFail */
    switch (status) {
        case RNSD_DEST_STATUS_DELIVERED:
            status_code = LXMF_ST_DELIVERED; gaveup = false;
            if (!was_awaiting) id.sent++;
            break;
        case RNSD_DEST_STATUS_PROOF_TIMEOUT:
            /* Egressed opportunistically (no link) and no delivery proof came
             * back. The message or its proof was lost, the peer is away, or the
             * own radio shed frames to contention (RADIO_BUSY): the same packet
             * goes out again, and then a Link (oppProofMissed). */
            if (was_awaiting) { if (id.sent) id.sent--; }
            oppProofMissed(id, peer_hex, mid, o->sent_s);
            return;
        case RNSD_DEST_STATUS_CANCELLED:
            status_code = LXMF_ST_CANCELLED; gaveup = false;
            if (!was_awaiting) id.failed++;
            break;
        case RNSD_DEST_STATUS_EVICTED:
            status_code = LXMF_ST_EVICTED;  id.failed++; break;
        case RNSD_DEST_STATUS_FAILED:
            /* rnsd gave up its path search — the next sweep asks again. */
            queueRequeue(id, peer_hex, mid, LXMF_ST_REQUESTING_PATH);
            return;
        case RNSD_DEST_STATUS_TOO_LARGE:
            status_code = LXMF_ST_TOO_LARGE; id.failed++; break;
        default:
            status_code = LXMF_ST_UNKNOWN;  id.failed++; break;
    }
    if (gaveup) msgFail(id.index, peer_hex, mid, status_code);
    else        msgSetStatus(id.index, peer_hex, mid, status_code);
    dbg("id %d: msg %s → %s (tries %s)", id.index, mid.c_str(),
        lxmfStatusName(status_code), gaveup ? "255" : "n");
}

/* Human-readable RNSD_DEST_AUX_RETRY reason byte. The reason *values* are
 * shared with rnsd via ports.h (RNSD_DEST_RETRY_REASON_*); only these display
 * strings are duplicated here so the message-status UI can show words instead
 * of a hex code. Keep this table in step with ports.h as reasons are added;
 * unmapped codes fall back to the numeric form below. */
static const char* retryReasonName(uint8_t reason)
{
    switch (reason) {
        case RNSD_DEST_RETRY_REASON_PATH_TIMEOUT: return "path timeout";
        default: return nullptr;
    }
}

static void applyOutStatus(lxmf_id_t& id, uint16_t send_id, uint8_t type,
                           const uint8_t* tail, size_t tail_n)
{
    outbound_t* o = outboundFindBySendId(id, send_id);
    if (!o) return;
    uint8_t st = 0;   /* 0 = informational only, leave the status unchanged */
    switch (type) {
        case RNSD_DEST_AUX_REQUESTING_PATH:
            /* rnsd emits this once per park: the initial no-path park,
             * and a re-park when the found path lacks a recallable
             * identity. */
            ++o->path_reqs;
            /* Start the path grace: past it the 1 Hz pass takes the send back
             * from rnsd and queues the message (resolveDirectSends). */
            if (!o->path_deadline_s) {
                uint32_t grace = LXMF_PATH_GRACE_S;
                if ((uint32_t)rnsdPathBudgetS() > grace) grace = (uint32_t)rnsdPathBudgetS();
                o->path_deadline_s = (uint32_t)(nowUnixMs() / 1000) + grace;
            }
            st = LXMF_ST_REQUESTING_PATH; break;
        case RNSD_DEST_AUX_PATH_KNOWN:        o->path_reqs = 0;
                                              o->path_deadline_s = 0;
                                              st = LXMF_ST_SENDING;         break;
        case RNSD_DEST_AUX_EGRESS_QUEUED:     st = LXMF_ST_SENDING;         break;
        case RNSD_DEST_AUX_LINK_ESTABLISHING: st = LXMF_ST_SENDING;         break;
        case RNSD_DEST_AUX_PATH_LOST:         st = LXMF_ST_REQUESTING_PATH; break;
        case RNSD_DEST_AUX_QUEUE_FULL: {
            /* rnsd's pending path-search table is full — it did NOT accept
             * this send. Release the in-flight slot and let the message wait
             * in the queue for the next sweep; nothing is dropped. */
            std::string peer = o->peer, mid = o->msg_key;
            o->used = false;
            if (id.pending) id.pending--;
            msgSetStatus(id.index, peer, mid, LXMF_ST_QUEUED);
            queueAdd(id.index, peer, mid);
            verb("id %d: send_id=%u held (rnsd queue full), queued %s",
                 id.index, (unsigned)send_id, mid.c_str());
            return;
        }
        case RNSD_DEST_AUX_RETRY: {
            /* A path retry: mark REQUESTING_PATH (the reason is implicit in
             * the status; log it for detail). */
            uint8_t tries     = tail_n >= 1 ? tail[0] : 0;
            const char* rname = tail_n >= 2 ? retryReasonName(tail[1]) : nullptr;
            dbg("id %d: %s path retry %u (%s)", id.index, o->msg_key.c_str(),
                (unsigned)tries, rname ? rname : "?");
            msgSetStatus(id.index, o->peer, o->msg_key, LXMF_ST_REQUESTING_PATH);
            return;
        }
        default: break;
    }
    if (st) msgSetStatus(id.index, o->peer, o->msg_key, st);
}

static void onOurDestRecv(int handle, size_t /*bytesAvail*/)
{
    lxmf_id_t* id = idForHandle(handle);
    if (!id) return;

    PSRAM_BSS static uint8_t buf[2048];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n == 0) return;

    switch (buf[0]) {
        case RNSD_DEST_IN_PACKET:
            /* opcode | <lxm wire = dest(16)|src|sig|packed>. */
            onInboundLxm(*id, buf + 1, n - 1);
            break;
        case RNSD_DEST_OUT_RESULT: {
            if (n < 9) { warn("OUT_RESULT short (%zu)", n); break; }
            uint16_t send_id = ((uint16_t)buf[1] << 8) | (uint16_t)buf[2];
            uint8_t status  = buf[3];
            uint32_t rtt_ms = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                            | ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
            uint8_t hops    = buf[8];
            if (!pingApplyOutResult(*id, send_id, status, rtt_ms, hops))
                applyOutResult(*id, send_id, status, rtt_ms, hops);
            break;
        }
        case RNSD_DEST_OUT_STATUS: {
            if (n < 4) { warn("OUT_STATUS short (%zu)", n); break; }
            uint16_t send_id = ((uint16_t)buf[1] << 8) | (uint16_t)buf[2];
            uint8_t type     = buf[3];
            if (!pingApplyOutStatus(*id, send_id, type))
                applyOutStatus(*id, send_id, type, buf + 4, n - 4);
            break;
        }
        default:
            warn("id %d: unknown our-dest opcode 0x%02x", id->index, (unsigned)buf[0]);
            break;
    }
}

static void onOurDestDisconnect(int handle)
{
    lxmf_id_t* id = idForHandle(handle);
    if (!id) return;
    warn("id %d: our-dest disconnected (handle=%d)", id->index, handle);
    id->handle = -1;
    storageSet(idEphPath(id->index, "up").c_str(), 0);
    publishReady(id->index);
    /* Reconnect attempted on the next 1 Hz publish tick. */
}

/* ─────────────── inbound DIRECT (Link → onInboundLxm) ───────────────
 *
 * lxmf registers for inbound Links on each
 * lxmf.delivery destination via rnsdDestListenLinks(handle,
 * LXMF_LINK_INBOX_PORT). rnsd accepts the Link and back-connects here
 * with an rnsd_link_incoming_t. The link is a packet-mode stream of
 * LXMF wire bytes with the 16-byte destination prefix stripped (the
 * Link *is* the destination) — we prepend our delivery dest hash and
 * run the exact same onInboundLxm pipeline as the opportunistic path.
 * This is what lets real-world LXMF peers (which default to DIRECT)
 * deliver to us. */

/* inlink_t + s_inlinks live beside the conv-link pool above; these are
 * receive-only (we never send over a peer's inbound link). */
static inlink_t* inlinkByHandle(int handle)
{
    for (auto& s : s_inlinks)
        if (s.used && s.handle == handle) return &s;
    return nullptr;
}

static int onLinkInboxConnect(int handle, const void* data, size_t len)
{
    if (len < sizeof(rnsd_link_incoming_t)) {
        warn("inlink: short connect payload %zu", len);
        return -1;
    }
    rnsd_link_incoming_t pl;
    std::memcpy(&pl, data, sizeof(pl));
    pl.tag[sizeof(pl.tag) - 1] = '\0';

    /* Map to the identity whose delivery dest the Link landed on. */
    int idx = -1;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_ids[n].used &&
            std::memcmp(s_ids[n].dest_hash, pl.local_dest_hash,
                        LXMF_DEST_HASH_LEN) == 0) { idx = n; break; }
    }
    if (idx < 0) {
        warn("inlink: no identity for local dest %s",
             bytesToHex(pl.local_dest_hash, LXMF_DEST_HASH_LEN).c_str());
        return -1;
    }
    inlink_t* slot = nullptr;
    for (auto& s : s_inlinks) if (!s.used) { slot = &s; break; }
    if (!slot) { warn("inlink: table full"); return -1; }
    slot->used     = true;
    slot->handle   = handle;
    slot->id_index = idx;
    slot->tag      = pl.tag;
    info("id %d: inbound Link %s (tag=%s) from %s",
         idx, bytesToHex(pl.link_id, 16).c_str(), pl.tag,
         bytesToHex(pl.remote_identity_hash, 16).c_str());
    return (int)(slot - s_inlinks);
}

/* Double-encrypted delivery: a link/resource payload
 * whose leading 16 bytes name NO loaded delivery dest may be a
 * destination-encrypted envelope blob (mR Identity token) rather than
 * plaintext LXMF wire. Try every loaded
 * identity; when a decrypt yields a plaintext that leads with that
 * identity's dest hash, feed it through the normal inbound pipeline.
 * Returns true iff the payload was consumed as an envelope; false leaves
 * the caller's existing handling (warn/drop) untouched. The scratch is a
 * single payload-sized heap block, freed before return; payloads beyond
 * s.lxmf.max_resource_size are refused outright. */
static bool tryDoubleEncrypted(const uint8_t* payload, size_t n)
{
    /* A valid token can't be smaller than its overhead plus the minimum
     * LXMF wire it must contain. */
    if (n < LXMF_OVERHEAD + 48) return false;
    if (n > (size_t)storageGetInt("s.lxmf.max_resource_size", 262144))
        return false;
    for (const auto& id : s_ids)
        if (id.used && std::memcmp(payload, id.dest_hash,
                                   LXMF_DEST_HASH_LEN) == 0)
            return false;              /* plaintext wire for us — not an envelope */
    uint8_t* pt = (uint8_t*)gp_alloc(n);
    if (!pt) return false;
    for (auto& id : s_ids) {
        if (!id.used) continue;
        size_t pt_len = n;
        if (!rnsdDecryptSelf(id.identity_key.c_str(), id.dest_hash,
                             payload, n, pt, &pt_len))
            continue;
        if (pt_len < LXMF_OVERHEAD ||
            std::memcmp(pt, id.dest_hash, LXMF_DEST_HASH_LEN) != 0)
            continue;                  /* decrypted, but not an LXM for this dest */
        info("id %d: double-encrypted envelope %zuB → %zuB wire", id.index,
             n, pt_len);
        onInboundLxm(id, pt, pt_len);
        gp_free(pt);
        return true;
    }
    gp_free(pt);
    return false;
}

static void onLinkInboxRecv(int handle, size_t /*bytesAvail*/)
{
    inlink_t* s = inlinkByHandle(handle);
    if (!s) return;
    lxmf_id_t& id = s_ids[s->id_index];

    PSRAM_BSS static uint8_t buf[2048];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n == 0) return;

    /* rnsd (onLinkPacketCb) forwards the Link plaintext verbatim: the full LXM
     * wire. Unlike opportunistic (which strips dest16 on the wire and the
     * receiver prepends it), a DIRECT Link packet carries the *full* wire
     * including the 16-byte destination hash — upstream `LXMessage.__as_packet`
     * DIRECT sends `self.packed` whole and `LXMRouter.delivery_packet` does
     * `lxmf_data = data` for LINK with no prepend (LXMRouter.py:1831-1833).
     * Hand it to the shared pipeline; onInboundLxm validates dh == our
     * delivery dest. */
    if (tryDoubleEncrypted(buf, n)) return;
    onInboundLxm(id, buf, n);
}

static void onLinkInboxDisconnect(int ref)
{
    if (ref < 0 || ref >= LXMF_MAX_INLINKS) return;
    inlink_t& s = s_inlinks[ref];
    if (!s.used) return;
    verb("id %d: inbound Link forward closed (%s)", s.id_index, s.tag.c_str());
    s.used = false;
    s.handle = -1;
    s.tag.clear();
}

/* ─────────────── LXMF proxy client ───────────────
 *
 *   C → S   CHANNEL (rnsd identifies with the account identity)
 *   S → C   HELLO    [label, limits, serving?]
 *   C → S   HANDOVER [privkey, display_name, ratchets]      (first provisioning only)
 *   S → C   SERVING  [ok]                     → unregister our dest, role := client
 *   S → C   MSG / STATUS / STATE / BODY       live, while the Channel is up
 *   C → S   SEND / FETCH / HANDED / CONFIG
 *   C → S   RELEASE                           → S → C RATCHETS, role := off
 *
 * The other half of the LXMF proxy: this device holds an account's keys and
 * lets ANOTHER device — an always-on one — register and announce the account's
 * `lxmf.delivery`, receive its mail and send its outbound. The rest of the
 * network sees an ordinary always-online LXMF node and needs to implement
 * nothing. The cost is stated plainly and not designed around: both devices
 * hold the account key and the cleartext.
 *
 * The invariant, and the source of most of the failure modes:
 *
 *     At every moment, exactly one device registers and announces the
 *     account's lxmf.delivery — never zero, never two.
 *
 * One per-identity key holds the role — `s.lxmf.id.<n>.proxy_role` = off |
 * server | client — so the both-ends-registered state is unreachable by
 * construction. `off` and `server` both register and announce here (the
 * difference is only whose account it is); `client` registers nothing,
 * announces nothing, and hands its delivery queue to the Channel.
 *
 * Both transitions are handshakes with a pending state, never a local flag
 * flip. Going ON, the client keeps its own destination registered until the
 * server confirms it is serving. Going OFF, the client stays proxied — fully
 * working, the server still delivering — until the server hands the ratchets
 * back. It never enters a state where it is neither.
 *
 * Deregistering tells the network nothing: the previous registrant's announces
 * keep bouncing around until they age out and peers keep their cached path for
 * as long as their path expiry allows. The new registrant announces at once and
 * the network converges as that announce spreads. We live with the window;
 * nothing here detects or corrects it.
 *
 * The transport is an RNS Channel, held permanently while the client is online,
 * so push is live and lxmf still feels immediate. Liveness is µR's own
 * keepalive — detection within a few minutes, no application heartbeat — and on
 * disconnect the client re-establishes with backoff. The Channel cannot live in
 * the conversation-link pool: `s.lxmf.link.idle_s` and the four-link LRU cap
 * exist precisely to kill long-held links.
 *
 * A MESSAGE is acknowledged; a STATUS is not, and the difference is what each
 * is worth. `HANDED` goes out once an inbound record — body included — is in
 * storage, which is strictly more than "the frame arrived": rnsd proves a
 * packet the moment the hand-off to the consumer task succeeds, before anything
 * is stored. Mail is worth a frame to say it was kept. A status word is not:
 * the Channel proves every envelope or tears the link down trying, so the
 * server learns this device has a `STATUS` from its own delivery proof
 * (`rnsd.chan.<tag>.outstanding` reaching zero) and nothing goes back. */

/* A frame past this rides a Resource on the Channel's hidden Link instead of
 * one Channel message. Conservative against the channel MDU (link MDU less the
 * envelope, less the two-byte msgtype prefix this handle frames with). */
#define LXMF_PROXY_MSG_MAX      300
/* Reconnect backoff, doubling to the ceiling. A proxied client with no Channel
 * is a client whose mail is piling up on the server, so the floor is short. */
#define LXMF_PROXY_BACKOFF_MIN_S 5
#define LXMF_PROXY_BACKOFF_MAX_S 300
/* A provisioning or release handshake that draws no answer in this long gives
 * up and returns to the state it started in — the one that is registered. */
#define LXMF_PROXY_HANDSHAKE_TTL_S 120
/* Heard `lxmproxy.server` announces, for the picker. Bounded: the client lists
 * proxies it has heard by LABEL and nobody ever types a hash. */
#define LXMF_PROXY_MAX_HEARD    16
/* Resource opaque ids for proxy sends, above both the uint16 send_id space and
 * the propagation client's 0x20000+ range, so aux matching can never collide. */
#define LXMF_PROXY_OPAQUE_BASE  0x30000u

enum proxy_role_t : uint8_t {
    PROXY_ROLE_OFF    = 0,   /* an ordinary identity: registers and announces here */
    PROXY_ROLE_SERVER = 1,   /* hosted here for somebody else: registers and announces */
    PROXY_ROLE_CLIENT = 2,   /* proxied elsewhere: registers nothing, holds the Channel */
};

/* Which handshake, if any, is in flight. The durable role never sits in one of
 * these — a reboot mid-handshake resumes as whichever end is registered, which
 * is always the safe one. */
enum proxy_phase_t : uint8_t {
    PROXY_PH_IDLE = 0,
    PROXY_PH_PROVISIONING,   /* still registered; waiting for SERVING */
    PROXY_PH_RELEASING,      /* still proxied; waiting for RATCHETS */
};

struct proxy_t {
    int         handle = -1;          /* RNSD_PORT_CHANNEL handle, -1 = down */
    std::string tag;                  /* keys rnsd.chan.<tag>.* */
    uint8_t     dest[16] = {};
    bool        have_dest = false;
    bool        active = false;       /* the Channel reached "active" */
    bool        serving = false;      /* the server says it holds the account */
    proxy_phase_t phase = PROXY_PH_IDLE;
    uint32_t    phase_deadline_s = 0;
    uint32_t    next_try_s = 0;
    uint32_t    backoff_s = LXMF_PROXY_BACKOFF_MIN_S;
    uint32_t    reopen_at_s = 0;      /* re-register our own dest at/after this */
    bool        was_ready = false;    /* proxyReady() last tick — the edge kicks the queue */
    std::string cfg_sig;              /* last CONFIG pushed, as one string */
    std::string label;                /* the operator's label for the server */
    uint32_t    max_envelope_kb = 0;  /* HELLO's ceiling on one message, 0 = not
                                       * said yet. Kept so an oversize draft
                                       * fails HERE: the server would only
                                       * answer TOO_LARGE after the body had
                                       * crossed the air, and a refusal that
                                       * costs the airtime of the thing being
                                       * refused is the one error worth
                                       * knowing before sending. */
};
static proxy_t s_proxy[LXMF_MAX_IDENTITIES];
static uint16_t s_proxyTagSeq = 0;
static uint32_t s_proxyOpaque = LXMF_PROXY_OPAQUE_BASE;
static int      s_proxy_ann_handle = -1;

/* Heard proxy servers: dest hex → label. Published as `lxmf.proxies.<hex>.*`
 * so a picker can list them by label; RAM only, like the announce catalogue. */
struct proxy_heard_t { std::string label; uint32_t last_s; };
static std::map<std::string, proxy_heard_t> s_proxyHeard;

/* SENDs still crossing to the server as a Resource, waiting for rnsd's
 * OUTBOUND_DONE echo. A Resource is proved part by part, so that echo is this
 * device's own evidence that the server has the message — no frame has to come
 * back to say so. Bounded by what one Channel carries at a time. */
struct proxy_send_t {
    uint32_t    opaque;
    int         id_index;
    std::string peer, mid;
};
static std::vector<proxy_send_t> s_proxySends;

static bool connectOurDest(lxmf_id_t& id);        /* fwd */
static void sendAnnounce(lxmf_id_t& id);          /* fwd */

static proxy_role_t proxyRole(int n)
{
    std::string v = storageGetStr(idPath(n, "proxy_role").c_str(), "off");
    if (v == "client") return PROXY_ROLE_CLIENT;
    if (v == "server") return PROXY_ROLE_SERVER;
    return PROXY_ROLE_OFF;
}

static const char* proxyRoleName(proxy_role_t r)
{
    switch (r) {
        case PROXY_ROLE_CLIENT: return "client";
        case PROXY_ROLE_SERVER: return "server";
        default:                return "off";
    }
}

/** True while this identity's mail belongs to a proxy server: it registers
 *  nothing here, and every send goes out over the Channel. */
static bool proxyIsClient(int n) { return proxyRole(n) == PROXY_ROLE_CLIENT; }

/** True when the Channel is up AND the server has confirmed it is serving —
 *  the only state in which handing it a message means anything. */
static bool proxyReady(int n)
{
    const proxy_t& p = s_proxy[n];
    return p.handle >= 0 && p.active && p.serving;
}

/* Published state, for the header indicator and the settings pane. Finished
 * strings only — no UI computes, compares or concatenates anything here. */
static void proxyPublish(int n)
{
    const proxy_t& p = s_proxy[n];
    proxy_role_t role = proxyRole(n);
    const char* st;
    if      (p.phase == PROXY_PH_PROVISIONING) st = "provisioning";
    else if (p.phase == PROXY_PH_RELEASING)    st = "releasing";
    else if (role == PROXY_ROLE_CLIENT)        st = "client";
    else if (role == PROXY_ROLE_SERVER)        st = "server";
    else                                       st = "";
    const char* link = p.active ? (p.serving ? "active" : "connecting")
                     : p.handle >= 0 ? "connecting" : "down";
    bool on_channel = (role == PROXY_ROLE_CLIENT || p.phase != PROXY_PH_IDLE);

    /* The one line the pane shows for this slot's proxy state, composed here so
     * neither surface has to know what any of it means. */
    std::string text;
    std::string who = p.label.empty()
                    ? storageGetStr(idPath(n, "proxy_dest").c_str(), "")
                    : p.label;
    switch (role) {
        case PROXY_ROLE_SERVER:
            text = "Hosted on this device for another"; break;
        case PROXY_ROLE_CLIENT:
            if (p.phase == PROXY_PH_RELEASING)
                text = "Releasing from " + who + " — still working until it answers";
            else if (p.serving && p.active) text = "Proxied by " + who;
            else if (p.active)              text = "Reconnecting to " + who;
            else                            text = "Proxied by " + who + " — offline";
            break;
        default:
            if (p.phase == PROXY_PH_PROVISIONING)
                text = p.phase_deadline_s
                     ? "Asking " + who + " to take this account"
                     : "Waiting for " + who + "'s operator to approve this account";
            else
                text = "Not proxied — this device answers on this address";
            break;
    }

    storageBegin();
    publishReady(n);   /* the role is half of it, and this is where the role lands */
    setStrIfChanged(idEphPath(n, "proxy_state"), st);
    /* The link line is only meaningful for the end that holds the Channel. */
    setStrIfChanged(idEphPath(n, "proxy_link"), on_channel ? link : "");
    setStrIfChanged(idEphPath(n, "proxy_label"), p.label.c_str());
    setStrIfChanged(idEphPath(n, "proxy_text"), text.c_str());
    /* Row gates. Truthiness only: "offer to proxy" and "offer to release" are
     * facts this task knows, never a comparison in a UI. */
    setStrIfChanged(idEphPath(n, "proxy_offer"),
                    (role == PROXY_ROLE_OFF && p.phase == PROXY_PH_IDLE) ? "1" : "");
    setStrIfChanged(idEphPath(n, "proxy_held"),
                    (role == PROXY_ROLE_CLIENT) ? "1" : "");
    /* Asking, and not yet answered — the row that offers to stop asking. It is
     * the only way out of a wait on an operator who never approves. */
    setStrIfChanged(idEphPath(n, "proxy_asking"),
                    (p.phase == PROXY_PH_PROVISIONING) ? "1" : "");
    storageEnd();
}

/* Every `lxmproxy.server` we have heard, by label, as one finished line each —
 * the picker's source, since nobody ever types a hash. */
static void proxyPublishHeard(void)
{
    std::string s;
    for (const auto& kv : s_proxyHeard) {
        if (!s.empty()) s += "\n";
        s += kv.second.label.empty() ? "(unnamed)" : kv.second.label;
        s += "  ";
        s += kv.first;
    }
    setStrIfChanged("lxmf.proxies_text",
                    s.empty() ? "No proxy servers heard yet." : s.c_str());
}

/* ── the Channel ── */

static void proxyHandleFrame(int n, const LxmproxyFrame& fr);

static void onProxyChanRecv(int handle, size_t /*bytesAvail*/)
{
    int n = -1;
    for (int k = 0; k < LXMF_MAX_IDENTITIES; ++k)
        if (s_proxy[k].handle == handle) { n = k; break; }
    if (n < 0) return;
    PSRAM_BSS static uint8_t buf[1024];
    size_t got = itsRecv(handle, buf, sizeof(buf), 0);
    /* The channel handle frames every message [msgtype:2 BE][payload]; we speak
     * MSGTYPE_RAW and read past the prefix. */
    if (got <= 2) return;
    LxmproxyFrame fr;
    if (!lxmproxyParse(buf + 2, got - 2, fr)) {
        warn("id %d: proxy frame malformed (%zuB)", n, got - 2);
        return;
    }
    proxyHandleFrame(n, fr);
}

static void onProxyChanDisc(int ref)
{
    if (ref < 0 || ref >= LXMF_MAX_IDENTITIES) return;
    proxy_t& p = s_proxy[ref];
    if (p.handle < 0) return;
    info("id %d: proxy Channel closed (%s)", ref, p.tag.c_str());
    p.handle  = -1;
    p.active  = false;
    p.serving = false;
    p.cfg_sig.clear();
    p.next_try_s = (uint32_t)(nowUnixMs() / 1000) + p.backoff_s;
    p.backoff_s  = p.backoff_s * 2 > LXMF_PROXY_BACKOFF_MAX_S
                 ? LXMF_PROXY_BACKOFF_MAX_S : p.backoff_s * 2;
    proxyPublish(ref);
}

/* Send one frame: a Channel message when it fits, else a Resource on the
 * Channel's hidden Link. The bytes are identical either way — the far end
 * parses the same thing whichever arrived. */
/* `opaque_out`, when given, is set to the Resource id a frame too big for one
 * Channel message went out under, and left at 0 for one that fitted. Only the
 * Resource has an echo — that is the whole difference the caller cares about. */
static bool proxySendFrame(int n, const std::vector<uint8_t>& frame,
                           uint32_t* opaque_out)
{
    if (opaque_out) *opaque_out = 0;
    proxy_t& p = s_proxy[n];
    if (p.handle < 0 || !p.active) return false;
    if (frame.size() > LXMF_PROXY_MSG_MAX) {
        void* buf = gp_alloc(frame.size());
        if (!buf) { warn("id %d: proxy resource malloc %zuB failed", n, frame.size()); return false; }
        memcpy(buf, frame.data(), frame.size());
        uint32_t opaque = s_proxyOpaque++;
        /* rnsd owns buf from here and frees it once the engine has copied it. */
        if (!rnsdChannelSendResource(p.tag.c_str(), buf, frame.size(), opaque))
            return false;
        if (opaque_out) *opaque_out = opaque;
        return true;
    }
    std::vector<uint8_t> msg;
    msg.reserve(2 + frame.size());
    msg.push_back(0x01); msg.push_back(0x00);      /* RNS Channel MSGTYPE_RAW */
    msg.insert(msg.end(), frame.begin(), frame.end());
    if (itsSend(p.handle, msg.data(), msg.size(), pdMS_TO_TICKS(200)) == 0) {
        warn("id %d: proxy send dropped (%zuB)", n, frame.size());
        return false;
    }
    return true;
}

static bool proxyOpenChannel(lxmf_id_t& id)
{
    proxy_t& p = s_proxy[id.index];
    if (p.handle >= 0) return true;
    if (!p.have_dest) {
        std::string d = storageGetStr(idPath(id.index, "proxy_dest").c_str(), "");
        p.have_dest = hexToBytes(d.c_str(), d.size(), p.dest, 16);
        if (!p.have_dest) return false;
    }
    char tag[24];
    std::snprintf(tag, sizeof(tag), "lxpx.%d.%04x", id.index,
                  (unsigned)s_proxyTagSeq++);
    p.tag = tag;
    /* identity_key non-empty makes rnsd identify the link with the ACCOUNT
     * identity once it is active — the entitlement the server checks. */
    int h = rnsdChannelOpen(p.dest, LXMPROXY_ASPECT, id.identity_key.c_str(), tag,
                            /*path_timeout_ms=*/0, /*link_timeout_ms=*/0,
                            /*ref=*/id.index, onProxyChanRecv, onProxyChanDisc);
    if (h < 0) {
        warn("id %d: proxy Channel open failed", id.index);
        return false;
    }
    p.handle = h;
    p.active = false;
    info("id %d: proxy Channel %s → %s", id.index, tag,
         bytesToHex(p.dest, 16).c_str());
    proxyPublish(id.index);
    return true;
}

static void proxyCloseChannel(int n)
{
    proxy_t& p = s_proxy[n];
    if (p.handle < 0) return;
    int h = p.handle;
    p.handle = -1; p.active = false; p.serving = false;
    p.cfg_sig.clear();
    /* Resources in flight on this Channel will never echo now. Their records
     * are SENDING_TO_PROXY and in the queue, which is where an interrupted
     * transfer belongs: a Resource that did not complete was never handed to
     * the server's consumer at all, so there is nothing there to collide with
     * and the next sweep simply sends it again. */
    for (size_t i = s_proxySends.size(); i-- > 0; )
        if (s_proxySends[i].id_index == n)
            s_proxySends.erase(s_proxySends.begin() + i);
    itsDisconnect(h);
}

/* ── registering and unregistering our own destination ──
 *
 * rnsd deregisters asynchronously on its own task, and opening a destination
 * that is still registered yields a silent outbound-only handle. So the
 * re-register is deferred: close the handle, arm `reopen_at_s`, and let the
 * 1 Hz tick open it once the slot has certainly cleared. `connectOurDest`
 * re-issues the inbound-Link listen on every open, which is what makes the
 * reopened destination reachable again. */
#define LXMF_PROXY_REOPEN_DELAY_S 3

static void proxyUnregisterOurDest(lxmf_id_t& id)
{
    if (id.handle < 0) return;
    int h = id.handle;
    id.handle = -1;
    storageSet(idEphPath(id.index, "up").c_str(), 0);
    publishReady(id.index);
    itsDisconnect(h);
    info("id %d: delivery destination deregistered (proxied)", id.index);
}

static void proxyArmReopen(int n)
{
    s_proxy[n].reopen_at_s = (uint32_t)(nowUnixMs() / 1000) + LXMF_PROXY_REOPEN_DELAY_S;
}

/* ── the settings the server holds and the client edits (CONFIG) ── */

/* The propagation-node list, as one line: `hash|name|check` per slot, slots
 * separated by newlines. It is what the SERVER should poll and upload through,
 * since it is the end that faces the world, but it is edited here because this
 * is the end with a UI. */
static std::string proxyPnList(void)
{
    std::string out;
    for (int i = 0; i < 8; ++i) {
        char k[40];
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.hash", i);
        std::string h = storageGetStr(k, "");
        if (h.size() != 32) continue;
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.name", i);
        std::string nm = storageGetStr(k, "");
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.check", i);
        int ck = storageGetInt(k, 1);
        if (!out.empty()) out += '\n';
        out += h; out += '|'; out += nm; out += '|'; out += (ck ? '1' : '0');
    }
    return out;
}

static std::string proxyConfigSig(int n)
{
    std::string s = storageGetStr(idPath(n, "display_name").c_str(), "");
    s += '\x1f'; s += std::to_string(storageGetInt("s.lxmf.stamp_cost", 8));
    s += '\x1f'; s += std::to_string(storageGetInt("s.lxmf.enforce_stamps", 0));
    s += '\x1f'; s += std::to_string(storageGetInt(idPath(n, "enabled").c_str(), 1));
    s += '\x1f'; s += proxyPnList();
    return s;
}

static void proxyPushConfig(int n)
{
    std::vector<std::pair<std::string, std::string>> m;
    m.emplace_back("display_name",   storageGetStr(idPath(n, "display_name").c_str(), ""));
    m.emplace_back("stamp_cost",     std::to_string(storageGetInt("s.lxmf.stamp_cost", 8)));
    m.emplace_back("enforce_stamps", std::to_string(storageGetInt("s.lxmf.enforce_stamps", 0)));
    m.emplace_back("enabled",        std::to_string(storageGetInt(idPath(n, "enabled").c_str(), 1)));
    m.emplace_back("pn",             proxyPnList());
    if (proxySendFrame(n, lxmproxyBuildConfig(m)))
        s_proxy[n].cfg_sig = proxyConfigSig(n);
}

/* ── inbound: store, then acknowledge ── */

/* Write one pushed message. Returns true once the record is in storage WITH its
 * body, which is the only thing that lets HANDED go out: acknowledging a
 * metadata-only record would have the server delete a body nobody ever fetched.
 *
 * What is left is what is owed, so there is no cursor: a reconnect re-pushes
 * the remainder and this dedups. A record we already hold with the body still
 * absent is not a duplicate — it is a pending fetch. */
static bool proxyStoreInbound(lxmf_id_t& id, const LxmproxyFrame& fr)
{
    std::string peer_hex = bytesToHex(fr.peer, 16);
    std::string mid_hex  = bytesToHex(fr.msg_id, LXMPROXY_MID_LEN);
    const char* mid_key  = mid_hex.c_str();

    bool exists = storageExists(msgPath(id.index, peer_hex, mid_hex, "status").c_str());
    bool absent = exists &&
        storageGetInt(msgPath(id.index, peer_hex, mid_hex, "body_absent").c_str(), 0) != 0;
    if (exists && !absent) return true;    /* already whole — re-ack and move on */

    bool withheld = !fr.have_body;
    storageBegin();
    if (!exists) {
        storageSet(msgPath(id.index, peer_hex, mid_hex, "dir").c_str(), "in");
        storageSet(msgPath(id.index, peer_hex, mid_hex, "peer").c_str(), peer_hex.c_str());
        storageSet(msgPath(id.index, peer_hex, mid_hex, "message_id").c_str(), mid_key);
        storageSet(msgPath(id.index, peer_hex, mid_hex, "status").c_str(), (int)LXMF_ST_RECEIVED);
        storageSet(msgPath(id.index, peer_hex, mid_hex, "ts").c_str(), (int)fr.ts);
        storageSet(msgPath(id.index, peer_hex, mid_hex, "read").c_str(), 0);
        storageSet(msgPath(id.index, peer_hex, mid_hex, "title").c_str(), fr.title.c_str());
        /* What this message replies to travels with the offer and not only with
         * the body, so a reply whose body fits inline — never fetched, so no
         * BODY frame ever follows — still lands here as a reply. */
        if (fr.have_reply_to) {
            storageSet(msgPath(id.index, peer_hex, mid_hex, "reply_to").c_str(),
                       bytesToHex(fr.reply_to, LXMPROXY_MID_LEN).c_str());
            if (!fr.reply_quote.empty() && fr.reply_quote.size() <= LXMF_REPLY_QUOTE_MAX)
                storageSet(msgPath(id.index, peer_hex, mid_hex, "reply_quote").c_str(),
                           fr.reply_quote.c_str());
        }
    }
    storageSet(msgPath(id.index, peer_hex, mid_hex, "body_size").c_str(), (int)fr.size);
    storageSet(msgPath(id.index, peer_hex, mid_hex, "body_absent").c_str(), withheld ? 1 : 0);
    if (!withheld)
        storageSet(msgPath(id.index, peer_hex, mid_hex, "content").c_str(), fr.content.c_str());
    if (!exists) {
        /* Contacts are not synced — both ends auto-create on first contact and
         * are allowed to diverge — so the peer's name rides along as a hint for
         * the case where this client never heard that peer announce. */
        std::string nm = fr.peer_name.empty() ? announceName(peer_hex) : fr.peer_name;
        if (!storageExists(contactPath(id.index, peer_hex, "hash").c_str())) {
            storageSet(contactPath(id.index, peer_hex, "hash").c_str(), peer_hex.c_str());
            storageSet(contactPath(id.index, peer_hex, "trust").c_str(), 0);
        }
        if (!nm.empty())
            storageSet(contactPath(id.index, peer_hex, "display_name").c_str(), nm.c_str());
        setIntIfChanged(contactPath(id.index, peer_hex, "last_seen"),
                        (int)(nowUnixMs() / 60000) * 60);
        /* A withheld body has no preview to show, so the title stands in — the
         * conversation list would otherwise say nothing at all about a message
         * that has arrived. */
        int recv = bumpConvDirectory(id.index, peer_hex, (int)fr.ts,
                                     withheld ? fr.title : fr.content,
                                     /*inbound=*/true);
        storageSet(msgPath(id.index, peer_hex, mid_hex, "recv_ts").c_str(), recv);
    }
    storageEnd();

    if (!exists) {
        lxmfClaimContact(peer_hex);
        id.received++;
        lxmfNotifySound();
        info("id %d: proxy recv mid=%s from=%s %uB%s", id.index, mid_key,
             peer_hex.c_str(), (unsigned)fr.size, withheld ? " (body withheld)" : "");
    }
    return !withheld;
}

/* ── outbound ── */

/* Hand a draft to the server. `local_key` is the idempotency key: a SEND
 * repeated after a reconnect meets the same record there and gets the same
 * STATUS back. The client's own timestamp goes with it, so the message_id both
 * ends derive is the same one. */
static void proxySend(lxmf_id_t& id, const std::string& peer_hex,
                      const std::string& mid)
{
    uint8_t dh[16];
    if (peer_hex.size() != 32 || !hexToDestHash(peer_hex, dh)) {
        msgFail(id.index, peer_hex, mid, LXMF_ST_BAD_PEER);
        return;
    }
    std::string title    = storageGetStr(msgPath(id.index, peer_hex, mid, "title").c_str(), "");
    std::string content  = storageGetStr(msgPath(id.index, peer_hex, mid, "content").c_str(), "");
    std::string reply_to = storageGetStr(msgPath(id.index, peer_hex, mid, "reply_to").c_str(), "");
    std::string reply_quote =
        storageGetStr(msgPath(id.index, peer_hex, mid, "reply_quote").c_str(), "");
    std::string method   = storageGetStr(msgPath(id.index, peer_hex, mid, "method").c_str(), "");
    std::string pn       = storageGetStr(contactPath(id.index, peer_hex, "pn").c_str(), "");

    uint8_t rt[32]; bool have_rt = hexToBytes(reply_to.c_str(), reply_to.size(), rt, 32);
    uint8_t pnh[16]; bool have_pn = hexToBytes(pn.c_str(), pn.size(), pnh, 16);
    /* An all-zero `pn` is "none set", not a node. */
    if (have_pn) {
        bool nz = false;
        for (int k = 0; k < 16; ++k) if (pnh[k]) { nz = true; break; }
        have_pn = nz;
    }

    /* The timestamp is stamped once and kept: a resend after a reconnect must
     * derive the same message_id on the server as the first attempt did. */
    int ts = storageGetInt(msgPath(id.index, peer_hex, mid, "ts").c_str(), 0);
    if (ts == 0) {
        ts = (int)(wallUnixMs() / 1000);
        storageBegin();
        storageSet(msgPath(id.index, peer_hex, mid, "ts").c_str(), ts);
        storageSet(msgPath(id.index, peer_hex, mid, "body_size").c_str(),
                   (int)content.size());
        storageEnd();
    }

    /* THE PREVIEW IS THE LAST MESSAGE, WHOEVER WROTE IT. The local send path
     * bumps the conversation directory when it packs the message, and a proxied
     * send never packs here — so without this the list only ever caught what
     * the other side sent, and every conversation read as if the peer spoke
     * last. `recv_ts` is the once-marker: bumpConvDirectory returns it and
     * nothing else writes it, so a resend after a reconnect finds it set and
     * does not count the same message twice. */
    if (storageGetInt(msgPath(id.index, peer_hex, mid, "recv_ts").c_str(), 0) == 0) {
        int recv = bumpConvDirectory(id.index, peer_hex, ts, content,
                                     /*inbound=*/false);
        storageSet(msgPath(id.index, peer_hex, mid, "recv_ts").c_str(), recv);
    }

    /* Too big for that server, decided here. HELLO says the ceiling, so there
     * is no reason to spend the air on a body only to be told; and a failure
     * the client resolves itself is one the proxy's verdicts never have to
     * carry, which keeps those verdicts meaning "it tried and could not". */
    const proxy_t& p = s_proxy[id.index];
    if (p.max_envelope_kb && content.size() > (size_t)p.max_envelope_kb * 1024) {
        warn("id %d: %zuB draft over the proxy's %u kB ceiling — not sending",
             id.index, content.size(), (unsigned)p.max_envelope_kb);
        msgFail(id.index, peer_hex, mid, LXMF_ST_TOO_LARGE);
        return;
    }

    std::vector<uint8_t> f = lxmproxyBuildSend(mid, dh, (uint32_t)ts, title, content,
                                               have_rt ? rt : nullptr, reply_quote,
                                               method.c_str(),
                                               have_pn ? pnh : nullptr);
    uint32_t opaque = 0;
    if (!proxySendFrame(id.index, f, &opaque)) {
        /* No Channel, or the send was refused: the message waits locally with
         * no checkmark until one comes back. */
        msgSetStatus(id.index, peer_hex, mid, LXMF_ST_QUEUED);
        queueAdd(id.index, peer_hex, mid);
        return;
    }
    /* HANDING IT OVER IS THE HANDOVER. The Channel is proved end to end and
     * retransmits until it is, so a SEND that has gone out is a SEND the server
     * has — there is no frame to wait for, and asking the server to send one
     * back would put a round trip on a 4 kbps radio to say what this device
     * already knows. From here the server holds the message and this queue has
     * nothing left to attempt; the next thing on the air about it is its
     * outcome.
     *
     * A body too big for one Channel message crosses as a Resource, which is
     * proved part by part and can still fail halfway. That one stays ours —
     * SENDING_TO_PROXY, in the queue — until rnsd echoes the transfer done. */
    if (opaque) {
        s_proxySends.push_back({ opaque, id.index, peer_hex, mid });
        msgSetStatus(id.index, peer_hex, mid, LXMF_ST_SENDING_TO_PROXY);
        queueAdd(id.index, peer_hex, mid);
    } else {
        msgSetStatus(id.index, peer_hex, mid, LXMF_ST_ON_OUR_PROXY);
    }
    dbg("id %d: proxy SEND %s → %s", id.index, mid.c_str(), peer_hex.c_str());
}

/* Ask for a body the server withheld (`lxmf.id.<n>.cmd.fetch = <peer>/<mid>`). */
static void proxyFetch(lxmf_id_t& id, const std::string& peer_hex,
                       const std::string& mid)
{
    uint8_t dh[16], mh[32];
    if (!hexToDestHash(peer_hex, dh)) return;
    if (!hexToBytes(mid.c_str(), mid.size(), mh, 32)) return;
    if (!proxySendFrame(id.index, lxmproxyBuildFetch(mh, dh)))
        warn("id %d: proxy fetch %s not sent (no Channel)", id.index, mid.c_str());
}

/* ── frame dispatch ── */

static void proxyHandleFrame(int n, const LxmproxyFrame& fr)
{
    lxmf_id_t& id = s_ids[n];
    proxy_t&   p  = s_proxy[n];
    if (!id.used) return;
    dbg("id %d: proxy %s", n, lxmproxyFrameName(fr.type));

    switch (fr.type) {
    case LXMPROXY_FR_HELLO: {
        p.label = fr.label;
        p.max_envelope_kb = fr.max_envelope_kb;
        if (fr.serving) {
            /* Already provisioned there — a reconnect, or a client that lost
             * the SERVING confirmation last time. */
            p.serving = true;
            if (p.phase == PROXY_PH_PROVISIONING) {
                proxyUnregisterOurDest(id);
                storageSet(idPath(n, "proxy_role").c_str(), "client");
                p.phase = PROXY_PH_IDLE;
                info("id %d: proxied by %s", n, p.label.c_str());
            }
            proxyPushConfig(n);
        } else if (p.phase == PROXY_PH_PROVISIONING ||
                   proxyRole(n) == PROXY_ROLE_CLIENT) {
            /* Either we are asking it to take the account, or we already think
             * it holds it and it does not — a server that lost the slot. This
             * device still has the key, so hand it over again rather than
             * sitting proxied by something that answers for nobody. */
            if (p.phase != PROXY_PH_PROVISIONING) {
                warn("id %d: %s is not serving this account — handing it over again",
                     n, p.label.c_str());
                p.phase = PROXY_PH_PROVISIONING;
                p.phase_deadline_s = (uint32_t)(nowUnixMs() / 1000) +
                                     LXMF_PROXY_HANDSHAKE_TTL_S;
            }
            /* Hand the account over. The whole retained ratchet set goes with
             * the key: peers encrypt to the ratchet in the last announce they
             * heard, and only that ratchet's private decrypts them, so a server
             * starting fresh could not read anything sent before its own
             * announce reached each peer. */
            uint8_t priv[64] = {};
            std::string hex = storageGetStr(id.identity_key.c_str(), "");
            if (!hexToBytes(hex.c_str(), hex.size(), priv, 64)) {
                err("id %d: proxy handover: no usable private key", n);
                break;
            }
            std::string rk = std::string("secrets.rnsd.ratchets.") +
                             bytesToHex(id.dest_hash, 16);
            std::string ratchets = storageGetStr(rk.c_str(), "");
            std::string dn = storageGetStr(idPath(n, "display_name").c_str(), "");
            info("id %d: proxy handover → %s (%zuB ratchet record)", n,
                 p.label.c_str(), ratchets.size());
            proxySendFrame(n, lxmproxyBuildHandover(priv, dn.c_str(), ratchets));
        } else {
            /* Not serving us and we are not asking it to — the operator has not
             * approved the account yet. Say so and keep the Channel. */
            if (!fr.reason.empty())
                info("id %d: proxy %s: %s", n, p.label.c_str(), fr.reason.c_str());
        }
        proxyPublish(n);
        break;
    }
    case LXMPROXY_FR_SERVING: {
        if (!fr.ok) {
            if (fr.hold) {
                /* Not no — not yet. The operator has not approved this account,
                 * and the server will send a fresh HELLO the moment they do, so
                 * hold the Channel and stay provisioning rather than making the
                 * user ask a second time for a reason they cannot see. The
                 * handshake deadline is suspended because what is being waited
                 * on is a person, not a protocol answer. */
                info("id %d: %s: %s", n, p.label.c_str(),
                     fr.reason.empty() ? "waiting for the operator" : fr.reason.c_str());
                p.phase_deadline_s = 0;
                proxyPublish(n);
                break;
            }
            warn("id %d: proxy refused to serve: %s", n,
                 fr.reason.empty() ? "(no reason)" : fr.reason.c_str());
            p.phase = PROXY_PH_IDLE;
            proxyPublish(n);
            break;
        }
        p.serving = true;
        if (p.phase == PROXY_PH_PROVISIONING) {
            /* Only now — the server is registered and announcing — does this
             * device stop being the registrant. */
            proxyUnregisterOurDest(id);
            storageSet(idPath(n, "proxy_role").c_str(), "client");
            p.phase = PROXY_PH_IDLE;
            /* The ratchet record is the server's now; everything that arrives
             * from here on comes decrypted over the Channel. */
            storageUnset((std::string("secrets.rnsd.ratchets.") +
                          bytesToHex(id.dest_hash, 16)).c_str());
            info("id %d: proxied by %s", n, p.label.c_str());
        }
        proxyPushConfig(n);
        proxyPublish(n);
        break;
    }
    case LXMPROXY_FR_MSG: {
        if (!fr.have_msg_id || !fr.have_peer) break;
        if (proxyStoreInbound(id, fr)) {
            /* Whole, and persisted: the server may stop owing it. */
            proxySendFrame(n, lxmproxyBuildHanded(fr.msg_id, fr.peer));
        }
        break;
    }
    case LXMPROXY_FR_BODY: {
        if (!fr.have_msg_id || !fr.have_peer) break;
        std::string peer_hex = bytesToHex(fr.peer, 16);
        std::string mid_hex  = bytesToHex(fr.msg_id, LXMPROXY_MID_LEN);
        if (!storageExists(msgPath(n, peer_hex, mid_hex, "status").c_str())) {
            warn("id %d: proxy BODY for an unknown message %s", n, mid_hex.c_str());
            break;
        }
        storageBegin();
        storageSet(msgPath(n, peer_hex, mid_hex, "content").c_str(), fr.content.c_str());
        storageSet(msgPath(n, peer_hex, mid_hex, "body_absent").c_str(), 0);
        storageSet(msgPath(n, peer_hex, mid_hex, "body_size").c_str(),
                   (int)fr.content.size());
        if (fr.have_reply_to) {
            storageSet(msgPath(n, peer_hex, mid_hex, "reply_to").c_str(),
                       bytesToHex(fr.reply_to, 32).c_str());
            if (!fr.reply_quote.empty() && fr.reply_quote.size() <= LXMF_REPLY_QUOTE_MAX)
                storageSet(msgPath(n, peer_hex, mid_hex, "reply_quote").c_str(),
                           fr.reply_quote.c_str());
        }
        storageEnd();
        info("id %d: proxy body %s (%zuB)", n, mid_hex.c_str(), fr.content.size());
        proxySendFrame(n, lxmproxyBuildHanded(fr.msg_id, fr.peer));
        break;
    }
    case LXMPROXY_FR_STATUS: {
        if (!fr.have_peer || fr.key.empty()) break;
        std::string peer_hex = bytesToHex(fr.peer, 16);
        /* A verdict for a message this device no longer has. Nothing is sent
         * back — receiving it is what settles it, and the Channel's own proof
         * has already told the server that. Logged, because the outcome itself
         * is now lost and that is worth being able to find. */
        if (!storageExists(msgPath(n, peer_hex, fr.key, "status").c_str())) {
            warn("id %d: proxy status %s for %s/%s — no such record here",
                 n, lxmfStatusName((uint8_t)fr.status), peer_hex.c_str(), fr.key.c_str());
            break;
        }
        uint8_t st = (uint8_t)fr.status;
        storageBegin();
        /* STATUS carries the server-side message_id, which is what maps our
         * local key onto its record. */
        if (fr.have_msg_id)
            storageSet(msgPath(n, peer_hex, fr.key, "message_id").c_str(),
                       bytesToHex(fr.msg_id, LXMPROXY_MID_LEN).c_str());
        storageEnd();
        /* A STATUS is an OUTCOME — the server sends no other kind. A message
         * sent through a proxy has five states, and every way the server's
         * attempt can have ended collapses into one of them. What is lost by
         * collapsing is kept: `proxy_status` holds the verdict verbatim, so the
         * detail page can say which failure it was while the conversation shows
         * one red ✕.
         *
         * Anything that is not an outcome is ignored — the server should not
         * have sent it, this device has nothing to do about a message somebody
         * else is still working on, and the outcome is coming. */
        if (!lxmfStatusIsVerdict(st)) {
            verb("id %d: proxy status %s for %s/%s is not an outcome — ignored",
                 n, lxmfStatusName(st), peer_hex.c_str(), fr.key.c_str());
        } else if (st == LXMF_ST_DELIVERED) {
            msgSetStatus(n, peer_hex, fr.key, LXMF_ST_OUR_PROXY_DELIVERED);
        } else if (st == LXMF_ST_CANCELLED) {
            /* Our own doing — a DROP, or a cancel from either end. It is not a
             * verdict on the recipient, so it keeps its own name. */
            msgSetStatus(n, peer_hex, fr.key, st);
        } else if (st == LXMF_ST_PROXY_REFUSED) {
            /* Not one of the five: the server would not TAKE it. That is
             * trouble reaching the proxy, which is ours to see as itself. */
            msgFail(n, peer_hex, fr.key, st);
        } else {
            storageSet(msgPath(n, peer_hex, fr.key, "proxy_status").c_str(), (int)st);
            msgFail(n, peer_hex, fr.key, LXMF_ST_OUR_PROXY_GAVE_UP);
        }
        /* Nothing goes back. The Channel proved this STATUS the moment it
         * arrived, and an envelope is either proved or the link dies trying —
         * so the server already knows this device has it, and a frame saying so
         * would cost a further 131 bytes out and a proof back, per message. */
        break;
    }
    case LXMPROXY_FR_STATE: {
        storageBegin();
        for (const auto& kv : fr.map) {
            if (kv.first == "announce_s")
                setIntIfChanged(idEphPath(n, "last_announce_s"), atoi(kv.second.c_str()));
            else if (kv.first == "quota")
                setStrIfChanged(idEphPath(n, "proxy_quota"), kv.second.c_str());
            else if (kv.first == "dest")
                setStrIfChanged(idEphPath(n, "dest_hash"), kv.second.c_str());
        }
        storageEnd();
        break;
    }
    case LXMPROXY_FR_RATCHETS: {
        /* The release landed. Take the ratchets back, become the registrant
         * again, and announce at once — the network converges as that announce
         * spreads, and nothing else says the destination moved. */
        if (!fr.ratchets.empty())
            storageSet((std::string("secrets.rnsd.ratchets.") +
                        bytesToHex(id.dest_hash, 16)).c_str(), fr.ratchets.c_str());
        storageSet(idPath(n, "proxy_role").c_str(), "off");
        storageUnset(idPath(n, "proxy_dest").c_str());
        p.phase = PROXY_PH_IDLE;
        p.serving = false;
        p.have_dest = false;
        proxyCloseChannel(n);
        proxyArmReopen(n);
        info("id %d: proxy released — registering here again", n);
        proxyPublish(n);
        break;
    }
    default:
        verb("id %d: proxy: unexpected frame %u", n, fr.type);
        break;
    }
}

/* ── transitions ── */

/* `lxmf.id.<n>.cmd.proxy_on = <32-hex lxmproxy.server dest>`. The client keeps
 * its own destination registered throughout: only SERVING takes it down. */
static void proxyOn(lxmf_id_t& id, const std::string& dest_hex)
{
    proxy_t& p = s_proxy[id.index];
    uint8_t dh[16];
    if (dest_hex.size() != 32 || !hexToBytes(dest_hex.c_str(), 32, dh, 16)) {
        warn("id %d: proxy_on needs a 32-hex lxmproxy.server dest", id.index);
        return;
    }
    if (proxyRole(id.index) == PROXY_ROLE_SERVER) {
        warn("id %d: this identity is hosted here — it cannot also be proxied", id.index);
        return;
    }
    memcpy(p.dest, dh, 16);
    p.have_dest = true;
    storageSet(idPath(id.index, "proxy_dest").c_str(), dest_hex.c_str());
    p.phase = PROXY_PH_PROVISIONING;
    p.phase_deadline_s = (uint32_t)(nowUnixMs() / 1000) + LXMF_PROXY_HANDSHAKE_TTL_S;
    p.backoff_s = LXMF_PROXY_BACKOFF_MIN_S;
    p.next_try_s = 0;
    proxyCloseChannel(id.index);
    proxyOpenChannel(id);
    proxyPublish(id.index);
}

/* `lxmf.id.<n>.cmd.proxy_off`. The client stays proxied — fully working, the
 * server still delivering — until RATCHETS comes back. */
static void proxyOff(lxmf_id_t& id)
{
    proxy_t& p = s_proxy[id.index];
    /* Still asking, not yet proxied — this device is the registrant and always
     * was, so stopping is local and immediate. It is also the only way out of a
     * wait on an operator who never answers. */
    if (proxyRole(id.index) != PROXY_ROLE_CLIENT) {
        if (p.phase != PROXY_PH_PROVISIONING) return;
        info("id %d: no longer asking %s to take this account", id.index,
             p.label.c_str());
        p.phase = PROXY_PH_IDLE;
        p.have_dest = false;
        storageUnset(idPath(id.index, "proxy_dest").c_str());
        proxyCloseChannel(id.index);
        proxyPublish(id.index);
        return;
    }
    p.phase = PROXY_PH_RELEASING;
    p.phase_deadline_s = (uint32_t)(nowUnixMs() / 1000) + LXMF_PROXY_HANDSHAKE_TTL_S;
    if (!proxySendFrame(id.index, lxmproxyBuildRelease()))
        info("id %d: release will go out when the Channel is back", id.index);
    proxyPublish(id.index);
}

/* `lxmf.id.<n>.cmd.proxy_force_off` — the dead-server override, and what it
 * costs, stated where an operator can read it:
 *
 *  - if the server ever returns it will register and announce the same address,
 *    and nothing on the network can tell the two apart;
 *  - anything it still holds is stranded — undelivered outbound, and inbound it
 *    accepted but never handed over;
 *  - the key is still on that box, and there is no revocation short of a new
 *    identity, which is a new address;
 *  - the ratchet state is stranded with it: until peers hear this device's
 *    fresh announce, what they send is encrypted to ratchets it does not hold. */
static void proxyForceOff(lxmf_id_t& id)
{
    proxy_t& p = s_proxy[id.index];
    warn("id %d: proxy force-off — the server keeps the key, the ratchets and "
         "anything it still holds", id.index);
    storageSet(idPath(id.index, "proxy_role").c_str(), "off");
    storageUnset(idPath(id.index, "proxy_dest").c_str());
    p.phase = PROXY_PH_IDLE;
    p.serving = false;
    p.have_dest = false;
    proxyCloseChannel(id.index);
    proxyArmReopen(id.index);
    proxyPublish(id.index);
}

/* ── heard servers (the picker) ── */

static void onProxyAnnSubDisc(int /*ref*/)
{
    warn("proxy announce sub: disconnected from rnsd");
    s_proxy_ann_handle = -1;
}

static void onProxyAnnounce(int handle, size_t /*bytesAvail*/)
{
    if (handle != s_proxy_ann_handle) return;
    uint8_t buf[512];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n <= LXMF_ANNOUNCE_HDR) return;
    std::string label;
    if (!lxmproxyParseAnnounce(buf + LXMF_ANNOUNCE_HDR, n - LXMF_ANNOUNCE_HDR, label))
        return;
    std::string hex = bytesToHex(buf + 1, 16);          /* dest_hash */
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    if (s_proxyHeard.find(hex) == s_proxyHeard.end() &&
        s_proxyHeard.size() >= LXMF_PROXY_MAX_HEARD) {
        /* Bounded: drop the least recently heard to make room. */
        auto oldest = s_proxyHeard.begin();
        for (auto it = s_proxyHeard.begin(); it != s_proxyHeard.end(); ++it)
            if (it->second.last_s < oldest->second.last_s) oldest = it;
        storageDeleteTree(("lxmf.proxies." + oldest->first).c_str());
        s_proxyHeard.erase(oldest);
    }
    s_proxyHeard[hex] = { label, now_s };
    storageBegin();
    storageSet(("lxmf.proxies." + hex + ".label").c_str(), label.c_str());
    storageSet(("lxmf.proxies." + hex + ".last").c_str(),  (int)now_s);
    storageEnd();
    proxyPublishHeard();
    DBG_REMOTE("proxy announce: %s \"%s\"", hex.c_str(), label.c_str());
}

static bool connectProxyAnnounceSub(void)
{
    if (s_proxy_ann_handle >= 0) return true;
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, LXMPROXY_ASPECT, sizeof(req.aspect));
    int h = itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof(req),
                       pdMS_TO_TICKS(2000), /*ref*/ 0,
                       onProxyAnnounce, onProxyAnnSubDisc);
    if (h < 0) { warn("proxy announce sub: connect failed"); return false; }
    s_proxy_ann_handle = h;
    /* Publish the (possibly empty) picker line now, so the settings form says
     * "none heard yet" rather than showing a blank where a list belongs. */
    proxyPublishHeard();
    info("proxy announce sub: connected (aspect=%s)", LXMPROXY_ASPECT);
    return true;
}

/* ── Resources on the proxy Channel ──
 * A frame too big for one Channel message arrives here instead. rnsd names the
 * hidden Link, and `rnsd.chan.byid.<link_id>` maps that to the channel tag —
 * which is how a proxy Resource is told from every other consumer's. */
static bool proxyResourceAux(const rnsd_link_resource_done_t& d)
{
    if (d.opcode == RNSD_LINK_RESOURCE_OUTBOUND_DONE ||
        d.opcode == RNSD_LINK_RESOURCE_FAILED) {
        if (d.opaque_id < LXMF_PROXY_OPAQUE_BASE) return false;
        bool ok = d.opcode == RNSD_LINK_RESOURCE_OUTBOUND_DONE;
        /* A SEND that crossed as a Resource ends here, and only here. The
         * transfer completing IS the server having the message; the transfer
         * failing puts it back in this device's queue to try again. Every other
         * frame is answered by the protocol (HANDED / STATUS) or by the
         * Channel's own proof, and has nothing to settle on the transfer. */
        for (size_t i = 0; i < s_proxySends.size(); ++i) {
            if (s_proxySends[i].opaque != d.opaque_id) continue;
            proxy_send_t ps = s_proxySends[i];
            s_proxySends.erase(s_proxySends.begin() + i);
            if (ok) {
                msgSetStatus(ps.id_index, ps.peer, ps.mid, LXMF_ST_ON_OUR_PROXY);
            } else {
                msgSetStatus(ps.id_index, ps.peer, ps.mid, LXMF_ST_QUEUED);
                queueAdd(ps.id_index, ps.peer, ps.mid);
            }
            return true;
        }
        if (!ok)
            warn("proxy: outbound resource failed (opaque=%u)", (unsigned)d.opaque_id);
        return true;
    }
    if (d.opcode != RNSD_LINK_RESOURCE_INBOUND_DONE) return false;
    std::string tag = storageGetStr(
        ("rnsd.chan.byid." + bytesToHex(d.link_id, 16)).c_str(), "");
    if (tag.empty()) return false;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_proxy[n].tag != tag) continue;
        LxmproxyFrame fr;
        if (d.buf && d.len && lxmproxyParse((const uint8_t*)d.buf, d.len, fr))
            proxyHandleFrame(n, fr);
        else
            warn("id %d: proxy resource frame malformed (%uB)", n, (unsigned)d.len);
        return true;
    }
    return false;
}

/* ── 1 Hz housekeeping ── */

static void proxyClientTick(void)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    if (s_proxy_ann_handle < 0) connectProxyAnnounceSub();

    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        lxmf_id_t& id = s_ids[n];
        proxy_t&   p  = s_proxy[n];
        if (!id.used) continue;
        proxyPublish(n);   /* the pane's rows follow the role and the Channel */

        /* A deferred re-registration whose deregistration has had time to land
         * on rnsd's own task. connectOurDest re-issues the inbound-Link listen. */
        if (p.reopen_at_s && now_s >= p.reopen_at_s) {
            p.reopen_at_s = 0;
            if (id.handle < 0 && proxyRole(n) != PROXY_ROLE_CLIENT) {
                if (connectOurDest(id)) sendAnnounce(id);
            }
        }

        proxy_role_t role = proxyRole(n);
        bool want_channel = (role == PROXY_ROLE_CLIENT) ||
                            (p.phase != PROXY_PH_IDLE);
        if (!want_channel) {
            if (p.handle >= 0) proxyCloseChannel(n);
            continue;
        }

        if (p.handle < 0) {
            if (now_s >= p.next_try_s) {
                if (!proxyOpenChannel(id))
                    p.next_try_s = now_s + LXMF_PROXY_BACKOFF_MIN_S;
            }
            continue;
        }

        /* Channel state comes from rnsd's own publication; the first transition
         * to active is what makes the protocol runnable. */
        std::string st = storageGetStr(("rnsd.chan." + p.tag + ".state").c_str(), "");
        if (!p.active && st == "active") {
            p.active = true;
            p.backoff_s = LXMF_PROXY_BACKOFF_MIN_S;
            info("id %d: proxy Channel active (%s)", n, p.tag.c_str());
            /* The server speaks first (HELLO), so there is nothing to send —
             * except a RELEASE the last Channel could not carry. */
            if (p.phase == PROXY_PH_RELEASING)
                proxySendFrame(n, lxmproxyBuildRelease());
            proxyPublish(n);
        } else if (p.active && (st == "failed" || st == "closed" || st.empty())) {
            proxyCloseChannel(n);
            p.next_try_s = now_s + p.backoff_s;
            p.backoff_s  = p.backoff_s * 2 > LXMF_PROXY_BACKOFF_MAX_S
                         ? LXMF_PROXY_BACKOFF_MAX_S : p.backoff_s * 2;
            proxyPublish(n);
            continue;
        }

        /* A handshake that draws no answer returns to the state it started in —
         * which is always the one that is registered somewhere. */
        if (p.phase != PROXY_PH_IDLE && p.phase_deadline_s &&
            now_s >= p.phase_deadline_s) {
            warn("id %d: proxy %s handshake timed out", n,
                 p.phase == PROXY_PH_PROVISIONING ? "provisioning" : "release");
            p.phase = PROXY_PH_IDLE;
            p.phase_deadline_s = 0;
            proxyPublish(n);
        }

        /* Account settings are edited here and held there; push on change. */
        bool ready = proxyReady(n);
        if (ready && proxyConfigSig(n) != p.cfg_sig) proxyPushConfig(n);

        /* The Channel just came back and there is outbound waiting on it. Sweep
         * now rather than at the next interval: the queue's cadence is sized
         * for a peer that may be unreachable for hours, and this is a server
         * that is demonstrably there. The sweep runs later in this same pass. */
        if (ready && !p.was_ready && s_queueNextSweep_s) {
            s_queueNextSweep_s = now_s;
            dbg("id %d: proxy back — sweeping the delivery queue now", n);
        }
        p.was_ready = ready;
    }
}

/* Drop every Channel — the ecosystem is going down. The durable role is
 * untouched: a stopped client is still a proxied client. */
static void proxyTeardown(void)
{
    if (s_proxy_ann_handle >= 0) {
        itsDisconnect(s_proxy_ann_handle);
        s_proxy_ann_handle = -1;
    }
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) proxyCloseChannel(n);
}

/* ─────────────── classic propagation-node (PN) client ───────────────
 *
 * Send and receive through standard `lxmf.propagation` store-and-forward
 * nodes, wire-compatible with the reference LXMF 0.9.8 LXMRouter:
 *
 *  upload — an explicit resend "via node" (cmd.send value
 *    `<peer>/<key>/pn:<hash>`): the packed wire's tail is
 *    destination-encrypted (rnsdEncryptFor), the node's announced
 *    propagation stamp is paid (PoW over the transient id, 1000-round
 *    workblock), and msgpack([timebase, [lxmf_data]]) goes over a fresh
 *    lxmf.propagation link — one link packet when it fits, else a
 *    Resource. The node's packet proof / resource ACK settles ON_PN
 *    (terminal: a propagation node issues no delivery proof; the
 *    recipient pulls the message on its own next sync).
 *
 *  sync — the node list (s.lxmf.pn.<i>.{hash,name,check}, client-owned,
 *    index-ordered, hash "" = free slot) is checked periodically
 *    (s.lxmf.pn.check_interval_s) and on demand (lxmf.cmd.pn_sync). One
 *    session at a time walks a (node × identity) queue: open a link,
 *    identify with the identity's key (the node serves only the
 *    lxmf.delivery dest derived from it), then loop `/get` request
 *    rounds — [nil,nil] lists transient ids; [wants,haves,limit_kb]
 *    fetches blobs (the node deletes the haves); [nil,got] confirms
 *    delivery so the node deletes what we ingested — until a round
 *    yields nothing new. Each blob is dest16 || destination-encrypted
 *    src+sig+payload; decrypt with the identity and feed the normal
 *    inbound pipeline. ITS_MAX_MSG_DATA caps a request at ~8 inline
 *    transient ids, hence the small per-round bounds and the loop.
 *
 * All state is plain statics touched only on the lxmf task. */

#define LXMF_PN_MAX             8     /* s.lxmf.pn.<i> list indices scanned */
#define LXMF_PN_UP_SESSIONS     2     /* concurrent upload links */
#define LXMF_PN_UP_TTL_S        90    /* upload with no settle → fail */
#define LXMF_PN_SYNC_TTL_S      180   /* whole sync session budget */
#define LXMF_PN_SYNC_ROUNDS_MAX 10    /* list/fetch/delete cycles per session */
#define LXMF_PN_PKT_MAX         360   /* upload bytes that ride one link packet */
#define LXMF_PN_STAMP_MAX_COST  20    /* refuse to grind PoW past this */
#define LXMF_PN_FETCH_MAX       6     /* wants per /get round (ITS aux budget) */
#define LXMF_PN_HAVES_MAX       2     /* haves per /get round (ditto) */
#define LXMF_PN_SEEN_TIDS_MAX   512   /* transient-id dedup set bound */

struct pn_node_t { std::string hex; std::string name; bool check; };

/* ---- the propagation-node collection ----
 *
 * The settings surfaces never write s.lxmf.pn. They write lxmf.pnode.add /
 * .remove / .set / .order and this file applies them, so one description drives
 * both surfaces and the hash check lives in one place instead of being a regex
 * in each. A rejection is a sentence on lxmf.pnode.error.
 *
 * The node's own 32-hex destination hash IS its id: it is what identifies the
 * node everywhere else (the status keys, the sync queue), it is unique by
 * construction, and it is safe as a key segment. */

static void pnError(const char* why) { storageSet("lxmf.pnode.error", why); }

/** Accepted-mutation ack, shared by the lxmf.pnode.* sentinels: the open form
 *  closes when this moves. Monotonic per boot — never a read-increment, since
 *  reads see the committed tree behind the actor's queue. */
static void pnAck()
{
    static int ack = 0;
    storageSet("lxmf.pnode.done", ++ack);
}

static std::string pnField(int idx, const char* field)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.%s", idx, field);
    return storageGetStr(k, "");
}

static int pnIndexOfHash(const std::string& hex)
{
    if (hex.empty()) return -1;
    for (int i = 0; i < LXMF_PN_MAX; ++i) if (pnField(i, "hash") == hex) return i;
    return -1;
}

static int pnCount()
{
    int n = 0;
    while (n < LXMF_PN_MAX && pnField(n, "hash").size() == 32) n++;
    return n;
}

static void pnWrite(int idx, const std::string& hex, const std::string& name, bool check)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.hash",  idx); storageSet(k, hex.c_str());
    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.name",  idx); storageSet(k, name.c_str());
    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.check", idx); storageSet(k, check ? 1 : 0);
}

/** Why this node is unacceptable, or "" if it is fine. */
static std::string pnRejection(const std::string& hex)
{
    if (hex.empty()) return "A propagation node needs its destination hash.";
    if (hex.size() != 32) return "A destination hash is exactly 32 hex characters.";
    for (char c : hex)
        if (!std::isxdigit((unsigned char)c)) return "A destination hash is hex only.";
    return "";
}

/** One status pill per node, as packed "text|colour" — the last check's outcome
 *  in the words the row shows, so neither surface formats a timestamp or maps
 *  an error string to a colour. */
static void pnPublishStatus(void)
{
    std::string syncing = storageGetStr("lxmf.pn.sync", "");
    for (int i = 0; i < LXMF_PN_MAX; ++i) {
        std::string hex = pnField(i, "hash");
        if (hex.size() != 32) continue;
        std::string err = storageGetStr(("lxmf.pn." + hex + ".last_err").c_str(), "");
        const char* pill;
        std::string composed;
        if (hex == syncing)                                  pill = "checking|amber";
        else if (!err.empty())    { composed = err + "|red"; pill = composed.c_str(); }
        else if (pnField(i, "check") == "1")                 pill = "checked|green";
        else                                                 pill = "";
        setStrIfChanged("lxmf.pnstat." + hex, pill);
    }
}

static void onPnodeAdd(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string payload = val;
    storageUnset(key);
    cJSON* o = cJSON_Parse(payload.c_str());
    cJSON* h = o ? cJSON_GetObjectItem(o, "hash") : nullptr;
    cJSON* nm = o ? cJSON_GetObjectItem(o, "name") : nullptr;
    std::string hex  = cJSON_IsString(h)  ? h->valuestring  : "";
    std::string name = cJSON_IsString(nm) ? nm->valuestring : "";
    if (o) cJSON_Delete(o);
    for (char& c : hex) c = (char)std::tolower((unsigned char)c);
    std::string why = pnRejection(hex);
    if (!why.empty())              { pnError(why.c_str()); return; }
    if (pnIndexOfHash(hex) >= 0)   { pnError("That node is already configured."); return; }
    int n = pnCount();
    if (n >= LXMF_PN_MAX)          { pnError("The node list is full."); return; }
    storageBegin();
    pnWrite(n, hex, name, true);
    pnError("");
    storageEnd();
    pnAck();
    info("pnode add: %s at %d", hex.c_str(), n);
}

static void onPnodeSet(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string payload = val;
    storageUnset(key);
    cJSON* o = cJSON_Parse(payload.c_str());
    cJSON* nm = o ? cJSON_GetObjectItem(o, "name") : nullptr;
    cJSON* ck = o ? cJSON_GetObjectItem(o, "check") : nullptr;
    cJSON* id = o ? cJSON_GetObjectItem(o, "_id") : nullptr;
    std::string name  = cJSON_IsString(nm) ? nm->valuestring : "";
    std::string check = cJSON_IsString(ck) ? ck->valuestring : "0";
    std::string hex   = cJSON_IsString(id) ? id->valuestring : "";
    if (o) cJSON_Delete(o);
    int idx = pnIndexOfHash(hex);
    if (idx < 0) { pnError("That node is no longer configured."); return; }
    storageBegin();
    /* The hash is the identity, so an editor changes the name and the check
     * flag and nothing else — a different hash is a different node. */
    pnWrite(idx, hex, name, check == "1");
    pnError("");
    storageEnd();
    pnAck();
}

/** Drop a node, compacting the list so it stays contiguous. */
static void onPnodeRemove(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string hex = val;
    storageUnset(key);
    int idx = pnIndexOfHash(hex), n = pnCount();
    if (idx < 0) { pnError("That node is no longer configured."); return; }
    storageBegin();
    for (int i = idx; i < n - 1; i++)
        pnWrite(i, pnField(i + 1, "hash"), pnField(i + 1, "name"),
                pnField(i + 1, "check") == "1");
    char tail[48];
    std::snprintf(tail, sizeof tail, "s.lxmf.pn.%d", n - 1);
    storageDeleteTree(tail);
    pnError("");
    storageEnd();
    pnAck();
    info("pnode remove: %s", hex.c_str());
}

/** An id order applied as a preference permutation — recognized hashes move
 *  into that relative order, the rest keep their place. */
static void onPnodeOrder(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string csv = val;
    storageUnset(key);
    int n = pnCount();
    if (n <= 1) return;
    std::vector<std::string> wanted;
    for (size_t pos = 0; pos <= csv.size(); ) {
        size_t comma = csv.find(',', pos);
        wanted.push_back(csv.substr(pos, comma == std::string::npos ? comma : comma - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::vector<pn_node_t> items(n);
    for (int i = 0; i < n; i++) {
        items[i].hex   = pnField(i, "hash");
        items[i].name  = pnField(i, "name");
        items[i].check = pnField(i, "check") == "1";
    }
    std::vector<int> slots, order;
    for (int i = 0; i < n; i++)
        for (const std::string& w : wanted)
            if (items[i].hex == w) { slots.push_back(i); break; }
    for (const std::string& w : wanted)
        for (int i = 0; i < n; i++)
            if (items[i].hex == w) { order.push_back(i); break; }
    if (slots.size() != order.size() || slots.empty()) return;
    storageBegin();
    for (size_t s = 0; s < slots.size(); s++)
        pnWrite(slots[s], items[order[s]].hex, items[order[s]].name, items[order[s]].check);
    pnError("");
    storageEnd();
    pnAck();
}

/* ---- identity create / import, as validating sentinels ----
 *
 * The two forms submit here rather than at lxmf.cmd.identity_*, which take bare
 * strings and answer only in the log. These take the form's field object, check
 * it, and report on their own <cmd>.error — so "128 hex characters" is stated
 * once, by the code that needs it, instead of as a regex in each UI. */

/* Both identity forms answer on their own `<cmd>.error` / `<cmd>.done`, and a
 * storage subscription is PREFIX-matched — so those two writes land right back
 * in the handler that made them. Without the exact-key test the first rejection
 * re-enters here with the error sentence as its payload, fails to parse a name
 * out of it, writes the same sentence again, and the device spends the rest of
 * its uptime doing that: an unbroken run of "notify drop: lxmf.identity.new.error"
 * in the log, and a form that never closes. The ack loops the same way, so a
 * SUCCESSFUL submission jams too. Same guard as ntp.tz.set (spangap-net). */
static void onIdentityNew(const char* key, const char* val)
{
    if (!key || std::strcmp(key, "lxmf.identity.new") != 0) return;
    if (!val || !*val) return;
    std::string payload = val;
    storageUnset(key);
    cJSON* o = cJSON_Parse(payload.c_str());
    cJSON* nm = o ? cJSON_GetObjectItem(o, "name") : nullptr;
    std::string name = cJSON_IsString(nm) ? nm->valuestring : "";
    if (o) cJSON_Delete(o);
    if (name.empty()) { storageSet("lxmf.identity.new.error", "An identity needs a display name."); return; }
    storageSet("lxmf.cmd.identity_new", name.c_str());
    /* Accepted (queued for creation): the form closes on the bump. */
    static int ack = 0;
    storageSet("lxmf.identity.new.done", ++ack);
}

static void onIdentityImport(const char* key, const char* val)
{
    if (!key || std::strcmp(key, "lxmf.identity.import") != 0) return;   /* see onIdentityNew */
    if (!val || !*val) return;
    std::string payload = val;
    storageUnset(key);
    cJSON* o = cJSON_Parse(payload.c_str());
    cJSON* k = o ? cJSON_GetObjectItem(o, "privkey") : nullptr;
    std::string hex = cJSON_IsString(k) ? k->valuestring : "";
    if (o) cJSON_Delete(o);
    for (char& c : hex) c = (char)std::tolower((unsigned char)c);
    if (hex.size() != 128) {
        storageSet("lxmf.identity.import.error", "A private key is exactly 128 hex characters.");
        return;
    }
    for (char c : hex)
        if (!std::isxdigit((unsigned char)c)) {
            storageSet("lxmf.identity.import.error", "A private key is hex only.");
            return;
        }
    storageSet("lxmf.cmd.identity_import", hex.c_str());
    /* Accepted (queued for import): the form closes on the bump. */
    static int ack = 0;
    storageSet("lxmf.identity.import.done", ++ack);
}

/* The "use a proxy" form, one per identity slot: `lxmf.id.<n>.setproxy` takes
 * {"dest":"<32-hex>"} and answers on the .error / .done pair. Named apart from
 * the published `proxy_*` keys because a storage subscription is PREFIX-matched
 * and those are written by this task — a shared prefix would have every publish
 * re-enter the handler. Same exact-key guard as onIdentityNew. */
static void onSetProxy(int n, const char* key, const char* val)
{
    char want[40];
    std::snprintf(want, sizeof want, "lxmf.id.%d.setproxy", n);
    if (!key || std::strcmp(key, want) != 0) return;
    if (!val || !*val) return;
    std::string payload = val;
    storageUnset(key);
    cJSON* o = cJSON_Parse(payload.c_str());
    cJSON* d = o ? cJSON_GetObjectItem(o, "dest") : nullptr;
    std::string dest = cJSON_IsString(d) ? d->valuestring : "";
    if (o) cJSON_Delete(o);
    for (char& c : dest) c = (char)std::tolower((unsigned char)c);

    std::string err_key = std::string(want) + ".error";
    if (dest.size() != 32 ||
        dest.find_first_not_of("0123456789abcdef") != std::string::npos) {
        storageSet(err_key.c_str(),
                   "A proxy server's address is 32 hexadecimal characters.");
        return;
    }
    lxmf_id_t* id = idAt(n);
    if (!id || !id->used) {
        storageSet(err_key.c_str(), "There is no identity in that slot.");
        return;
    }
    if (proxyRole(n) == PROXY_ROLE_SERVER) {
        storageSet(err_key.c_str(),
                   "This identity is hosted here for another device; it cannot "
                   "also be proxied.");
        return;
    }
    storageSet(err_key.c_str(), "");
    proxyOn(*id, dest);
    static int ack = 0;
    storageSet((std::string(want) + ".done").c_str(), ++ack);
}

static void onSetProxy0(const char* k, const char* v) { onSetProxy(0, k, v); }
static void onSetProxy1(const char* k, const char* v) { onSetProxy(1, k, v); }
static void onSetProxy2(const char* k, const char* v) { onSetProxy(2, k, v); }
static void onSetProxy3(const char* k, const char* v) { onSetProxy(3, k, v); }
static storage_change_cb_t s_setproxy_stubs[LXMF_MAX_IDENTITIES] = {
    onSetProxy0, onSetProxy1, onSetProxy2, onSetProxy3,
};

/* The configured node list, in index order, malformed/empty slots skipped. */
static std::vector<pn_node_t> pnNodeList()
{
    std::vector<pn_node_t> out;
    for (int i = 0; i < LXMF_PN_MAX; ++i) {
        char k[40];
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.hash", i);
        std::string h = storageGetStr(k, "");
        uint8_t dh[16];
        if (!hexToBytes(h.c_str(), h.size(), dh, 16)) continue;
        pn_node_t nd;
        nd.hex = h;
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.name", i);
        nd.name = storageGetStr(k, "");
        std::snprintf(k, sizeof k, "s.lxmf.pn.%d.check", i);
        nd.check = storageGetInt(k, 1) != 0;
        out.push_back(std::move(nd));
    }
    return out;
}

/* Parse a propagation-node announce app_data — reference 0.9.8 msgpack
 * [legacy_false, timebase, node_active, per_transfer_kb, per_sync_kb,
 *  [stamp_cost, flexibility, peering_cost], metadata] — for the fields a
 * client needs. Returns false when the shape doesn't validate. */
static bool pnParseNodeAppData(const uint8_t* p, size_t n, bool& active,
                               int& stamp_cost, uint32_t& transfer_kb)
{
    if (!p || n < 2) return false;
    mpScan s{p, n, 0, 0, 0};
    uint8_t b = p[s.i++];
    size_t cnt;
    uint64_t v;
    if (b >= 0x90 && b <= 0x9F) cnt = b & 0x0F;
    else if (b == 0xDC) { if (!mpReadBe(s, 2, v)) return false; cnt = (size_t)v; }
    else return false;
    if (cnt < 7) return false;
    if (!mpScanNext(s)) return false;                  /* [0] legacy flag */
    if (!mpScanNext(s)) return false;                  /* [1] node timebase */
    if (s.i >= s.n) return false;
    active = (p[s.i] == 0xC3);                         /* [2] node state */
    if (!mpScanNext(s)) return false;
    uint64_t lim = 0;                                  /* [3] per-transfer kB */
    if (mpReadUint(s, lim)) transfer_kb = (uint32_t)lim;
    else { transfer_kb = 0; if (!mpScanNext(s)) return false; }
    if (!mpScanNext(s)) return false;                  /* [4] per-sync limit */
    stamp_cost = -1;                                   /* [5] [cost, flex, peering] */
    if (s.i < s.n && (p[s.i] & 0xF0) == 0x90 && (p[s.i] & 0x0F) >= 1) {
        ++s.i;
        uint64_t c = 0;
        if (mpReadUint(s, c)) stamp_cost = (int)c;
    }
    return true;
}

/* The node's last-heard announce, via rnsd's identity cache. */
static bool pnNodeInfo(const uint8_t node[16], bool& active,
                       int& stamp_cost, uint32_t& transfer_kb)
{
    uint8_t app[512];
    size_t  len = sizeof(app);
    if (!rnsdRecallAppData(node, app, &len)) return false;
    return pnParseNodeAppData(app, len, active, stamp_cost, transfer_kb);
}

/* ── upload (PROPAGATED) sessions ── */

struct pn_up_t {
    bool        used = false;
    int         handle = -1;
    int         id_index = -1;
    std::string peer, mid;
    uint8_t     node[16] = {};
    std::string tag;
    bool        is_resource = false;
    uint32_t    opaque = 0;              /* resource-settle correlation */
    int         proof_base_proven = 0;   /* packet-settle counters, baselined */
    int         proof_base_timeouts = 0;
    uint32_t    started_s = 0;
};
static pn_up_t s_pnUps[LXMF_PN_UP_SESSIONS];
/* Opaque ids above the uint16 send_id space so aux matching can never
 * collide. */
static uint32_t s_pnOpaque = 0x20000;
static uint16_t s_pnTagSeq = 0;

/* Settle the upload either way: the record goes terminal (tries=255, wire
 * dropped) — ON_PN is final because a propagation node never proves
 * delivery to the sender. */
static void pnUpSettle(pn_up_t& s, uint8_t status)
{
    int n = s.id_index;
    std::string peer = s.peer, mid = s.mid;
    int h = s.handle;
    s = pn_up_t{};
    if (h >= 0) itsDisconnect(h);
    msgFail(n, peer, mid, status);
    lxmf_id_t* id = idAt(n);
    if (id && id->used) {
        if (status == LXMF_ST_ON_PN) id->sent++;
        else                         id->failed++;
    }
    info("id %d: pn upload %s → %s", n, mid.c_str(), lxmfStatusName(status));
}

static pn_up_t* pnUpByHandle(int handle)
{
    for (auto& s : s_pnUps)
        if (s.used && s.handle == handle) return &s;
    return nullptr;
}

/* The node's only traffic to an uploader is an error signal: a link packet
 * carrying msgpack([errcode]) right before it tears the link down. */
static void onPnUpRecv(int handle, size_t /*bytesAvail*/)
{
    pn_up_t* s = pnUpByHandle(handle);
    if (!s) return;
    PSRAM_BSS static uint8_t buf[512];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (!n) return;
    if ((buf[0] & 0xF0) != 0x90) return;
    mpScan sc{buf, n, 1, 0, 0};
    uint64_t code = 0;
    if (!mpReadUint(sc, code)) return;
    warn("id %d: pn node signalled error 0x%02x for %s", s->id_index,
         (unsigned)code, s->mid.c_str());
    pnUpSettle(*s, LXMF_ST_PN_REJECTED);
}

static void onPnUpDisc(int ref)
{
    if (ref < 0 || ref >= LXMF_PN_UP_SESSIONS) return;
    pn_up_t& s = s_pnUps[ref];
    if (!s.used) return;
    s.handle = -1;                    /* link is gone — no disconnect needed */
    /* The node may prove the packet and drop the link before the 1 Hz poll
     * ran — check the proof counter before declaring failure. */
    if (!s.is_resource &&
        storageGetInt(("rnsd.links." + s.tag + ".tx_proven").c_str(), 0) >
            s.proof_base_proven) {
        pnUpSettle(s, LXMF_ST_ON_PN);
        return;
    }
    verb("id %d: pn upload link closed pre-settle (%s)", s.id_index, s.tag.c_str());
    pnUpSettle(s, LXMF_ST_PN_FAIL);
}

/* Upload the message to `node`. Explicit-only — invoked from cmd.send's
 * `pn:<hash>` via segment; there is no automatic fallback. Owns every
 * failure settle. */
static void pnUploadStart(lxmf_id_t& id, const std::string& peer_hex,
                          const std::string& mid, const uint8_t node[16])
{
    for (const auto& s : s_pnUps)
        if (s.used && s.peer == peer_hex && s.mid == mid) return;  /* already going */
    pn_up_t* slot = nullptr;
    for (auto& s : s_pnUps) if (!s.used) { slot = &s; break; }
    if (!slot) {
        warn("id %d: pn upload %s refused: sessions busy", id.index, mid.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_OUTBOX_FULL);
        return;
    }
    uint8_t dh[16];
    if (peer_hex.size() != 32 || !hexToDestHash(peer_hex, dh)) {
        msgFail(id.index, peer_hex, mid, LXMF_ST_BAD_PEER);
        return;
    }
    if (!idEnabled(id.index)) {
        msgFail(id.index, peer_hex, mid, LXMF_ST_DISABLED);
        return;
    }

    std::vector<uint8_t> wire;
    std::string msg_id_hex;
    if (!resolveOutboundWire(id, peer_hex, mid, dh, wire, msg_id_hex)) return;

    /* lxmf_data = dest16 || destination-encrypted(src+sig+payload) — the
     * node stores the blob blind, keyed by its SHA-256 (the transient id,
     * computed before the stamp is appended). */
    uint8_t pk[RNSD_PUBKEY_LEN];
    if (!rnsdRecallPubkey(dh, pk)) {
        rnsdRequestPath(dh);
        warn("id %d: pn upload %s: recipient identity unknown", id.index, mid.c_str());
        msgFail(id.index, peer_hex, mid, LXMF_ST_PN_FAIL);
        return;
    }
    std::vector<uint8_t> lxmf_data(LXMF_DEST_HASH_LEN +
                                   (wire.size() - LXMF_DEST_HASH_LEN) +
                                   RNSD_ENCRYPT_OVERHEAD);
    std::memcpy(lxmf_data.data(), wire.data(), LXMF_DEST_HASH_LEN);
    size_t ct_len = lxmf_data.size() - LXMF_DEST_HASH_LEN;
    if (!rnsdEncryptFor(pk, dh, wire.data() + LXMF_DEST_HASH_LEN,
                        wire.size() - LXMF_DEST_HASH_LEN,
                        lxmf_data.data() + LXMF_DEST_HASH_LEN, &ct_len)) {
        msgFail(id.index, peer_hex, mid, LXMF_ST_PN_FAIL);
        return;
    }
    lxmf_data.resize(LXMF_DEST_HASH_LEN + ct_len);
    uint8_t tid[RNSD_HASH_LEN];
    rnsdSha256(lxmf_data.data(), lxmf_data.size(), tid);

    /* The node's announced propagation stamp cost (PoW over the transient
     * id, 1000-round workblock). Unknown announce → send unstamped and let
     * a cost-enforcing node reject rather than guess a cost. */
    bool active = false;
    int cost = -1;
    uint32_t transfer_kb = 0;
    if (!pnNodeInfo(node, active, cost, transfer_kb))
        warn("id %d: pn %s announce unknown — sending unstamped",
             id.index, bytesToHex(node, 8).c_str());
    if (cost > LXMF_PN_STAMP_MAX_COST) {
        warn("id %d: pn stamp cost %d > max %d — sending unstamped",
             id.index, cost, LXMF_PN_STAMP_MAX_COST);
        cost = 0;
    }
    if (cost > 0) {
        uint8_t stamp[LXMF_STAMP_LEN];
        if (lxmfStampGenerate(tid, cost, stamp, stampYield, nowUnixMs,
                              LXMF_STAMP_ROUNDS_PN))
            lxmf_data.insert(lxmf_data.end(), stamp, stamp + LXMF_STAMP_LEN);
    }

    /* propagation_packed = msgpack([timebase, [lxmf_data]]). */
    std::vector<uint8_t> packed;
    packed.reserve(lxmf_data.size() + 16);
    mpPackArrayHeader(packed, 2);
    mpPackFloat64(packed, (double)wallUnixMs() / 1000.0);
    mpPackArrayHeader(packed, 1);
    mpPackBin(packed, lxmf_data.data(), lxmf_data.size());

    if (transfer_kb > 0 && packed.size() > (size_t)transfer_kb * 1000) {
        warn("id %d: pn upload %s exceeds node transfer limit (%zu B > %u kB)",
             id.index, mid.c_str(), packed.size(), (unsigned)transfer_kb);
        msgFail(id.index, peer_hex, mid, LXMF_ST_TOO_LARGE);
        return;
    }

    char tag[24];
    std::snprintf(tag, sizeof(tag), "lxpn.%04x", (unsigned)s_pnTagSeq++);
    int h = rnsdLinkOpen(node, "lxmf.propagation", id.identity_key.c_str(), tag,
                         /*path_timeout_ms=*/15000, /*link_timeout_ms=*/0,
                         /*ref=*/(int)(slot - s_pnUps), onPnUpRecv, onPnUpDisc);
    if (h < 0) {
        msgFail(id.index, peer_hex, mid, LXMF_ST_LINK_OPEN_FAIL);
        return;
    }
    slot->used     = true;
    slot->handle   = h;
    slot->id_index = id.index;
    slot->peer     = peer_hex;
    slot->mid      = mid;
    std::memcpy(slot->node, node, 16);
    slot->tag       = tag;
    slot->started_s = (uint32_t)(nowUnixMs() / 1000);

    /* rnsd buffers one pre-active packet / Resource and flushes it on
     * establishment, so send right away. */
    bool ok;
    if (packed.size() <= LXMF_PN_PKT_MAX) {
        slot->is_resource = false;
        std::string base = "rnsd.links." + slot->tag;
        slot->proof_base_proven   = storageGetInt((base + ".tx_proven").c_str(), 0);
        slot->proof_base_timeouts = storageGetInt((base + ".proof_timeouts").c_str(), 0);
        ok = itsSend(h, packed.data(), packed.size(), 0) != 0;
    } else {
        slot->is_resource = true;
        slot->opaque = s_pnOpaque++;
        void* rb = gp_alloc(packed.size());
        ok = false;
        if (rb) {
            std::memcpy(rb, packed.data(), packed.size());
            /* rnsd owns rb from here (it frees on its own failure too). */
            ok = rnsdLinkSendResource(slot->tag.c_str(), rb, packed.size(),
                                      slot->opaque);
        }
    }
    if (!ok) { pnUpSettle(*slot, LXMF_ST_PN_FAIL); return; }
    storageBegin();
    storageSet(msgPath(id.index, peer_hex, mid, "status").c_str(), (int)LXMF_ST_SENDING);
    storageSet(msgPath(id.index, peer_hex, mid, "tries").c_str(),  0);
    storageEnd();
    info("id %d: pn upload %s → node %s (%zu B, %s%s)", id.index, mid.c_str(),
         bytesToHex(node, 16).c_str(), packed.size(),
         slot->is_resource ? "resource" : "packet", cost > 0 ? ", stamped" : "");
}

/* Resource-aux settle for pn uploads (opaque ids 0x20000+). Returns true
 * when consumed. */
static bool pnResourceAux(const rnsd_link_resource_done_t& d)
{
    if (d.opcode != RNSD_LINK_RESOURCE_OUTBOUND_DONE &&
        d.opcode != RNSD_LINK_RESOURCE_FAILED) return false;
    for (auto& s : s_pnUps) {
        if (!s.used || !s.is_resource || s.opaque != d.opaque_id) continue;
        pnUpSettle(s, d.opcode == RNSD_LINK_RESOURCE_OUTBOUND_DONE
                          ? LXMF_ST_ON_PN : LXMF_ST_PN_FAIL);
        return true;
    }
    return false;
}

/* ── sync (message retrieval) ── */

enum { PN_PH_IDLE = 0, PN_PH_WAIT_ACTIVE, PN_PH_LIST, PN_PH_FETCH, PN_PH_DEL };

struct pn_sync_job_t { std::string node_hex; int id_index; };
static std::vector<pn_sync_job_t> s_pnSyncQ;

struct pn_sync_t {
    int         phase = PN_PH_IDLE;
    int         handle = -1;
    std::string tag, node_hex;
    uint8_t     node[16] = {};
    int         id_index = -1;
    int         req_id = -1;
    int         rounds = 0;              /* list/fetch/delete cycles done */
    std::vector<std::string> got;        /* binary tids ingested this round */
    int         got_msgs = 0;            /* messages handed to the pipeline */
    uint32_t    started_s = 0;
};
static pn_sync_t s_pnSync;

/* Transient ids already ingested — filters re-listed blobs (a failed
 * delete-confirm round re-serves them) out of the next round's wants.
 * RAM-only and bounded; the message-id dedup in onInboundLxm is the
 * authoritative backstop. */
static std::set<std::string> s_pnSeenTids;

static void pnSeenTid(const std::string& tid)
{
    if (s_pnSeenTids.size() >= LXMF_PN_SEEN_TIDS_MAX) s_pnSeenTids.clear();
    s_pnSeenTids.insert(tid);
}

static void pnSyncDone(const char* err_msg)
{
    if (s_pnSync.phase == PN_PH_IDLE) return;
    std::string pre = "lxmf.pn." + s_pnSync.node_hex + ".";
    storageBegin();
    storageSet((pre + "last_check_s").c_str(), (int)(nowUnixMs() / 1000));
    storageSet((pre + "last_err").c_str(), err_msg ? err_msg : "");
    if (!err_msg) storageSet((pre + "last_got").c_str(), s_pnSync.got_msgs);
    storageEnd();
    if (err_msg)
        warn("pn sync %s id %d: %s", s_pnSync.node_hex.c_str(),
             s_pnSync.id_index, err_msg);
    else
        info("pn sync %s id %d: complete, %d message(s)",
             s_pnSync.node_hex.c_str(), s_pnSync.id_index, s_pnSync.got_msgs);
    int h = s_pnSync.handle;
    s_pnSync = pn_sync_t{};               /* reset BEFORE disconnect — the disc
                                           * callback then sees IDLE and no-ops */
    if (h >= 0) itsDisconnect(h);
    setStrIfChanged("lxmf.pn.sync", "");
}

static void onPnSyncRecv(int handle, size_t /*bytesAvail*/)
{
    /* The node speaks to a syncing client through request responses, not
     * link packets — drain and ignore. */
    uint8_t buf[256];
    itsRecv(handle, buf, sizeof buf, 0);
}

static void onPnSyncDisc(int /*ref*/)
{
    if (s_pnSync.phase == PN_PH_IDLE) return;
    s_pnSync.handle = -1;
    pnSyncDone("link closed");
}

/* Issue one `/get` request on the session link. `data` must be a complete
 * msgpack object (data_packed). Returns false when the request could not
 * be queued (the caller settles the session). */
static bool pnSyncRequest(const std::vector<uint8_t>& data, int next_phase)
{
    int rid = rnsdLinkRequest(s_pnSync.tag.c_str(), "/get",
                              data.data(), data.size(),
                              RNSD_LINK_RESOURCE_AUX_PORT, /*data_packed=*/true);
    if (rid < 0) return false;
    s_pnSync.req_id = rid;
    s_pnSync.phase  = next_phase;
    return true;
}

/* Start a list round: [nil, nil] → the node returns the transient ids it
 * holds for our identified destination. */
static bool pnSyncListRound(void)
{
    if (++s_pnSync.rounds > LXMF_PN_SYNC_ROUNDS_MAX) return false;
    s_pnSync.got.clear();
    std::vector<uint8_t> req = { 0x92, 0xC0, 0xC0 };
    return pnSyncRequest(req, PN_PH_LIST);
}

/* One downloaded lxmf_data blob: leading 16-byte dest hash + the
 * destination-encrypted src+sig+payload (the node strips the propagation
 * stamp before serving). Decrypt with the session identity and feed the
 * normal inbound pipeline. True when a message was handed on. */
static bool pnIngestBlob(lxmf_id_t& id, const uint8_t* p, size_t n)
{
    if (n < LXMF_OVERHEAD) return false;
    if (std::memcmp(p, id.dest_hash, LXMF_DEST_HASH_LEN) != 0) {
        verb("pn sync: blob for foreign dest %s — dropped",
             bytesToHex(p, 8).c_str());
        return false;
    }
    std::vector<uint8_t> wire(n);         /* plaintext is smaller than the ct */
    std::memcpy(wire.data(), p, LXMF_DEST_HASH_LEN);
    size_t pt_len = wire.size() - LXMF_DEST_HASH_LEN;
    if (!rnsdDecryptSelf(id.identity_key.c_str(), id.dest_hash,
                         p + LXMF_DEST_HASH_LEN, n - LXMF_DEST_HASH_LEN,
                         wire.data() + LXMF_DEST_HASH_LEN, &pt_len)) {
        verb("pn sync: blob decrypt failed (%zu B)", n);
        return false;
    }
    wire.resize(LXMF_DEST_HASH_LEN + pt_len);
    if (wire.size() < LXMF_OVERHEAD) return false;
    onInboundLxm(id, wire.data(), wire.size());
    return true;
}

/* A `/get` response (or failure) landing on the resource-aux port for the
 * sync session's in-flight request. Consumes every REQUEST_* aux (nothing
 * else in lxmf issues link requests); owns d.buf. */
static bool pnRequestAux(const rnsd_link_resource_done_t& d)
{
    if (d.opcode != RNSD_LINK_REQUEST_RESPONSE &&
        d.opcode != RNSD_LINK_REQUEST_FAILED) return false;
    pn_sync_t& sy = s_pnSync;
    if (sy.phase == PN_PH_IDLE || (int)d.opaque_id != sy.req_id) {
        if (d.buf) rnsdResourceRelease(d.buf);      /* stale — drop */
        return true;
    }
    if (d.opcode == RNSD_LINK_REQUEST_FAILED) {
        if (d.buf) rnsdResourceRelease(d.buf);
        /* The delete-confirm round is best-effort — the ingest already
         * succeeded; the tid dedup absorbs a re-serve next session. */
        if (sy.phase == PN_PH_DEL) pnSyncDone(nullptr);
        else                       pnSyncDone("request failed");
        return true;
    }
    const uint8_t* p = (const uint8_t*)d.buf;
    size_t n = d.len;

    if (sy.phase == PN_PH_LIST) {
        /* msgpack list of bin32 transient ids, or a bare error int. */
        std::vector<std::string> wants, haves;
        bool ok = false;
        if (p && n) {
            uint8_t b = p[0];
            if ((b & 0xF0) == 0x90 || b == 0xDC || b == 0xDD) {
                mpScan s{p, n, 1, 0, 0};
                uint64_t cnt = 0;
                if ((b & 0xF0) == 0x90) { cnt = b & 0x0F; ok = true; }
                else if (b == 0xDC)     ok = mpReadBe(s, 2, cnt);
                else                    ok = mpReadBe(s, 4, cnt);
                for (uint64_t k = 0; ok && k < cnt; ++k) {
                    std::string tid;
                    if (!mpReadStrOrBin(s, tid) || tid.size() != 32) { ok = false; break; }
                    if (s_pnSeenTids.count(tid)) {
                        if (haves.size() < LXMF_PN_HAVES_MAX) haves.push_back(tid);
                    } else if (wants.size() < LXMF_PN_FETCH_MAX) {
                        wants.push_back(tid);
                    }
                }
            } else {
                uint64_t code = 0;
                mpScan s{p, n, 0, 0, 0};
                if (mpReadUint(s, code)) {
                    rnsdResourceRelease(d.buf);
                    pnSyncDone(code == 0xF0 ? "node: not identified"
                             : code == 0xF1 ? "node: access denied"
                             : code == 0xF6 ? "node: throttled"
                                            : "node error");
                    return true;
                }
            }
        } else {
            ok = true;                                 /* empty list */
        }
        rnsdResourceRelease(d.buf);
        if (!ok) { pnSyncDone("bad list response"); return true; }
        if (wants.empty() && haves.empty()) { pnSyncDone(nullptr); return true; }
        /* Fetch round: [wants, haves, limit_kb] — the node serves the wants
         * (within limit_kb) and deletes the haves. */
        std::vector<uint8_t> req;
        mpPackArrayHeader(req, 3);
        mpPackArrayHeader(req, wants.size());
        for (auto& t : wants) mpPackBin(req, (const uint8_t*)t.data(), t.size());
        mpPackArrayHeader(req, haves.size());
        for (auto& t : haves) mpPackBin(req, (const uint8_t*)t.data(), t.size());
        int limit_kb = storageGetInt("s.lxmf.max_resource_size", 262144) / 1000;
        mpPackInt(req, limit_kb > 0 ? limit_kb : 1);
        if (!pnSyncRequest(req, PN_PH_FETCH)) pnSyncDone("request failed");
        return true;
    }

    if (sy.phase == PN_PH_FETCH) {
        /* msgpack list of raw lxmf_data blobs. */
        lxmf_id_t* id = idAt(sy.id_index);
        bool ok = false;
        if (p && n && id && id->used) {
            uint8_t b = p[0];
            if ((b & 0xF0) == 0x90 || b == 0xDC || b == 0xDD) {
                mpScan s{p, n, 1, 0, 0};
                uint64_t cnt = 0;
                if ((b & 0xF0) == 0x90) { cnt = b & 0x0F; ok = true; }
                else if (b == 0xDC)     ok = mpReadBe(s, 2, cnt);
                else                    ok = mpReadBe(s, 4, cnt);
                for (uint64_t k = 0; ok && k < cnt; ++k) {
                    std::string blob;
                    if (!mpReadStrOrBin(s, blob)) { ok = false; break; }
                    uint8_t tid[RNSD_HASH_LEN];
                    rnsdSha256((const uint8_t*)blob.data(), blob.size(), tid);
                    std::string tids((const char*)tid, RNSD_HASH_LEN);
                    sy.got.push_back(tids);
                    pnSeenTid(tids);
                    if (pnIngestBlob(*id, (const uint8_t*)blob.data(), blob.size()))
                        sy.got_msgs++;
                }
            }
        }
        rnsdResourceRelease(d.buf);
        if (!ok) { pnSyncDone("bad fetch response"); return true; }
        if (sy.got.empty()) { pnSyncDone(nullptr); return true; }
        /* Delete-confirm round: [nil, got] — the node drops the copies we
         * just ingested. Its response chains into the next list round. */
        std::vector<uint8_t> req;
        mpPackArrayHeader(req, 2);
        req.push_back(0xC0);
        mpPackArrayHeader(req, sy.got.size());
        for (auto& t : sy.got) mpPackBin(req, (const uint8_t*)t.data(), t.size());
        if (!pnSyncRequest(req, PN_PH_DEL)) pnSyncDone(nullptr);
        return true;
    }

    if (sy.phase == PN_PH_DEL) {
        rnsdResourceRelease(d.buf);
        /* Round complete — list again; more may be held than one round's
         * ITS-bounded wants could name. */
        if (!pnSyncListRound()) pnSyncDone(nullptr);
        return true;
    }

    if (d.buf) rnsdResourceRelease(d.buf);
    return true;
}

/* Queue a sync of `node_hex` for every usable identity (dedup'd against
 * the queue and the active session). */
static void pnEnqueueSync(const std::string& node_hex)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (!s_ids[n].used || !idEnabled(n)) continue;
        bool dup = s_pnSync.phase != PN_PH_IDLE &&
                   s_pnSync.node_hex == node_hex && s_pnSync.id_index == n;
        for (auto& j : s_pnSyncQ)
            if (j.node_hex == node_hex && j.id_index == n) dup = true;
        if (!dup) s_pnSyncQ.push_back({ node_hex, n });
    }
}

/* Advance the sync machine: start the next queued session, watch the link
 * establish, kick the first request. Request rounds then chain through
 * pnRequestAux. */
static void pnSyncAdvance(uint32_t now_s)
{
    pn_sync_t& sy = s_pnSync;
    if (sy.phase == PN_PH_IDLE) {
        if (s_pnSyncQ.empty()) return;
        pn_sync_job_t job = s_pnSyncQ.front();
        s_pnSyncQ.erase(s_pnSyncQ.begin());
        lxmf_id_t* id = idAt(job.id_index);
        uint8_t node[16];
        if (!id || !id->used || !idEnabled(job.id_index) ||
            !hexToDestHash(job.node_hex, node)) return;
        uint8_t pk[RNSD_PUBKEY_LEN];
        if (!rnsdRecallPubkey(node, pk)) {
            /* Never heard the node announce — ask for a path (which
             * prompts one) and let the next check retry. */
            rnsdRequestPath(node);
            storageSet(("lxmf.pn." + job.node_hex + ".last_err").c_str(),
                       "node unknown — path requested");
            return;
        }
        char tag[24];
        std::snprintf(tag, sizeof(tag), "lxpn.s%03x",
                      (unsigned)(s_pnTagSeq++ & 0xFFF));
        int h = rnsdLinkOpen(node, "lxmf.propagation",
                             id->identity_key.c_str(), tag,
                             /*path_timeout_ms=*/15000, /*link_timeout_ms=*/0,
                             /*ref=*/0, onPnSyncRecv, onPnSyncDisc);
        if (h < 0) {
            storageSet(("lxmf.pn." + job.node_hex + ".last_err").c_str(),
                       "link open failed");
            return;
        }
        sy.phase  = PN_PH_WAIT_ACTIVE;
        sy.handle = h;
        sy.tag    = tag;
        sy.node_hex = job.node_hex;
        std::memcpy(sy.node, node, 16);
        sy.id_index  = job.id_index;
        sy.started_s = now_s;
        setStrIfChanged("lxmf.pn.sync", job.node_hex.c_str());
        dbg("pn sync: link %s → node %s (id %d)", tag,
            job.node_hex.c_str(), job.id_index);
        return;
    }
    if (now_s - sy.started_s > LXMF_PN_SYNC_TTL_S) { pnSyncDone("timeout"); return; }
    if (sy.phase == PN_PH_WAIT_ACTIVE) {
        std::string st = storageGetStr(("rnsd.links." + sy.tag + ".state").c_str(), "");
        if (st == "failed" || st == "closed" || st == "closing") {
            pnSyncDone("link failed");
            return;
        }
        if (st != "active") return;
        /* Identified request session: the node serves only the
         * lxmf.delivery dest derived from the identified identity. The
         * identify and the request ride the same link in order. */
        rnsdLinkIdentify(sy.tag.c_str());
        if (!pnSyncListRound()) pnSyncDone("request failed");
    }
}

/* Deadline of the next periodic check; 0 = not armed yet (pnScheduleChecks
 * arms it one grace window after bring-up). */
static uint32_t s_pnNextCheck_s = 0;

/* Periodic check of the nodes marked for checking. First pass ~2 min
 * after bring-up, then every s.lxmf.pn.check_interval_s (0 = manual only,
 * via lxmf.cmd.pn_sync). */
static void pnScheduleChecks(uint32_t now_s)
{
    int interval = storageGetInt("s.lxmf.pn.check_interval_s", 1800);
    if (interval <= 0) { s_pnNextCheck_s = 0; return; }
    if (s_pnNextCheck_s == 0) { s_pnNextCheck_s = now_s + 120; return; }
    if ((int32_t)(now_s - s_pnNextCheck_s) < 0) return;
    s_pnNextCheck_s = now_s + (uint32_t)interval;
    for (auto& nd : pnNodeList())
        if (nd.check) pnEnqueueSync(nd.hex);
}

/* A manual check-everything (lxmf.cmd.pn_sync = all) stands in for the
 * periodic one, so push the timer out a whole interval: pressing the button
 * means "now", not "now and again in a minute". */
static void pnRearmNextCheck(void)
{
    int interval = storageGetInt("s.lxmf.pn.check_interval_s", 1800);
    if (interval <= 0) return;                     /* manual only — nothing armed */
    s_pnNextCheck_s = (uint32_t)(nowUnixMs() / 1000) + (uint32_t)interval;
}

/* 1 Hz housekeeping: settle packet-class uploads off the link's proof
 * counters, reap stalled sessions, drive the sync machine. */
static void pnClientTick(void)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    for (auto& s : s_pnUps) {
        if (!s.used) continue;
        if (!s.is_resource) {
            std::string base = "rnsd.links." + s.tag;
            if (storageGetInt((base + ".tx_proven").c_str(), 0) > s.proof_base_proven) {
                pnUpSettle(s, LXMF_ST_ON_PN);
                continue;
            }
            if (storageGetInt((base + ".proof_timeouts").c_str(), 0) > s.proof_base_timeouts ||
                storageGetStr((base + ".state").c_str(), "") == "failed") {
                pnUpSettle(s, LXMF_ST_PN_FAIL);
                continue;
            }
        }
        if (now_s - s.started_s > LXMF_PN_UP_TTL_S) pnUpSettle(s, LXMF_ST_PN_FAIL);
    }
    pnScheduleChecks(now_s);
    pnSyncAdvance(now_s);
}

/* Park-path teardown: drop every pn link so rnsd frees the slots. */
static void pnTeardown(void)
{
    for (auto& s : s_pnUps)
        if (s.handle >= 0) { int h = s.handle; s = pn_up_t{}; itsDisconnect(h); }
    if (s_pnSync.handle >= 0) {
        int h = s_pnSync.handle;
        s_pnSync = pn_sync_t{};
        itsDisconnect(h);
        setStrIfChanged("lxmf.pn.sync", "");
    }
    s_pnSyncQ.clear();
}

/* ─────────────── Resource aux (rnsd → lxmf) ───────────────
 *
 * rnsd sends one rnsd_link_resource_done_t aux frame to
 * RNSD_LINK_RESOURCE_AUX_PORT when a Resource transfer concludes:
 *   INBOUND_DONE  — buf holds the reassembled LXM wire; we own it and
 *                   must rnsdResourceRelease() it.
 *   OUTBOUND_DONE — our big DIRECT send was proven; settle the outbox.
 *   FAILED        — transfer aborted (buf null). */
static void onResourceAux(TaskHandle_t /*sender*/, const void* data, size_t len)
{
    if (len < sizeof(rnsd_link_resource_done_t)) {
        warn("resource-aux: short frame %zu", len);
        return;
    }
    rnsd_link_resource_done_t d;
    std::memcpy(&d, data, sizeof(d));

    /* Propagation-node client first: /get responses (REQUEST_* opcodes)
     * and pn-upload resource settles (opaque ids 0x20000+). */
    if (pnRequestAux(d)) return;
    if (pnResourceAux(d)) return;

    /* Then the proxy Channel: a frame too big for one Channel message rides a
     * Resource on the Channel's hidden Link, and `rnsd.chan.byid.<link_id>` is
     * what tells one of those from every other consumer's transfer. It takes
     * ownership of nothing, so the release below still runs. */
    if (proxyResourceAux(d)) {
        if (d.opcode == RNSD_LINK_RESOURCE_INBOUND_DONE && d.buf)
            rnsdResourceRelease(d.buf);
        return;
    }

    if (d.opcode == RNSD_LINK_RESOURCE_INBOUND_DONE) {
        int idx = -1;
        for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
            if (s_ids[n].used &&
                std::memcmp(s_ids[n].dest_hash, d.local_dest_hash,
                            LXMF_DEST_HASH_LEN) == 0) { idx = n; break; }
        }
        if (idx < 0 && d.buf && d.len >= LXMF_DEST_HASH_LEN) {
            /* A resource can also land on a conv link WE opened (the peer
             * replies over our link instead of opening its own). rnsd's aux
             * then carries the REMOTE dest in local_dest_hash — an outbound
             * link has no local landing dest — so nothing matches above.
             * The wire saves us: DIRECT LXMs are the full packed message
             * including the leading 16-byte destination (see
             * onLinkInboxRecv), so recover the identity from the payload;
             * onInboundLxm re-validates that hash anyway. */
            const uint8_t* wire_dest = (const uint8_t*)d.buf;
            for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                if (s_ids[n].used &&
                    std::memcmp(s_ids[n].dest_hash, wire_dest,
                                LXMF_DEST_HASH_LEN) == 0) { idx = n; break; }
            }
        }
        if (idx < 0 && d.buf && d.len > 0) {
            /* No identity claims the leading 16 bytes — the resource may
             * be a double-encrypted envelope. */
            if (tryDoubleEncrypted((const uint8_t*)d.buf, d.len)) {
                rnsdResourceRelease(d.buf);
                return;
            }
        }
        if (idx < 0) {
            /* rnsd routed a concluded resource to our aux port, so the
             * link's local destination is one WE registered — yet no loaded
             * identity slot claims it. Dump what we do have loaded so the
             * mismatch is diagnosable from the log alone. */
            warn("resource-aux: inbound resource (%uB, link %s) for unknown dest %s — dropping",
                 (unsigned)d.len,
                 bytesToHex(d.link_id, 16).c_str(),
                 bytesToHex(d.local_dest_hash, LXMF_DEST_HASH_LEN).c_str());
            for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                if (s_ids[n].used)
                    warn("resource-aux: loaded id %d dest %s", n,
                         bytesToHex(s_ids[n].dest_hash, LXMF_DEST_HASH_LEN).c_str());
            }
        } else if (d.buf && d.len > 0) {
            info("id %d: inbound resource %uB → onInboundLxm", idx, (unsigned)d.len);
            onInboundLxm(s_ids[idx], (const uint8_t*)d.buf, d.len);
        }
        rnsdResourceRelease(d.buf);   /* we own it; release even if dropped */
        return;
    }

    if (d.opcode == RNSD_LINK_RESOURCE_OUTBOUND_DONE ||
        d.opcode == RNSD_LINK_RESOURCE_FAILED) {
        bool ok = (d.opcode == RNSD_LINK_RESOURCE_OUTBOUND_DONE);
        for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
            lxmf_id_t& id = s_ids[n];
            if (!id.used) continue;
            for (auto& o : id.outboxes) {
                if (!o.used || !o.is_resource || o.send_id != d.opaque_id)
                    continue;
                const std::string& mid      = o.msg_key;
                const std::string& peer_hex = o.peer;
                if (ok) {
                    /* The resource transfer ACK is proof-grade: the peer
                     * reassembled and acknowledged the full wire. */
                    msgSetStatus(id.index, peer_hex, mid, LXMF_ST_DELIVERED);
                    id.sent++;
                    info("id %d: DIRECT resource delivered mid=%s tag=%s",
                         id.index, mid.c_str(), o.link_tag.c_str());
                } else {
                    queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
                    std::string rst = storageGetStr(
                        ("rnsd.links." + o.link_tag + ".resource.state").c_str(), "");
                    warn("id %d: DIRECT resource failed mid=%s tag=%s (%s)",
                         id.index, mid.c_str(), o.link_tag.c_str(), rst.c_str());
                }
                if (id.pending > 0) id.pending--;
                /* The link persists across sends (the fast settle path
                 * mirrors resolveDirectSends): keep + first-delivery
                 * identify on success, drop on failure. */
                directLinkSettle(o.link_tag, ok, (uint32_t)(nowUnixMs() / 1000));
                o.used        = false;
                o.direct      = false;
                o.is_resource = false;
                return;
            }
        }
        /* No outbox match → an inbound transfer FAILED; nothing to free
         * (rnsd already released its buffer on the failure path). */
        return;
    }

    warn("resource-aux: unknown opcode %u", d.opcode);
}

/* Resolve outbound DIRECT sends from the 1 Hz tick by watching the
 * rnsd-published `rnsd.links.<tag>.*` state (RNSD_PORT_LINK carries no
 * OUT_RESULT). `tx_packets>=1` means rnsd's pre-active outbox flushed
 * our one LXM packet onto the established Link → treat as "sent"
 * (egress acknowledged, mirrors opportunistic SENT semantics; true
 * proof-on-delivery would need a feedback channel — follow-up).
 * A `failed` state, or no resolution by the slot's deadline, fails it. */

static void resolveDirectSends(void)
{
    uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        lxmf_id_t& id = s_ids[n];
        if (!id.used) continue;
        for (auto& o : id.outboxes) {
            if (!o.used) continue;
            const std::string& mid      = o.msg_key;
            const std::string& peer_hex = o.peer;

            if (!o.direct) {
                /* Path grace spent: rnsd has been searching for the whole of
                 * LXMF_PATH_GRACE_S. Take the send back — cancel it rnsd-side,
                 * free the slot — and queue the message; the next sweep asks
                 * for the path once more instead of leaving an hour-long search
                 * in rnsd's four-slot table. */
                if (!o.awaiting_proof && o.path_deadline_s && now_s >= o.path_deadline_s) {
                    sendCancel(id, o.send_id);
                    if (id.pending > 0) id.pending--;
                    o.used = false;
                    o.path_deadline_s = 0;
                    queueRequeue(id, peer_hex, mid, LXMF_ST_REQUESTING_PATH);
                    dbg("id %d: msg %s no path within the grace — queued", id.index,
                        mid.c_str());
                    continue;
                }
                /* Opportunistic proof backstop: stage is "sent" and rnsd owes
                 * us a second OUT_RESULT (DELIVERED / PROOF_TIMEOUT). If that
                 * frame was lost (ITS drop, rnsd restart), settle here as a
                 * proof timeout would. */
                if (o.awaiting_proof && now_s >= o.proof_deadline_s) {
                    o.used = false;
                    o.awaiting_proof = false;
                    dbg("id %d: msg %s proof backstop (no OUT_RESULT)",
                        id.index, mid.c_str());
                    oppProofMissed(id, peer_hex, mid, o.sent_s);
                }
                continue;
            }

            std::string base = "rnsd.links." + o.link_tag;
            std::string st   = storageGetStr((base + ".state").c_str(), "");
            int tx           = storageGetInt((base + ".tx_packets").c_str(), 0);

            /* rnsd publishes its establishment budget when it processes the
             * open, which can land after the send took its deadline — so adopt
             * it here too. Only ever extends: a send already past its own
             * deadline is not revived by a budget arriving late. */
            /* The budget is per attempt, and rnsd makes a fresh attempt
             * toward a peer it has heard from when one times out, each of
             * which may first wait for a path again: `attempt` counts them. */
            {
                uint32_t estab = (uint32_t)storageGetInt(
                    (base + ".estab_timeout_s").c_str(), 0);
                int attempt = storageGetInt((base + ".attempt").c_str(), 1);
                if (attempt < 1) attempt = 1;
                uint32_t floor_s = o.started_s +
                    ((uint32_t)rnsdPathBudgetS() + estab) * (uint32_t)attempt;
                if (estab && floor_s > o.direct_deadline_s)
                    o.direct_deadline_s = floor_s;
            }

            bool done = false, ok = false;
            std::string err;               /* human text for the log only */
            /* The link never carried the send and it was not for want of a
             * path: the capped retry (linkFailed), not the sweep. */
            bool link_failed = false;

            if (o.is_resource) {
                /* Resource sends settle on the transfer outcome, NOT on
                 * link "active" — the Link must stay up for the whole
                 * transfer. The OUTBOUND_DONE aux (onResourceAux) is the
                 * fast path; this is the timeout/teardown fallback. */
                std::string rst = storageGetStr((base + ".resource.state").c_str(), "");
                if (rst == "sent") { done = true; ok = true; }
                else if (rst.rfind("failed", 0) == 0 || st == "failed" || st == "closed") {
                    /* rnsd writes the transfer's outcome as `failed:<dir>:<n>`
                     * and clears last_error at open, so a link that closed
                     * before the transfer began has neither: say which. */
                    done = true;
                    err  = storageGetStr((base + ".last_error").c_str(), "");
                    bool began = !rst.empty() && rst != "sending";
                    link_failed = !began && err != "no_path";
                    if (err.empty())
                        err = began ? rst : "link " + st + " before the transfer began";
                } else if (now_s >= o.direct_deadline_s) {
                    done = true; err = "resource timeout";
                }
                if (!done) continue;
                if (ok) {
                    /* resource.state "sent" = the transfer ACK arrived —
                     * proof-grade, same as the OUTBOUND_DONE fast path. */
                    msgSetStatus(id.index, peer_hex, mid, LXMF_ST_DELIVERED);
                    id.sent++;
                    info("id %d: DIRECT resource delivered mid=%s tag=%s",
                         id.index, mid.c_str(), o.link_tag.c_str());
                } else {
                    warn("id %d: DIRECT resource failed mid=%s tag=%s (%s)",
                         id.index, mid.c_str(), o.link_tag.c_str(), err.c_str());
                    if (link_failed) linkFailed(id, peer_hex, mid);
                    else queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
                }
                if (id.pending > 0) id.pending--;
                /* The link persists across sends: keep + first-delivery
                 * identify on success, drop on failure (it's suspect). */
                directLinkSettle(o.link_tag, ok, now_s);
                o.used   = false;
                o.direct = false;
                continue;
            }

            /* Packet-class send already egressed (stage "sent"): watch the
             * link's proof counters for the delivered/no-proof settle.
             * Holding the slot keeps the link serialized (convBusy), so
             * only THIS send can move the counters past the baseline. */
            if (o.awaiting_proof) {
                int proven = storageGetInt((base + ".tx_proven").c_str(), 0);
                int touts  = storageGetInt((base + ".proof_timeouts").c_str(), 0);
                if (proven > o.proof_base_proven) {
                    msgSetStatus(id.index, peer_hex, mid, LXMF_ST_DELIVERED);
                    info("id %d: DIRECT delivered mid=%s tag=%s",
                         id.index, mid.c_str(), o.link_tag.c_str());
                    directLinkSettle(o.link_tag, true, now_s);
                    o.used = false; o.direct = false; o.awaiting_proof = false;
                    o.link_wire.clear();
                } else if (touts > o.proof_base_timeouts && st == "active" &&
                           !o.link_resent && !o.link_wire.empty() &&
                           itsSend(o.link_handle, o.link_wire.data(), o.link_wire.size(), 0) != 0) {
                    /* rnsd's receipt timed out on a link that is still up.
                     * A proof is one frame on the way back and nothing
                     * repairs it, so the message may well be there: send
                     * the same wire once more on the same link. The
                     * recipient drops a message id it holds and proves the
                     * copy all the same. */
                    o.link_resent         = true;
                    o.proof_base_timeouts = touts;
                    o.proof_base_proven   = proven;
                    o.proof_deadline_s    = now_s + proofBackstopS();
                    info("id %d: msg %s resent over link %s (proof lost)",
                         id.index, mid.c_str(), o.link_tag.c_str());
                } else if (touts > o.proof_base_timeouts ||
                           st == "failed" || st == "closed" || st.empty() ||
                           now_s >= o.proof_deadline_s) {
                    /* No proof on a link send (rnsd's receipt timed out, the
                     * link died, or our backstop hit). The link is suspect —
                     * drop it, so the next sweep relinks — and the message goes
                     * back to the queue. */
                    o.used = false; o.direct = false; o.awaiting_proof = false;
                    o.link_wire.clear();
                    directLinkSettle(o.link_tag, false, now_s);
                    queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
                    dbg("id %d: DIRECT no delivery proof mid=%s tag=%s",
                        id.index, mid.c_str(), o.link_tag.c_str());
                }
                continue;
            }

            if (st == "active" || tx >= 1) {
                done = true; ok = true;
            } else if (st == "failed") {
                done = true;
                err  = storageGetStr((base + ".last_error").c_str(), "link failed");
                link_failed = err != "no_path";
                if (err == "establish_timeout") {
                    std::string budget = storageGetStr((base + ".estab_budget").c_str(), "");
                    if (!budget.empty()) err += " (" + budget + ")";
                }
            } else if (st == "closed") {
                done = true; ok = (tx >= 1);
                if (!ok) { err = "link closed before send"; link_failed = true; }
            } else if (st.empty() && now_s >= o.direct_deadline_s) {
                done = true; err = "direct timeout";
            } else if (now_s >= o.direct_deadline_s + 15) {
                /* Stuck establishing. The deadline already carries rnsd's own
                 * budget for this link, so this is 15 s past the point rnsd
                 * itself gives up — not a guess that undercuts it. */
                done = true; err = "direct timeout";
                link_failed = st != "awaiting_path";
            }
            if (!done) continue;

            if (ok) {
                /* Egress accepted → "sent" (one grey check). Hold the slot
                 * and switch to proof-watching: rnsd keeps the packet
                 * receipt and bumps tx_proven / proof_timeouts when the
                 * link-packet proof lands or times out. */
                msgSetStatus(id.index, peer_hex, mid, LXMF_ST_AWAITING_PROOF);
                id.sent++;
                if (id.pending > 0) id.pending--;
                info("id %d: DIRECT sent mid=%s tag=%s (awaiting proof)",
                     id.index, mid.c_str(), o.link_tag.c_str());
                for (auto& c : s_convlinks) {
                    if (!c.used || c.tag != o.link_tag) continue;
                    c.last_used_s = now_s;
                    break;
                }
                o.awaiting_proof   = true;
                o.proof_deadline_s = now_s + proofBackstopS();
                continue;                       /* slot stays — proof phase */
            }

            /* The link never carried the send. It is suspect: drop it, and the
             * message waits for the next sweep to relink. */
            warn("id %d: DIRECT failed mid=%s tag=%s (%s, link attempts %d)",
                 id.index, mid.c_str(), o.link_tag.c_str(), err.c_str(),
                 storageGetInt((base + ".attempt").c_str(), 0));
            if (id.pending > 0) id.pending--;
            for (auto& c : s_convlinks) {
                if (!c.used || c.tag != o.link_tag) continue;
                convDrop(c);
                break;
            }
            o.used   = false;
            o.direct = false;
            if (link_failed) linkFailed(id, peer_hex, mid);
            else queueRequeue(id, peer_hex, mid, LXMF_ST_RETRYING_LINK);
        }
    }
}

/* ─────────────── command processing (self-clearing keys) ─────────────── */

/* Storage subscriptions are narrow by design: `lxmf.cmd.` for identity-
 * level commands (one sub, installed in lxmfInit) and `lxmf.id.<n>.cmd.`
 * for per-identity commands (one sub per allocated slot, added on
 * createIdentityForSlot/loadIdentityForSlot, removed on destroyIdentity).
 * Firmware-side state writes (`s.lxmf.id.<n>.msgs.*`, ephemeral stats,
 * etc.) are NOT in any subscribed scope, so no self-notify churn. */

/* cmd.send — a fresh delivery of this message, whatever it did before: the try
 * count restarts, any earlier queue entry goes (so the timeout restarts with
 * it), and the first attempt is made at once. */
static void processSend(lxmf_id_t& id, const std::string& peer_hex,
                        const std::string& mid)
{
    queueRemove(id.index, peer_hex, mid);
    storageSet(msgPath(id.index, peer_hex, mid, "tries").c_str(), 0);
    processReady(id, peer_hex, mid);
}

/* cmd.cancel — abort an in-flight send. If we have an outbox slot for
 * the message, push OUT_CANCEL; rnsd will reply with OUT_RESULT status=2
 * which applyOutResult maps to stage=cancelled. Otherwise the message is
 * already terminal — just stamp the stage. */
static void processCancel(lxmf_id_t& id, const std::string& peer_hex,
                          const std::string& mid)
{
    outbound_t* o = nullptr;
    for (auto& slot : id.outboxes)
        if (slot.used && slot.peer == peer_hex && slot.msg_key == mid) {
            o = &slot; break;
        }

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

    /* Not in flight — mark cancelled directly (which also leaves the queue). */
    msgSetStatus(id.index, peer_hex, mid, LXMF_ST_CANCELLED);
}

/* cmd.cancel = "all" — every outbound of this identity that has not settled.
 *
 * The fan-out is here rather than in the caller because `cmd.cancel` is a
 * self-clearing sentinel: a writer looping over N messages would overwrite the
 * key before this task had drained the previous value, and all but the last
 * would simply be lost. One write, one pass, on the task that owns the data.
 *
 * The set is read from storage rather than from s_queue, which holds only what
 * is WAITING for another attempt — a message on its first attempt is settling
 * against an outbox slot and is not in it, and that is exactly a send somebody
 * would want to call back. The predicate is queueSweep's, so "all" cancels
 * precisely what `lxmf unfinished` listed. */
static void processCancelAll(lxmf_id_t& id)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.", id.index);
    /* Collected before acting: processCancel writes through storage, and
     * mutating the tree under its own walk is not a thing to rely on. */
    std::vector<std::pair<std::string, std::string>> hits;
    for (const auto& peer : collectTokens(prefix))
        for (const auto& key : collectTokens(msgPrefix(id.index, peer))) {
            if (storageGetStr(msgPath(id.index, peer, key, "dir").c_str(), "") != "out")
                continue;
            const int st    = storageGetInt(msgPath(id.index, peer, key, "status").c_str(), 0);
            const int tries = storageGetInt(msgPath(id.index, peer, key, "tries").c_str(), 0);
            if (!statusInProgress(st) || tries == LXMF_TRIES_GAVEUP) continue;
            hits.emplace_back(peer, key);
        }
    for (const auto& h : hits) processCancel(id, h.first, h.second);
    info("id %d: cancelled %zu unfinished outbound", id.index, hits.size());
}

/* cmd.delete — wipe a message record (frees `wire` storage), or, when
 * `mid` is empty (sentinel value was "<peer>/" or "<peer>"), the whole
 * conversation subtree. The whole-conversation form is the primitive
 * evictable_storage.md reuses. */
static void processDelete(lxmf_id_t& id, const std::string& peer_hex,
                          const std::string& mid)
{
    /* A delete of something already gone is a no-op, not a second deletion.
     * The sentinel has more than one writer (both frontends, and lxmproxy
     * acknowledging a record it has handed over), so the same message can be
     * asked for twice; without this the log reports a deletion per ask and
     * every one of them reads as a distinct message lost. */
    if (!mid.empty() &&
        !storageExists(msgPath(id.index, peer_hex, mid, "status").c_str())) {
        dbg("id %d: delete %s/%s — already gone", id.index,
            peer_hex.c_str(), mid.c_str());
        return;
    }

    /* An outbound the proxy is holding for us goes with it. Deleting a message
     * here and leaving a machine elsewhere still trying to send it is the one
     * outcome nobody would expect from a delete — and if it has already gone
     * out, the server simply stops owing us its status. Before the local wipe,
     * while the key is still ours to name. */
    if (!mid.empty() && proxyIsClient(id.index) && proxyReady(id.index) &&
        storageGetStr(msgPath(id.index, peer_hex, mid, "dir").c_str(), "") == "out") {
        uint8_t dh[16];
        if (hexToDestHash(peer_hex, dh))
            proxySendFrame(id.index, lxmproxyBuildDrop(mid, dh));
    }

    /* storageDeleteTree → deleteFromTree splits on the LAST dot, so the
     * argument must be the node path WITHOUT a trailing dot (cf.
     * destroyIdentity's idPath(n,"")). msgPrefix's trailing dot is for
     * storageForEach/collectTokens only — do not reuse it here. */
    queueRemove(id.index, peer_hex, mid);
    /* Stop any in-flight send(s) for what we're about to wipe: push
     * OUT_CANCEL so rnsd clears the pending send and stops emitting route
     * requests for a recipient no one is messaging anymore. Free the
     * outbox slot locally too — the resulting OUT_RESULT (cancelled)
     * then no-ops in applyOutResult (unknown send_id) instead of
     * re-creating storage under a just-deleted message. mid empty =
     * whole conversation → cancel every slot for this peer. */
    for (auto& o : id.outboxes) {
        if (!o.used || o.peer != peer_hex) continue;
        if (!mid.empty() && o.msg_key != mid) continue;
        if (id.handle >= 0) {
            uint8_t f[3] = {
                RNSD_DEST_OUT_CANCEL,
                (uint8_t)(o.send_id >> 8),
                (uint8_t)(o.send_id & 0xFF),
            };
            itsSend(id.handle, f, sizeof(f), pdMS_TO_TICKS(200));
            dbg("id %d: delete cancels in-flight send_id=%u (%s/%s)",
                id.index, (unsigned)o.send_id, peer_hex.c_str(),
                o.msg_key.c_str());
        }
        /* Proof-phase slots already settled pending at "sent". */
        if (!o.awaiting_proof && id.pending > 0) id.pending--;
        o.used = false;
        o.awaiting_proof = false;
    }

    /* The conversation directory is MAINTAINED, not derived (bumpConvDirectory):
     * `count` and `unread` are counters, so a message that goes away has to be
     * counted back out or the badge keeps a deleted message alive and the list
     * shows a total the thread cannot account for. Read the two fields the
     * correction needs while the record still exists. Unread-ness is not stored
     * per message — it is `dir == "in"` and a timestamp past the conversation's
     * read watermark, the same test the thread view applies. The whole-
     * conversation branch below drops the contact subtree entirely, counters
     * and all, so this is the single-message case only. */
    if (!mid.empty()) {
        bool inbound = storageGetStr(msgPath(id.index, peer_hex, mid, "dir").c_str(),
                                     "") == "in";
        int  ts      = storageGetInt(msgPath(id.index, peer_hex, mid, "ts").c_str(), 0);
        int  count   = storageGetInt(contactPath(id.index, peer_hex, "count").c_str(), 0);
        int  unread  = storageGetInt(contactPath(id.index, peer_hex, "unread").c_str(), 0);
        storageBegin();
        if (count > 0)
            storageSet(contactPath(id.index, peer_hex, "count").c_str(), count - 1);
        if (inbound && unread > 0 && ts > convReadTs(id.index, peer_hex))
            storageSet(contactPath(id.index, peer_hex, "unread").c_str(), unread - 1);
        storageEnd();
    }

    char path[160];
    if (mid.empty())
        std::snprintf(path, sizeof(path), "s.lxmf.id.%d.msgs.%s",
                      id.index, peer_hex.c_str());
    else
        std::snprintf(path, sizeof(path), "s.lxmf.id.%d.msgs.%s.%s",
                      id.index, peer_hex.c_str(), mid.c_str());
    storageDeleteTree(path);
    if (mid.empty()) {
        /* Whole-conversation delete is also the "remove contact" action:
         * the contact (address book entry) is derived purely from the
         * synced contacts.<peer> subtree, so dropping only the messages
         * leaves the peer behind in every frontend. Wipe the contact
         * subtree too. The client re-stubs a contact solely from stored
         * messages (convPeers), which are now gone, so it stays removed. */
        char cpath[160];
        std::snprintf(cpath, sizeof(cpath), "s.lxmf.id.%d.contacts.%s",
                      id.index, peer_hex.c_str());
        storageDeleteTree(cpath);
        info("id %d: deleted conversation + contact %s",
             id.index, peer_hex.c_str());
    } else {
        info("id %d: deleted msg %s/%s", id.index,
             peer_hex.c_str(), mid.c_str());
    }
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
                connectOurDest(s_ids[n]);
                /* An identity nobody has ever announced does not exist as far
                 * as the mesh is concerned, and the one-shot startup announce
                 * fired long before this slot did. Set the stored announce
                 * here; rnsd coalesces it with anything else set in the same
                 * minute and airs it on every interface. */
                sendAnnounce(s_ids[n]);
            }
        }
        else if (std::strcmp(tail, "identity_import") == 0) {
            /* `<128-hex>` or `<128-hex>|<display_name>[|<role>]`. The tail is
             * what an lxmproxy server hands in when it takes an account over:
             * the name the account already advertises, and the role that says
             * this slot is hosted here for somebody else. */
            std::string raw = val, hex = raw, dn, role;
            size_t bar = raw.find('|');
            if (bar != std::string::npos) {
                hex = raw.substr(0, bar);
                dn  = raw.substr(bar + 1);
                size_t bar2 = dn.find('|');
                if (bar2 != std::string::npos) { role = dn.substr(bar2 + 1); dn = dn.substr(0, bar2); }
            }
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
                        storageDefault(idPath(n, "default_method").c_str(), "auto");
                        if (!dn.empty())   storageSet(idPath(n, "display_name").c_str(), dn.c_str());
                        if (!role.empty()) storageSet(idPath(n, "proxy_role").c_str(), role.c_str());
                        storageEnd();
                        connectOurDest(s_ids[n]);
                        sendAnnounce(s_ids[n]);   /* as for identity_new */
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
        else if (std::strcmp(tail, "pn_sync") == 0) {
            /* "all" → every check-marked node; a 32-hex value → that node
             * (whether or not check-marked). Queued per usable identity.
             * "all" stands in for this interval's periodic pass, so it
             * re-arms the timer a whole interval out. */
            std::string v = val;
            if (v.size() == 32) {
                pnEnqueueSync(v);
            } else {
                auto nodes = pnNodeList();
                /* With nothing marked the timer never fires, so an explicit
                 * check means every configured node — asking and getting
                 * nothing is the one useless answer. */
                bool any_marked = false;
                for (auto& nd : nodes) if (nd.check) any_marked = true;
                for (auto& nd : nodes)
                    if (nd.check || !any_marked) pnEnqueueSync(nd.hex);
                pnRearmNextCheck();
            }
        }
        else if (std::strcmp(tail, "pn_add") == 0) {
            /* "<32hex>[|name]" → append at the first free list index (the
             * on-device settings pane's add path; the web writes the list
             * directly). */
            std::string v = val, name;
            size_t bar = v.find('|');
            if (bar != std::string::npos) { name = v.substr(bar + 1); v = v.substr(0, bar); }
            uint8_t dh[16];
            if (!hexToBytes(v.c_str(), v.size(), dh, 16)) {
                warn("pn_add: bad hash \"%s\"", val);
            } else {
                int slot = -1;
                for (int i = 0; i < LXMF_PN_MAX && slot < 0; ++i) {
                    char k[40];
                    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.hash", i);
                    if (storageGetStr(k, "").size() != 32) slot = i;
                }
                if (slot < 0) {
                    warn("pn_add: list full (%d nodes)", LXMF_PN_MAX);
                } else {
                    char k[40];
                    storageBegin();
                    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.hash", slot);
                    storageSet(k, v.c_str());
                    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.name", slot);
                    storageSet(k, name.c_str());
                    std::snprintf(k, sizeof k, "s.lxmf.pn.%d.check", slot);
                    storageSet(k, 1);
                    storageEnd();
                    info("pn_add: node %s at index %d", v.c_str(), slot);
                }
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
static void publishLinks(void);   /* defined below; called for a snappy icon update */

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

    /* send/cancel/delete value is "<peer>/<key>" (per-contact store).
     * delete also accepts "<peer>/" or "<peer>" → whole conversation.
     * send accepts an optional third segment "<peer>/<key>/pn:<hash>" —
     * upload to that propagation node instead of the direct paths.
     * link_open/link_close/ping take a bare "<peer>".
     * announce ignores the value. */
    std::string raw = val;
    std::string peer_hex, mid, via;
    size_t slash = raw.find('/');
    if (slash == std::string::npos) {
        peer_hex = raw;                 /* delete: whole conversation */
    } else {
        peer_hex = raw.substr(0, slash);
        mid      = raw.substr(slash + 1);
        size_t slash2 = mid.find('/');
        if (slash2 != std::string::npos) {
            via = mid.substr(slash2 + 1);
            mid = mid.substr(0, slash2);
        }
    }

    try {
        if (std::strcmp(verb, "announce") == 0) {
            if (id.used) sendAnnounce(id);
            else         warn("id %d: announce requested but slot is empty", n);
        }
        else if (!id.used) {
            warn("id %d: cmd.%s for absent identity", n, verb);
        }
        else if (std::strcmp(verb, "send") == 0) {
            if (mid.empty()) {
                warn("id %d: cmd.send needs <peer>/<key> (got \"%s\")", n, val);
            } else if (via.rfind("pn:", 0) == 0) {
                uint8_t node[16];
                std::string nh = via.substr(3);
                if (!hexToBytes(nh.c_str(), nh.size(), node, 16))
                    warn("id %d: cmd.send bad pn hash \"%s\"", n, nh.c_str());
                else
                    pnUploadStart(id, peer_hex, mid, node);
            } else if (!via.empty()) {
                warn("id %d: cmd.send unknown via \"%s\"", n, via.c_str());
            } else {
                processSend(id, peer_hex, mid);
            }
        }
        else if (std::strcmp(verb, "retry") == 0) {
            /* Try this one again NOW. Proxied, that is a frame naming the
             * record — the body is already on the server and putting it back on
             * the air to say "again" would cost the whole message to carry one
             * bit. Unproxied there is nothing to ask: this device owns the
             * queue, so it is an ordinary send. */
            if (mid.empty()) {
                warn("id %d: cmd.retry needs <peer>/<key> (got \"%s\")", n, val);
            } else if (proxyIsClient(n)) {
                uint8_t dh[16];
                if (!proxyReady(n))
                    warn("id %d: cmd.retry with no Channel to the proxy", n);
                else if (!hexToDestHash(peer_hex, dh))
                    warn("id %d: cmd.retry bad peer \"%s\"", n, peer_hex.c_str());
                else
                    proxySendFrame(n, lxmproxyBuildRetry(mid, dh));
            } else {
                processSend(id, peer_hex, mid);
            }
        }
        else if (std::strcmp(verb, "cancel") == 0) {
            if (peer_hex == "all" && mid.empty())
                processCancelAll(id);
            else if (mid.empty())
                warn("id %d: cmd.cancel needs <peer>/<key> or \"all\" (got \"%s\")", n, val);
            else
                processCancel(id, peer_hex, mid);
        }
        else if (std::strcmp(verb, "delete") == 0) {
            processDelete(id, peer_hex, mid);
        }
        else if (std::strcmp(verb, "link_open") == 0) {
            uint8_t dh[16];
            if (peer_hex.size() != 32 || !hexToDestHash(peer_hex, dh))
                warn("id %d: link_open bad peer \"%s\"", n, peer_hex.c_str());
            else if (!convGet(id, peer_hex, dh, /*open_if_missing=*/true))
                warn("id %d: link_open to %s failed", n, peer_hex.c_str());
            publishLinks();
        }
        else if (std::strcmp(verb, "link_close") == 0) {
            if (convlink_t* c = convFind(id.index, peer_hex)) convDrop(*c);
            publishLinks();
        }
        else if (std::strcmp(verb, "ping") == 0) {
            pingStart(id, peer_hex);
        }
        else if (std::strcmp(verb, "fetch") == 0) {
            if (mid.empty())
                warn("id %d: cmd.fetch needs <peer>/<message_id>", n);
            else
                proxyFetch(id, peer_hex, mid);
        }
        else if (std::strcmp(verb, "proxy_on") == 0) {
            proxyOn(id, peer_hex);     /* the whole value is the server's dest */
        }
        else if (std::strcmp(verb, "proxy_off") == 0) {
            proxyOff(id);
        }
        else if (std::strcmp(verb, "proxy_force_off") == 0) {
            proxyForceOff(id);
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

static std::string idSetProxyKey(int n)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "lxmf.id.%d.setproxy", n);
    return buf;
}

static void subscribePerIdCmds(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return;
    storageSubscribeChanges(idCmdScope(n).c_str(), s_id_cmd_stubs[n]);
    storageSubscribeChanges(idSetProxyKey(n).c_str(), s_setproxy_stubs[n]);
}

static void unsubscribePerIdCmds(int n)
{
    if (n < 0 || n >= LXMF_MAX_IDENTITIES) return;
    storageUnsubscribe(idCmdScope(n).c_str());
    storageUnsubscribe(idSetProxyKey(n).c_str());
}

/* ─────────────── periodic publish ─────────────── */

static TickType_t s_lastPublishTick = 0;
static TickType_t s_lastBackfillTick = 0;   /* throttles backfillContactNames() */
#define LXMF_PUBLISH_INTERVAL_MS 1000
#define LXMF_PUBLISH_IDLE_MS     5000   /* no UI watching: stretch the tick for light sleep */

/* How often the maintenance tick fires. When a UI wants telemetry (LCD build or
 * WiFi up so a browser can read the keys) keep it snappy at 1 Hz; on a headless,
 * WiFi-down node nobody reads the stats, so drop to 5 s — the bundled maintenance
 * (link reaping, send resolution) is all minute-scale and tolerates it, and this
 * stops the tick from capping light sleep. */
static uint32_t lxmfTickIntervalMs(void) {
    return uiTelemetryWanted() ? LXMF_PUBLISH_INTERVAL_MS : LXMF_PUBLISH_IDLE_MS;
}

/* When non-zero, the absolute tick at which the startup announce fires. Armed
 * once at task startup and never by interface events: a NEW interface gets our
 * last announce from rnsd's per-interface replay, pinned to that interface —
 * re-announcing from here would broadcast, and a flapping peer's
 * register/deregister cycle then spends every OTHER radio's airtime (LoRa most
 * expensively) on its churn. After the startup announce, only the periodic
 * schedule announces. */
static TickType_t s_announce_due_tick = 0;
/* The first announce of the session is held this long after task start, so rnsd
 * + the transports are up and stable before we advertise. */
#define LXMF_FIRST_ANNOUNCE_DELAY_MS 30000

/* Both publishers below exist for readers — an on-device pane or a browser —
 * and with neither present every key they write is one nobody will ever read.
 * The cost is not the key: it is the storage window, the compare-read per key,
 * the string building, and the change fan-out that wakes the storage actor and
 * the notify task behind it. So they stand down entirely on a headless,
 * WiFi-down node, and the tick they hang off becomes a walk over a few small
 * arrays. A UI appearing means WiFi came up (or the build has a screen, where
 * the gate is always open), so the next tick repopulates the keys — the same
 * contract iface-lora and rnsd publish under. */
static void publishStats(void)
{
    if (!uiTelemetryWanted()) return;
    storageBegin();
    storageSet("lxmf.up", 1);
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        lxmf_id_t& id = s_ids[n];
        if (!id.used) continue;
        /* Only on change — these are under lxmf.id.*, which the on-device UI
         * subscribes to; rewriting unchanged counts at 1 Hz rebuilt the LXMF UI
         * every second (idle CPU, even when hidden). */
        setIntIfChanged(idEphPath(n, "stats.sent"),     (int)id.sent);
        setIntIfChanged(idEphPath(n, "stats.received"), (int)id.received);
        setIntIfChanged(idEphPath(n, "stats.pending"),  (int)id.pending);
        setIntIfChanged(idEphPath(n, "stats.failed"),   (int)id.failed);
        /* The settings rows for this slot, as finished text: whether the slot
         * is occupied at all (the gate every row of the block hangs on), the
         * name to show, and the traffic counters as one line. Composing them
         * here is what lets a static descriptor describe an identity block. */
        setIntIfChanged(idEphPath(n, "used"), 1);
        std::string label = storageGetStr(idPath(n, "display_name").c_str(), "");
        if (label.empty()) label = storageGetStr(idPath(n, "label").c_str(), "");
        if (label.empty()) label = "(unnamed)";
        setStrIfChanged(idEphPath(n, "label_text"), label.c_str());
        char traffic[80];
        std::snprintf(traffic, sizeof traffic, "sent %u \xC2\xB7 received %u \xC2\xB7 pending %u \xC2\xB7 failed %u",
                      (unsigned)id.sent, (unsigned)id.received,
                      (unsigned)id.pending, (unsigned)id.failed);
        setStrIfChanged(idEphPath(n, "traffic"), traffic);
        setStrIfChanged(idEphPath(n, "state_text"),
                        proxyIsClient(n) ? "proxied"
                        : storageGetInt(idEphPath(n, "up").c_str(), 0) ? "up" : "down");
    }
    storageEnd();
    pnPublishStatus();
}

/* The per-destination inbound gate, mirrored from `lxmf.id.<n>.accept` onto
 * rnsd. A store that has run out of room for an account closes it and rnsd then
 * drops that destination's inbound WITHOUT proving, which leaves the message on
 * the sender's side where their own retry loop holds it — the only "mailbox
 * full" LXMF has. The key is ephemeral: a fresh boot accepts, and whoever wants
 * it shut re-asserts. Only lxmf holds the destination handle, which is why the
 * gate is a key here rather than a call from the straddle that needs it. */
static void applyAcceptGates(void)
{
    static uint8_t s_applied[LXMF_MAX_IDENTITIES] = { 1, 1, 1, 1 };
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        lxmf_id_t& id = s_ids[n];
        if (!id.used || id.handle < 0) continue;
        uint8_t want = storageGetInt(idEphPath(n, "accept").c_str(), 1) ? 1 : 0;
        if (want == s_applied[n]) continue;
        if (rnsdDestSetAccept(id.handle, want != 0)) {
            s_applied[n] = want;
            info("id %d: inbound %s", n, want ? "accepting" : "gated (store full)");
        }
    }
}

/* Per-peer conversation-link state for the UIs, ephemeral and keyed by
 * peer: lxmf.id.<n>.link.<peer> = "active" | "establishing". The key is
 * UNSET when no link is open, so a link torn down for ANY reason (remote
 * close, idle reap, failure, or a local close command) clears the header
 * icon on the next tick — this is re-derived from the live pool every
 * second, not event-driven. Only outgoing conversation links (ones we
 * opened) count — the icon never reflects a peer's inbound link into us,
 * since we don't send over those. */
static std::vector<std::string> s_pubLinkKeys;
static void publishLinks(void)
{
    if (!uiTelemetryWanted()) return;   /* header icons, for a reader that isn't there */
    std::vector<std::string> keys, vals;
    auto note = [&](int n, const std::string& peer, const char* state) {
        if (peer.size() != 32) return;
        std::string k = idEphPath(n, ("link." + peer).c_str());
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) {                 /* active wins over establishing */
                if (std::strcmp(state, "active") == 0) vals[i] = "active";
                return;
            }
        keys.push_back(k);
        vals.emplace_back(state);
    };
    for (auto& c : s_convlinks) {
        if (!c.used || c.handle < 0) continue;
        std::string st = storageGetStr(("rnsd.links." + c.tag + ".state").c_str(), "");
        if (st == "failed" || st == "closed" || st == "closing") continue;  /* down */
        note(c.id_index, c.peer_hex, st == "active" ? "active" : "establishing");
    }
    storageBegin();
    for (auto& old : s_pubLinkKeys) {           /* clear peers no longer open */
        bool still = false;
        for (auto& k : keys) if (k == old) { still = true; break; }
        if (!still) storageUnset(old.c_str());
    }
    for (size_t i = 0; i < keys.size(); ++i)
        setStrIfChanged(keys[i], vals[i].c_str());
    storageEnd();
    s_pubLinkKeys.swap(keys);
}

/* collectTokens is defined later (in the CLI section). */
/* Count entries under `lxmf.announces.` without allocating per-entry —
 * each entry is one packed leaf, so we just count leaves. Replaces the
 * old `collectTokens` call which built a std::vector<std::string> per
 * summary tick. */
static int s_ann_count_tmp = 0;
[[maybe_unused]] static int annCount(void)
{
    s_ann_count_tmp = 0;
    forEachAnnounce([](const std::string&, const AnnounceEntry&) { s_ann_count_tmp++; });
    return s_ann_count_tmp;
}

/* ─────────────── bootstrap ─────────────── */

/* Boot-time re-assertion of the directory claim on every stored contact, and of
 * the key we hold for it. rnsd keeps claims compiled into its persisted image,
 * so the claim is usually a restamp rather than news — but a discarded image
 * costs the claim its record AND every key in it, and a claim alone only
 * reserves an empty one. Seeding the stored keys back is what makes a discarded
 * image cost the routes it should cost and nothing more: a message from a known
 * contact still verifies on arrival, with no path request in the way. */
static void lxmfPreloadContactClaims(void)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; n++) {
        s_seedPeers.clear();
        std::string cpre = "s.lxmf.id." + std::to_string(n) + ".contacts";
        storageForEach(cpre.c_str(), seedCollectPeer);
        for (auto& peer : s_seedPeers) {
            lxmfClaimContact(peer);
            lxmfSyncContactKey(n, peer);
        }
    }
    s_seedPeers.clear();
}

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
struct MsgRef { std::string peer; std::string key; };
static std::vector<MsgRef>      s_msgs_list;          /* (peer, msg key) from last `msgs`/thread */
static const char*              s_peer_list_label = "";  /* "contacts" / "announces" / "chats" */

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
    ensureConvFile(id_n, peer_hex);
    storageBegin();
    storageSet(msgPath(id_n, peer_hex, mid, "dir").c_str(),     "out");
    storageSet(msgPath(id_n, peer_hex, mid, "peer").c_str(),    peer_hex.c_str());
    storageSet(msgPath(id_n, peer_hex, mid, "title").c_str(),   "");
    storageSet(msgPath(id_n, peer_hex, mid, "content").c_str(), text.c_str());
    storageSet(msgPath(id_n, peer_hex, mid, "status").c_str(), (int)LXMF_ST_DRAFT);
    /* cmd.send fires *last* so the record is fully present when the
     * lxmf task sees the sentinel. Value is "<peer>/<key>". */
    char send_key[40];
    std::snprintf(send_key, sizeof(send_key), "lxmf.id.%d.cmd.send", id_n);
    storageSet(send_key, (peer_hex + "/" + mid).c_str());
    storageEnd();
    cliPrintf("queued %s → %s\n", mid.c_str(), peer_hex.c_str());
}

/* Resolve a CLI `<peer>` argument. Accepts:
 *   - 32-char hex (the LXMF destination hash) → returned directly
 *   - a positive integer N → the Nth entry from the most recent
 *     `lxmf contacts` / `lxmf announces` listing
 *   - any other text → case-insensitive substring match against the
 *     selected identity's contacts (nick and display_name) and against
 *     `lxmf.announces.<hex>.display_name`; exactly one match returns
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

static void peerNameLookupRec(const std::string& hex, const AnnounceEntry& e)
{
    if (!s_peer_name_ctx) return;
    if (!nameContainsCI(e.name, s_peer_name_ctx->query)) return;
    for (const auto& m : s_peer_name_ctx->matches)   /* already via contacts */
        if (m.hash == hex) return;
    s_peer_name_ctx->matches.push_back({hex, e.name});
}

/* Contact half of the name lookup: a nick is local and often the only
 * handle a peer has here, so it matches alongside display_name. */
static void peerNameLookupContacts(int sel, PeerNameLookupCtx& ctx)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.contacts.", sel);
    for (const auto& h : collectTokens(prefix)) {
        std::string nick = storageGetStr(contactPath(sel, h, "nick").c_str(), "");
        std::string name = storageGetStr(contactPath(sel, h, "display_name").c_str(), "");
        if (!nameContainsCI(nick, ctx.query) && !nameContainsCI(name, ctx.query))
            continue;
        if (name.empty()) name = nick;
        ctx.matches.push_back({h, name});
    }
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
            cliPrintf("no peer list — run `lxmf contacts` or `lxmf announces` first\n");
            return "";
        }
        if ((size_t)n > s_peer_list.size()) {
            cliPrintf("peer index %ld out of range (last %s had %zu entries)\n",
                      n, s_peer_list_label, s_peer_list.size());
            return "";
        }
        return s_peer_list[(size_t)n - 1];
    }

    /* Treat the remainder as a case-insensitive substring lookup: our own
     * contacts first (nick or display_name), then the cross-identity
     * announce catalogue for peers we have no contact record for. */
    PeerNameLookupCtx ctx{arg, {}};
    peerNameLookupContacts(selectedId(), ctx);
    s_peer_name_ctx = &ctx;
    forEachAnnounce(peerNameLookupRec);
    s_peer_name_ctx = nullptr;

    if (ctx.matches.empty()) {
        cliPrintf("no peer matches \"%s\"\n", arg.c_str());
        return "";
    }
    if (ctx.matches.size() == 1) return ctx.matches[0].hash;

    /* Multiple matches — refuse the command, but populate the peer list
     * so the user can pick by line number on a retry:
     *   lxmf send 2 "hi"   */
    s_peer_list.clear();
    s_peer_list_label = "name match";
    cliPrintf("ambiguous \"%s\" — %zu matches:\n",
              arg.c_str(), ctx.matches.size());
    cliPrintf("%-3s %-32s %s\n", "#", "destination", "name");
    int row = 1;
    for (const auto& m : ctx.matches) {
        s_peer_list.push_back(m.hash);
        cliPrintf("%-3d %-32s %s\n",
                  row++, m.hash.c_str(), sanitizeForLog(m.name).c_str());
    }
    cliPrintf("(retry with the line number, a longer substring, or the 32-hex hash)\n");
    return "";
}

/* ── `lxmf id` ── */

static void cliId(const char* rest)
{
    /* No arg: print all. */
    while (*rest == ' ') rest++;
    if (!*rest) {
        int sel = selectedId();
        /* The leading column is the selection mark, and the header carries it
         * too (as a blank) or every column below sits two characters right of
         * its own title. Both hashes: peers write to the DESTINATION, while
         * the IDENTITY is what a node sees when this account identifies on a
         * link — an operator matching an allow list needs that one. */
        cliPrintf("%s %-3s %-12s %-32s %s\n", " ", "id", "label",
                  "destination", "identity");
        for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
            lxmf_id_t& id = s_ids[n];
            if (!id.used) continue;
            std::string lbl = storageGetStr(idPath(n, "label").c_str(), "");
            uint8_t ih[RNSD_IDENT_HASH_LEN] = {};
            bool have_ih = rnsdIdentityHash(id.identity_key.c_str(), ih);
            cliPrintf("%s %-3d %-12s %-32s %s\n",
                      n == sel ? "*" : " ", n, lbl.c_str(),
                      bytesToHex(id.dest_hash, LXMF_DEST_HASH_LEN).c_str(),
                      have_ih ? bytesToHex(ih, RNSD_IDENT_HASH_LEN).c_str() : "?");
        }
        return;
    }
    /* With arg: switch. */
    int n = std::atoi(rest);
    lxmf_id_t* id = idAt(n);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", n); return; }
    storageSet("s.lxmf.cli.selected_id", n);
    uint8_t ih[RNSD_IDENT_HASH_LEN] = {};
    bool have_ih = rnsdIdentityHash(id->identity_key.c_str(), ih);
    cliPrintf("selected id %d (dest %s, identity %s)\n", n,
              bytesToHex(id->dest_hash, LXMF_DEST_HASH_LEN).c_str(),
              have_ih ? bytesToHex(ih, RNSD_IDENT_HASH_LEN).c_str() : "?");
}

/* ── `lxmf chats` / `lxmf msgs [<peer>|<status>]` ──
 *
 * The per-contact store makes
 * this two-level: no arg → conversation list; <peer> → that thread;
 * a bare status name → cross-conversation filter. */

struct MsgRow {
    std::string peer;
    std::string key;
    std::string dir;
    std::string status;   /* lxmfStatusName() of the record's status code */
    std::string title;
    int         ts;
    int         read;
};

/* A typed word → the canonical ALL-CAPS status name, "" if it names none.
 * Case-insensitive, so `lxmf msgs delivered` works. This is the one place a
 * status is resolved from text; stored codes are still never parsed back. */
static std::string statusWordCanon(const std::string& s)
{
    if (s.empty()) return "";
    std::string up = s;
    for (char& c : up) c = (char)std::toupper((unsigned char)c);
    /* The whole byte, not up to some named member: the enum has gaps in it and
     * grows at the end, and a bound written as the last name it had goes stale
     * silently — the filter simply stops matching the newest statuses. Unused
     * codes name nothing, so walking all of them costs a null check each. */
    for (int code = 0; code < 256; code++) {
        const char* name = lxmfStatusName((uint8_t)code);
        if (*name && up == name) return name;
    }
    return "";
}

static std::string peerDisplayName(int sel, const std::string& peer)
{
    std::string nm = storageGetStr(contactPath(sel, peer, "display_name").c_str(), "");
    if (nm.empty())
        nm = bestHeardName(peer);
    return nm;
}

static MsgRow readMsgRow(int sel, const std::string& peer, const std::string& key)
{
    MsgRow r;
    r.peer  = peer;
    r.key   = key;
    r.dir   = storageGetStr(msgPath(sel, peer, key, "dir").c_str(),   "");
    r.status = lxmfStatusName((uint8_t)storageGetInt(msgPath(sel, peer, key, "status").c_str(), 0));
    r.title = storageGetStr(msgPath(sel, peer, key, "title").c_str(), "");
    r.ts    = storageGetInt(msgPath(sel, peer, key, "ts").c_str(),    0);
    r.read  = (r.ts <= convReadTs(sel, peer)) ? 1 : 0;   /* per-conversation watermark */
    return r;
}

static void printMsgRows(int sel, std::vector<MsgRow>& rows, bool show_peer)
{
    std::sort(rows.begin(), rows.end(),
              [](const MsgRow& a, const MsgRow& b) { return a.ts > b.ts; });
    s_msgs_list.clear();
    if (rows.empty()) return;
    if (show_peer)
        cliPrintf("%-3s %-3s %-17s %-16s %-6s %s\n",
                  "#", "dir", "status", "peer", "unread", "title");
    else
        cliPrintf("%-3s %-3s %-17s %-6s %s\n",
                  "#", "dir", "status", "unread", "title");
    int n = 1;
    for (const auto& r : rows) {
        s_msgs_list.push_back({ r.peer, r.key });
        const char* unread = (r.dir == "in" && !r.read) ? "*" : "";
        if (show_peer) {
            std::string p16 = r.peer.size() >= 16 ? r.peer.substr(0, 16) : r.peer;
            cliPrintf("%-3d %-3s %-17s %-16s %-6s %s\n",
                      n++, r.dir.c_str(), r.status.c_str(), p16.c_str(),
                      unread, r.title.c_str());
        } else {
            cliPrintf("%-3d %-3s %-17s %-6s %s\n",
                      n++, r.dir.c_str(), r.status.c_str(),
                      unread, r.title.c_str());
        }
    }
}

/* `lxmf chats` — one row per conversation (peer subtree). */
static void cliChats(int sel)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.", sel);
    auto peers = collectTokens(prefix);

    struct ChatRow {
        std::string peer, name, last_title;
        int count = 0, unread = 0, last_ts = 0;
    };
    std::vector<ChatRow> rows;
    rows.reserve(peers.size());
    for (const auto& peer : peers) {
        ChatRow c;
        c.peer = peer;
        c.name = peerDisplayName(sel, peer);
        int read_ts = convReadTs(sel, peer);
        for (const auto& key : collectTokens(msgPrefix(sel, peer))) {
            c.count++;
            int ts  = storageGetInt(msgPath(sel, peer, key, "ts").c_str(), 0);
            if (ts > read_ts &&
                storageGetStr(msgPath(sel, peer, key, "dir").c_str(), "") == "in")
                c.unread++;
            if (ts >= c.last_ts) {
                c.last_ts    = ts;
                c.last_title = storageGetStr(msgPath(sel, peer, key, "title").c_str(), "");
            }
        }
        rows.push_back(std::move(c));
    }
    std::sort(rows.begin(), rows.end(),
              [](const ChatRow& a, const ChatRow& b) { return a.last_ts > b.last_ts; });

    s_peer_list.clear();
    s_peer_list_label = "chats";
    cliPrintf("id %d  %zu conversation(s)\n", sel, rows.size());
    if (rows.empty()) return;
    cliPrintf("%-3s %-16s %-5s %-6s %-11s %s\n",
              "#", "peer", "msgs", "unread", "last", "who");
    int n = 1;
    for (const auto& c : rows) {
        s_peer_list.push_back(c.peer);
        std::string p16 = c.peer.size() >= 16 ? c.peer.substr(0, 16) : c.peer;
        std::string who = !c.name.empty() ? c.name
                        : (c.last_title.empty() ? "(unknown)" : c.last_title);
        cliPrintf("%-3d %-16s %-5d %-6d %-11d %s\n",
                  n++, p16.c_str(), c.count, c.unread, c.last_ts,
                  sanitizeForLog(who).c_str());
    }
}

static void cliMsgs(const char* rest)
{
    while (*rest == ' ') rest++;
    std::string arg = rest;
    int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    if (arg.empty()) { cliChats(sel); return; }

    /* The filter takes any status name, and there was no way to find out which
     * from here — a filter whose vocabulary is only in the source is a filter
     * for whoever wrote it. Grouped by what the name tells you about the
     * message, since that is what a reader is choosing between. */
    if (arg == "?" || arg == "statuses") {
        /* Named, not ranged: the enum has gaps and a range would quietly take in
         * whatever is added between its ends. Anything not named below still
         * matches as a filter — it falls into "gave up" and is listed there. */
        static const uint8_t kSettled[] = { LXMF_ST_DELIVERED, LXMF_ST_CANCELLED,
                                            LXMF_ST_RECEIVED,
                                            LXMF_ST_OUR_PROXY_DELIVERED };
        static const uint8_t kElsewhere[] = { LXMF_ST_ON_OUR_PROXY, LXMF_ST_ON_PN };
        auto line = [](const char* label, const uint8_t* v, size_t n) {
            cliPrintf("%s:\n ", label);
            for (size_t i = 0; i < n; i++) cliPrintf(" %s", lxmfStatusName(v[i]));
            cliPrintf("\n");
        };
        cliPrintf("in progress (the delivery queue will try again):\n ");
        for (int c = 0; c < 256; c++)
            if (statusInProgress(c)) cliPrintf(" %s", lxmfStatusName((uint8_t)c));
        cliPrintf("\n");
        line("settled",        kSettled,   sizeof kSettled);
        line("held elsewhere (another node is sending it; not ours to retry)",
                               kElsewhere, sizeof kElsewhere);
        cliPrintf("gave up or never started:\n ");
        for (int c = 0; c < 256; c++) {
            if (statusInProgress(c) || c == LXMF_ST_DRAFT) continue;
            bool named = false;
            for (uint8_t v : kSettled)   if (v == c) named = true;
            for (uint8_t v : kElsewhere) if (v == c) named = true;
            if (named) continue;
            const char* nm = lxmfStatusName((uint8_t)c);
            if (*nm) cliPrintf(" %s", nm);
        }
        cliPrintf("\nalso DRAFT. Case-insensitive: `lxmf msgs delivered`.\n");
        return;
    }

    const std::string want = statusWordCanon(arg);
    if (!want.empty()) {
        /* Cross-conversation filter — walk every peer subtree. */
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.", sel);
        std::vector<MsgRow> rows;
        for (const auto& peer : collectTokens(prefix))
            for (const auto& key : collectTokens(msgPrefix(sel, peer))) {
                MsgRow r = readMsgRow(sel, peer, key);
                if (r.status == want) rows.push_back(std::move(r));
            }
        cliPrintf("id %d  %zu message(s) with status=%s\n",
                  sel, rows.size(), want.c_str());
        printMsgRows(sel, rows, /*show_peer=*/true);
        return;
    }

    /* Otherwise a conversation: 32-hex / list-# / name substring. */
    std::string peer = cliResolvePeer(arg);
    if (peer.empty()) return;

    std::vector<MsgRow> rows;
    for (const auto& key : collectTokens(msgPrefix(sel, peer)))
        rows.push_back(readMsgRow(sel, peer, key));
    std::string name = peerDisplayName(sel, peer);
    cliPrintf("id %d  conversation with %s%s%s  %zu message(s)\n",
              sel,
              name.empty() ? peer.c_str() : sanitizeForLog(name).c_str(),
              name.empty() ? "" : " (", name.empty() ? "" : (peer + ")").c_str(),
              rows.size());
    printMsgRows(sel, rows, /*show_peer=*/false);
}

/* ── `lxmf unfinished` / `lxmf cancel` ──
 *
 * Unsettled = outbound this identity has not finished with: the delivery queue
 * will attempt it again. The test is `statusInProgress() && tries != GAVEUP` —
 * the SAME one queueResume uses to decide what to pick up after a reboot, and
 * deliberately not a second opinion about it, because a listing that disagreed
 * with the queue about what is still in play would be worse than no listing.
 *
 * Two things that look terminal are not, and one that looks live is not:
 *   - `AWAITING_PROOF` is unfinished. The message is on the air and nothing has
 *     answered; that is exactly the state worth seeing.
 *   - `CANCELLED` does NOT set tries = GAVEUP, so `tries == 255` alone is not an
 *     is-it-finished test. statusInProgress excludes it by status instead.
 *   - `ON_OUR_PROXY` / `ON_PN` are excluded: another node holds the message and
 *     is sending it, and OUR queue will not retry it. Cancelling here would
 *     settle a local record while the holder went on delivering, so the two
 *     would disagree about what happened. For the proxy there is now a frame
 *     that says stop — `DROP`, which a delete sends — but an `unfinished`
 *     listing is about what this queue will attempt, and this queue will not.
 */
struct UnsettledRow {
    std::string peer, key, status, title;
    int         ts = 0, tries = 0;
};

static std::vector<UnsettledRow> collectUnsettled(int sel)
{
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.msgs.", sel);
    std::vector<UnsettledRow> rows;
    for (const auto& peer : collectTokens(prefix))
        for (const auto& key : collectTokens(msgPrefix(sel, peer))) {
            if (storageGetStr(msgPath(sel, peer, key, "dir").c_str(), "") != "out") continue;
            const int st    = storageGetInt(msgPath(sel, peer, key, "status").c_str(), 0);
            const int tries = storageGetInt(msgPath(sel, peer, key, "tries").c_str(), 0);
            if (!statusInProgress(st) || tries == LXMF_TRIES_GAVEUP) continue;
            UnsettledRow r;
            r.peer   = peer;
            r.key    = key;
            r.status = lxmfStatusName((uint8_t)st);
            r.title  = storageGetStr(msgPath(sel, peer, key, "title").c_str(), "");
            r.ts     = storageGetInt(msgPath(sel, peer, key, "ts").c_str(), 0);
            r.tries  = tries;
            rows.push_back(std::move(r));
        }
    /* Oldest first: that is the order the queue works them in, and the one at
     * the top is the one holding its conversation up. */
    std::sort(rows.begin(), rows.end(),
              [](const UnsettledRow& a, const UnsettledRow& b) { return a.ts < b.ts; });
    return rows;
}

/* "18m" / "42s" / "3h" — a span is read at a glance or not at all. */
static std::string briefDur(long s)
{
    if (s < 0) s = 0;
    /* Sized for the widest `long` the compiler admits, plus its suffix. */
    char b[24];
    if      (s < 90)      std::snprintf(b, sizeof b, "%lds", s);
    else if (s < 90 * 60) std::snprintf(b, sizeof b, "%ldm", s / 60);
    else                  std::snprintf(b, sizeof b, "%ldh", s / 3600);
    return b;
}

static std::string briefAge(int ts)
{
    if (ts <= 0) return "-";
    return briefDur((long)(nowUnixMs() / 1000) - (long)ts);
}

static void cliUnsettled(void)
{
    const int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    std::vector<UnsettledRow> rows = collectUnsettled(sel);
    s_msgs_list.clear();
    if (rows.empty()) { cliPrintf("id %d  nothing unfinished\n", sel); return; }

    cliPrintf("%-3s %-16s %-17s %-5s %-5s %s\n",
              "#", "peer", "status", "tries", "age", "title");
    int n = 1;
    for (const auto& r : rows) {
        s_msgs_list.push_back({ r.peer, r.key });
        std::string nm = peerDisplayName(sel, r.peer);
        std::string who = nm.empty() ? r.peer.substr(0, 16) : sanitizeForLog(nm);
        if (who.size() > 16) who.resize(16);
        cliPrintf("%-3d %-16s %-17s %-5d %-5s %s\n",
                  n++, who.c_str(), r.status.c_str(), r.tries,
                  briefAge(r.ts).c_str(),
                  r.title.empty() ? "(no title)" : r.title.c_str());
    }
    /* When the queue will next walk them, since that is the question the
     * listing raises. Zero means the queue is idle and holds nothing. */
    const uint32_t now_s = (uint32_t)(nowUnixMs() / 1000);
    if (!s_queueNextSweep_s)
        cliPrintf("%zu unfinished, oldest %s — none queued for a sweep\n",
                  rows.size(), briefAge(rows.front().ts).c_str());
    else
        cliPrintf("%zu unfinished, oldest %s — next sweep in %s\n",
                  rows.size(), briefAge(rows.front().ts).c_str(),
                  briefDur(s_queueNextSweep_s > now_s
                           ? (long)(s_queueNextSweep_s - now_s) : 0).c_str());
}

/* `lxmf cancel <n>|all` — settle those CANCELLED through lxmf's own sentinel,
 * so the outbox slot, the delivery queue and rnsd's in-flight send are all
 * unwound by the one path that already knows how (processCancel). */
static void cliCancel(const char* rest)
{
    while (*rest == ' ') rest++;
    const int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
    if (!*rest) { cliPrintf("usage: lxmf cancel <n>|all  (see `lxmf unfinished`)\n"); return; }

    auto cancelOne = [&](const std::string& peer, const std::string& key) {
        char k[64];
        std::snprintf(k, sizeof k, "lxmf.id.%d.cmd.cancel", sel);
        /* "all" goes bare; a single message is "<peer>/<key>". */
        storageSet(k, key.empty() ? peer.c_str() : (peer + "/" + key).c_str());
    };

    if (std::strcmp(rest, "all") == 0) {
        /* One sentinel write, fanned out on lxmf's own task (processCancelAll):
         * writing this key once per message would overwrite it before the task
         * drained the previous value. The count is read back rather than
         * predicted, since the set is re-derived there and a message may settle
         * on its own between the two. */
        const size_t before = collectUnsettled(sel).size();
        if (!before) { cliPrintf("nothing unfinished\n"); return; }
        cancelOne("all", "");
        cliPrintf("cancelling %zu unfinished — `lxmf unfinished` to confirm\n", before);
        return;
    }

    const int n = std::atoi(rest);
    if (n < 1 || (size_t)n > s_msgs_list.size()) {
        cliPrintf("cancel: index out of range (run `lxmf unfinished` first)\n");
        return;
    }
    const MsgRef ref = s_msgs_list[(size_t)n - 1];
    /* The index space is shared with `msgs`, which lists inbound and settled
     * records too, so the row is re-read rather than trusted: cancelling
     * something already finished would overwrite a real outcome with CANCELLED. */
    const int st    = storageGetInt(msgPath(sel, ref.peer, ref.key, "status").c_str(), 0);
    const int tries = storageGetInt(msgPath(sel, ref.peer, ref.key, "tries").c_str(), 0);
    if (storageGetStr(msgPath(sel, ref.peer, ref.key, "dir").c_str(), "") != "out") {
        cliPrintf("cancel: #%d is inbound — only an outbound send can be cancelled\n", n);
        return;
    }
    if (!statusInProgress(st) || tries == LXMF_TRIES_GAVEUP) {
        cliPrintf("cancel: #%d has already settled (%s) — nothing to cancel\n",
                  n, lxmfStatusName((uint8_t)st));
        return;
    }
    cancelOne(ref.peer, ref.key);
    cliPrintf("cancelled %s → %s\n", ref.key.c_str(), ref.peer.substr(0, 16).c_str());
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
    const MsgRef& ref = s_msgs_list[(size_t)n - 1];
    const std::string& peer = ref.peer;
    const std::string& mid  = ref.key;

    std::string dir     = storageGetStr(msgPath(sel, peer, mid, "dir").c_str(),     "");
    uint8_t status      = (uint8_t)storageGetInt(msgPath(sel, peer, mid, "status").c_str(), 0);
    int tries           = storageGetInt(msgPath(sel, peer, mid, "tries").c_str(),   0);
    std::string title   = storageGetStr(msgPath(sel, peer, mid, "title").c_str(),   "");
    std::string content = storageGetStr(msgPath(sel, peer, mid, "content").c_str(), "");
    std::string reply_to = storageGetStr(msgPath(sel, peer, mid, "reply_to").c_str(), "");
    std::string quote    = storageGetStr(msgPath(sel, peer, mid, "reply_quote").c_str(), "");
    int ts              = storageGetInt(msgPath(sel, peer, mid, "ts").c_str(),       0);

    cliPrintf("─── id %d  msg #%d  %s ───\n", sel, n, mid.c_str());
    cliPrintf("dir:    %s\n", dir.c_str());
    cliPrintf("status: %s  (tries %d%s)\n", lxmfStatusName(status), tries,
              tries == LXMF_TRIES_GAVEUP ? " — gave up" : "");
    cliPrintf("peer:   %s\n", peer.c_str());
    cliPrintf("ts:     %d\n", ts);
    if (!reply_to.empty()) cliPrintf("reply_to: %s\n", reply_to.c_str());
    if (!quote.empty())    cliPrintf("quoting: %s\n", quote.c_str());
    if (!title.empty())  cliPrintf("title:  %s\n", title.c_str());
    cliPrintf("\n%s\n", content.c_str());

    if (dir == "in") convMarkRead(sel, peer, ts);   /* watermark up to this msg */
}

/* ── `lxmf c[ontacts] [<substring>]` ──
 *
 * The optional argument is a case-insensitive substring filter over both
 * display_name and nick — the two handles a contact is known by here. */

struct ContactRow {
    std::string hash;
    std::string nick;
    std::string display_name;
    int trust;
    int last_seen;
};

static void cliContacts(const char* rest)
{
    while (rest && *rest == ' ') rest++;
    std::string filter = (rest && *rest) ? std::string(rest) : "";
    while (!filter.empty() && filter.back() == ' ') filter.pop_back();

    int sel = selectedId();
    lxmf_id_t* id = idAt(sel);
    if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }

    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "s.lxmf.id.%d.contacts.", sel);
    auto hashes = collectTokens(prefix);

    std::vector<ContactRow> rows;
    rows.reserve(hashes.size());
    for (const auto& h : hashes) {
        /* A contact is a peer we've exchanged at least one message with — the
         * directory count, not the record's existence. A record also backs
         * announce-only state (display_name, a client-set pn), and those peers
         * belong to the announce catalogue (`lxmf announces`), not here. */
        if (storageGetInt(contactPath(sel, h, "count").c_str(), 0) <= 0) continue;
        ContactRow r;
        r.hash         = h;
        r.nick         = storageGetStr(contactPath(sel, h, "nick").c_str(),         "");
        r.display_name = storageGetStr(contactPath(sel, h, "display_name").c_str(), "");
        if (r.display_name.empty()) {
            /* Fall back to the cross-identity announce-catalogue entry. */
            char annKey[120];
            std::snprintf(annKey, sizeof(annKey),
                          "lxmf.announces.%s.name", h.c_str());
            r.display_name = storageGetStr(annKey, "");
        }
        if (!nameContainsCI(r.display_name, filter) &&
            !nameContainsCI(r.nick, filter)) continue;
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

    if (filter.empty())
        cliPrintf("id %d  %zu contact(s)\n", sel, rows.size());
    else
        cliPrintf("id %d  %zu contact(s) matching \"%s\"\n",
                  sel, rows.size(), filter.c_str());
    if (rows.empty()) return;
    cliPrintf("%-3s %-32s %-5s %-12s %s\n",
              "#", "destination", "trust", "nick", "display_name");
    int n = 1;
    for (const auto& r : rows) {
        s_peer_list.push_back(r.hash);
        cliPrintf("%-3d %-32s %-5d %-12s %s\n",
                  n++, r.hash.c_str(), r.trust,
                  r.nick.c_str(), r.display_name.c_str());
    }
}

/* ── `lxmf announces [<hash>|<name substring>]` ──
 *
 * Three modes:
 *   - no arg               → stream the entire announce catalogue
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
 * callback inherits that lock. For a few-thousand-row catalogue
 * that's ~tens of ms of held lock; lxmf's own storage writes during
 * that window queue at the announce-fanout ITS recv buffer, no data
 * loss. */

/* Streaming walker for `lxmf announces` (with optional substring
 * filter). Each `lxmf.announces.<hex>` is a single packed leaf; one
 * row per leaf. */
struct AnnounceStreamState {
    int row_num;
    int now_s;
};
static AnnounceStreamState s_ann_stream;
static std::string         s_ann_filter;   /* empty = no filter; not 32-hex */

static void annEmitRow(const std::string& hex, const AnnounceEntry& e)
{
    if (!nameContainsCI(e.name, s_ann_filter)) return;
    int age = (e.last_s > 0) ? (s_ann_stream.now_s - e.last_s) : -1;
    s_ann_stream.row_num++;
    s_peer_list.push_back(hex);
    cliPrintf("%-3d %-32s %-5d %-5d %-7d %s\n",
              s_ann_stream.row_num, hex.c_str(),
              e.hops, e.cost, age,
              sanitizeForLog(e.name).c_str());
}

/* Direct lookup — `lxmf announces <32-hex>`. One row, instant. */
static void cliAnnouncesByHash(const std::string& hex)
{
    int now_s = (int)(nowUnixMs() / 1000);
    AnnounceEntry e;
    if (!readAnnounce(hex, &e)) {
        cliPrintf("(no announce entry for %s)\n", hex.c_str());
        return;
    }
    int age = e.last_s > 0 ? (now_s - e.last_s) : -1;
    cliPrintf("%-3s %-32s %-5s %-5s %-7s %s\n",
              "#", "destination", "hops", "cost", "age(s)", "name");
    s_peer_list.clear();
    s_peer_list_label = "announces";
    s_peer_list.push_back(hex);
    cliPrintf("%-3d %-32s %-5d %-5d %-7d %s\n",
              1, hex.c_str(), e.hops, e.cost, age,
              sanitizeForLog(e.name).c_str());
}

static void cliAnnounces(const char* rest)
{
    while (rest && *rest == ' ') rest++;
    std::string arg = (rest && *rest) ? std::string(rest) : "";

    /* 32-hex → direct lookup. */
    if (arg.size() == 32) {
        uint8_t dh[16];
        if (hexToDestHash(arg, dh)) { cliAnnouncesByHash(arg); return; }
        /* not valid hex despite the length — fall through to substring */
    }

    s_peer_list.clear();
    s_peer_list_label = "announces";

    s_ann_stream       = AnnounceStreamState{};
    s_ann_stream.now_s = (int)(nowUnixMs() / 1000);
    s_ann_filter       = arg;   /* empty = no filter */

    cliPrintf("%-3s %-32s %-5s %-5s %-7s %s\n",
              "#", "destination", "hops", "cost", "age(s)", "name");

    forEachAnnounce(annEmitRow);

    if (s_ann_stream.row_num == 0) {
        if (arg.empty()) cliPrintf("(no LXMF destinations heard yet)\n");
        else             cliPrintf("(no match for \"%s\")\n", arg.c_str());
    } else if (arg.empty()) {
        cliPrintf("%d destination(s)\n", s_ann_stream.row_num);
    } else {
        cliPrintf("%d match(es) for \"%s\"\n", s_ann_stream.row_num, arg.c_str());
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
        cliPrintf("<peer> = 32-hex destination, a number from `contacts`/`announces`, "
                  "or a name/nick substring\n");
        return;
    }
    std::string peer_arg(rest, sp - rest);
    while (*sp == ' ') sp++;
    if (!*sp) { cliPrintf("send: empty message\n"); return; }
    std::string text = sp;

    /* Test-rig affordance (mirrors `rnsd link`/`clink`): `@randN`
     * substitutes an N-byte incompressible printable body so a
     * >74-part outbound Resource can be exercised without typing it
     * (device CLI line buffer is 128 B). */
    if (text.size() > 5 && text.compare(0, 5, "@rand") == 0) {
        char* end = nullptr;
        long n = std::strtol(text.c_str() + 5, &end, 10);
        if (end && *end == '\0' && n > 0 && n <= 262144) {
            static const char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
            std::string body;
            body.resize((size_t)n);
            for (size_t i = 0; i < (size_t)n; ++i)
                body[i] = alphabet[cheapRand() % 62];
            text.swap(body);
            cliPrintf("send: generated %ld-byte random body\n", n);
        } else {
            cliPrintf("send: bad @rand size (1..262144)\n");
            return;
        }
    }

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

    if (std::strcmp(args, "help") == 0) { cliPrintf("%-*s LXMF messaging: identities, chats, send\n", CLI_HELP_COL, "lxmf [...]"); return; }
    if (cliWantsHelp(args)) {
        cliPrintf("lxmf create <name>      generate a new identity with display_name=<name>\n");
        cliPrintf("lxmf destroy <n>        wipe identity at slot <n> (secrets + storage)\n");
        cliPrintf("lxmf id                 list identities (* = selected)\n");
        cliPrintf("lxmf id <n>             switch selected identity\n");
        cliPrintf("lxmf chats              list conversations for selected id (numbered)\n");
        cliPrintf("lxmf msgs [<arg>]       no arg = chats; <peer> = thread; <status> = filter;\n");
        cliPrintf("                      `?` = the status names the filter takes\n");
        cliPrintf("lxmf unfinished          outbound not yet finished — what the queue will retry\n");
        cliPrintf("lxmf cancel <n>|all     settle those CANCELLED (<n> from the last listing)\n");
        cliPrintf("lxmf read <n>           print msg n from last listing; marks read\n");
        cliPrintf("lxmf c[ontacts] [<arg>] list contacts for selected id (numbered);\n");
        cliPrintf("                      arg = name/nick substring filter\n");
        cliPrintf("lxmf announces [<arg>]  every lxmf.delivery announce we've heard;\n");
        cliPrintf("                      arg = 32-hex (instant lookup) or name substring\n");
        cliPrintf("lxmf send <peer> <msg>  send msg; <peer> = 32-hex, list-#, or name/nick\n");
        cliPrintf("lxmf a[nnounce]         emit a delivery announce for selected id\n");
        cliPrintf("lxmf link <act> <peer>  open|close|status a conversation link to <32-hex peer>\n");
        cliPrintf("lxmf p[ing] <peer>      probe peer (32-hex, list-#, name/nick):\n");
        cliPrintf("                      rtt + both ends' txpwr/rssi/snr\n");
        cliPrintf("lxmf proxy [<cmd>]      proxy this identity: no arg = state,\n");
        cliPrintf("                      `servers` = the ones we've heard,\n");
        cliPrintf("                      `on <32-hex>` / `off` / `force-off`\n");
        return;
    }
    /* Bare `lxmf` → identity list as status. */
    if (!*args) args = "id";

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
        if (slot == -2) {
            cliPrintf("queued \"%s\": lxmf is not running yet, it creates the identity when it starts\n", rest);
        } else if (slot < 0) {
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
    /* `ch…` is chats, `c…` is contacts — checked in that order so the
     * one-letter abbreviation lands on contacts. `cancel` is spelled in full
     * and must stay ABOVE the contacts line for the same reason: contacts
     * matches on one letter, so a `cancel` tested after it would be read as a
     * contact listing and silently do nothing it was asked to. */
    if (cliVerbIs(verb.c_str(), "chats", 2))
                             { int s = selectedId();
                               lxmf_id_t* i = idAt(s);
                               if (!i || !i->used) cliPrintf("no identity at slot %d\n", s);
                               else cliChats(s);
                               return; }
    if (verb == "msgs")      { cliMsgs(rest); return; }
    if (verb == "unfinished") { cliUnsettled(); return; }
    if (verb == "cancel")    { cliCancel(rest); return; }
    if (verb == "read")      { cliRead(rest); return; }
    if (cliVerbIs(verb.c_str(), "contacts", 1)) { cliContacts(rest); return; }
    if (verb == "announces") { cliAnnounces(rest); return; }
    if (verb == "send")      { cliSend(rest); return; }
    if (cliVerbIs(verb.c_str(), "announce", 1)) {
        int sel = selectedId();
        lxmf_id_t* id = idAt(sel);
        if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
        /* Sentinel — actual send runs on the lxmf task (which owns the
         * our-dest handle). Storage subscription wakes it. */
        storageSet(idEphPath(sel, "cmd.announce").c_str(), 1);
        cliPrintf("announce requested for id %d (%s)\n",
                  sel, bytesToHex(id->dest_hash, LXMF_DEST_HASH_LEN).c_str());
        return;
    }
    if (verb == "link") {
        int sel = selectedId();
        lxmf_id_t* id = idAt(sel);
        if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
        while (*rest == ' ') rest++;
        const char* sp2 = std::strchr(rest, ' ');
        std::string act = sp2 ? std::string(rest, sp2 - rest) : std::string(rest);
        std::string peer = sp2 ? std::string(sp2 + 1) : std::string();
        while (!peer.empty() && peer.front() == ' ') peer.erase(peer.begin());
        if (peer.size() != 32) { cliPrintf("usage: lxmf link open|close|status <32-hex peer>\n"); return; }
        if (act == "status") {
            std::string st = storageGetStr(idEphPath(sel, ("link." + peer).c_str()).c_str(), "");
            cliPrintf("link to %s: %s\n", peer.c_str(), st.empty() ? "down" : st.c_str());
        } else if (act == "open") {
            storageSet(idEphPath(sel, "cmd.link_open").c_str(), peer.c_str());
            cliPrintf("link open requested to %s\n", peer.c_str());
        } else if (act == "close") {
            storageSet(idEphPath(sel, "cmd.link_close").c_str(), peer.c_str());
            cliPrintf("link close requested to %s\n", peer.c_str());
        } else {
            cliPrintf("usage: lxmf link open|close|status <32-hex peer>\n");
        }
        return;
    }
    if (cliVerbIs(verb.c_str(), "ping", 1)) {
        int sel = selectedId();
        lxmf_id_t* id = idAt(sel);
        if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
        while (*rest == ' ') rest++;
        std::string arg = rest;
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
        if (arg.empty()) {
            cliPrintf("usage: lxmf ping <peer>\n");
            cliPrintf("<peer> = 32-hex destination, a number from the last listing, "
                      "or a name/nick substring\n");
            return;
        }
        std::string peer = cliResolvePeer(arg);
        if (peer.empty()) return;
        std::string who = peerDisplayName(sel, peer);
        std::string label = peer;
        if (!who.empty()) label += " (" + sanitizeForLog(who) + ")";

        /* Sentinel — the probe goes out on the lxmf task, which owns the
         * our-dest handle, and writes its outcome under lxmf.ping.<peer>.*.
         * The prompt then waits for that outcome: a probe you have to poll for
         * by hand reads as a dead command. */
        std::string cmd_key = idEphPath(sel, "cmd.ping");
        storageSet(cmd_key.c_str(), peer.c_str());
        cliPrintf("probing %s...\n", label.c_str());

        auto fld = [&](const char* f) {
            return storageGetStr(pingPath(peer, f).c_str(), ""); };

        /* The wait itself is a read of the client's input (rnsh waits out its
         * channel setup the same way). That gives three things one blocking
         * call can't: Ctrl-C arrives as a keystroke, the cli task's ITS inbox
         * keeps being serviced (park it and new CLI connections are refused
         * for the whole probe), and a closed session ends the wait. Without a
         * session — cron, `spangap cli` — there is no input to block on and no
         * one to interrupt, so a plain delay paces the loop instead. */
        char kc;
        const bool interactive = (cliReadRaw(&kc, 1, 0) >= 0);
        TickType_t t0       = xTaskGetTickCount();
        TickType_t deadline = t0 + pdMS_TO_TICKS((pingTimeoutS() + 2) * 1000);
        std::string st;
        bool aborted = false;
        for (;;) {
            if ((int)(xTaskGetTickCount() - deadline) >= 0) break;
            if (interactive) {
                int r = cliReadRaw(&kc, 1, 100);
                if (r < 0) return;                      /* client went away */
                if (r > 0 && (kc == 0x03 || kc == 0x04)) { aborted = true; break; }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            /* While the sentinel is still there the lxmf task has not started
             * this probe, so any terminal state under the peer belongs to the
             * previous one — keep waiting rather than reporting it as ours. */
            if (storageExists(cmd_key.c_str())) continue;
            st = fld("state");
            if (!st.empty() && st != "probing" && st != "path") break;
        }
        unsigned waited = (unsigned)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (aborted) {
            /* The probe is the lxmf task's, not ours to recall — say where it
             * lands so the measurement isn't presumed lost. */
            cliPrintf("cancelled after %u ms — probe still in flight, "
                      "settles under lxmf.ping.%s\n", waited, peer.c_str());
            return;
        }
        if (st.empty() || st == "probing" || st == "path") {
            cliPrintf("no result after %u ms (still %s)\n",
                      waited, st.empty() ? "unstarted" : st.c_str());
            return;
        }
        cliPrintf("%s after %u ms", st.c_str(), waited);

        /* The round trip is the probe's own measurement; what follows is the
         * radio's (SUPE, via lora.<n>.meas.*) — one line per direction it has
         * measured, or the last level heard where it can have no loss. It is
         * printed whatever became of the probe, because it is a measurement of
         * the link rather than of the probe, and an outcome with nothing behind
         * it is what a silent peer and an unheard one look like alike. A radio
         * that has measured nothing prints nothing rather than a row of zeros. */
        std::string rtt  = fld("rtt_ms"), hops = fld("hops");
        if (!rtt.empty()) {
            cliPrintf(" | rtt=%s ms", rtt.c_str());
            /* A link measures the round trip, not the path. `rnpath <hash>` is
             * what says how many hops it took; printing 0 here would answer a
             * question this probe never asked. */
            if (!hops.empty() && hops != "0") cliPrintf(" hops=%s", hops.c_str());
        }
        cliPrintf("\n");
        std::string link = lxmfPingLink(peer, "\n");
        if (!link.empty()) cliPrintf("%s\n", link.c_str());
        return;
    }
    if (verb == "proxy") {
        int sel = selectedId();
        lxmf_id_t* id = idAt(sel);
        if (!id || !id->used) { cliPrintf("no identity at slot %d\n", sel); return; }
        while (*rest == ' ') rest++;
        std::string arg = rest;
        if (arg.empty()) {
            const proxy_t& p = s_proxy[sel];
            cliPrintf("role      %s\n", proxyRoleName(proxyRole(sel)));
            std::string d = storageGetStr(idPath(sel, "proxy_dest").c_str(), "");
            cliPrintf("server    %s%s%s\n", d.empty() ? "(none)" : d.c_str(),
                      p.label.empty() ? "" : "  ", p.label.c_str());
            cliPrintf("channel   %s\n",
                      storageGetStr(idEphPath(sel, "proxy_link").c_str(), "down").c_str());
            std::string q = storageGetStr(idEphPath(sel, "proxy_quota").c_str(), "");
            if (!q.empty()) cliPrintf("quota     %s\n", q.c_str());
            return;
        }
        if (arg == "servers") {
            if (s_proxyHeard.empty()) {
                cliPrintf("no lxmproxy.server announces heard yet\n");
                return;
            }
            for (const auto& kv : s_proxyHeard)
                cliPrintf("%s  %s\n", kv.first.c_str(),
                          kv.second.label.empty() ? "(unnamed)" : kv.second.label.c_str());
            return;
        }
        if (arg.rfind("on ", 0) == 0) {
            std::string d = arg.substr(3);
            while (!d.empty() && d.front() == ' ') d.erase(d.begin());
            if (d.size() != 32) { cliPrintf("usage: lxmf proxy on <32-hex server dest>\n"); return; }
            proxyOn(*id, d);
            cliPrintf("provisioning with %s — this device stays registered "
                      "until the server confirms it is serving\n", d.c_str());
            return;
        }
        if (arg == "off") {
            if (proxyRole(sel) != PROXY_ROLE_CLIENT) {
                cliPrintf("identity %d is not proxied\n", sel);
                return;
            }
            proxyOff(*id);
            cliPrintf("releasing — this device stays proxied and working until "
                      "the server hands the ratchets back\n");
            return;
        }
        if (arg == "force-off") {
            proxyForceOff(*id);
            cliPrintf("forced off. The server keeps the key, the ratchet state and\n"
                      "anything it still holds; if it returns it will announce this\n"
                      "same address and nothing on the network can tell you apart.\n");
            return;
        }
        cliPrintf("usage: lxmf proxy [servers | on <32-hex> | off | force-off]\n");
        return;
    }

    cliPrintf("unknown subcommand `%s`. try `lxmf -h`.\n", verb.c_str());
}

/* ─────────────── task ─────────────── */

static TickType_t nextDeadline(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t due = s_lastPublishTick + pdMS_TO_TICKS(lxmfTickIntervalMs());
    if (due <= now) return 0;
    return due - now;
}

/* A nomad browser (web → lxmf.url_web, on-device LCD → lxmf.url_lcd) tapped an
 * lxmf@<hash> link and wrote "<dest hash>[:<nonce>]" here. Our only job is the
 * identity-independent path request, so an unknown contact's identity (pubkey +
 * display name) gets discovered — exactly the unknown-sender flow used on
 * inbound. The UI surfaces (lxmf web module / lxmf_lcd) bring themselves forward
 * off these same keys, and the conversation is created by the send path on the
 * first message. The nonce makes a repeat tap a fresh value, so the key is NOT
 * consumed here — unsetting it raced the browser sync (set+unset coalesced in
 * one flush and the SPA's mirror never saw the hash). */
static void onOpenContactUrl(const char* /*key*/, const char* val)
{
    if (!val || !*val) return;                 /* a clear */
    if (std::strlen(val) < LXMF_DEST_HASH_LEN * 2) return;
    uint8_t dh[LXMF_DEST_HASH_LEN];
    bool ok = hexToBytes(val, LXMF_DEST_HASH_LEN * 2, dh, LXMF_DEST_HASH_LEN);
    if (!ok) return;
    uint8_t pubkey[RNSD_PUBKEY_LEN];
    if (!rnsdRecallPubkey(dh, pubkey)) {
        info("open-contact %s: unknown identity — issuing path request", val);
        rnsdRequestPath(dh);
    }
}

/* Re-establish every rnsd connection lxmf's live operation depends on: one
 * our-dest per loaded identity, plus the lxmf.delivery announce
 * subscription. Runs on first task entry and on every resume from
 * park — each connect helper no-ops when its handle is already live, so the
 * work loop's own reconnect paths and this share one code path. Teardown (the
 * park path in lxmfTaskMain) drops these so rnsd frees the server slots; this
 * brings them back. Not the one-time client/server init or storage subs —
 * those persist across a park and must never be repeated. */
static void lxmfBringUp(void)
{
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_ids[n].used) connectOurDest(s_ids[n]);
    }
    connectAnnounceSub();
    connectProxyAnnounceSub();
}

static void lxmfTaskMain(void*)
{
    info("[%s] task up", TAG);

    /* Local bootstrap — pure crypto + storage, no rnsd/clock/network needed.
     * Load each stored identity, compute and publish its delivery address, and
     * install its per-id command subs. Done *before* the rns.ready barrier so a
     * just-reset device surfaces its mailbox (and stored history) almost at
     * once; only connecting the dest + announcing waits on rnsd below. The send
     * path is guarded on a live dest handle, so a command arriving in this
     * pre-connect window fails cleanly instead of touching an unconnected dest. */
    loadAllIdentities();

    /* Re-assert the directory claim on every stored contact before we connect,
     * and hand rnsd back the key we hold for each. Pure storage + rnsd's
     * directory aux — no rns.ready needed. */
    lxmfPreloadContactClaims();

    /* Every outbound still in progress in storage goes back on the delivery
     * queue: a reboot mid-delivery must not leave a message QUEUED forever. */
    queueScanStorage();

    /* No boot barrier here: the RNS orchestrator only calls lxmfStart() once rnsd
     * is up and past its boot window (clock valid, network settled), so rnsd's
     * ports are ready and the first announce can't be 1970-stamped. */

    /* lxmf is both an ITS client (RNSD_PORT_DEST / RNSD_PORT_LINK /
     * announce-fanout connects) and a server hosting
     * LXMF_LINK_INBOX_PORT for rnsd's inbound-Link back-connects.
     * itsServerInit sets up the shared inbox; itsClientInit reuses it. */
    if (!itsServerInit()) err("lxmf itsServerInit failed");
    itsServerPortOpen(LXMF_LINK_INBOX_PORT, ITS_PACKET,
                      /*maxHandles=*/LXMF_MAX_INLINKS,
                      /*toCap=*/4096, /*fromCap=*/4096,
                      /*depth=*/0, /*maxMsg=*/4096);
    itsServerOnConnect(LXMF_LINK_INBOX_PORT,    onLinkInboxConnect);
    itsServerOnDisconnect(LXMF_LINK_INBOX_PORT, onLinkInboxDisconnect);
    itsServerOnRecv(LXMF_LINK_INBOX_PORT,       onLinkInboxRecv);

    /* Resource hand-off is a one-shot aux frame from rnsd
     * (not a connection) — open the port aux-only and register the
     * handler. */
    itsServerPortOpen(RNSD_LINK_RESOURCE_AUX_PORT, /*packetBased=*/false,
                      /*maxHandles=*/1, /*toSize=*/0, /*fromSize=*/0);
    itsOnAux(RNSD_LINK_RESOURCE_AUX_PORT, onResourceAux);

    /* itsClient initialisation — one connection per identity, the
     * announce-fanout subscription, the conversation-link pool, and the
     * proxy Channel. */
    itsClientInit(24);

    /* Identity-level commands (clients write `lxmf.cmd.identity_*`). All
     * per-identity command subs are added by createIdentityForSlot /
     * loadIdentityForSlot via subscribePerIdCmds. */
    storageSubscribeChanges("lxmf.cmd.", onIdentityLevelCmd);

    /* Catch up on a sentinel written before this subscription existed. Storage
     * delivers CHANGES, not state, so a write that lands before the subscriber
     * is registered is heard by nobody — and the LCD onboarding wizard writes
     * lxmf.cmd.identity_new from the boot screen, while this task is still
     * waiting for the RNS orchestrator to start it. That is a device the
     * operator named during setup and which then has no identity at all, with
     * nothing on any surface to say why.
     *
     * Only the two creation commands: every other lxmf.cmd.* addresses an
     * identity that must already exist, so an unconsumed one is stale rather
     * than pending. The handler unsets the key, so this runs at most once. */
    for (const char* pend : { "lxmf.cmd.identity_new", "lxmf.cmd.identity_import" }) {
        std::string v = storageGetStr(pend, "");
        if (v.empty()) continue;
        info("%s was written before we were listening — running it now", pend);
        onIdentityLevelCmd(pend, v.c_str());
    }

    /* The settings surfaces: the propagation-node collection and the two
     * identity forms. Both validate here and answer on their own .error key,
     * which is why no UI carries a hash or key-length rule of its own. */
    storageSubscribeChanges("lxmf.pnode.add",       onPnodeAdd);
    storageSubscribeChanges("lxmf.pnode.set",       onPnodeSet);
    storageSubscribeChanges("lxmf.pnode.remove",    onPnodeRemove);
    storageSubscribeChanges("lxmf.pnode.order",     onPnodeOrder);
    storageSubscribeChanges("lxmf.identity.new",    onIdentityNew);
    storageSubscribeChanges("lxmf.identity.import", onIdentityImport);

    /* No iface-event announce subscription: rnsd's per-interface announce
     * replay hands a new interface our last announce, pinned to it. */

    /* Open-a-conversation links from the nomad browser (web → lxmf.url_web,
     * on-device LCD → lxmf.url_lcd). We do the identity-independent path
     * request here; the UIs bring themselves forward off the same keys. */
    storageSubscribeChanges("lxmf.url_web", onOpenContactUrl);
    storageSubscribeChanges("lxmf.url_lcd", onOpenContactUrl);

    /* Live-mirror s.lxmf.debug.only_local into the cached bool so
     * toggling at runtime takes effect on the next announce write. */
    storageSubscribeChanges("s.lxmf.debug.only_local",
        [](const char* /*key*/, const char* val) {
            s_dbg_only_local = val && val[0] && std::atoi(val) != 0;
        });

  for (;;) {   /* Park, don't delete: this task lives across rns stop/start, so its
                * ITS ports + shared inbox are reused, not re-init'd. The bring-up
                * below (re)connects the rnsd conns torn down on the last park; the
                * teardown after the work loop drops them again. */
    /* Connect each loaded identity's delivery dest, and the announce
     * fan-out subscription (lxmf.delivery). This is the step
     * that actually needs rnsd; the identities themselves (keys + dest hashes)
     * were loaded before the loop. We do NOT announce here — the one-shot
     * startup announce is armed below and fires from the 1 Hz tick. */
    lxmfBringUp();

    /* Arm the first announce 30 s out (measured from bring-up) so the rest of
     * the stack — rnsd + every transport — is up and stable before we
     * advertise. Interfaces registering later get this announce from rnsd's
     * per-interface replay, pinned to them. */
    s_announce_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LXMF_FIRST_ANNOUNCE_DELAY_MS);

    s_lastPublishTick = xTaskGetTickCount();
    publishStats();

    while (!s_stop) {
        itsPoll(nextDeadline());

        TickType_t now = xTaskGetTickCount();
        if (now - s_lastPublishTick >= pdMS_TO_TICKS(lxmfTickIntervalMs())) {
            publishStats();
            resolveDirectSends();   /* settle outbound DIRECT */
            for (auto& id : s_ids) if (id.used) pingTick(id);   /* ping deadline backstop */
            convReap();             /* close conversation links idle past s.lxmf.link.idle_s */
            publishLinks();         /* per-peer link state for the header icons */
            proxyClientTick();      /* the proxy Channel, its handshakes, CONFIG */
            applyAcceptGates();     /* the per-destination inbound gate */
            pnClientTick();         /* propagation-node uploads + sync machine */
            /* The delivery queue's sweep, when its interval has come round. */
            if (s_queueNextSweep_s &&
                (uint32_t)(nowUnixMs() / 1000) >= s_queueNextSweep_s)
                queueSweep();
            queueResendDue();       /* unproven opportunistic packets, failed links' retries */
            /* Reconnect anything that dropped since the last tick. */
            for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                lxmf_id_t& id = s_ids[n];
                if (!id.used || id.handle >= 0) continue;
                connectOurDest(id);
            }
            if (s_announce_sub_handle < 0) connectAnnounceSub();

            /* Backstop: replay anything buffered on a now-known sender.
             * Covers the path-response case the announce-fanout hook
             * misses — without this, decrypted messages from a peer
             * whose identity isn't cached buffer and never deliver. */
            for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                lxmf_id_t& id = s_ids[n];
                if (id.used && !id.pending_verify.empty())
                    drainAllPendingVerify(id);
            }

            /* Recover names for hash-only contacts from whatever the announce
             * catalogue / identity cache now holds. Throttled: the caches warm
             * from inbound announces, so a periodic sweep catches contacts the
             * per-announce back-fill missed (heard before the contact existed,
             * or evicted from one cache but not the other). */
            if (now - s_lastBackfillTick >= pdMS_TO_TICKS(60000)) {
                s_lastBackfillTick = now;
                backfillContactNames();
            }

            /* The startup announce, once. */
            if (s_announce_due_tick != 0 &&
                (int32_t)(now - s_announce_due_tick) >= 0) {
                s_announce_due_tick = 0;
                for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
                    lxmf_id_t& id = s_ids[n];
                    if (id.used && id.handle >= 0) sendAnnounce(id);
                }
            }

            /* No periodic re-announce here, and no interval setting for one.
             * lxmf's job is to keep its stored announce CURRENT with rnsd —
             * sendAnnounce() sets it, and rnsd holds the bytes; how often those
             * bytes go on the air belongs to each interface, which is the only
             * thing that knows what airtime costs on its own medium. See
             * rnsd.h, "the announce beat". */

            s_lastPublishTick = now;
        }
    }

    /* rns stop. The persistent state (s_ids, dedup ring, buffers) is PSRAM_BSS,
     * not heap — it stays put for the next lxmfStart(); the one-time client/
     * server init and storage subs above likewise persist and are never redone.
     *
     * Teardown (the whole point of parking rather than deleting): itsDisconnect
     * every rnsd connection we hold so rnsd fires onDisconnect and frees the
     * matching server slots — the our-dest handles (RNSD_PORT_DEST) and the
     * announce subscription (RNSD_PORT_ANNOUNCES). Also drop the conversation
     * links (RNSD_PORT_LINK). Each handle back to -1 so a resume reconnects
     * cleanly via lxmfBringUp(). */
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n) {
        if (s_ids[n].handle >= 0) {
            itsDisconnect(s_ids[n].handle);
            s_ids[n].handle = -1;
        }
    }
    if (s_announce_sub_handle >= 0) {
        itsDisconnect(s_announce_sub_handle);
        s_announce_sub_handle = -1;
    }
    for (auto& c : s_convlinks) convDrop(c);
    /* Remaining RNSD_PORT_LINK holders — inbound DIRECT links. Empty on a
     * quiescent node, but disconnect any live one so its rnsd link slot frees
     * across a stop/start cycle. */
    for (auto& s : s_inlinks)  if (s.used && s.handle >= 0) { itsDisconnect(s.handle); s = inlink_t{}; }
    proxyTeardown();
    pnTeardown();

    /* Park on the inbox until lxmfStart() clears s_stop and notifies. */
    s_parked = true;
    info("[%s] stopped", TAG);
    while (s_stop) itsPoll(portMAX_DELAY);
    s_parked = false;
  }
}

/* ── RNS lifecycle hooks (registered with the orchestrator; see rnsServiceRegister) ── */
static void lxmfStart(void) {
    s_stop = false;
    if (!s_task)
        s_task = spawnTask(lxmfTaskMain, TAG, 8192, nullptr, 1, 0, STACK_PSRAM);
    else
        xTaskNotifyGive(s_task);   /* un-park the resident task */
}

static void lxmfStop(void) {
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);   /* break the work loop; the task parks, not deleted */
    for (int i = 0; i < 300 && !s_parked; i++) delay(10);   /* await park */
    if (!s_parked) warn("[%s] stop timed out", TAG);
}

/* ── One-shot message-store migrations ──────────────────────────────────────
 * Old layouts are decoded with their retained schema, translated, and rewritten
 * as the current layout in place before the live store is registered. Two source
 * layouts are handled: v2 (fixstr stage/last_error/attempts) and v3 (u8
 * status/tries with hex-text message_id + thread). Both target v4 (raw-binary
 * DATA message_id, thread replaced by reply_to). Marker-gated → runs once per
 * layout. v2 stored per-message status as fixstr `stage`(12) + `last_error`(48) +
 * u8 `attempts`; v3+ packs it into u8 `status` (LxmfStatus) + u8 `tries`
 * (255 = gave up). */
static const sdb_schema& lxmfMsgSchemaV2()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 1;
        x.schema_ver = 2;
        x.u8("attempts")
         .fixstr("dir", 4).fixstr("stage", 12).fixstr("method", 16).fixstr("last_error", 48)
         .u32("ts")
         .text("title").text("content").text("thread").text("message_id");
        return x;
    }();
    return s;
}

/* Old status text — v2 English, the interim short tokens, or a dynamic note — to
 * an LxmfStatus code. 0 (none) when unrecognized. */
static uint8_t migStatusFromText(const std::string& e)
{
    static const struct { const char* s; uint8_t c; } M[] = {
        {"noproof", LXMF_ST_NO_PROOF}, {"no delivery proof", LXMF_ST_NO_PROOF},
        {"noroute", LXMF_ST_NO_ROUTE}, {"no route to recipient", LXMF_ST_NO_ROUTE},
        {"toolarge", LXMF_ST_TOO_LARGE}, {"too large for opportunistic", LXMF_ST_TOO_LARGE},
        {"evicted", LXMF_ST_EVICTED}, {"evicted (resource limit)", LXMF_ST_EVICTED},
        {"cancelled", LXMF_ST_CANCELLED},
        {"badpeer", LXMF_ST_BAD_PEER}, {"bad peer", LXMF_ST_BAD_PEER},
        {"disabled", LXMF_ST_DISABLED}, {"identity disabled", LXMF_ST_DISABLED},
        {"mailboxstarting", LXMF_ST_MAILBOX_STARTING}, {"mailbox still starting", LXMF_ST_MAILBOX_STARTING},
        {"packfail", LXMF_ST_PACK_FAIL}, {"pack/sign failed", LXMF_ST_PACK_FAIL},
        {"outboxfull", LXMF_ST_OUTBOX_FULL}, {"outbox full", LXMF_ST_OUTBOX_FULL},
        {"linkopenfail", LXMF_ST_LINK_OPEN_FAIL}, {"link open failed", LXMF_ST_LINK_OPEN_FAIL},
        {"resmalloc", LXMF_ST_RES_MALLOC}, {"resource malloc failed", LXMF_ST_RES_MALLOC},
        {"ressend", LXMF_ST_RES_SEND}, {"resource send failed", LXMF_ST_RES_SEND},
        {"linksenddrop", LXMF_ST_LINK_SEND_DROP}, {"link send dropped", LXMF_ST_LINK_SEND_DROP},
        {"packetsenddrop", LXMF_ST_PACKET_SEND_DROP}, {"OUT_PACKET send dropped", LXMF_ST_PACKET_SEND_DROP},
        {"restransfer", LXMF_ST_RES_TRANSFER}, {"resource transfer failed", LXMF_ST_RES_TRANSFER},
        {"link failed", LXMF_ST_LINK_FAIL},
        {"link closed before send", LXMF_ST_LINK_CLOSED},
        {"unknown", LXMF_ST_UNKNOWN}, {"unknown status", LXMF_ST_UNKNOWN},
        {"rnsd busy — queued", LXMF_ST_QUEUED}, {"busy — queued", LXMF_ST_QUEUED},
    };
    for (auto& m : M) if (e == m.s) return m.c;
    if (e.rfind("retry", 0) == 0) return LXMF_ST_REQUESTING_PATH;   /* any "retry …" note */
    return 0;
}

struct MigRec {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> byKey;
    std::vector<std::string> order;    /* record keys in arena (arrival) order */
};
static void migCollect(const char* key, const char* field, const char* val, void* ctx)
{
    auto* m = static_cast<MigRec*>(ctx);
    if (m->byKey.find(key) == m->byKey.end()) m->order.push_back(key);
    m->byKey[key][field] = val ? val : "";
}

/* Returns the conversation's max recv_ts (0 if nothing migrated). */
static long migrateMsgFile(const std::string& path)
{
    sdb_store in;
    in.schema = &lxmfMsgSchemaV2();
    in.path   = path;
    if (!sdbLoad(&in) || sdbRecordCount(&in) == 0) { sdbEvict(&in); return 0; }  /* not v2 / empty */

    MigRec rec;
    sdbForEach(&in, migCollect, &rec);
    sdbEvict(&in);

    sdb_store out;
    out.schema = &lxmfMsgSchema();     /* v3 */
    out.path   = path + ".v3";
    sdbInitEmpty(&out);

    long runMax = 0;   /* running max of ts, in arena order → monotonic recv_ts */
    for (const std::string& key : rec.order) {
        auto& f = rec.byKey[key];
        auto get = [&](const char* n) -> std::string {
            auto it = f.find(n); return it != f.end() ? it->second : std::string();
        };
        std::string stage = get("stage");
        uint8_t ecode = migStatusFromText(get("last_error"));
        int attempts  = atoi(get("attempts").c_str());
        int tri = attempts < 0 ? 0 : (attempts > 254 ? 254 : attempts);   /* keep non-terminal < 255 */
        uint8_t status;
        if      (stage == "failed")    { status = ecode ? ecode : (uint8_t)LXMF_ST_UNKNOWN; tri = 255; }
        else if (stage == "cancelled")   status = LXMF_ST_CANCELLED;
        else if (stage == "sent")      { if (ecode == LXMF_ST_NO_PROOF) { status = LXMF_ST_NO_PROOF; tri = 255; }
                                         else                             status = LXMF_ST_AWAITING_PROOF; }
        else if (stage == "delivered")   status = LXMF_ST_DELIVERED;
        else if (stage == "received")  { status = LXMF_ST_RECEIVED; tri = 0; }
        else if (stage == "sending" ||
                 stage == "queued")      status = (ecode == LXMF_ST_REQUESTING_PATH) ? LXMF_ST_REQUESTING_PATH
                                                : (stage == "sending" ? LXMF_ST_SENDING : LXMF_ST_QUEUED);
        else                           { status = LXMF_ST_DRAFT; tri = 0; }   /* draft / unknown */

        long ts = atol(get("ts").c_str());
        long recv = ts > runMax ? ts : runMax;    /* monotonic in arena order */
        runMax = recv;

        sdbSetField(&out, key.c_str(), "status",  std::to_string(status).c_str());
        sdbSetField(&out, key.c_str(), "tries",   std::to_string(tri).c_str());
        sdbSetField(&out, key.c_str(), "recv_ts", std::to_string(recv).c_str());
        /* `thread` is dropped (v4 has no thread; reply_to has no v2 source).
         * `message_id` copies as its hex string — the v4 DATA field decodes it. */
        for (const char* pf : { "dir", "method", "ts", "title", "content", "message_id" }) {
            auto it = f.find(pf);
            if (it != f.end()) sdbSetField(&out, key.c_str(), pf, it->second.c_str());
        }
    }
    bool ok = sdbFlush(&out);
    sdbEvict(&out);
    if (ok) { fs_remove(path.c_str()); fs_rename((path + ".v3").c_str(), path.c_str()); }
    else    { fs_remove((path + ".v3").c_str()); return 0; }
    return runMax;
}

/* The pre-DATA v3 message schema (hdr_size 40): hex-text message_id + thread.
 * v3 → v4 (drop `thread`, `message_id` hex-text → 32-byte DATA, add `reply_to`)
 * is purely structural, so it needs no conversion code: registered as a legacy
 * hint layout, it lets the store's generic auto-migrator decode a v3 file and
 * re-pack it as the current layout. Retained only for that registration. */
static const sdb_schema& lxmfMsgSchemaV3()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 1;
        x.schema_ver = 3;
        x.u8("tries").u8("status")
         .u32("recv_ts")
         .fixstr("dir", 4).fixstr("method", 16)
         .u32("ts")
         .text("title").text("content").text("thread").text("message_id");
        return x;
    }();
    return s;
}

static void lxmfMigrateMsgs()
{
    /* No marker file: the header peek below is a bounded inflate, so a
     * steady-state boot is a cheap peek-per-file. The one semantic hop (the
     * fixstr v2 layout → status/tries/recv_ts) is dispatched by hand; every other
     * older on-disk layout is purely structural and left to the store's generic
     * auto-migrator (via the legacy hint schemas registered before this runs). */
    int files = 0;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; n++) {
        std::string dir = fsStatePath(("/lxmf/msgs/" + std::to_string(n)).c_str());
        int dh = fs_opendir(dir.c_str());
        if (dh < 0) continue;
        std::vector<std::string> stems;          /* peer hashes = file stems */
        fs_dirent_t ent;
        while (fs_readdir(dh, &ent)) {
            size_t nl = strlen(ent.name);
            if (nl > 6 && strcmp(ent.name + nl - 6, ".db.gz") == 0)
                stems.emplace_back(ent.name, nl - 6);
        }
        fs_closedir(dh);

        std::vector<std::pair<std::string, long>> peerMax;   /* peer → max recv_ts */
        for (auto& stem : stems) {
            std::string path = dir + "/" + stem + ".db.gz";
            uint16_t fid = 0, fver = 0, fhdr = 0;
            if (!sdbPeekHeader(path.c_str(), &fid, &fver, &fhdr)) {
                warn("lxmf mig: id %d %s: unreadable header — left as-is\n", n, stem.c_str());
                continue;
            }
            if (fid == 1 && fhdr == 88) {
                /* The fixstr v2 layout carries no recv_ts and packs status as free
                 * text: a semantic conversion the generic migrator can't express.
                 * Convert it here (straight to the current layout) and record the
                 * synthesized recv_ts to seed the directory. */
                long mx = migrateMsgFile(path);
                files++;
                if (mx > 0) peerMax.emplace_back(stem, mx);
            } else if (sdbUpgradeFileIfStale(path.c_str(), &lxmfMsgSchema())) {
                files++;   /* structural upgrade (v3 hex-text, or v4 in the old file frame) */
            }
            /* A conversion is heavy (gunzip + rebuild + gzip + flash write) and
             * runs synchronously on the init task. Yield after each file so the
             * first-boot migration of many conversations feeds the task WDT and
             * spreads the write/flash load instead of one long stall. */
            vTaskDelay(1);
        }

        /* Carry the semantic conversion's running max into the conversation
         * directory, so the first post-migration message keeps recv_ts monotonic
         * (last_ts is the running max the write path clamps against). Structural
         * upgrades already preserve recv_ts, so only the v2 hop needs this. */
        if (!peerMax.empty()) {
            sdb_store cs;
            cs.schema = &lxmfContactSchema();
            cs.path   = fsStatePath(("/lxmf/contacts/" + std::to_string(n) + ".db.gz").c_str());
            if (sdbLoad(&cs)) {
                bool dirty = false;
                for (auto& pm : peerMax) {
                    if (!sdbHasRecord(&cs, pm.first.c_str())) continue;
                    std::string cur;
                    sdbGetField(&cs, pm.first.c_str(), "last_ts", cur);
                    if (atol(cur.c_str()) < pm.second) {
                        sdbSetField(&cs, pm.first.c_str(), "last_ts",
                                    std::to_string(pm.second).c_str());
                        dirty = true;
                    }
                }
                if (dirty) sdbFlush(&cs);
            }
            sdbEvict(&cs);
        }
    }
    if (files) info("lxmf: upgraded %d conversation file(s) to the current layout\n", files);
}

/* The v1 contact schema (hdr_size 29): `hash` as hex text, no pubkey. v2 makes
 * `hash` a raw 16-byte DATA field and adds a raw 64-byte `pubkey` (unset until an
 * announce is heard) — a purely structural change. Registered as a legacy hint
 * layout so the store's generic auto-migrator decodes a v1 file and re-packs it
 * as the current layout; retained only for that registration. */
static const sdb_schema& lxmfContactSchemaV1()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 2;
        x.schema_ver = 1;
        x.u32("count").u32("last_ts").u32("unread").u32("read_ts").u32("last_seen")
         .u8("trust")
         .text("hash").text("display_name").text("nick").text("preview");
        return x;
    }();
    return s;
}

/* The message layout just before delivered_ts was appended (hdr_size 104): DATA
 * message_id/reply_to already present. A format_ver-1 file, so it needs this
 * hint for the auto-migrator to decode it; the newer field defaults in on
 * migration. */
static const sdb_schema& lxmfMsgSchemaV4a()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 1;
        x.schema_ver = 4;
        x.u8("tries").u8("status")
         .u32("recv_ts")
         .fixstr("dir", 4).fixstr("method", 16)
         .u32("ts")
         .data("message_id", 32).data("reply_to", 32)
         .text("title").text("content");
        return x;
    }();
    return s;
}

/* The last pre-descriptor contact layout (hdr_size 109): DATA hash/pubkey
 * already present. A format_ver-1 file, so it needs this hint; the newer fields
 * default in on migration. */
static const sdb_schema& lxmfContactSchemaV2a()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 2;
        x.schema_ver = 2;
        x.u32("count").u32("last_ts").u32("unread").u32("read_ts").u32("last_seen")
         .u8("trust")
         .data("hash", 16).data("pubkey", 64)
         .text("display_name").text("nick").text("preview");
        return x;
    }();
    return s;
}

/* Register the retired on-disk layouts so the store's generic auto-migrator can
 * decode format_ver-1 files (which carry no embedded descriptor). Every layout
 * that may still be on flash needs an entry, keyed by (schema_id, hdr_size); the
 * one exception is the semantic v2 message layout (hdr 88), handled by
 * lxmfMigrateMsgs() and never auto-converted. Must run before the stores load.
 * Going forward, format_ver-2 files self-describe and need no new entries here —
 * this list only has to cover the pre-descriptor layouts already in the field. */
static void lxmfRegisterLegacyLayouts()
{
    sdbRegisterLegacyLayout(&lxmfMsgSchemaV3());        /* messages hdr 40  */
    sdbRegisterLegacyLayout(&lxmfMsgSchemaV4a());       /* messages hdr 104 */
    sdbRegisterLegacyLayout(&lxmfContactSchemaV1());    /* contacts hdr 29  */
    sdbRegisterLegacyLayout(&lxmfContactSchemaV2a());   /* contacts hdr 109 */
}

/* Upgrade each identity's contact file to the current layout. Purely structural
 * (v1 → v2, or a v2 file still in the old file frame → the descriptor frame), so
 * the generic auto-migrator does the whole job; this only walks the slots. No
 * marker: the peek inside sdbUpgradeFileIfStale is a bounded inflate, cheap to
 * run every boot. */
static void lxmfMigrateContacts()
{
    int files = 0;
    for (int n = 0; n < LXMF_MAX_IDENTITIES; n++) {
        std::string path = fsStatePath(("/lxmf/contacts/" + std::to_string(n) + ".db.gz").c_str());
        if (sdbUpgradeFileIfStale(path.c_str(), &lxmfContactSchema())) files++;
        vTaskDelay(1);
    }
    if (files) info("lxmf: upgraded %d contact file(s) to the current layout\n", files);
}

void LxmfService::onInit()
{
    /* Register the per-conversation message store and migrate any existing
     * cfgRoot message history into it (once; guarded + crash-safe). Must run
     * before any message read/write so routing serves the record store. */
    {
        /* Convert older on-disk layouts to the current schemas before the live
         * stores touch them. Register the legacy hint schemas first so the generic
         * auto-migrator can decode descriptor-less v1 files. Contacts before msgs:
         * the msgs migration carries each conversation's max recv_ts into the
         * contact directory, which must be at the current layout for that to land. */
        lxmfRegisterLegacyLayouts();
        lxmfMigrateContacts();
        lxmfMigrateMsgs();

        storage_db_opts opts;
        opts.persist = "lxmf/msgs/$1/$2.db.gz";   /* $1=identity slot, $2=peer hash */
        opts.evict   = STORAGE_DB_RELOAD;
        storageStructuredDB("lxmf_msgs", "s.lxmf.id.$.msgs.$", &lxmfMsgSchema(), opts);

        /* Conversation directory: one record per peer, one file per identity. */
        storage_db_opts copts;
        copts.persist = "lxmf/contacts/$1.db.gz";   /* $1 = identity slot; peers = records */
        copts.evict   = STORAGE_DB_RELOAD;
        copts.browserMirror = true;   /* the conversation directory: browser fetches + live-mirrors */
        storageStructuredDB("lxmf_contacts", "s.lxmf.id.$.contacts", &lxmfContactSchema(), copts);

        /* Heard-announce catalogue: global, RAM-only, self-capped. No migration
         * (ephemeral — refills from the live announce stream). */
        storage_db_opts aopts;
        aopts.persist = nullptr;
        aopts.evict   = STORAGE_DB_DROP;
        aopts.cap     = (uint32_t)storageGetInt("s.lxmf.max_announces", 2048);
        aopts.browserMirror = true;   /* the mesh catalogue: browser fetches + live-mirrors */
        storageStructuredDB("lxmf_announces", "lxmf.announces", &lxmfAnnounceSchema(), aopts);

        storageDbMigrate();                      /* fresh device: packs msgs + contacts at once */
        storageDbMigrateStore("lxmf_contacts");  /* already-split device: pack contacts now */
        lxmfSeedDirectory();   /* backfill the directory for pre-migration conversations */
    }

    /* Storage defaults gated on version. */
    if (storageGetInt("s.lxmf.version", 0) < LXMF_VERSION) {
        storageBegin();
        storageDefault("s.lxmf.enforce_stamps",         0);
        storageDefault("s.lxmf.auto_ticket",            1);
        storageDefault("s.lxmf.max_announces",          2048);  /* announce-catalogue cap; 0 disables eviction */
        storageDefault("s.lxmf.debug.only_local",       0);     /* demote announce dbg lines to verb */
        storageDefault("s.lxmf.link.idle_s",            600);   /* conv-link idle close (10 min); lxmf owns
                                                                 * the warm-hold now that rnsd never parks.
                                                                 * 0 = keep open (LRU + Reticulum STALE bound it) */
        storageSet("s.lxmf.version", LXMF_VERSION);
        storageEnd();
    }

    /* Stamp knobs, set unconditionally (not behind the version gate) so
     * they land on already-initialised devices too:
     *  - stamp_cost: the single advertised proof-of-work cost (0 = none).
     *  - generate_stamps: pay a peer's advertised cost when sending.
     * Whether we *require* inbound stamps is the enforce_stamps toggle. */
    storageBegin();
    storageDefault("s.lxmf.stamp_cost",      8);
    storageDefault("s.lxmf.generate_stamps", 1);
    /* The delivery queue (§6): how often a queued message gets another attempt,
     * and how long it may keep trying before it is failed DELIVERY_TIMEOUT.
     * Both in minutes. */
    storageDefault("s.lxmf.delivery_interval", 10);
    storageDefault("s.lxmf.delivery_timeout",  60);

    /* On-the-mesh view horizon (seconds): the LCD "On the Mesh" tab hides any
     * dest whose last announce is older than this. UI-only — the announce
     * catalogue itself is bounded by max_announces, not this. 0 = show all.
     * Unconditional (not version-gated) so it lands on already-initialised
     * devices too. */
    storageDefault("s.lxmf.on_mesh_expire",  3600);

    /* Message-notification sound (played via the optional spangap/audio engine).
     * Default to the bundled ding shipped into /fixed; the path is a setting so
     * users can point it at their own device-rate WAV. sound_enabled is the
     * on/off toggle exposed in the LXMF settings. Set unconditionally (not
     * behind the version gate) so they land on already-initialised devices. */
    storageDefault("s.lxmf.sound",         FS_FIXED "/lxmf/ding.wav");
    storageDefault("s.lxmf.sound_enabled", 1);
    /* strftime format for per-message timestamps in the thread, honoured by both
     * the LCD bubbles and the web UI (which runs a small strftime shim). */
    storageDefault("s.lxmf.msg_time_format", "%H:%M");
    /* Propagation-node check cadence: how often the nodes marked for
     * checking (s.lxmf.pn.<i>.check) are synced. 0 = manual only. */
    storageDefault("s.lxmf.pn.check_interval_s", 1800);
    storageEnd();

    s_dbg_only_local = storageGetInt("s.lxmf.debug.only_local", 0) != 0;

    cliRegisterCmd("lxmf", cliLxmf);

    /* Register with the RNS orchestrator instead of self-spawning: rnsStart()
     * calls lxmfStart() (which spawns lxmfTaskMain) once rnsd is up and past its
     * boot window, and rnsStop() calls lxmfStop(). Core 0, prio 1, 8 KB PSRAM
     * stack — pinned to the transport core (alongside rnsd, prio 1) that feeds
     * it, so inbound-message bursts don't contend with the LCD/audio tasks. */
    rnsServiceRegister(TAG, lxmfStart, lxmfStop);

    /* The on-device LXMessenger launcher tile registers via lxmfLcdRegister(),
     * a when: spangap/spangap-lcd init: hook (conditional/spangap-lcd/ slice).
     * Not called from here, so non-LCD builds never reference it. */
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
        info("lxmfCreateIdentity: lxmf task not running yet — \"%s\" is queued", display_name);
        return -2;
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

int lxmfImportIdentity(const char* privkey_hex, const char* display_name,
                       const char* role, bool sync)
{
    if (!privkey_hex || std::strlen(privkey_hex) != 128) {
        warn("lxmfImportIdentity: expect 128 hex chars");
        return -1;
    }
    std::string v = privkey_hex;
    v += '|'; if (display_name) v += display_name;
    v += '|'; if (role)         v += role;
    storageSet("lxmf.cmd.identity_import", v.c_str());
    if (!sync) return 0;

    if (!waitForCmdProcessed("lxmf.cmd.identity_import", pdMS_TO_TICKS(5000))) {
        warn("lxmfImportIdentity: timeout waiting for lxmf task");
        return -1;
    }
    /* Sentinel cleared — find the slot now holding this key. The private key is
     * the only thing that identifies the slot unambiguously (two accounts may
     * share a display name; none may share a key). */
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n)
        if (storageGetStr(secretsPath(n, "privkey").c_str(), "") == privkey_hex)
            return n;
    return -1;
}

int lxmfSlotForDest(const uint8_t dest_hash[16])
{
    std::string want = bytesToHex(dest_hash, 16);
    for (int n = 0; n < LXMF_MAX_IDENTITIES; ++n)
        if (storageGetStr(idEphPath(n, "dest_hash").c_str(), "") == want) return n;
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
