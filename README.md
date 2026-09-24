# lxmf — LXMF messaging on the mesh

**lxmf** is the device's LXMF mailbox: it sends and receives signed
[LXMF](https://github.com/markqvist/LXMF) messages over [rns](../rns),
holds up to four independent identities, and advertises each one so other
nodes can reach it. It interoperates with stock LXMF clients (Sideband,
NomadNet, MeshChat) wire-for-wire.

LXMF ("Lightweight Extensible Message Format") is Mark Qvist's
human-messaging layer on top of Reticulum: signed, store-and-forwardable
messages addressed to a node's self-generated cryptographic identity, with
no servers and no accounts.

## Origins

LXMF here is implemented from scratch in C++ against the
[`markqvist/LXMF`](https://github.com/markqvist/LXMF) Python reference,
pinned to **LXMF 0.9.8**. It sits *above* the Reticulum stack and has zero
microReticulum includes — it reaches the network only through rnsd's
byte-array C API (sha256, sign, verify, destination-hash, identity
generate/erase/recall, request-path). The wire codec, signature scope,
delivery-mode selection, and proof-of-work stamps are all native. The wire
format and the per-byte deltas from upstream are in
[INTERNALS.md](INTERNALS.md).

## What it does

- **Messaging.** Packs, signs, and transmits outbound messages; verifies,
  de-duplicates, and stores inbound ones. Signatures are checked against the
  sender's Reticulum identity — forged or corrupt messages are dropped
  silently.
- **Identities.** Up to `LXMF_MAX_IDENTITIES = 4` independent mailboxes
  (`lxmf.delivery` destinations), each with its own keypair, contacts, and
  message history, fully separate. **No identity is created automatically** — a
  device with none runs as a transport-only node (it relays and tracks the
  mesh but has no mailbox of its own).
- **Selectable delivery mode.** Per message, lxmf picks a single
  opportunistic packet, a Reticulum Link (DIRECT), or a Resource transfer for
  large bodies. A per-identity/global method chooses how eagerly to hold a
  Link — from the default `link-always` through `link-if-one-exists` (ride an
  already-open Link, else opportunistic) and `link-if-big` down to
  `opportunistic-or-fail`. The conversation header shows the live Link state
  and toggles it open/closed on tap.
- **Announces.** Each enabled identity keeps its delivery destination's announce
  current with rnsd; each interface decides how often it goes on the air. Every
  inbound `lxmf.delivery` announce on the mesh is
  collected into a shared, cross-identity **heard-peer list** of everyone
  the device has heard of.
- **Stamps.** Pays and (optionally) enforces LXMF proof-of-work stamps as
  spam friction.
- **Propagation nodes.** Sends through and receives from classic
  `lxmf.propagation` store-and-forward nodes (see below) — a message can
  be resent via a node from its detail page, and a user-ordered node list
  is polled for held messages.
- **Message-notification sound.** Plays a short sound on inbound delivery
  via the optional [audio](../audio) engine.

## How it interacts with other straddles — storage is the API

There is **no DataChannel and no ITS port for consumers**. Every action is a
storage read or write. The lxmf task subscribes to its own command keys and
reacts; it publishes message records and live state back into storage.

```
Browser / LCD app / CLI ──storage read+write──► s.lxmf.* · lxmf.* · secrets.lxmf.*
                                                       │  (lxmf subscribes to *.cmd.*)
                                                       ▼
                                                  ┌────────┐
                                                  │  lxmf  │ ──► rnsd ──► interfaces
                                                  └────────┘
```

Internally lxmf is a consumer of rnsd: one hosted-destination (our-dest)
connection per identity on `RNSD_PORT_DEST`, outbound Links on
`RNSD_PORT_LINK`, and the announce fan-out on `RNSD_PORT_ANNOUNCES`. It also
hosts two private inbound ports for rnsd's back-connects (inbound Links and
Resource hand-off). None of that is client-facing — frontends only touch
storage.

Consequences a frontend can rely on:

- **Multi-frontend coherence is free.** Two browser tabs, the CLI, and the
  on-device app all see the same inbox. Mark a message read in one and the
  others reflect it. No locking, no merge.
- **Single writer per field.** Client-owned and firmware-owned fields are
  disjoint. You create a message record and set its content; the firmware
  owns `status`, `tries`, `wire`, `message_id`.

lxmf starts automatically when the straddle is in the build (its init is
folded into the generated startup dispatcher, after rnsd). The task always
runs — even with zero identities — so the announce subscription and the
command handlers are live.

### Key namespaces

| Prefix | Persistence | Who writes | Purpose |
|---|---|---|---|
| `s.lxmf.*` | survives reboot | client + firmware (disjoint fields) | identities, messages, contacts, config |
| `secrets.lxmf.*` | survives reboot | firmware | private keys (never leave the device) |
| `lxmf.*` | RAM, re-published ~1 Hz | firmware | live status, stats, heard-peer list |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*`, `lxmf.url_*` | transient | client writes | imperative actions (below) |

## Commands — self-clearing keys

Imperative actions are **command keys**: you write the key, the firmware
performs the action and (for the identity/per-identity commands) deletes the
key. Presence = request in flight; absence = done.

**Identity-level** (`lxmf.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.cmd.identity_new` | optional label | generate a new identity, allocate the next slot, bring its mailbox up |
| `lxmf.cmd.identity_import` | 128-hex private key | import a key into a new slot |
| `lxmf.cmd.identity_destroy` | `<n>` | wipe identity `n` — its secret, all its storage, its subscriptions |
| `lxmf.cmd.pn_sync` | `all` or `<32-hex>` | check propagation nodes for held messages now (all check-marked, or one node) |
| `lxmf.cmd.pn_add` | `<32-hex>[\|<name>]` | append a propagation node at the first free list index |

**Per-identity** (`lxmf.id.<n>.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.id.<n>.cmd.send` | `<peer>/<key>[/pn:<hash>]` | pack, sign, and transmit the draft at `s.lxmf.id.<n>.msgs.<peer>.<key>`; the optional `pn:` segment uploads to that propagation node instead |
| `lxmf.id.<n>.cmd.cancel` | `<peer>/<key>`, or bare `all` | cancel an in-flight send; `all` finishes every outbound of this identity that has not finished. The fan-out is done on lxmf's task because this key is self-clearing — a writer looping over it would overwrite each value before the task read it |
| `lxmf.id.<n>.cmd.delete` | `<peer>/<key>`, or bare `<peer>` | delete one message; bare `<peer>` deletes the whole conversation (and the contact with it). A proxied outbound is dropped on the proxy too (`DROP`), so a message deleted here is not still being sent by a machine elsewhere. Deleting one message counts it back out of the conversation directory — `count` always, and `unread` when it was an inbound past the read watermark — since those are maintained counters rather than derived, and a deleted message must not go on lighting the badge |
| `lxmf.id.<n>.cmd.retry` | `<peer>/<key>` | try this one again NOW. Proxied, that is a `RETRY` frame naming the record — the body is already on the server, and putting it back on the air to say "again" would cost the whole message to carry one bit. Unproxied it is an ordinary send, since this device owns the queue |
| `lxmf.id.<n>.cmd.announce` | any | emit a delivery announce for identity `n` now |
| `lxmf.id.<n>.cmd.ping` | `<peer>` | probe that contact — a link establishment where there is no link, else one packet on the open one (see **Ping**) |
| `lxmf.id.<n>.cmd.fetch` | `<peer>/<message_id>` | ask the proxy server for a body it withheld (see **Being proxied**) |
| `lxmf.id.<n>.cmd.proxy_on` | `<32-hex>` | ask that `lxmproxy.server` to take this account; this device stays registered until the server confirms |
| `lxmf.id.<n>.cmd.proxy_off` | any | hand the account back; this device stays proxied and working until the server answers |
| `lxmf.id.<n>.cmd.proxy_force_off` | any | the dead-server override — read what it costs first |

To make a command key atomic with its data, write the data fields and the
command key in one `storageBegin()/storageEnd()` transaction — the firmware
then sees a fully-populated record the instant the command key fires.

### Clickable `lxmf@<hash>` links

Micron pages rendered by [nomad](../nomad)'s browser turn an `lxmf@<32-hex>`
token into a clickable link. Activating one writes the destination hash to
one of two ephemeral command keys:

| Key | Written by | Reaction |
|---|---|---|
| `lxmf.url_web` | the web nomad browser | the lxmf web panel comes forward and opens the conversation |
| `lxmf.url_lcd` | the on-device nomad browser | the on-device LXMF app comes forward (`lcdShowProgram("LXMF")`) and opens the thread |

The value is the 32-hex destination hash, optionally suffixed `:<nonce>` so
re-tapping the same hash re-fires. The lxmf **core task** reacts to *both*
keys: if the destination's identity is unknown it issues a path request, so
the contact's name and send-capability light up once the announce arrives.
The two UI surfaces each react only to their own key. These keys are **not**
self-deleting (the nonce already makes each tap a fresh value).

The reverse also holds: a message body may quote a **Nomad page URL**
(`<32-hex hash>:/path`, e.g. `a8d2…338:/page/index.mu`), which the messenger
renders as a tappable link. Activating one writes `nomad.url_web` /
`nomad.url_lcd` (same nonce trick), and [nomad](../nomad)'s browser comes
forward on that page — the symmetric counterpart of the `lxmf.url_*` keys above.

## Identities

Per loaded identity you can observe:

```
s.lxmf.id.<n>.label          "main" | "imported" | user-set
s.lxmf.id.<n>.enabled        1 (default) — 0 = identity disabled: no announce, no send, inbound dropped
s.lxmf.id.<n>.display_name   utf-8, advertised in announces
s.lxmf.id.<n>.default_method  per-identity delivery method (see below); `auto`
                             (what a new identity gets) or empty falls back to
                             the global s.lxmf.default_method
lxmf.id.<n>.up               1 once the mailbox is connected — this device's own
                             delivery destination is registered with rnsd
lxmf.id.<n>.ready            1 once the slot can take a message: `up`, or the
                             account is proxied (see **Being proxied**), where
                             sends ride the Channel and `up` is permanently 0.
                             This is what a composer, a window and a send gate
                             on; `up` alone would strand a proxied account
lxmf.id.<n>.dest_hash        hex16 — this identity's lxmf.delivery address
lxmf.id.<n>.identity_hash    hex16 — the identity under it: what a node sees
                             when this account identifies on a link, and what
                             its other aspects are derived from
lxmf.id.<n>.link.<peer>      conversation-link state to <peer>, ephemeral:
                             absent (down) | establishing | active
```

From firmware, `lxmf.h` exposes:

```cpp
int  lxmfCreateIdentity (const char* display_name, bool sync = false);
bool lxmfDestroyIdentity(int n,                    bool sync = false);
```

These write the corresponding `lxmf.cmd.*` command key. With `sync = true` the
call blocks (≤ 5 s) until the firmware finishes, then returns the allocated
slot (create) or success (destroy). The CLI's `lxmf create` / `lxmf destroy`
use the sync form so they can report the outcome.

## Sending a message

Messages are stored **per contact**: `<peer>` is the 32-hex destination,
`<key>` a local key (convention `o_<unix_ms>_<rand4>`).

1. Write the draft record:

   ```
   s.lxmf.id.<n>.msgs.<peer>.<key>.dir      = out
   s.lxmf.id.<n>.msgs.<peer>.<key>.peer     = <32-hex destination>
   s.lxmf.id.<n>.msgs.<peer>.<key>.title    = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.content  = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.reply_to = <hex64 message_id, or absent>
   s.lxmf.id.<n>.msgs.<peer>.<key>.reply_quote = <utf-8 fragment, or absent>
   s.lxmf.id.<n>.msgs.<peer>.<key>.status   = 0            # DRAFT
   ```

   (`peer` is both the path segment and a field — the field is kept for the
   indexed-query contract.) A `DRAFT` record is filtered out of the thread, so
   you may edit it freely; the web composer skips this state entirely, keeping
   typed text in RAM and writing the record already `QUEUED`.

2. Commit with `lxmf.id.<n>.cmd.send = <peer>/<key>` (ideally in the same
   transaction as step 1), or `<peer>/<key>/pn:<32-hex>` to upload it to a
   propagation node instead of delivering it directly.

3. Watch `…status` — a single `u8` code carrying both the lifecycle stage and,
   if it stops, the reason:

   ```
   DRAFT → QUEUED → REQUESTING_PATH → SENDING → AWAITING_PROOF → DELIVERED
                 ↖ RETRYING_LINK / RETRYING_DELIVERY / REQUESTING_PATH ↙   (back in the queue)
                                              ↘ DELIVERY_TIMEOUT | LINK_FAIL | TOO_LARGE | … | CANCELLED
   ```

   The companion `…tries` byte counts delivery attempts, and `tries == 255`
   is the **one** definitive terminal marker: below it the message is still
   in play and the delivery queue will try again, whatever the status says.
   `DELIVERED` means a cryptographic delivery proof (or the proof-grade
   Resource transfer acknowledgement) arrived — nothing else does.

**The delivery queue.** An attempt that cannot deliver *yet* — no path found
within a minute, no proof back, a conversation Link that failed or is busy, no
free outbox slot — puts the message in the delivery queue rather than failing
it. The queue is swept every `s.lxmf.delivery_interval` minutes (default 10)
while it holds anything, and a message queued for longer than
`s.lxmf.delivery_timeout` minutes (default 60) ends as `DELIVERY_TIMEOUT`; a
message whose Link failed twice ends sooner, as `LINK_FAIL` (below). A
reboot resumes every in-progress outbound it finds in storage. Between attempts
nothing is held open: no outbox slot, no path search in rnsd. Local errors that
another attempt cannot fix — a body too large for the chosen method, a
malformed peer, a disabled identity — fail at once.

**An unproven opportunistic packet is sent again, as it is, before anything
opens a Link.**

```
A → B   LXM packet                   one packet; B's rnsd proves it
A       no proof in s.rnsd.proof_timeout_s
A → B   the same packet              ≥ the spacing after the first, same message_id
A → B   the same packet              ≥ the spacing after that
A → B   Link                         the sweep's attempt, if still unproven
```

A proof that went missing looks exactly like a message that went missing: the
proof is a packet on the same radio and is lost as easily. So the identical
wire — same bytes, same `message_id` — goes out again, twice, each no sooner
than the larger of rnsd's proof window and its path budget (`rnsdPathBudgetS`,
96 s) after the send before it. That is safe because B's rnsd proves every copy
that reaches it and B's lxmf keeps the first and drops the rest on
`message_id`, so a copy of a message already delivered costs one packet and
shows nothing twice. It is cheaper than a Link on LoRa, where a Link spends a
request (86 B), a proof (118 B), a round-trip measurement (83 B) and an
identify (211 B) before the message itself. The message waits in the delivery
queue meanwhile as `RETRYING_DELIVERY`; each re-send is logged at info
("re-sent as the same packet (attempt n)"). Only when both re-sends have gone
unanswered is the conversation kicked, and the sweep runs in the same 1 Hz pass
with its Link. `opportunistic-or-fail` never escalates: it keeps re-sending at
the same spacing until the delivery timeout. The `message_id` survives a reboot
too: the timestamp is stamped once, in whole seconds, and kept on the record, so
a re-pack from storage yields the same id.

**A sweep makes one attempt per conversation, over a Link.** A sweep's attempt
asks whether the peer is there at all, and a Link asks that once for every
message waiting: seven messages waiting for one peer are seven passengers on
one attempt, not seven attempts. An unreachable peer therefore costs one Link
open per sweep however much mail is waiting for them, and no body goes on the
air until they have answered. As each delivery finishes, the next message for
that peer follows it over the still-open Link at once rather than waiting out
another interval — so a conversation that comes back drains in seconds, not in
one message per ten minutes. A conversation whose oldest message has a re-send
scheduled spends its sweep turn on that re-send. `opportunistic-or-fail` is
exempt, being an explicit instruction never to open a Link.

**A Link that fails gets one retry, five minutes later.**

```
A → B   Link        rnsd: up to 3 establishments while B was heard from lately
A       failed      establish_timeout, or the link closed before the send
A → B   Link        5 min later, the message's one retry
A       LINK_FAIL   if that fails too — tries = 255
```

rnsd has already spent its own attempts on each of these (a fresh establishment
per attempt, while the peer has been heard from in the last 30 minutes — rns
`INTERNALS.md` §5.1), so what reaches lxmf as a failed Link is a peer that is
there but hard to reach, or one that is not there at all. The first earns one
more try after the medium has had time to change; the second says so within
minutes instead of sitting in the queue for the whole delivery timeout. The
retry is this conversation's turn: a sweep does not make a second attempt
beside it. The count lives in the queue entry, so a reboot or a fresh
`cmd.send` starts it over. A message that is waiting for a **path** is not a
failed Link: it stays on the sweep and the delivery timeout above.

**A message on a Link whose proof does not come back is sent once more on the
same Link.**

```
A → B   LXM over the Link             one Link packet; B's rnsd proves it
A       no proof in s.rnsd.proof_timeout_s, Link still active
A → B   the same wire, same Link      once; logged "msg M resent over link T (proof lost)"
A       no proof again                the Link is dropped, the message is queued (RETRYING_LINK)
```

The proof is one frame on its way back, and nothing along the path repairs a
lost one, so a message whose proof went missing is most often already at B. The
same wire goes out once over the Link that carried it; B's lxmf drops the
`message_id` it holds and its rnsd proves the copy, which settles the message
as `DELIVERED`. Only a second unproven send treats the Link as suspect. This
applies to a message that fits one Link packet; a Resource settles on its own
transfer acknowledgement.

**Delivery method.** lxmf resolves per-message `method` →
`s.lxmf.id.<n>.default_method` → global `s.lxmf.default_method` →
`link-always`; `auto` or empty at the first two levels means "no choice here",
and `auto` is what a new identity is created with. The four methods form a
spectrum of link eagerness:

| method | behaviour |
| --- | --- |
| `link-always` *(default)* | always a Reticulum Link. The Link identifies the sender, which an opportunistic packet does not: a recipient that has no path to the sender cannot verify a packet and never shows it. |
| `link-if-one-exists` | ride our own already-open conversation Link to the peer if one exists, else opportunistic. A peer's inbound Link into us never counts — we only ride Links we opened. |
| `link-if-big` | opportunistic when the wire fits one packet, a Link only when it's oversize. |
| `opportunistic-or-fail` | never a Link; oversize hard-fails with `last_error = "too large for opportunistic"`. |

A message fits opportunistic when `title + content + ~32 B` is within one
packet (budget ~311 B). Oversize forces a Link in every mode **except**
`opportunistic-or-fail`, and a Link carries large bodies as a Resource
transfer. **A sweep's attempt forces a Link on exactly the same terms** — the
method describes the first attempt and its re-sends; the delivery queue decides
the rest (above). The legacy names still parse: `direct`→`link-always`,
`opportunistic`→`opportunistic-or-fail`.

**Why a packet by default.** On a radio mesh a message that fits one packet is
cheapest as one packet: a Link costs about 500 B of air before the message, and
most peers are there and prove. The case a Link answers better — a peer whose
stack proves Link traffic but not opportunistic packets, answering every
packet with silence while its user reads the message — costs the two re-sends
and then gets its Link. Set `link-always` for a peer known to behave that way,
or on a mesh where the handshake is cheap. Upstream LXMF leaves the choice to
the application (an `LXMessage` with no method picks DIRECT) and its router
re-sends an unproven opportunistic packet as it is, which is what the re-sends
above do.

**Link toggle.** The conversation header (web and LCD) shows a link icon —
green when a Link to the peer is open, amber while establishing, grey when
down — that tapping opens or closes on demand (`lxmf.id.<n>.cmd.link_open` /
`cmd.link_close`, value `<peer>`; CLI `lxmf link open|close|status <peer>`).
It reflects a Link torn down for any reason, tracking the per-second
`lxmf.id.<n>.link.<peer>` state.

A message that gave up (`tries == 255`) is not retried on its own — write
`cmd.send` again to re-send it, which restarts its try count and its
delivery timeout.

## Replying to a message

```
A → B   LXM { content: "on my way",
              fields: { 0x30: <32 B message hash of B's message>,
                        0x31: "where are you?"   (only when a fragment was picked) } }
```

`FIELD_REPLY_TO` (`0x30`) names the message being answered by its
`message_id`; `FIELD_REPLY_QUOTE` (`0x31`) carries the fragment of it the
sender selected. **A plain reply sends `0x30` alone** — both ends hold the
quoted message and draw the preview line from their own copy, so the air
carries a hash and not a second copy of the text. Selecting part of the message
first is what puts `0x31` on the wire.

**A quote is drawn only where it is true.** A received fragment is shown only
if it really occurs in the message `reply_to` names; otherwise the block falls
back to that message's own opening line, and a reply to a message this device
does not hold says just "replying to a message". So a quote block can never put
words in the other party's bubble. The fragment as it arrived is still on the
message's detail page, verified or not.

Both frontends work the same way:

- **The quote sits where the reply will.** Picking *reply* puts the quoted line
  directly above the typing area with an (x) that abandons the reply and keeps
  the text; once sent, the same block sits at the head of the balloon, at both
  ends. Tapping a quote scrolls the conversation to the message it came from
  and marks it briefly — on the LCD, opening the history page it is on if it is
  older than the page being read.
- **Three actions per message**, in Signal's order: reply, details, delete. On
  the web they replace the bubble's three-dot menu, appearing on hover, on a
  click, or when text in the bubble is selected — a click no longer walks off to
  the detail page, which is what the (i) is for. On the LCD a long press puts
  the same three icons over the bubble, selecting the word under the finger on
  the way if the press landed on text; dragging from there grows the selection
  (the thread stops scrolling while a selection is up).
- **Reply is available once a message has an id.** An outbound message has no
  `message_id` until the firmware has packed it, and there is nothing to name
  until then, so the action is disabled for exactly that window.

Upstream's clients (Sideband, NomadNet, MeshChat) implement none of the reply
fields, so a reply reaches them as an ordinary message: nothing breaks, the
quote is simply not drawn.

## Propagation nodes

Classic LXMF **propagation nodes** (reference `lxmf.propagation`
store-and-forward, as run by NomadNet et al.) are supported as a client,
wire-compatible with LXMF 0.9.8:

- **Node list.** A global, ordered list at `s.lxmf.pn.<i>.{hash,name,check}`
  (`i` = 0…7; a slot whose `hash` isn't 32-hex is free). Client-owned —
  view, add, reorder and delete it from the LXMF settings panel (web) or
  the on-device settings pane; the web panel also renames in place.
  `check = 1` marks a node to be polled for messages held for this
  device's identities.
- **Sending.** An outbound message's detail page (web and LCD) has a
  **Resend** button opening a dialog: resend **directly**, via the
  **contact's node** (when one is set on the contact), or via one of **our
  nodes**. A node resend encrypts the wire to the recipient, pays the node's
  announced propagation stamp (a separate, smaller proof-of-work than the
  recipient delivery stamp), and uploads over an `lxmf.propagation` link.
  Success ends the message as **`ON_PN`** — final, shown as the single
  "stored for pickup" tick: a propagation node never proves delivery; the
  recipient collects the message on its own next sync. Failures end as
  `PN_FAIL` (link/transfer) or `PN_REJECTED` (the node refused, e.g. an
  under-paid stamp).
- **Per-contact node.** Each contact's details page can name that
  contact's propagation node (any dest hash — a quick-pick of our own
  list plus free entry). It is offered as the resend dialog's second
  option; nothing is sent to it automatically.
- **Receiving.** Every node marked `check` is synced on a timer
  (`s.lxmf.pn.check_interval_s`, default 1800; `0` = manual only) and on
  demand (`lxmf.cmd.pn_sync = all` or a node hash). By hand, that is the
  **envelope button** the messenger shows while at least one node is
  configured — right of the Contacts / On-the-Mesh tabs on the LCD, in the
  window's top-right corner on the web. It says the check will happen in
  the background and restarts the interval, so the next automatic pass is a
  whole interval away. A sync identifies over the link so the node
  serves exactly this identity's mail, downloads the held messages
  through the normal verify/dedup/store pipeline, and confirms them so
  the node deletes its copies. Per-node results land in
  `lxmf.pn.<hash>.{last_check_s,last_err,last_got}` (RAM);
  `lxmf.pn.sync` holds the node currently being checked.

Command keys: `lxmf.id.<n>.cmd.send = <peer>/<key>/pn:<hash>`
(resend via node), `lxmf.cmd.pn_sync`, and
`lxmf.cmd.pn_add = <hash>[|<name>]` (append to the list — used by the
on-device pane).

This device does not *run* a propagation node — it is a client only.

## Being proxied — an always-on device holds the account

```
this device                            the server
  cmd.proxy_on <server dest> ─ CHANNEL ─►    (identifies with the account key)
                             ◄─ HELLO        label, limits, serving?
  HANDOVER [key, name, ratchets] ─────►
                             ◄─ SERVING      it is registered and announcing
  deregister our own destination, role := client
                             ◄─ MSG …        mail, live, while we are online
  SEND … ────────────────────────────►       (the Channel proves it: ✓)
                             ◄─ STATUS       once, at the end: how it went
                                             (the Channel proves that too, and
                                              the server deletes on the proof)
```

An account normally lives on the device in front of you: it registers its
`lxmf.delivery` destination, announces it, and can only receive while it is
switched on. A **proxy server** ([lxmproxy](../lxmproxy)) takes that over. It
holds the same keys, registers and announces the same address, receives the
mail and sends the outbound; this device holds a permanently-open Channel to it
and exchanges messages over that. The rest of the network sees an ordinary
always-online LXMF node.

The cost is stated plainly and not designed around: **both devices hold the
account key and the cleartext.**

What it buys: messages arrive while this device is off, and the server delivers
its outbound while it is away. A proxied device advertises nothing, so there is
no announce tick from a moving radio and no waiting for paths to propagate to
it — it initiates every link it needs and the answers ride home on them.

What it assumes is that the server's uplink is better than this device's. When
both sit on the same LoRa mesh every message crosses the air twice, once to the
server and once here, and only the offline half is bought.

**Exactly one registrant.** At every moment exactly one device registers and
announces the account's `lxmf.delivery` — never zero, never two. One
per-identity key holds it:

```
s.lxmf.id.<n>.proxy_role = off | server | client
```

A `client` slot is therefore **never `up`** — it has no destination of its own
registered here, and that is the correct steady state, not a stage it is
waiting to leave. Everything that asks "can I send from this account?" asks
`lxmf.id.<n>.ready` instead, which is true for a proxied slot the moment it
loads: with the Channel up the send goes to the server, and without it the
message waits in the local delivery queue, exactly as it would for an
unreachable peer.

`off` is the ordinary case: this device answers on the address. `server` means
this device is hosting somebody *else's* account (lxmproxy set it) and still
registers and announces. `client` means the account belongs to a server: this
device registers nothing, announces nothing, and hands its delivery queue to
the Channel. Making it one enum is what makes the both-ends-registered state
unreachable.

Both transitions are handshakes, never a local flag flip:

- **Turning it on** — this device keeps its own destination registered until the
  server confirms it is serving.
- **Turning it off** — this device stays proxied, fully working, with the server
  still delivering, until the server hands the account back. It never enters a
  state where it is neither.

Deregistering tells the network nothing: the old registrant's announces keep
bouncing around until they age out, and peers keep their cached path until it
expires. The new registrant announces at once and the network converges as that
announce spreads. There is a window; nothing detects or corrects it.

**Where the address does and does not disappear.** The moment the server
confirms, this device deregisters the destination — it stops being in
`rnsdHostedDestsForEach`, `rnsd.dest.<n>` goes, and nothing announces it from
here again. What does *not* change is anybody's memory of having heard it: other
nodes' announce caches and path tables age out on their own schedule, and
`lora n` builds its own rows from announces this radio was heard transmitting
and never retires a local one. That listing asks rnsd what is hosted *now*
before printing a row under "us", so a handed-over address drops off it — but
the underlying observation is still there, and a peer that has not yet heard the
server's announce will still try this device for a while.

**Talking to an account this device hosts.** If this device is a proxy server
and you message the account it holds from another identity here, the message
never touches the network: Reticulum has no path to its own destinations, so it
is packed, signed, and handed straight to the recipient slot's inbound
pipeline — stored, and pushed on to the owner's roaming device over their
Channel like any other inbound. It ends as `DELIVERED` because it genuinely is.

**Dead-server override.** If the server is physically gone the release never
lands, and **Force off** is the way out. What it costs:

- if the server ever returns it will register and announce the same address, and
  nothing on the network can tell the two apart;
- anything it still holds is stranded — undelivered outbound, and inbound it
  accepted but never handed over;
- the key is still on that box, and there is no revocation short of a new
  identity, which is a new address;
- the ratchet state is stranded with it: until peers hear this device's fresh
  announce, what they send is encrypted to ratchets it does not hold.

**Picking a server.** Nobody types a hash. Every `lxmproxy.server` announce this
device hears is recorded (`lxmf.proxies.<dest>.{label,last}`) and offered by
the operator's label — `lxmf proxy servers` on the CLI, the **Heard** line in
the settings form. What is stored per identity is the one you chose:

```
s.lxmf.id.<n>.proxy_dest    32-hex lxmproxy.server destination
```

**While proxied, an outbound has five states and no others** — however many the
server itself passes through:

| state | glyph | means |
|---|---|---|
| `QUEUED` | none | nothing has left this device yet |
| `SENDING_TO_PROXY` | `…` | a long message still crossing to the proxy |
| `ON_OUR_PROXY` | ✓ | our proxy has it and is trying |
| `OUR_PROXY_DELIVERED` | ✓✓ | it reached the recipient — a real LXMF proof, relayed |
| `OUR_PROXY_GAVE_UP` | ✕ | our proxy tried and stopped |

**The proxy says one thing per message: how it went.** That it took the message
is not news worth a frame — the Channel is proved end to end, so a SEND that has
gone out is a SEND the proxy has, and the ✓ appears without anything coming
back. Only a message too long for one Channel message sits at `…` for a while,
because that one crosses as a transfer that can fail halfway.

The server's own machinery — path requests, link retries, proof waits — is its
business and not a state of ours, so none of it crosses at all.
Nothing is lost: the failure it stopped on is kept verbatim on the record as
`proxy_status` and named on the message's detail page ("Our proxy gave up: it
tried and stopped: `NO_ROUTE`"). Saying it there rather than in the conversation
keeps the distinction that matters — that it is the **proxy's verdict on
reaching the recipient**, not this device's trouble reaching the proxy. Trouble
of that kind has its own answers: `PROXY_REFUSED` when the server will not take
the message at all, and a plain `QUEUED` when there is no Channel to offer it
to.

A body over the server's `max_envelope_kb` never leaves: `HELLO` states the
ceiling and the client fails the draft `TOO_LARGE` itself rather than spending
the air on a body only to be refused.

**An open link outranks the proxy.** A conversation link is dialled with the
account's identity and carries its own traffic; none of it goes through the
registration the server holds. So when one is open to the peer, a send goes
straight there — fewer hops, no copy on somebody else's box, and a delivery
proof of our own rather than a relayed one. The proxy takes over again the
moment the link is not there. Return traffic the peer does not put on that same
link still arrives by way of the proxy, which is the address the world knows.

Messages that travelled that way carry `via_link` on the record, and both
frontends draw a small link mark for them — on an outgoing message before the
checkmarks, since how it travelled comes before how it landed. Only while
proxied: unproxied, every message goes direct and the mark would say nothing.

**Deleting an outbound tells the proxy** (`DROP`), so a message deleted here is
not still being sent by a machine elsewhere; if it has already gone, the server
simply stops owing us its status. **Resend, while proxied and with no link
open, is `RETRY`** — one frame naming the record. The body is already on the
server, and putting it back on the air to say "again" would cost the whole
message to carry one bit.

Inbound arrives as ordinary message records. A body over the link's own
threshold is **withheld**: the bubble offers a download instead of the text, and
`cmd.fetch` (the button) asks for it. That state is durable, so a reboot
mid-decision still knows the body is missing:

```
msgs.<peer>.<id>.body_absent   1 = the body is still on the server
msgs.<peer>.<id>.body_size     the body's length either way
```

**A direct link still works.** Opening a conversation Link to a peer needs only
the identity key, not a registered destination, and we identify on our own links
so peers reply over them. So a proxied device can open one deliberately and talk
to a peer with the proxy out of the way — it just never does so on its own
initiative, because the proxy is the default path. Anything that rides the
mailbox handle — `cmd.ping`, `opportunistic-or-fail` — is unavailable while
proxied, and messages sent or received over a direct link leave holes in the
server's copy of the thread.

**Settings.** Account-scoped settings — display name, stamp cost, stamp
enforcement, the propagation-node list, whether the identity is enabled — live
on the server, because it is the end that faces the world, but are only ever
*edited* here, because this is the end with a UI. They are pushed on change and reconciled on connect. The
server pushes back what only it knows (real announce state, quota use), so the
UI shows truth rather than intent. **Contacts are not synced**: both ends
auto-create on first contact and are allowed to diverge, and each pushed message
carries the peer's display name as a hint for a peer this device never heard
announce.

Turn it on from Settings → Reticulum Mesh → LXMF Messages (per identity), or
from the CLI:

```
lxmf proxy                   role, server, Channel state, quota
lxmf proxy servers           the lxmproxy.server announces we have heard
lxmf proxy on <32-hex>       ask that server to take this account
lxmf proxy off               hand it back (stays working until it answers)
lxmf proxy force-off         the dead-server override, with the costs above
```

## Receiving a message

Inbound messages are verified, de-duplicated, and stored at
`s.lxmf.id.<n>.msgs.<peer>.<message_id>.*` with `status = RECEIVED`,
`dir = in`, `read = 0`. `<peer>` is the sender's 32-hex destination; the
64-hex key is the real LXMF `message_id`. Dedup survives reboots, so the
same message arriving twice is stored once.

The sender is stubbed into the per-identity address book at
`s.lxmf.id.<n>.contacts.<peer>.*` (with `trust = 0`) on first contact, and
`last_seen` is refreshed. To mark a message read, set `…read = 1` (the
firmware ignores this field — it is purely for your UI).

A message from a sender the device can't yet verify (it has never heard that
sender's announce, so rnsd has no cached public key) is **buffered, not
dropped**: the raw wire is parked in a small per-identity queue (25 deep,
30-minute TTL, oldest evicted on overflow) and the device asks the network for
a path — which prompts the sender to re-announce. Once the sender's identity
is known the buffered message is replayed, verified, and stored. Opportunistic
LXMF has no retransmission, so buffering is what keeps a single-packet message
from a not-yet-known sender from being lost.

## Delivery status & proofs

One `u8` `status` code per message carries the lifecycle stage, the terminal
outcome and the give-up reason together. The names below are what the CLI and
both frontends print; the numbers are persisted, so the list is append-only
(`LxmfStatus` in `esp-idf/include/lxmf.h`, mirrored in the browser's
`modules/lxmf.ts`).

| group | statuses | meaning |
|---|---|---|
| progress | `DRAFT` `QUEUED` `REQUESTING_PATH` `SENDING` `AWAITING_PROOF` `RETRYING_LINK` `RETRYING_DELIVERY` `SENDING_TO_PROXY` | still in play; the delivery queue will try again |
| finished | `DELIVERED` `OUR_PROXY_DELIVERED` `CANCELLED` `RECEIVED` | proof received (ours, or relayed by our proxy) / user cancelled / inbound |
| gave up | `DELIVERY_TIMEOUT` `LINK_FAIL` `TOO_LARGE` `BAD_PEER` `DISABLED` `PACK_FAIL` `RES_MALLOC` `RES_SEND` `EVICTED` `OUR_PROXY_GAVE_UP` … | why it stopped: out of time, a local error another attempt cannot fix, or our proxy's own verdict (which failure is on the record as `proxy_status`) |
| held by another node | `ON_OUR_PROXY` `ON_PN` `PROXY_REFUSED` `PN_FAIL` `PN_REJECTED` | proxy-server / propagation-node states |

The companion `tries` byte, not the status, is the definitive terminal marker:
`tries == 255` means gave up, and below that the message is still live whatever
the status reads. A one-shot status sets it to 255 the moment it occurs.

**Egress is not delivery.** Opportunistic packets get no native
acknowledgement, so a proof timeout is *not* a failure — the message may well
have arrived, the peer may not prove inbound, or the proof was lost. The
message goes back to the delivery queue as `RETRYING_DELIVERY` (or, after its
one re-send on the same Link, `RETRYING_LINK` for a Link send) and the identical
wire goes out again at the next sweep — the recipient dedups on `message_id`, so a proof that was merely
lost costs nothing — until the delivery timeout. Only a cryptographic delivery
proof (or the proof-grade Resource transfer acknowledgement) produces
`DELIVERED`.

Both frontends render this on outbound bubbles as the ALL-CAPS status name
plus a glyph: grey `…` while in play, two green checks for `DELIVERED` (which
needs no name), a grey ✕ for `CANCELLED`, a red ✕ once `tries` hits 255. A
message held by another node — handed to our proxy
(`ON_OUR_PROXY`) or uploaded to a propagation node (`ON_PN`) — gets a single
open-circle tick: a machine that is not mine has it, no proof of arrival.
`OUR_PROXY_DELIVERED` shows the same two checks as `DELIVERED`: they are one
fact about the message and differ only in which machine holds the proof.

## Ping

```
no link yet:  us ──LINKREQUEST──► peer ;  us ◄──LRPROOF── peer   (then dropped)
link open:    us ──probe (peer_dest | our_dest) on the link──► peer
              us ◄────────────── the link's own proof ───────── peer
```

The contact detail page's **Ping** button (web and LCD, plus `lxmf ping <peer>`)
measures a contact: the round trip, and — when a radio has heard the peer — the
path loss in both directions from the radio's own measurements (SUPE, published
as `lora.<n>.meas.*`; see [iface-lora](../iface-lora)).

**A ping is a link when there is no link.** The probe used to be a bare packet
to the peer's delivery destination, answered by its delivery proof. That
measures a round trip and nothing else — and the round trip is the half of the
answer people look at least. What they read is the line under it: the path loss
each way, which needs a power the far end *stated* and a report of what it heard
from us. A packet stating no power and asking for nothing produces neither, so a
contact that had never been messaged answered a press with zeros, and the same
press after one message answered properly — the message having done the exchange
the probe did not.

Establishing a link *is* that exchange. The request goes out carrying this
radio's power, the far end proves it by accepting and reports what it heard, µR
measures the round trip itself and rnsd publishes it as
`rnsd.links.<tag>.rtt_ms`. One question, and the whole answer comes back. The
link is dropped again once it has: what was wanted was the measurement, not a
session.

Where a conversation link is already open the probe rides that instead, as the
32-byte payload above, settling on the link's own delivery proof. That link has
been paid for already, its establishment produced the readings, and tearing down
a link somebody is talking over to measure it would be a strange way to answer a
button. The payload is our own `lxmf.delivery` hash and nothing else: it is not
a valid LXM wire, and the peer's lxmf names it as a probe and drops it *after*
proving it, since proving happens on hand-off and before any parsing.

Results are published per peer, RAM-only, overwritten by the next probe:

```
lxmf.ping.<peer>.state       probing | path | ok | no-proof | no-route |
                             timeout | cancelled | failed
lxmf.ping.<peer>.ts          unix seconds of the last state change
lxmf.ping.<peer>.rtt_ms      round trip, where something MEASURED one: µR's own
                             timing of the link handshake or of the packet
                             proof. Absent when nothing did
lxmf.ping.<peer>.answer_ms   press to outcome. Always there on a settled probe
lxmf.ping.<peer>.hops        0 — a link measures the round trip, not the path;
                             `rnpath <hash>` is what says how many hops it took
```

**A round trip is a measurement, not an elapsed time.** The two are far apart
on a first probe: a contact whose path is not cached spends tens of seconds
finding one before anything is measured, and reporting that wait as `rtt_ms`
said "30000 ms" about a link that answers in 300. So `rtt_ms` is written only
from µR's own timing — `rnsd.links.<tag>.rtt_ms` for a probe that dialled,
`.tx_rtt_ms` for one that rode an open link — and `answer_ms`, always present,
is how long the operator waited. A surface with no `rtt_ms` to show says
"answered in N ms" rather than dressing the wait up as a measurement.

That is the whole record: **the probe's own findings, and nothing else.** What
the radio measured is read live from `lora.<n>.meas.*` and never copied here.
iface-lora republishes those on its own 15 s beat, so a copy taken when a probe
settled was stale as soon as the radio next heard the peer — and on a *first*
probe it was empty, because the link had come up seconds earlier and had not
been published yet. That is what made a first ping answer with nothing and the
second one answer properly.

The reading is shown under **every** outcome, not just a good one: it measures
the link rather than the probe, and it is worth most where the probe came back
empty, since nothing answering while the peer is heard at −95 dBm is a different
fault from nothing answering from a peer never heard at all. A direction the
radio has not measured gets no line — a peer that does not speak SUPE states no
power, so no level of it is a loss, and what it gets instead is what this radio
knows on its own. Every surface renders it through one function
(`lxmfPingLink`, and its browser mirror `peerMeasOf` + `pingLinkLines`), so the
contact page, the screen and the console cannot drift into describing the same
measurement three different ways:

```
ok after 1204 ms | rtt=1102 ms hops=1
us->them 97 dB path loss, SNR 7 dB @ tx +10 dBm (10 mW)
them->us 95 dB path loss, SNR 9 dB @ tx +22 dBm (158 mW)
```

A path loss is a level measured against the power the **far end** transmitted
at, and only a SUPE peer states that power. So a peer outside the protocol has
no loss in either direction, and the line is what this radio knows on its own —
the level it read and the power it sent at:

```
heard @ -95 dBm / SNR 9.5 dB @ tx +22 dBm (158 mW)
```

No ages on any of it: these are the readings, and the probe that printed them
just ran.

One ping per identity is in flight at a time; pressing again supersedes rather
than queues, and a probe that draws no result at all ends as `timeout` so the
display never sticks on `probing`.

**Superseding ends the old probe outright, link and all.** A probe's link *is*
the probe: rnsd holds a Link for exactly as long as the consumer's ITS handle,
and refuses that link's tag to anyone else while the handle lives. A supersede
that only forgets the handle leaves a link keepaliving on the air with nobody
to answer for it and — the tag naming the peer — fails every later probe of
that contact at the dial, until µR lets the orphan go stale minutes later.
Messaging the contact would appear to cure that, since a probe with a
conversation link to ride never dials at all. The link tag also carries a
rotating counter (`lxmf.ping<n>.<peer8>.<xx>`), so no probe can inherit the
name, or the leftover state tree, of the one before it.

**The probe's deadline is derived from what it waits on.** A probe with no link
to ride first needs a path, and that search runs on rnsd's budget — so the
deadline is rnsd's path budget (`rnsdPathBudgetS`: `s.rnsd.link.path_timeout_s`,
or a round trip across the widest gateway distance if that is longer — 96 s)
plus 10 s for the handshake that follows, and the two cannot disagree. A deadline shorter than the
path search would declare a probe dead while rnsd is still legitimately looking,
which shows up as a first ping that answers with nothing and a second one, path
now cached, that measures fine.

**The button is never disabled and a press always answers.** The probe is a
link, dialled with the identity, so it owes nothing to this device's rnsd
registration — an account that is proxied, or one whose registration has not
come up yet, probes like any other. A press retries the registration on the way
past anyway, since a ping during rnsd's startup window is a good moment to bring
the mailbox up.

The buttons fire and forget — the keys above are what they render. `lxmf ping`
instead holds the prompt until `state` finishes, then prints the outcome with
the time it waited. It waits by reading the client's input in 100 ms slices,
which is what makes **Ctrl-C** (or Ctrl-D) end the wait, keeps the cli task's
ITS inbox serviced so new CLI connections aren't refused for the probe's
duration, and ends the wait when the session closes. Cancelling ends the
*wait*, not the probe: it is the lxmf task's, and its result still lands under
`lxmf.ping.<peer>`. Without an interactive session (cron, `spangap cli`) there
is nothing to read, and a plain delay paces the loop instead.

The probe's deadline must stay inside rnsd's proof window
(`s.rnsd.proof_timeout_s`): a probe that gives up before the transport does
reports `no-proof` for a proof still in flight. Raising rnsd's path budget
raises the probe's deadline with it, so the two have to be read together.

## Announces

- Each enabled identity **sets** its announce with rnsd ~30 s after startup, and
  again whenever what it advertises changes — including the moment it is created
  or imported. There is no periodic re-announce here and no interval setting for
  one: how often those bytes go on the air belongs to each interface, which is
  the only thing that knows what airtime costs on its medium. Every interface
  pane carries the interval and an **Announce now** button; see
  [rns/README.md](../rns/README.md), "The announce tick". Force one identity's
  announce with `lxmf.id.<n>.cmd.announce`.
- Every `lxmf.delivery` announce the device hears is written to the
  **heard-peer list**, one record per destination:

  ```
  lxmf.announces.<dest_hex>.{last,cost,hops,ratchet,name}
  ```

  It is ephemeral (RAM), bounded by `s.lxmf.max_announces` (default 2048,
  `0` disables eviction; oldest evicted on overflow), and is the source for
  a "people we've heard of" picker. It is distinct from per-identity
  `contacts`, which is each identity's own address book.

## Stamps (proof-of-work)

LXMF lets a recipient advertise a **stamp cost** in its announce — a
proof-of-work price (in bits; each bit doubles the work) a sender pays per
message as spam friction. Three knobs, all surfaced in the Settings panel
and the on-device settings pane:

- **`s.lxmf.stamp_cost`** (slider 0–18, default 8): the single cost we
  advertise to everyone. `0` advertises none. The cap of 18 keeps it
  generatable on-device; validation is cheap at any cost. This is *only*
  what we advertise.
- **`s.lxmf.generate_stamps`** (default 1): when sending to a peer who
  advertises a cost > 0, compute a stamp meeting it and append it. The PoW
  runs on the lxmf task (~4 s on a T-Deck, dominated by the one-time
  768 KB workblock build, so cost barely matters over this range); skipped
  entirely when the peer advertises no cost. A peer advertising above 18 is
  refused (sent unstamped) rather than freezing the task for minutes.
- **`s.lxmf.enforce_stamps`** (default 0): when on, drop inbound messages
  lacking a valid stamp for the cost we advertise.

Both generation and validation yield ~every 500 ms so the rest of the system
keeps ticking.

Uploading to a propagation node pays a **second, separate stamp** on top of
whatever the recipient's own cost put in the message: the node's announced
propagation cost, over a third-size workblock (256 KB against the recipient
stamp's 768 KB). It is paid whenever the node's announce advertises one — the
three knobs above govern only the recipient stamp. A node whose announce we
have never heard is sent an unstamped upload rather than one paying a guessed
cost, and it may reject it (`PN_REJECTED`).

## Storage variables

### Settings (`s.lxmf.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.lxmf.max_announces` | `2048` | Heard-peer list entry cap; `0` = no eviction. |
| `s.lxmf.stamp_cost` | `8` | Advertised PoW cost (bits, 0–18; `0` = none). |
| `s.lxmf.generate_stamps` | `1` | Pay a peer's advertised stamp cost when sending. |
| `s.lxmf.enforce_stamps` | `0` | Drop inbound without a valid stamp for our cost. |
| `s.lxmf.link_timeout` | `0` | Conversation-Link establishment budget, seconds; `0` = let rnsd derive it from the next hop's interface speed. Whichever decides it, rnsd publishes the result as `rnsd.links.<tag>.estab_timeout_s` and the send's own deadline adopts it — a DIRECT send never gives up on a link that is still inside the budget rnsd granted it. |
| `s.lxmf.link.idle_s` | `600` | Close a conversation Link idle past this many seconds (10 min); `0` = keep open (LRU at the 4-link cap and Reticulum's STALE teardown still bound it). |
| `s.lxmf.default_method` | `link-always` | The delivery method a message uses when neither it nor its identity names one (see "Delivery method"). |
| `s.lxmf.delivery_interval` | `10` | Minutes between sweeps of the delivery queue — how often a message that could not be delivered yet gets another attempt. |
| `s.lxmf.delivery_timeout` | `60` | Minutes a message may sit in the delivery queue before it ends as `DELIVERY_TIMEOUT`. Measured from when it was first queued; a reboot restarts it. |
| `s.lxmf.pn.<i>.hash` | — | Propagation-node list, index-ordered (`i` = 0–7); non-32-hex = free slot. |
| `s.lxmf.pn.<i>.name` | `""` | Optional display name for that node. |
| `s.lxmf.pn.<i>.check` | `1` | Poll this node for held messages. |
| `s.lxmf.pn.check_interval_s` | `1800` | Propagation-node check cadence; `0` = manual only. |
| `s.lxmf.sound` | `/fixed/lxmf/ding.wav` | Message-notification WAV (point at your own device-rate file if you like). |
| `s.lxmf.sound_enabled` | `1` | Play the notification sound on inbound delivery. |
| `s.lxmf.debug.only_local` | `0` | Demote per-heard-peer list debug logs to verbose. |
| `s.lxmf.cli.selected_id` | `0` | The CLI's selected identity. |

`s.lxmf.max_resource_size` (default 262144) gates the largest inbound
Resource and is consumed by rnsd, not lxmf — it is documented in
[rns](../rns).

### Per-identity (`s.lxmf.id.<n>.*`)

```
label            "main" | "imported" | user-set
enabled          1 (default); 0 = disabled
display_name     utf-8, advertised in announces
default_method   auto | link-always | link-if-one-exists | link-if-big | opportunistic-or-fail
                 (auto or empty ⇒ inherit global s.lxmf.default_method, default link-if-big)
proxy_role       off (default) | server | client — which device registers and
                 announces this account (see Being proxied)
proxy_dest       32-hex lxmproxy.server destination, while proxied
contacts.<peer>.{hash,pubkey,nick,display_name,trust,last_seen,pn}   address book (firmware stubs on first inbound/outbound; display_name follows the peer's announces; pubkey = the contact's own key, so verifying a message from them needs no route and no announce; pn = this contact's propagation node, all-zero = none)
msgs.<peer>.<key>.{dir,status,tries,peer,title,content,reply_to,reply_quote,method,ts,recv_ts,
                   read,message_id,body_absent,body_size,handed,told,offered,
                   via_link,proxy_status}
                                 per-conversation message records
                                 (handed/told are the proxy SERVER's;
                                  proxy_status is the CLIENT's record of
                                  which failure its proxy stopped on)
```

### Runtime (`lxmf.*`, RAM)

```
lxmf.up                          task alive
lxmf.id.<n>.up                   identity's mailbox connected
lxmf.id.<n>.ready                the slot can take a message (up, or proxied)
lxmf.id.<n>.dest_hash            hex16 lxmf.delivery address
lxmf.id.<n>.identity_hash        hex16 identity hash behind that address
lxmf.id.<n>.last_announce_s      unix seconds of last announce
lxmf.id.<n>.stats.{sent,received,pending,failed}
lxmf.id.<n>.proxy_state          "" | provisioning | client | releasing | server
lxmf.id.<n>.proxy_link           down | connecting | active — the Channel to the
                                 server; "" while this device answers itself
lxmf.id.<n>.proxy_text           the pane's one line, composed by the firmware
lxmf.id.<n>.proxy_label          the server's operator label
lxmf.id.<n>.proxy_quota          what the server reports it is holding for us
lxmf.id.<n>.accept               0 gates this destination's inbound in rnsd —
                                 dropped WITHOUT a proof, so the sender retries.
                                 Set by whatever ran out of room; a boot accepts.
lxmf.announces.<dest_hex>.{last,cost,hops,ratchet,name}
                                 heard-peer list (RAM, browser-mirrored)
lxmf.proxies.<dest_hex>.{label,last}   heard lxmproxy.server list
lxmf.proxies_text                the same, as one finished line each
lxmf.ping.<peer>.*               latest probe result for that contact — see Ping above.
```

**Signal display.** The contacts list and the conversation header show amber
bars for a peer a radio has heard, from SUPE's per-peer record of it
(`lora.<n>.meas.*` — the slot whose `tags` holds the first six hex characters
of the peer's destination hash); the header falls back to the gateway signal
(`rnsd.gw.*`) when no radio has heard the peer. Messages themselves carry no
radio data.

### Secrets

```
secrets.lxmf.id.<n>.privkey      128-hex Ed25519+X25519 key (wiped by identity_destroy)
```

## CLI — `lxmf`

All verbs act on the **selected identity** (`s.lxmf.cli.selected_id`,
default 0) unless noted.

```
lxmf create <name>          generate a new identity (prints the slot; "queued"
                            when the lxmf task is not up yet — it starts once
                            the device password is set, so a setup script's
                            create lands there and is run at start; or failure)
lxmf destroy <n>            wipe identity at slot <n> (secrets + storage)
lxmf id                     list identities (* = selected): slot, label, the
                            lxmf.delivery destination, and the identity hash
                            behind it
lxmf id <n>                 switch selected identity
lxmf chats                  list conversations (one row per peer; numbered)
lxmf msgs [<arg>]           no arg = chats; <peer> = that thread (newest first);
                            a bare status name = cross-conversation filter
                            (case-insensitive, e.g. `lxmf msgs delivered`);
                            `?` lists the names it takes, grouped by what each
                            one says about the message
lxmf unfinished              outbound from this identity that has not finished,
                            oldest first: peer, status, tries, age, title, and
                            when the delivery queue next sweeps. The test is the
                            one queueResume uses — `statusInProgress()` and
                            `tries != 255` — so the listing is exactly what the
                            queue will attempt again, never a second opinion
                            about it. ON_OUR_PROXY and ON_PN are absent by that
                            definition: another node holds those and ours will
                            not retry them.
lxmf cancel <n>|all         end those as CANCELLED, through the same
                            `cmd.cancel` a UI uses — an outbox slot is unwound
                            with OUT_CANCEL, anything else is stamped and leaves
                            the queue. `<n>` is from the last numbered listing
                            and is re-read before acting, so an index that has
                            since finished is refused rather than overwritten;
                            `all` is one command key write that lxmf's own task
                            fans out, so it means what is unfinished when that
                            task reads it, not when the listing was printed.
lxmf read <n>               print message #n from the last `lxmf msgs`; marks it read
lxmf c[ontacts] [<arg>]     list this identity's contacts (numbered); <arg> =
                            case-insensitive substring of display_name or nick
lxmf announces [<arg>]      cross-identity heard-peer list; <arg> = 32-hex
                            (one row) or a name substring; no arg = full dump
lxmf send <peer> <msg>      send; <peer> = 32-hex, a number from the last numbered
                            listing, or a name/nick substring
lxmf a[nnounce]             announce the selected identity now
lxmf p[ing] <peer>          probe <peer> (same forms as `send`); holds the prompt
                            until the probe finishes, its deadline expires, or
                            Ctrl-C, then prints the outcome, the wait, and both
                            ends' signal
lxmf proxy [<cmd>]          no arg = this identity's proxy state; `servers` =
                            the lxmproxy.server announces heard; `on <32-hex>`,
                            `off`, `force-off` (see **Being proxied**)
```

Numbered listings (`chats`, `msgs`, `unfinished`, `contacts`, `announces`) feed
the index arguments of `read` / `send` / `ping` / `cancel` / `msgs <#>`. One
index space serves them all, which is why `cancel` re-reads the record it lands
on rather than trusting that the last listing was `unfinished`. A `<peer>` substring is
matched case-insensitively against this identity's contacts (display_name and
nick) first, then the heard-peer list; multiple matches print a numbered
disambiguation list instead of acting, so the retry can pick a line number. Run
any of these on-device with `spangap cli "<command>"`.

## Frontends

**Settings** (Settings → Reticulum Mesh → LXMF Messages): described by the
`settings:` block in `straddle.yaml` and lowered by the build to both surfaces.
The four identity slots are rows gated on the slot being occupied; the
propagation nodes are a collection over the `lxmf.pnode.*` command keys; the create
and import forms submit to `lxmf.identity.*`, where the 128-hex and 32-hex rules
are stated once instead of as a regex per UI.

**Browser** (`browser/`, registered via `registerLxmf`): a Pinia store + RPC
(`modules/lxmf.ts`), the chat window (`panels/MessagesWindow.vue`), and the chat
components
(`components/lxmf/`: `PeerAvatar`, `ConversationList`, `ContactCard`,
`MessageBubble`, `Composer`, `AnnouncesView`, `ConversationThread`).

**On-device LCD app** (`esp-idf/conditional/spangap-lcd/src/lxmf_lcd.cpp`):
the **LXMF** app, an `LcdApp` installed via `lcdInstall(new LxmfApp())`. The
whole file lives
under `conditional/spangap-lcd/` and is compiled and registered only when
the [spangap-lcd](../spangap-lcd) straddle is in the build (the
`lxmfLcdRegister` init hook is `when:`-gated) — no `#if` anywhere.

**Picking an account.** With one identity neither surface asks. With more than
one, both ask on the way in and nowhere else — there is no in-window identity
switcher on either.

- **Web:** the dock icon raises a chooser listing every ready account; picking
  one opens or raises *that account's own window*. Each identity has its own
  independent window, so two accounts sit side by side, both live, each with
  its own conversation open. The **LXMF Messages** menu lists them the same
  way, one item per identity, as a second route to the same windows.
- **LCD:** every tap on the launcher tile re-offers the picker, so the second
  and further accounts are reachable without stopping the app; picking the one
  already in view goes back to the conversation the picker covered. The other
  two ways in carry their own answer and skip it — a recents switch resumes
  work in progress, and a tapped `lxmf@` link (`lcdShowProgram`) means a
  specific conversation under the identity already chosen. One app, one
  identity at a time.

**A conversation reopens where you left it.** Switching away and coming back
lands on the message that was at the top of the screen — on the LCD, on the
history page it was on — rather than at the newest, for as long as the session
lasts. A browser reload, an app close or a reboot forgets it, deliberately:
where somebody had a thread scrolled to is not worth a write, and every
conversation starting at its newest message is the right answer to a fresh
start. One left AT its newest opens on whatever arrived meanwhile.

Both frontends share the contact-info pattern: clicking anywhere on a
conversation's header (or, on the LCD, a contact row's circled-i — the info
icon is a cue, not the sole target) opens a per-peer info page showing the
destination hash grouped in fours for eye comparison (the web copy button
still yields the bare unspaced hex) and holding the delete-conversation
flow behind an explicit "Are you sure?" confirm. The page's back chevron
returns to whichever screen opened it — contact overview or message view.

The page opens with a row of buttons, each sized to its own text: **Delete**
(the conversation, behind the confirm) and **Ping**. The web Ping result is a
popover under the button, dismissed by a touch anywhere; the LCD's is inline
under the button row instead, which leaves the button reachable so a re-measure
is one press.

## What it owns

```
lxmf/
├── esp-idf/
│   ├── include/
│   │   ├── lxmf.h            public API (identity create / import / destroy)
│   │   ├── lxmf_stamp.h      stamp generate/validate
│   │   └── lxmproxy_wire.h   the LXMF-proxy frames, shared with the server
│   ├── src/
│   │   ├── lxmf.cpp          the lxmf task: identities, mailbox, send/recv,
│   │   │                     announces, and the proxy CLIENT
│   │   ├── lxmf_stamp.cpp    LXStamper-compatible PoW (self-contained SHA-256/HMAC/HKDF)
│   │   └── lxmproxy_wire.cpp the proxy frame codec (own minimal msgpack)
│   ├── conditional/spangap-lcd/src/lxmf_lcd.cpp   on-device LXMF app (LVGL)
│   └── data/lxmf/ding.wav                          notification sound → /fixed/lxmf/ding.wav
└── browser/
    └── src/{modules,panels,components}/…           web UI (see Frontends)
```

## Dependencies

- [rns](../rns) — the Reticulum stack; lxmf is a consumer over rnsd's ITS
  ports and byte-array API. Interfaces are decoupled — pick whichever ones
  suit the deployment.
- [audio](../audio) — soft, default-on dependency (`spangap/audio`) for the
  notification sound; pruned silently when absent (every call site is gated,
  so a build without it still links, just with no sound).

The other direction: [lxmproxy](../lxmproxy) — the proxy SERVER — requires this
straddle. The client half and the shared frame codec are here, so every node can
be proxied without carrying the server code.

## Read next

- [INTERNALS.md](INTERNALS.md) — the wire codec, the ITS framing, the
  task/threading model, the identity model, our deltas from upstream LXMF,
  and maintainer pitfalls.
