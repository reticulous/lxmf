# lxmf — internals

Maintainer reference for the `lxmf` task: the wire codec, the ITS framing,
the storage schema, the identity model, and interop. The [README](README.md)
is the operator guide. This document is self-authoritative.

`lxmf.cpp/.h` runs as one FreeRTOS task and has **zero microReticulum (mR)
includes**. LXMF is a layer *above* the Reticulum stack, implemented from
scratch against the **`markqvist/LXMF` Python reference, pinned to LXMF
0.9.8**; mR is rnsd's concern, reached only through rnsd's byte-array C API
(`rnsd.h`: sha256, sign, verify, destination-hash, identity
generate/erase/recall, request-path). So "upstream" below means upstream
**LXMF**, not mR.

---

## 1. Everything this straddle adds (vs upstream LXMF)

Upstream couples an `LXMRouter` co-process with a UI and a private store.
Our realisation keeps the wire format byte-compatible but replaces the
architecture and resolves several behaviours the storage model forces.

1. **No `LXMRouter` — a firmware task + the storage SoT.** The inbox is
   `message_id → wire + sidecar fields`; every message is re-verifiable from
   its bytes alone, and every frontend is coherent with zero merge logic.
   This is the headline divergence (and why a port was rejected).
2. **Native in-tree msgpack codec.** LXMF needs only
   fixarray/fixmap/str/bin/uint/int/float/nil, so `lxmf.cpp` inlines its own
   writer (`mpPack*`) and walker (`mpScan`/`mpScanNext`) — no allocator
   dependency, no Arduino MsgPack. The walker bails on `fixext`/`ext` (LXMF
   uses none); the decoder tolerates `float32/64` timestamps from the Python
   reference (`f*1000 → uint64` ms) though we always pack `uint64`.
   Implemented in `lxmPackPayload`, `lxmPackWire`, `lxmParsePayload`,
   `lxmMessageIdHex`.
3. **Method auto-selection in firmware.** Upstream makes the caller pick the
   mode; we resolve per-message → per-identity default → `auto` and
   auto-promote oversize (or a warm conversation Link) to DIRECT/Resource
   (§6), because the storage API has no good place for a human to pick
   transport per send.
4. **Opportunistic 16-byte strip lives in rnsd, not lxmf.** On the wire,
   OPPORTUNISTIC drops the leading dest hash; lxmf always hands rnsd the
   **full** wire (`dest||src||sig||packed`), and rnsd does the strip on
   `OUT_PACKET` and the prepend on `IN_PACKET`. **DIRECT does not
   strip/prepend** — a Link packet carries the full wire, so lxmf passes the
   Link payload verbatim both ways (prepending on the Link path doubles the
   dest and the signature fails).
5. **Resource hand-off is a shared-memory aux, not in-band.** Large messages
   go via `rnsdLinkSendResource` + the `LXMF_LINK_RESOURCE_AUX_PORT (101)`
   completion aux, keeping the data path type-byte-free.
6. **`OUT_RESULT status=0 ≡ sent`, never `delivered`.** Opportunistic gets
   no native ack; only a proven DIRECT/Resource transfer is `delivered`.
   `applyOutResult` must not optimistically upgrade.
7. **Unknown-sender inbound is buffered, then replayed.** When
   `rnsdRecallPubkey` misses (we've never heard the sender announce), the raw
   wire is parked in a per-identity `pending_verify` queue
   (`LXMF_MAX_PENDING_VERIFY = 25`, `LXMF_PENDING_VERIFY_TTL_MS = 30 min`,
   oldest evicted at the cap) and a path request is issued. Once the sender
   becomes recallable — via their announce (`drainPendingVerify` from the
   announce-fanout) or any path-request response (the 1 Hz
   `drainAllPendingVerify` backstop) — the buffered wire replays through
   `onInboundLxm` and verifies. Opportunistic LXMF has no retransmission, so
   buffering is what keeps a single-packet message from a not-yet-known sender
   from being lost.
8. **Per-contact message store.** `msgs.<peer>.<key>` rather than a flat
   `msgs.<key>` pile; `cmd.{send,cancel,delete}` value is `<peer>/<key>`
   (`delete` also takes a bare `<peer>` = whole conversation). The `<peer>`
   subtree is the seam any future retention/eviction hangs on; this layout
   itself adds no eviction.
9. **Proof-of-work stamps** (`lxmf_stamp.cpp`) — generate and validate,
   self-contained SHA-256/HMAC/HKDF kept off the shared HW SHA engine (§8).
10. **Notification sound** — inbound delivery plays `s.lxmf.sound` through
    the optional spangap/audio engine, every call site gated on
    `CONFIG_STRADDLE_AUDIO`.

### Not present (upstream features deliberately unbuilt)

State these as absent, not "coming":

- **Tickets** (the contact-exemption shortcut). `FIELD_TICKET = 0x0C` is
  parsed on inbound and logged, but never stored, used, or issued (upstream's
  value shape is `[expiry_unix_s, ticket_16B]`, with 21 d expiry / 14 d renew
  / 5 d grace lifetimes). No stamp is ever skipped — every stamped send pays
  full PoW. `s.lxmf.auto_ticket` is read by nothing.
- **PROPAGATED mode** (`0x03`) and running as a propagation node — no
  store-and-forward, no `lxmf.propagation` destination, no prop-node `/get`
  (upstream propagation-node stamps use a 1000-round workblock and peering
  stamps 25, vs the recipient stamp's fixed 3000 — §8).
- **PAPER mode** (`0x05`).
- **Group conversations** (`FIELD_GROUP = 0x0B` reserved), **audio
  messages** (`FIELD_AUDIO = 0x07`; such messages display
  "[audio — unsupported]"), and **externalised blob attachments**.
- **Auto-give-up.** `applyOutStatus` counts `RETRY` aux frames against a
  per-message budget but the automatic `OUT_CANCEL` at
  `MAX_DELIVERY_ATTEMPTS` is not wired (user-initiated cancel is); a failed
  send is re-issued by the client writing `cmd.send` again.
- **Multi-identity UX.** The schema is an array from day one
  (`LXMF_MAX_IDENTITIES = 4`); single-identity simply runs at `n = 0`. There
  is no picker / generate / import flow beyond the settings panes.

---

## 2. The lxmf task

One FreeRTOS task, **core 1, prio 1, 8 KB PSRAM stack**, single wait point
`itsPoll(deadline())`. There is no DataChannel and no `LXMF_PORT_*` for
consumers. Every frontend reads/writes storage; lxmf subscribes to its own
command subtree and reacts. The control surface collapses to "writes to
storage drive every state change"; the only thing not expressible by
subscription — "do X now, no persistent state" — uses the self-clearing-key
convention (§3).

```cpp
itsServerInit(); open LXMF_LINK_INBOX_PORT (100) + LXMF_LINK_RESOURCE_AUX_PORT (101)
itsClientInit(LXMF_MAX_IDENTITIES + 1);            // +1 announce subscription
storageSubscribeChanges("lxmf.cmd.",        onIdentityLevelCmd);
storageSubscribeChanges("rnsd.iface_event_seq", onRnsdIfaceEvent);
storageSubscribeChanges("lxmf.url_web",     onOpenContactUrl);
storageSubscribeChanges("lxmf.url_lcd",     onOpenContactUrl);
loadAllIdentities();                               // load/create slots + per-id cmd subs
for n used: connectOurDest();
connectAnnounceSub();                              // RNSD_PORT_ANNOUNCES, "lxmf.delivery"
for (;;) {
  itsPoll(nextDeadline());                         // ITS callbacks OR 1 Hz deadline
  if (1 s elapsed) {
    publishStats(); resolveDirectSends(); convReap();
    retry deferred queued sends; reconnect dropped our-dests / announce sub;
    drainAllPendingVerify (replay buffered inbound on now-known senders);
    periodic + debounced announce
  }
}
```

It waits on `rns.ready` (bounded ~120 s) before starting, and `waitForTime`
before connecting, so the first announce is never 1970-stamped. The first
announce is armed ~30 s out; an interface-up debounce
(`rnsd.iface_event_seq`) re-arms a `now+10 s` window so a burst of ifaces
yields one announce.

Callbacks (mailbox recv, announce-sub recv, cmd handlers, resource aux) run
inline from `itsPoll` dispatch on the lxmf task — there is no event-pending
flag and no scan walker.

## 3. Storage-as-API and the subscription surface

**Single writer per subkey.** Client-owned and firmware-owned fields are
strictly disjoint, so the firmware's subscriptions match exactly what
clients write — no self-notify churn.

| Field | Owner | Notes |
|---|---|---|
| `msgs.<peer>.<key>.{peer,title,content,thread,method}` | client | editable while `stage==draft` |
| `msgs.<peer>.<key>.read` | client | inbound flag; firmware ignores it |
| `msgs.<peer>.<key>.stage` | client writes initial `draft`; firmware owns all later values | transitions only via `cmd.*` |
| `msgs.<peer>.<key>.{wire,message_id,attempts,last_error}` | firmware | derived after pack |
| `contacts.<m>.*` | client, except: firmware stubs on first inbound/outbound, refreshes `display_name` on every announce and `last_seen` on every inbound | `nick`/`trust` are purely client-owned |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*` | client writes, firmware deletes | imperative actions |

**Self-clearing command keys.** One-shot actions with no persistent state;
existence = request, absence = ack. The firmware-side subscription surface
is deliberately narrow:

| Subscription | Installed | Fires for | Handler |
|---|---|---|---|
| `lxmf.cmd.` | once in `lxmfTaskMain` | `identity_new` / `identity_import` / `identity_destroy` | `onIdentityLevelCmd` |
| `lxmf.id.<n>.cmd.` | per slot, by `createIdentityForSlot`/`loadIdentityForSlot`; removed by `destroyIdentity` | `send` / `cancel` / `delete` / `announce` | static `onIdCmd<n>` → `handleIdCmd(n,…)` |
| `lxmf.url_web`, `lxmf.url_lcd` | once | a tapped `lxmf@<hash>` link | `onOpenContactUrl` (path request only) |
| `rnsd.iface_event_seq` | once | an interface coming up | `onRnsdIfaceEvent` (announce debounce) |
| `s.lxmf.debug.only_local` | once | live debug-verbosity toggle | inline lambda |

The slot index is captured at compile time in four static stubs
(`onIdCmd0…3`) so the callback stays `(key,val)` with no key parsing. **The
firmware never subscribes to `s.lxmf.*` or `lxmf.id.<n>.*` (non-`cmd`).**
Every firmware write to message records or ephemeral mirrors is silent
relative to its own subscriptions — the cmd sentinels are the only wake
sources. Resist widening these prefixes "for symmetry."

`onOpenContactUrl` does **not** consume its key: the value is
`<hash>[:<nonce>]` and the nonce already makes a repeat tap a fresh change.
Unsetting it raced the browser sync (set+unset coalesced in one flush and
the SPA mirror never saw the hash), so it is left in place.

## 4. Wire format

`LXMessage.pack()`:

```
destination_hash(16) | source_hash(16) | Ed25519 sig(64) | msgpack(payload)
```

- `LXMF_OVERHEAD = 112` B; the destination hash is **16 bytes**.
- Payload msgpack tuple order is **`[timestamp, title, content, fields]`** —
  **title before content.** (The upstream README says content-then-title;
  the code is authoritative. Getting this wrong = zero interop.)
- Signature scope is **`dest || src || packed || SHA-256(dest || src ||
  packed)`** — the SHA-256 of the data is signed *in addition to* the data.
  Signing only the data does not interop.
- `message_id = SHA-256(dest || src || packed)` — never on the wire; both
  sides re-derive it.
- `src` is the sender's **`lxmf.delivery` destination hash**, not the
  identity hash. The recipient does `Identity.recall(src_hash)`, and that map
  is keyed by destination hash; a wrong `src` is a silent drop.
- `transient_id = SHA-256(encrypted_lxmf_data)` is the propagation store's
  index — **`transient_id ≠ message_id`** (unused until propagation lands).

Constants: `LXMF_DEST_HASH_LEN = 16`, `LXMF_SIG_LEN = 64`,
`LXMF_OVERHEAD = 112`, `LXMF_OPP_PAYLOAD_MAX = 383` (RNS ENCRYPTED_MDU —
single-packet plaintext ceiling). `FIELD_THREAD` is stored hex64 but packed
as raw 32 B on the wire (`lxmPackPayload` converts both ways).

### Delivery modes (`LXMessage` mode codes)

| Mode | Code | Single-packet content | Mechanics |
|---|---|---|---|
| OPPORTUNISTIC | 0x01 | payload ≤ 383 B (ENCRYPTED_MDU); ~290 B title+content after src16+sig64+msgpack | one RNS encrypted packet, ECDH AES-128 per packet |
| DIRECT | 0x02 | ~319 B/pkt, larger via Resource | RNS Link, ratcheted |
| PROPAGATED | 0x03 | — | not implemented here |
| PAPER | 0x05 | — | not implemented here |

### Field registry (`LXMF.py`, msgpack int keys)

`0x01 EMBEDDED_LXMS, 0x02/0x03 TELEMETRY[_STREAM], 0x04 ICON_APPEARANCE,
0x05 FILE_ATTACHMENTS, 0x06 IMAGE, 0x07 AUDIO, 0x08 THREAD, 0x09/0x0A
COMMANDS/RESULTS, 0x0B GROUP, 0x0C TICKET, 0x0D EVENT, 0x0E RNR_REFS, 0x0F
RENDERER, 0xFB-0xFD CUSTOM_*, 0xFE NON_SPECIFIC, 0xFF DEBUG`.
`RENDERER ∈ {PLAIN, MICRON, MARKDOWN, BBCODE}`.

### Identity & destinations

LXMF uses an `RNS.Destination.SINGLE` (Ed25519 + X25519, ratchets enforced)
on aspect `lxmf.delivery`. The identity is the standard RNS Identity — there
is no LXMF-specific keypair. lxmf only computes `Destination::hash(id,
"lxmf", "delivery")` as a static pre-computation for storage publishing;
rnsd hosts the actual IN `Destination`.

## 5. ITS framing — `RNSD_PORT_DEST` (the mailbox)

One bidirectional connection per identity (lxmf is the active connector both
directions — only it knows where its keys live). Connect payload
`rnsd_mailbox_connect_t`: `aspect = "lxmf.delivery"`, `identity_key` (a
storage path, never the key bytes — keys are passed to rnsd by reference),
`dest_type`. rnsd derives the dest hash, registers it for inbound dispatch,
hosts the `Destination`, and owns `Transport::request_path` and the
in-flight retry table.

In-band frames (first byte = opcode):

| Opcode | Dir | Payload | Handler |
|---|---|---|---|
| `0x01 OUT_PACKET` | lxmf→rnsd | `send_id(2) \| lxm_wire` | `processSend` |
| `0x02 OUT_RESULT` | rnsd→lxmf | `send_id(2) \| status(1) \| rtt_ms(4 BE) \| hops(1)` | `applyOutResult` |
| `0x03 OUT_CANCEL` | lxmf→rnsd | `send_id(2)` | `processCancel` |
| `0x04 IN_PACKET` | rnsd→lxmf | full LXM plaintext | `onInboundLxm` |
| `0x05 OUT_STATUS` | rnsd→lxmf | `send_id(2) \| type(1) \| tail` | `applyOutStatus` |
| `0x06 ANNOUNCE` | lxmf→rnsd | `app_data` | `sendAnnounce` |

`OUT_RESULT.status`: `0` sent (opportunistic egress acknowledged) · `1`
delivered (DIRECT/Resource proof) · `2` cancelled (after our `OUT_CANCEL`) ·
`3` evicted (rnsd resource limit). **There is no `failed` status** — rnsd
never gives up on its own; path/link/retry trouble is narrated via
out-of-band `OUT_STATUS` aux frames (`REQUESTING_PATH`, `PATH_KNOWN`,
`EGRESS_QUEUED`, `LINK_ESTABLISHING`, `RESOURCE_PROGRESS`,
`RETRY{attempt,reason}`, `PATH_LOST`). LXMF owns the give-up policy. `send_id`
is a 16-bit per-identity correlator (wraps to 1, skipping 0), resolved back
to the message key via `outboundFindBySendId`. The same port serves
`rnprobe` (connect with `identity_key = ""`).

**Inbound Links and Resources.** `connectOurDest` also calls
`rnsdDestListenLinks(handle, LXMF_LINK_INBOX_PORT = 100)`; rnsd flips
`accepts_links(true)` on the `lxmf.delivery` dest and back-connects each
accepted inbound Link to that port, feeding bytes into the shared
`onInboundLxm`. Large transfers complete on
`LXMF_LINK_RESOURCE_AUX_PORT = 101` with `rnsd_link_resource_done_t`
opcodes `RNSD_LINK_RESOURCE_{INBOUND_DONE,OUTBOUND_DONE,FAILED}`
(`onResourceAux`); the inbound buffer is rnsd-owned and released via
`rnsdResourceRelease` even on the drop path.

**Backchannel identification, both directions** (mirrors upstream
LXMRouter's `backchannel_identified`). Outbound: after the *first
delivered* settle on a conversation Link, `directLinkSettle` sends
`rnsdLinkIdentify(tag)` — rnsd signs a `LINKIDENTIFY` with the identity
the link was opened with — so the peer can reuse our Link for replies
instead of opening its own. Once per link; a lost aux merely degrades to
the peer opening its own reply link. Inbound: when a peer identifies on
a Link into us, rnsd validates the signature and publishes
`rnsd.links.<tag>.remote_identity` + `.remote_dest` (the peer's dest
hash on the link's own aspect); `inlinkPollIdentified` (1 Hz) copies
`remote_dest` into the inlink slot, and `processReady` then prefers —
in order — a warm conversation Link, an identified inbound Link
(the backchannel), a fresh Link. Riding the peer's link saves a full
LR/LRPROOF handshake, which is the expensive part over half-duplex
LoRa. Anonymous (never-identified) inbound Links are receive-only,
exactly as before.

## 6. Outbound lifecycle

```
client: write msgs.<peer>.<localkey>.{peer,title,content,thread,method,stage=draft}
client: write lxmf.id.<n>.cmd.send = <peer>/<localkey>      ◄── commit (same txn ideal)

handleIdCmd → split val on '/' → delete sentinel → processSend(id,peer,localkey):
  peer arrives from the sentinel (it IS the record's path segment)
  validate 32-hex → 16 B dest
  pack [ts_ms, title, content, fields]  (title BEFORE content)
  optional stamp (lxmPackWire appends payload element [4] if peer advertises a cost)
  sign over dest||src||packed||SHA-256(...)
  message_id = SHA-256(dest||src||packed); persist wire + message_id; stage=queued
  allocate outbox slot (send_id); processReady decides transport:

processReady — method resolution (after the wire is packed, so oversize
  is measured on the real payload, not a content estimate):
  msgs.<id>.method → s.lxmf.id.<n>.default_method → "auto"
  oversize = (wire.size() - 16 > LXMF_OPP_PAYLOAD_MAX=383)   # strip dest16, vs ENCRYPTED_MDU
  "direct"        → use a Link
  "opportunistic" → fail if oversize, else OUT_PACKET on the mailbox handle
  "auto"          → use a Link if oversize OR a conversation Link to peer is already
                    warm OR the peer has an identified inbound Link (backchannel)
  large + Link    → rnsdLinkSendResource (Resource over the Link)
```

Local outbound key is `o_<unix_ms>_<rand4>` (the real `message_id` isn't
stable while a draft mutates; it's carried as a sidecar). Inbound records key
directly by `message_id`, both under the `<peer>` subtree. The outbox slot
(`outbound_t`) carries `peer` alongside `msg_key`, so
`applyOutResult`/`applyOutStatus`/`resolveDirectSends`/`onResourceAux`
rebuild the path from a `send_id` without re-reading storage. The outbox is
8 deep; a 9th in-flight send → `last_error = "outbox full"`.

DIRECT uses **persistent per-peer conversation Links**: the first DIRECT send
to a peer opens a Link (`rnsdLinkOpen(peer, "lxmf.delivery", …,
tag="lxmf.id<n>.<mid8>")`) that is kept and reused for the whole chat, in
both directions, reaped by `convReap` past `s.lxmf.link.idle_s` (default
600 s). Because `RNSD_PORT_LINK` has no `OUT_RESULT`, the 1 Hz
`resolveDirectSends()` settles `sent`/`delivered` from
`rnsd.links.<tag>.{state, tx_packets, tx_proven, proof_timeouts, last_error}`
(baselines captured at send time) and the Resource ACK
(`RNSD_LINK_RESOURCE_OUTBOUND_DONE`) is proof-grade.

## 7. Inbound lifecycle

`onInboundLxm` (fed by `IN_PACKET`, inbound-Link, and inbound-Resource):

1. Length ≥ `LXMF_OVERHEAD = 112`.
2. `wire[0..16] == id.dest_hash` (else rnsd routing weirdness — warn, drop).
3. `rnsdRecallPubkey(src_hash)`; if absent → buffer the wire in the
   per-identity `pending_verify` queue (25 deep, 30-min TTL, oldest evicted at
   the cap) and `rnsdRequestPath(src_hash)`, then **return**.
   `drainPendingVerify` (announce-fanout) and the 1 Hz `drainAllPendingVerify`
   backstop replay it through this same path once the sender is recallable.
4. If a stamped payload (`0x95` header), `lxmSplitStamp` rewrites it back to
   the unstamped `0x94` form to recover the exact bytes the sender signed.
5. Verify Ed25519 over `dest||src||packed||SHA-256(...)`. Bad → drop.
6. `message_id = SHA-256(dest||src||packed)`, hex64.
7. Dedup: in-RAM ring (`s_dedup_ring`, 64 entries) **and** storage existence
   (`…msgs.<peer>.<mid>.stage`, authoritative and reboot-durable).
8. If `s.lxmf.enforce_stamps`, validate the stamp against our advertised
   `s.lxmf.stamp_cost`; drop post-dedup if missing/under-cost.
9. `lxmParsePayload`; persist under `…msgs.<peer>.<mid>.*` (`<peer> = src`)
   with `stage=received`, `dir=in`, `read=0`; stub `contacts.<src>`
   (trust=0, copy `display_name` from the announce catalogue if heard);
   refresh `last_seen`; play the notification sound if enabled; `received++`.

## 8. Stamps

Hashcash PoW per the reference LXStamper: a 32-byte nonce chosen so that
`SHA-256(workblock || nonce)`, as a big-endian 256-bit integer, is
`≤ 2^(256 − cost)`. The `workblock` is a **768 KB** buffer expanded from the
`message_id` via HKDF-SHA256 over a **fixed 3000 rounds** (both ends must
derive the identical block — *not* a tunable). Implemented in
`lxmf_stamp.cpp` with self-contained SHA-256/HMAC/HKDF kept off the shared
hardware SHA engine.

- The stamp is payload element **[4]**, appended **after** signing, so it is
  neither signed nor part of `message_id`. On the wire a stamped payload is a
  5-element fixarray (`0x95`); inbound, `lxmSplitStamp` rewrites the header
  to `0x94` to recover the signed bytes.
- **Generation** (`s.lxmf.generate_stamps`) runs only when the recipient's
  announce advertised cost > 0. The 768 KB block is built once, a midstate is
  cached, and each nonce attempt compresses only the final 64-byte block.
  ~4 s on a T-Deck (build dominates), yielding ~every 500 ms. A peer cost
  above `LXMF_STAMP_MAX_COST = 18` is refused (sent unstamped).
- **Validation** (`s.lxmf.enforce_stamps`) is one workblock hash; also yields
  ~every 500 ms. Allocation failure fails closed.

## 9. Announces

`sendAnnounce(id)` builds msgpack `[display_name_or_nil, stamp_cost]` and
pushes `ANNOUNCE | app_data`; rnsd calls `listener_dest.announce(app_data)`.
Two triggers, both per used identity with a live mailbox: an interface-up
debounce (`rnsd.iface_event_seq` arms `now+10 s`, re-arming on each iface-up;
armed once ~30 s after startup) and a periodic re-announce (1 Hz check vs
`s.lxmf.announce_interval_s`, default 1800; `0` disables).

Inbound: lxmf subscribes rnsd's `RNSD_PORT_ANNOUNCES` fan-out filtered to
`lxmf.delivery`. rnsd forwards matches as
`hops(1)|dest(16)|identity(16)|app_data(N)`; `onAnnounceFromRnsd` parses
(`parseLxmfAnnounce` handles the shapes seen in the wild), skips own dests,
and writes **one packed leaf per destination**:

```
lxmf.announces.<dest_hex> = "<last_s>|<cost>|<hops>|<ratchet>|<name>"
```

`last_s` first so `atoi(value)` recovers it for the eviction walker without
parsing the rest; `name` last so embedded `|` need no escaping. One cJSON
node per entry keeps the PSRAM block count down. Bounded by
`s.lxmf.max_announces` (default 2048; `annCountAndMaybeOldest` evicts the
smallest `last_s` on overflow — O(N), only on a new dest at capacity).

The catalogue is a bare-prefix key: RAM-only, gone on reboot. So
`onAnnounceFromRnsd` also writes the announced name verbatim into
`contacts.<dest>.display_name` of every slot that has that contact — the
announce is authoritative for the name (a nameless announce clears it), and
this write is what makes it reboot-durable. Unconditional (no read-compare —
storage no-ops identical values); frontends therefore never need to promote
announce names into contacts themselves, they only fall back to the live
catalogue for non-contact peers.

**Concurrency:** `AnnounceFanout::received_announce` runs on the **rnsd**
task (inside `Transport::inbound`) but only does `memcpy + itsSend(timeout=0)`
per subscriber; all announce-catalogue storage writes happen on the **lxmf**
task in `onAnnounceFromRnsd`. rnsd never touches storage in the announce
path, so announce traffic cannot overflow rnsd's recv queue.

## 10. Identity model

The schema is an array (`…id.<n>.*`, `n ∈ [0, LXMF_MAX_IDENTITIES = 4)`);
single-identity runs at `n = 0`. Each slot is fully compartmentalised —
contacts, messages, drafts siloed by path; `destroyIdentity(n)` wipes
`secrets.lxmf.id.<n>.privkey`, `s.lxmf.id.<n>.*`, and `lxmf.id.<n>.*` and
nothing else.

LXMF owns its identities, not rnsd. It generates Ed25519+X25519 keypairs via
an `rnsd.h` helper on the lxmf task (no rnsd round-trip), stores them at
`secrets.lxmf.id.<n>.privkey`, and tells rnsd "use that key" at connect time
by passing the **storage key name** in the ITS payload — never the secret
bytes. rnsd never needs to know lxmf has multiple identities.

In-RAM per-slot state (`s_ids[LXMF_MAX_IDENTITIES]`): the `identity_key`
string (a storage path, *not* an `RNS::Identity`), the precomputed
`dest_hash[16]`, the ITS handle, the 8-deep outbox (`send_id → message key`),
counters, and `last_announce_tick`. `bootstrapFirstBoot` loads every slot
from secrets; if none load it auto-creates slot 0 ("main"). There is **no**
auto-announce at boot.

## 11. Storage schema

Frontends query by indexed fields (`peer`, `thread`, `stage`, `dir`,
`read`), not by key. Every `msgs.<id>.*` line below is
`s.lxmf.id.<n>.msgs.<peer>.<id>.*` on disk (`<peer>` = 32-hex destination =
the conversation subtree).

### Global (`s.lxmf.*`)

```
announce_interval_s  1800   periodic re-announce s (0 = on demand only)
max_announces        2048   announce-catalogue cap (0 = no eviction)
stamp_cost           16     advertised PoW cost (0–18; 0 = none)
generate_stamps      1      pay a peer's advertised cost when sending
enforce_stamps       0      drop inbound without a valid stamp for our cost
auto_ticket          1      no effect (tickets unimplemented; read by nothing)
link_timeout         0      conversation-Link establishment budget s (0 = rnsd derives)
link.idle_s          600    close a conversation Link idle past N s (0 = keep open)
sound                /fixed/lxmf/ding.wav   notification WAV
sound_enabled        1      play the notification sound on inbound delivery
debug.only_local     0      demote per-announce dbg lines to verbose
cli.selected_id      0      the CLI's selected identity
```

`s.lxmf.max_resource_size` (262144) is read by rnsd (the inbound-Resource
size gate), documented in [rns](../rns), not here.

### Per-identity (`s.lxmf.id.<n>.*`)

```
label · enabled (1) · display_name · default_method (auto)
contacts.<m>.{hash,nick,display_name,trust,last_seen}   (firmware stubs on first inbound/outbound; display_name re-written from every announce)

msgs.<id>.dir            in | out
msgs.<id>.stage          draft(client) | queued | sending | sent | delivered | failed | cancelled | received
msgs.<id>.peer           hex16 (redundant with the path segment, kept for indexed query)
msgs.<id>.title / content
msgs.<id>.thread         hex64 root message_id, "" if none
msgs.<id>.method         opportunistic | direct | auto (hint while draft)
msgs.<id>.ts             unix s
msgs.<id>.read           inbound only, 0 | 1
msgs.<id>.wire           hex of the packed LXM (firmware)
msgs.<id>.message_id     hex64 SHA-256 (firmware)
msgs.<id>.attempts       (OUT_STATUS RETRY count)
msgs.<id>.last_error     short UI string
```

`<key>` is inbound → the real `message_id`; outbound → local
`o_<unix_ms>_<rand4>` (with `message_id` as a sidecar once packed).

Each conversation subtree (`s.lxmf.id.<n>.msgs.<peer>`) is registered as its
own **external storage file** (`storageNewTreeFile` via `ensureConvFile`,
[spangap-core storage docs](../spangap-core/docs/storage.md)), so a chatty
conversation rewrites only its own small file instead of growing `root.json`
on every change. `ensureConvFile` runs on every write path — the inbound
persist, `cmd.send` processing, and the CLI send — so a browser-composed
draft that first landed in `root.json` self-heals: the registration on
`cmd.send` detaches it into its own file at the next flush. Deleting a
conversation (or the identity) via `storageDeleteTree` drops and unregisters
the file. This only changes *where the bytes flush*: the subtree is still
fully resident in the in-RAM tree and still syncs to the browser (§13's
retention pitfall stands).

Two alternatives to the per-contact layout are settled. Having firmware scan
every `msgs.*` subtree for a bare `<key>` (so `cmd.*` values could omit the
peer) is rejected — it reintroduces exactly the O(all-messages) walk the
layout exists to avoid, which is why `cmd.{send,cancel,delete}` values carry
`<peer>/<key>`. Unifying `contacts.<peer>` and `msgs.<peer>` under a single
`peer.<peer>.*` tree (destroying a contact would drop their messages in one
op) is a real option but a wider blast radius — considered and deferred.

### Secrets / ephemeral

```
secrets.lxmf.id.<n>.privkey   128-hex Ed25519+X25519 (wiped by identity_destroy)

lxmf.up · lxmf.id.<n>.up · lxmf.id.<n>.dest_hash · lxmf.id.<n>.last_announce_s ·
lxmf.id.<n>.stats.{sent,received,pending,failed} ·
lxmf.announces.<dest_hex> "<last_s>|<cost>|<hops>|<ratchet>|<name>"
```

## 12. Frontends

**Browser** (`browser/`, `registerLxmf`): `modules/lxmf.ts` (Pinia + RPC),
`panels/LxmfPanel.vue` (Settings → LXMF), `panels/MessagesWindow.vue` (chat
window), and `components/lxmf/` (`PeerAvatar`, `ConversationList`,
`ContactCard`, `MessageBubble`, `Composer`, `AnnouncesView`,
`ConversationThread`). `MessageBubble.vue` renders the stage glyph and shows
`last_error` as its tooltip.

The frontend write contract (`modules/lxmf.ts`, `CmdQueue`): a record write
plus its `cmd` sentinel must go as **one** `sendJson` nested patch — the
queue `deepAssign`s the record data and the sentinel into a single patch —
never two separate `set()` calls. Command serialization is **per `cmd` key**
(one queue per full sentinel path, so per identity too), not global:
different cmd kinds never block each other; only same-kind writes queue.
Completion is settled on the record's firmware-owned *effect* (e.g.
`message_id` appearing, or a `failed` stage) — never on the sentinel
disappearing, because ephemeral `lxmf.*` deletions are not propagated back
over the storage DataChannel, so "sentinel gone" is unobservable and using
it produced false send failures.

**On-device LCD app** (`esp-idf/conditional/spangap-lcd/src/lxmf_lcd.cpp`):
the **LXMF** app — `class LxmfApp : public LcdApp`, constructed
`LcdApp({ .name = "LXMF", .iconBasename = "lxmf" })` and installed via
`lcdInstall(new LxmfApp())`. A settings pane is registered with
`lcdRegisterSettings("Mesh Network/LXMF", "LXMF Messages", lxmfSettingsPane,
2)`. Both, plus the `lxmf.url_lcd` subscription, are wired by
`lxmfLcdRegister()`, the `when: spangap/spangap-lcd` init hook. The whole
file lives under `conditional/spangap-lcd/`, compiled only when the lcd
straddle is staged, so **no `#if` guards anything**. The app reacts to its
`s.lxmf.id` / `lxmf.id` / `lxmf.announces` storage subscriptions and renders
outbound bubble glyphs with `LV_SYMBOL_OK` / `LV_SYMBOL_CLOSE`. The
`lxmf.url_lcd` handler calls `lcdShowProgram("LXMF")` then opens the thread,
both on the lcd task.

## 13. Maintainer pitfalls

- **mR `Log.h` macro clash.** mR declares `info/warn/error/debug/msg` as free
  functions in `namespace RNS` (with `#define msg`), and spangap's `log.h`
  macros corrupt them on parse. The top of `lxmf.cpp` does
  `#pragma push_macro` + `#undef` for each name around the mR-touching
  includes, then `pop_macro`. A build dying in `Log.h` means a missing
  bracket here.
- **`storageDeleteTree` wants a node path with NO trailing dot.**
  `deleteFromTree` does `strrchr(path,'.')` and detaches the segment after
  the last dot; a trailing dot detaches `""` → silent no-op (returns false,
  so not even the save-timer or browser-null-notify fire). `processDelete`
  builds its own dotless path; the trailing-dot `msgPrefix` is for
  `storageForEach`/`collectTokens` only. Don't unify the two — a trailing-dot
  delete silently frees nothing.
- **`storageForEach` returns leaves, not subtrees.** The CLI's
  `collectTokens` extracts the token between known prefix/suffix segments;
  there is no "list subtrees" API. `collectTokens("…msgs.")` yields the
  **peer** tokens (the conversation list), `collectTokens("…msgs.<peer>.")`
  the keys within one thread.
- **No retention/eviction on the message store.** Bodies live in the
  RAM-backed config tree on the small (~256 KB) `/state` partition;
  `s.lxmf.max_resource_size` (~the whole partition) is the only inbound gate.
  Sustained large inbound fills `/state`, which cascades (storage writes
  fail → ITS/CLI wedge → an rnsd path-table watchdog panic). This is a
  platform/storage gap, not an lxmf-protocol defect; the interim mitigation
  is `cmd.delete` housekeeping (including the bare-`<peer>`
  whole-conversation form).
- **`Identity::recall` cache.** rnsd raises mR's known-destinations cache to
  1000 so inbound from an infrequent correspondent doesn't drop on eviction;
  "inbound from unknown sender" for a peer that just announced may still be a
  cache miss.
- **Never construct an `RNS::Destination` here.** mR's ctor auto-registers
  with Transport; rnsd hosts the IN destination. lxmf only computes the dest
  hash as a static pre-computation for storage publishing.
- **No `thread_local`.** Plain `static` is correct — an ITS recv callback for
  a port dispatches only on the registering task — and libgcc's lazy TLS init
  has corrupted the FreeRTOS scheduler at boot.
- **Announce-due comparisons must be signed.** `sendAnnounce` rewrites
  `last_announce_tick` after the rnsd call, which can land just past the
  loop's captured `now`; an unsigned `TickType_t` subtraction underflows and
  re-fires immediately. The code casts to `int32_t` before the `>= 0` test —
  keep it.

## 14. Interop checklist (must stay true)

1. Payload tuple `[timestamp, title, content, fields]` — title before
   content (the upstream README is wrong).
2. Signature scope `dest||src||packed||SHA-256(dest||src||packed)` — the
   hash is signed alongside the data.
3. Destination hash is 16 bytes.
4. `src` = the sender's `lxmf.delivery` dest hash, not the identity hash.
5. The recipient stamp is payload element [4], appended after signing
   (`0x95` header inbound, rewritten to `0x94` to verify).
6. `transient_id ≠ message_id`.
7. The stamp workblock is 768 KB over a fixed 3000 HKDF rounds — do not
   change the round count.
8. bz2 in the Resource path is live — bzip2 1.0.8 is vendored in the µR
   component ([rns](../rns) §10, `Resource.cpp`), so a compressible inbound
   Resource decompresses and delivers, and outbound Resource payloads are
   compressed whenever that shrinks them (every Reticulum/NomadNet client
   speaks bz2 here).
