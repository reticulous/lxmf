/**
 * lxmf_lcd.cpp — on-device "LXMessenger" launcher program (LVGL).
 *
 * Models the web UI LXMF messenger across three screens within the one program
 * layer (mirroring the Settings program's page model):
 *   - Identity picker: shown first only when more than one identity is usable
 *     (up + announcing). A tap selects the slot and the program continues into
 *     the list — the web spawns one Messages window per identity; on a single
 *     launcher tile we pick up front instead. One usable identity skips it;
 *     none falls through to the list's "create an identity" guidance.
 *   - List: two tabs, each with its own search box on top (focused on entry so
 *     a hardware keyboard types straight into it). Contacts is selected first.
 *       Contacts:    peers you've messaged, newest-comms first; name +
 *                     last-message preview, a last-heard-announce age badge at
 *                     the right, and a phone-style swipe-left-to-delete (reveals
 *                     a red trashcan; the tap writes the delete sentinel). A
 *                     bare 32-hex query offers "message this address".
 *       On the Mesh:  every dest heard announcing within s.lxmf.on_mesh_expire
 *                     seconds (default 3600; 0 = all), newest first, age-badged.
 *                     No dedup with Contacts. The announce catalogue can hold
 *                     thousands, so it's capped (MESH_ROW_CAP) with a footer
 *                     noting the remainder; the search box narrows it.
 *   - Thread:   a back chevron + peer name + scroll-to-bottom chevron (top
 *     bar), the message bubbles (in left / out right), and a compose row
 *     (textarea + Send) at the end of the chat. The compose field is focused
 *     when the thread opens. The thread runs fullscreen (no system status bar).
 *
 * Layout is tuned for density: 1px vertical padding throughout, a smaller font,
 * single-line conversation summaries. Message/preview/name text is run through
 * printable() — control bytes and any codepoint the font can't draw are dropped,
 * so unrenderable unicode never leaves placeholder boxes (printable codepoints
 * the font does carry, e.g. accented Latin, pass through unchanged).
 *
 * Storage is the API (same keys the browser uses):
 *   s.lxmf.id.<n>.msgs.<peer>.<key>.{dir,content,ts,read,stage}   messages
 *   s.lxmf.id.<n>.contacts.<peer>.display_name                    names
 *   lxmf.id.<n>.up / .dest_hash                                   identity live
 *   lxmf.announces.<hex> = "<last>|<cost>|<hops>|<ratchet>|<name>" heard peers
 *   lxmf.id.<n>.cmd.send = "<peer>/<key>"                         send sentinel
 * Everything runs on the lcd task; storage subscriptions are dispatched there,
 * so we touch LVGL straight from the change callback.
 */
#include "lcd.h"
#include "lcd_app.h"   /* LcdApp + lcdInstall */
#include "lxmf_app.h"  /* LxmfApp — this straddle's services: class */
#include "mem.h"
#include "storage.h"
#include "compat.h"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace {

/* ---- look & feel (dense) ---- */

/* The vector UI face at the platform zoom (was the montserrat_12 bitmap). Set by
 * refreshFont() at the top of each top-level build, before any label or the
 * printable() coverage check reads it. All UI + its storage callbacks run on the
 * lcd task, so the (non-thread-safe) vector glyph lookups are safe. The bitmap
 * default only covers the pre-build window. */
const lv_font_t* kFont = &lv_font_montserrat_12_latin;
void refreshFont() { kFont = lcdFont(LcdFace::UI, (int)(14 * lcdUiScale() + 0.5f)); }

const int HDR_H = 20;   /* thread top bar height */

/* On-the-mesh render cap: heard announces can number in the thousands
 * (s.lxmf.max_announces defaults to 2048). One LVGL row per peer would blow the
 * heap and stall the list, so we sort by recency and draw at most this many; a
 * footer notes the remainder, and the search box narrows the column. */
const int MESH_ROW_CAP = 48;

/* Debounce the search: rebuildList walks the message store + the (up to
 * thousands) announce catalogue and re-creates every row, which runs on the lcd
 * task at a higher prio than the core-1 keyboard poll. Doing it inline on every
 * keystroke stalls that poll long enough that the C3 keyboard (which holds only
 * the last unread key, no real FIFO) overwrites keys typed during the rebuild —
 * they go missing. So coalesce: rebuild once the typing pauses this long. */
const int SEARCH_DEBOUNCE_MS = 300;

/* Keep only what the UI font can actually draw: strip C0/C1 control bytes and any
 * codepoint with no glyph (emoji, CJK, …). Valid UTF-8 multibyte sequences whose
 * glyph the font carries (accented Latin, if present) pass through unchanged.
 * `oneLine` folds CR/LF/TAB to a single space (previews); else newlines stay. */
std::string printable(std::string_view in, bool oneLine) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)          { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0){ cp = b & 0x07; len = 4; }
        else { i++; continue; }                       /* stray continuation / invalid lead */
        if (i + len > n) break;                        /* truncated tail */
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;

        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) continue;   /* C0 / DEL / C1 */
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) continue;
        out.append(in.data() + at, len);
    }
    return out;
}

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

/* ---- model (lcd-task-only, no locks) ---- */

struct Msg {
    std::string peer, key, content, stage;
    long ts = 0;
    bool in = false, read = false;
};

struct Ann { std::string hash, name; long last = 0; };   /* a heard announce */

int               g_id = -1;            /* active identity index, -1 = none/unpicked */
std::string       g_msgsPrefix;         /* "s.lxmf.id.N.msgs" */
std::vector<Msg>  g_msgs;               /* all non-draft messages for g_id */
std::vector<Ann>  g_anns;               /* heard announces (on-the-mesh column) */
std::vector<std::string> g_rowPeers;    /* peer per clickable list row (click index) */
std::vector<std::string> g_nomadTargets;/* "<hash>:<path>" per tappable Nomad link in the open thread */
std::string       g_curPeer;            /* peer of the open thread */
std::string       g_pendingOpenPeer;    /* contact tapped in nomad, awaiting an identity pick */
std::string       g_qContacts;          /* Contacts-tab search-box text */
std::string       g_qMesh;              /* On-the-Mesh-tab search-box text */
int               g_activeTab = 0;      /* 0 = Contacts, 1 = On the Mesh (default Contacts) */
bool              g_subscribed = false;
bool              g_listRefreshPending = false;   /* a coalesced list rebuild is queued */
lv_timer_t*       g_searchTimer = nullptr;        /* one-shot search debounce (null = idle) */

lv_obj_t* s_layer    = nullptr;
lv_obj_t* s_idpick   = nullptr;         /* identity picker (first screen, >1 ident) */
lv_obj_t* s_contacts = nullptr;         /* list screen: tab bar + two tab pages */
lv_obj_t* s_tabBtnC  = nullptr;         /* "Contacts" tab button */
lv_obj_t* s_tabBtnM  = nullptr;         /* "On the Mesh" tab button */
lv_obj_t* s_tabContacts = nullptr;      /* Contacts page (search + list); shown when tab 0 */
lv_obj_t* s_tabMesh  = nullptr;         /* On-the-Mesh page (search + list); shown when tab 1 */
lv_obj_t* s_searchC  = nullptr;         /* search textarea atop the Contacts list */
lv_obj_t* s_searchM  = nullptr;         /* search textarea atop the On-the-Mesh list */
lv_obj_t* s_listC    = nullptr;         /* Contacts row container (cleaned+rebuilt) */
lv_obj_t* s_listM    = nullptr;         /* On-the-Mesh row container (cleaned+rebuilt) */
lv_obj_t* s_thread   = nullptr;         /* conversation screen (built once, reused) */
lv_obj_t* s_msgList  = nullptr;         /* scroll container inside s_thread */
lv_obj_t* s_bubbles  = nullptr;         /* bubble column (cleaned+rebuilt) inside s_msgList */
lv_obj_t* s_threadName = nullptr;       /* header peer-name label */
lv_obj_t* s_compose  = nullptr;         /* compose textarea (last child of s_msgList) */
lv_obj_t* s_newIdTa  = nullptr;         /* "Add identity" name field (settings pane) */
lv_obj_t* s_importTa = nullptr;         /* "Import identity" hex field (settings pane) */

void refreshMsgs();
void refreshAnnounces();
void rebuildList(bool keepScroll = false);
void rebuildThread();
void showContacts();
void setActiveTab(int n);
void openThread(const std::string& peer);
void scheduleListRefresh();
void maybeOpenPending();
void onLcdOpenUrl(const char* key, const char* val);

/* ---- deferred focus ----
 * The launcher tile (or a tapped row) is focused into the input group on
 * click-release — AFTER our open handlers run — so an immediate focus is stolen
 * back. Fire it from a one-shot timer once that settles (mirrors cliFocus). */
lv_obj_t* g_focusTarget = nullptr;
void focusTimerCb(lv_timer_t*) {
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    g_focusTarget = o;
    lv_timer_t* ft = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(ft, 1);
}

/* ---- small text helpers ---- */

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}
bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
/* needle is already lowercased + trimmed; hash is lowercase hex. */
bool qmatch(const std::string& needle, const std::string& name, const std::string& hash) {
    if (needle.empty()) return true;
    return lower(name).find(needle) != std::string::npos ||
           hash.find(needle) != std::string::npos;
}

/* ---- identity ---- */

/* Loaded = the firmware has published this slot's delivery address. That happens
 * from local crypto at boot, before the mailbox connects — so a loaded
 * identity's stored history is viewable straight away, and only sending waits
 * for `up`. A config-only slot (no private key) never gets a dest_hash, so it's
 * excluded — which is exactly right, it can never come up. */
void loadedIds(std::vector<int>& out) {
    out.clear();
    for (int n = 0; n < 4; n++) {
        char k[40];
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        if (!storageGetStr(k, "").empty()) out.push_back(n);
    }
}

/* Fully up = the firmware has connected this slot's delivery dest, so it can
 * send/receive. Sending is gated on this even once history is already shown. */
bool idUp(int n) {
    if (n < 0) return false;
    char k[40];
    snprintf(k, sizeof k, "lxmf.id.%d.up", n);
    return storageGetInt(k, 0) == 1;
}

/* Persistent slots that exist in config (the label survives reboot), split by
 * whether they're enabled. Available from storage the instant we open, long
 * before the firmware brings a mailbox up. A configured-but-not-yet-up slot is
 * the normal post-reset state while rnsd + the mailbox come up — distinct from
 * "no identity at all", so the empty-list guidance can say the right thing
 * instead of falsely telling the user to create one. */
void configuredIds(std::vector<int>& enabled, std::vector<int>& disabled) {
    enabled.clear(); disabled.clear();
    for (int n = 0; n < 4; n++) {
        char k[48];
        snprintf(k, sizeof k, "s.lxmf.id.%d.label", n);
        if (!storageExists(k)) continue;
        snprintf(k, sizeof k, "s.lxmf.id.%d.enabled", n);
        (storageGetInt(k, 1) != 0 ? enabled : disabled).push_back(n);
    }
}

std::string idLabel(int n) {
    char k[48];
    snprintf(k, sizeof k, "s.lxmf.id.%d.display_name", n);
    std::string s = storageGetStr(k, "");
    if (s.empty()) { snprintf(k, sizeof k, "s.lxmf.id.%d.label", n); s = storageGetStr(k, ""); }
    if (s.empty()) s = "identity " + std::to_string(n);
    return printable(s, true);
}

void selectId(int n) {
    g_id = n;
    g_msgsPrefix = (n >= 0) ? ("s.lxmf.id." + std::to_string(n) + ".msgs") : "";
    refreshMsgs();
}

std::string peerName(const std::string& peer) {
    char k[96];
    snprintf(k, sizeof k, "s.lxmf.id.%d.contacts.%s.display_name", g_id, peer.c_str());
    std::string n = storageGetStr(k, "");
    if (!n.empty()) return printable(n, true);
    /* No stored contact name — fall back to the name we last heard this dest
     * announce (already parsed into g_anns for the on-the-mesh column). Covers
     * contacts stubbed hash-only before an announce arrived, and any left over
     * from before the send path learned to seed the name. */
    for (auto& an : g_anns)
        if (an.hash == peer && !an.name.empty()) return an.name;
    return peer.substr(0, 8) + "...";
}

/* ---- message store: rebuilt from storage via a leaf walk ---- */

void msgCb(const char* key, const char* val) {
    /* key = "s.lxmf.id.N.msgs.<peer>.<mkey>.<field>" */
    size_t plen = g_msgsPrefix.size();
    if (strncmp(key, g_msgsPrefix.c_str(), plen) != 0) return;
    const char* rest = key + plen;
    if (*rest == '.') rest++;
    const char* d1 = strchr(rest, '.'); if (!d1) return;
    std::string peer(rest, d1 - rest);
    const char* mk = d1 + 1;
    const char* d2 = strchr(mk, '.'); if (!d2) return;
    std::string mkey(mk, d2 - mk);
    const char* field = d2 + 1;
    if (strchr(field, '.')) return;     /* deeper than a leaf field */

    Msg* m = nullptr;
    for (auto& x : g_msgs) if (x.peer == peer && x.key == mkey) { m = &x; break; }
    if (!m) { g_msgs.emplace_back(); m = &g_msgs.back(); m->peer = peer; m->key = mkey; }

    if      (!strcmp(field, "dir"))     m->in   = (val && !strcmp(val, "in"));
    else if (!strcmp(field, "content")) m->content = val ? val : "";
    else if (!strcmp(field, "ts"))      m->ts   = val ? atol(val) : 0;
    else if (!strcmp(field, "read"))    m->read = (val && atoi(val) != 0);
    else if (!strcmp(field, "stage"))   m->stage = val ? val : "";
}

void refreshMsgs() {
    g_msgs.clear();
    if (g_id < 0) return;
    storageForEach(g_msgsPrefix.c_str(), msgCb);
    g_msgs.erase(std::remove_if(g_msgs.begin(), g_msgs.end(),
                                [](const Msg& m) { return m.stage == "draft"; }),
                 g_msgs.end());
}

/* ---- announce catalogue: "<last>|<cost>|<hops>|<ratchet>|<name>" leaves ---- */

void annCb(const char* key, const char* val) {
    const char* tail = key + (sizeof("lxmf.announces.") - 1);
    if (strchr(tail, '.')) return;       /* bare <hex> leaf only (no nested key) */
    Ann a;
    a.hash = tail;
    if (val) {
        a.last = atol(val);              /* first field, atol stops at the '|' */
        const char* p = val;
        int pipes = 0;
        while (*p && pipes < 4) { if (*p == '|') pipes++; ++p; }
        if (pipes == 4) a.name = printable(p, true);   /* everything past pipe #4 */
    }
    g_anns.push_back(std::move(a));
}

void refreshAnnounces() {
    g_anns.clear();
    storageForEach("lxmf.announces.", annCb);
    std::sort(g_anns.begin(), g_anns.end(),
              [](const Ann& a, const Ann& b) { return a.last > b.last; });
}

/* ---- compose / send ---- */

void sendMessage(const std::string& peer, const std::string& text) {
    if (g_id < 0 || !idUp(g_id) || peer.empty() || text.empty()) return;
    static unsigned seq = 0;
    char key[40];
    snprintf(key, sizeof key, "o_%u_%u", (unsigned)millis(), ++seq);
    char base[120];
    snprintf(base, sizeof base, "s.lxmf.id.%d.msgs.%s.%s", g_id, peer.c_str(), key);
    char k[200];
    long ts = (long)time(nullptr);
    auto setf = [&](const char* f, const char* v) {
        snprintf(k, sizeof k, "%s.%s", base, f);
        storageSet(k, v);
    };
    /* One atomic write: all fields plus the send sentinel land together, so the
       lxmf task never picks up a half-written message and the s.lxmf.id
       subscription fires once for the whole message instead of once per field. */
    storageBegin();
    setf("dir", "out");
    setf("peer", peer.c_str());
    setf("title", "");
    setf("content", text.c_str());
    setf("thread", "");
    /* Optimistic stage: `queued`, not `draft`, so the bubble appears the
       instant we send (renders as the "..." in-flight chip below). The
       lxmf task drives it on to sent/delivered/failed. */
    setf("stage", "queued");
    snprintf(k, sizeof k, "%s.ts", base);
    storageSet(k, (int)ts);
    /* Send sentinel (ephemeral): "<peer>/<key>". The lxmf task drives the stage. */
    char sentinel[48], payload[80];
    snprintf(sentinel, sizeof sentinel, "lxmf.id.%d.cmd.send", g_id);
    snprintf(payload, sizeof payload, "%s/%s", peer.c_str(), key);
    storageSet(sentinel, payload);
    storageEnd();

    /* Optimistic render: append the outgoing message to the in-RAM store now,
       on the lcd task, so onSend can draw the bubble immediately. Without this
       the bubble only appears once the storage write has round-tripped through
       the storage task and back via the change subscription — seconds under
       load. The eventual refreshMsgs() rebuilds g_msgs from storage and dedups
       on peer+key, so this entry is replaced in place rather than doubled. */
    Msg m;
    m.peer = peer; m.key = key; m.content = text; m.stage = "queued";
    m.ts = ts; m.in = false; m.read = false;
    g_msgs.push_back(std::move(m));
}

void onSend(lv_event_t*) {
    if (!s_compose || g_curPeer.empty()) return;
    if (g_id < 0 || !idUp(g_id)) return;   /* mailbox not up yet — sending is held */
    const char* t = lv_textarea_get_text(s_compose);
    if (t && *t) {
        sendMessage(g_curPeer, t);
        lv_textarea_set_text(s_compose, "");
        rebuildThread();     /* draw the optimistic bubble now, not on the storage round-trip */
        /* Paint it NOW. The Send button happens to fire in the lcd loop's input
         * step, right before lv_timer_handler renders — the bubble shows at once.
         * Keyboard Enter fires inside the loop's aux-drain instead, and before
         * that drain finishes it is fed our own send's storage-change echoes
         * (each a full refreshMsgs + rebuildThread — seconds under load), so the
         * bubble only appeared after the storm. One inline refresh makes both
         * paths visually identical: bubble first, then the echoes. */
        lv_refr_now(nullptr);
        deferFocus(s_compose);   /* keep the cursor in the entry box for the next message */
    }
}

/* "Add identity" commit — reads the name field and writes the ephemeral create
 * sentinel (the lxmf task picks a free slot and generates the identity). Fired
 * by the explicit Create button or by Enter, so it works without the keystroke
 * being obvious. No-op on an empty name. */
void onAddIdentity(lv_event_t*) {
    if (!s_newIdTa) return;
    const char* t = lv_textarea_get_text(s_newIdTa);
    if (t && *t) {
        storageSet("lxmf.cmd.identity_new", t);
        lv_textarea_set_text(s_newIdTa, "");
    }
}

/* Import commit — writes the ephemeral import sentinel with a 128-hex private
 * key (the lxmf task validates + picks a free slot). No-op on an empty field. */
void onImportIdentity(lv_event_t*) {
    if (!s_importTa) return;
    const char* t = lv_textarea_get_text(s_importTa);
    if (t && *t) {
        storageSet("lxmf.cmd.identity_import", t);
        lv_textarea_set_text(s_importTa, "");
    }
}

/* ---- per-identity Destroy (two-tap arm; wipes key + all of its storage) ----
 * Destroy is irreversible, so the first tap arms ("Confirm?") and a 3 s timer
 * disarms it; the second tap within that window writes the destroy sentinel. */
struct DestroyState { int slot; bool armed; lv_obj_t* lbl; lv_timer_t* t; };

void destroyDisarm(lv_timer_t* tm) {
    auto* d = static_cast<DestroyState*>(lv_timer_get_user_data(tm));
    d->armed = false; d->t = nullptr;
    if (d->lbl && lv_obj_is_valid(d->lbl)) lv_label_set_text(d->lbl, "Destroy");
}

void onDestroyClick(lv_event_t* e) {
    auto* d = static_cast<DestroyState*>(lv_event_get_user_data(e));
    if (!d->armed) {
        d->armed = true;
        lv_label_set_text(d->lbl, "Confirm?");
        d->t = lv_timer_create(destroyDisarm, 3000, d);
        lv_timer_set_repeat_count(d->t, 1);
    } else {
        if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
        char v[8];
        snprintf(v, sizeof v, "%d", d->slot);
        storageSet("lxmf.cmd.identity_destroy", v);
        d->armed = false;
        lv_label_set_text(d->lbl, "Destroy");
    }
}

void onDestroyDelete(lv_event_t* e) {
    auto* d = static_cast<DestroyState*>(lv_event_get_user_data(e));
    if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
    free(d);
}

/* One identity's admin block in the settings pane: name + up/down status, dest
 * (live), enabled toggle, and a two-tap Destroy. Mirrors the web LxmfPanel row.
 * destKey/enKey must be string literals — lcdSettingValue/Switch keep the key by
 * pointer — so the caller passes one literal pair per slot. */
void lxmfIdentityBlock(lv_obj_t* p, int n, const char* destKey, const char* enKey) {
    char k[48];

    lv_obj_t* hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(hdr, 6, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(k, sizeof k, "s.lxmf.id.%d.display_name", n);
    std::string nm = storageGetStr(k, "");
    if (nm.empty()) { snprintf(k, sizeof k, "s.lxmf.id.%d.label", n); nm = storageGetStr(k, ""); }
    if (nm.empty()) nm = "identity " + std::to_string(n);
    mkLabel(hdr, printable(nm, true) + "  (slot " + std::to_string(n) + ")", lv_color_white());

    snprintf(k, sizeof k, "lxmf.id.%d.up", n);
    bool up = storageGetInt(k, 0) == 1;
    mkLabel(hdr, up ? "up" : "down", up ? lv_color_hex(0x6fb98f) : lv_color_hex(0xa06868));

    lcdSettingValue (p, "dest",    destKey);
    lcdSettingSwitch(p, "enabled", enKey);

    lv_obj_t* b = lv_button_create(p);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5a2a2a), 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, "Destroy");
    lv_obj_center(l);
    auto* d = static_cast<DestroyState*>(gp_alloc(sizeof(DestroyState)));
    d->slot = n; d->armed = false; d->lbl = l; d->t = nullptr;
    lv_obj_add_event_cb(b, onDestroyClick,  LV_EVENT_CLICKED, d);
    lv_obj_add_event_cb(b, onDestroyDelete, LV_EVENT_DELETE,  d);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
}

void markRead(const std::string& peer) {
    for (auto& m : g_msgs) {
        if (m.peer == peer && m.in && !m.read) {
            char k[200];
            snprintf(k, sizeof k, "s.lxmf.id.%d.msgs.%s.%s.read", g_id, peer.c_str(), m.key.c_str());
            storageSet(k, 1);
            m.read = true;
        }
    }
}

/* ---- scrolling ---- */

void scrollThreadBottom(bool anim) {
    if (!s_msgList) return;
    lv_obj_update_layout(s_msgList);
    lv_obj_scroll_to_y(s_msgList, LV_COORD_MAX, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

/* ---- navigation ---- */

void showContacts() {
    lcdProgramFullscreen(false);            /* status bar back for the list screen */
    if (s_thread) lv_obj_add_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    if (s_idpick) lv_obj_add_flag(s_idpick, LV_OBJ_FLAG_HIDDEN);
    if (s_contacts) {
        refreshAnnounces();
        rebuildList();
        lv_obj_remove_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
        setActiveTab(g_activeTab);          /* re-show the last tab + focus its search */
    }
    g_curPeer.clear();
}

void buildThreadShell() {
    s_thread = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_thread);
    lv_obj_set_size(s_thread, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_thread, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_thread, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_thread, LV_FLEX_FLOW_COLUMN);

    /* Header: back chevron + peer name + scroll-to-bottom chevron. */
    lv_obj_t* hdr = lv_obj_create(s_thread);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { showContacts(); }, LV_EVENT_CLICKED, nullptr);

    s_threadName = mkLabel(hdr, "", lv_color_white());
    lv_obj_align(s_threadName, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t* down = mkLabel(hdr, LV_SYMBOL_DOWN, lv_color_hex(0xc0c8d0));
    lv_obj_align(down, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(down, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(down, 12);
    lv_obj_add_event_cb(down, [](lv_event_t*) { scrollThreadBottom(true); }, LV_EVENT_CLICKED, nullptr);

    /* Scroll area (grows to fill): the bubble column only. The compose row is a
     * sibling below it (not a child), so it stays pinned to the bottom. */
    s_msgList = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_msgList);
    lv_obj_set_width(s_msgList, lv_pct(100));
    lv_obj_set_flex_grow(s_msgList, 1);
    lv_obj_set_flex_flow(s_msgList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_msgList, 4, 0);
    lv_obj_set_style_pad_ver(s_msgList, 1, 0);
    lv_obj_set_style_pad_row(s_msgList, 1, 0);

    s_bubbles = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, lv_pct(100));
    lv_obj_set_height(s_bubbles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bubbles, 1, 0);
    lv_obj_remove_flag(s_bubbles, LV_OBJ_FLAG_SCROLLABLE);

    /* Compose row: textarea + Send. A direct child of the thread AFTER the scroll
     * area (not inside it), so it's pinned to the bottom of the screen and never
     * scrolls away with the messages. A thin top border sets it off from the
     * bubbles above (mirroring the scroll area's old pad_hor with its own). */
    lv_obj_t* comp = lv_obj_create(s_thread);
    lv_obj_remove_style_all(comp);
    lv_obj_set_width(comp, lv_pct(100));
    lv_obj_set_height(comp, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(comp, 2, 0);
    lv_obj_set_style_pad_hor(comp, 4, 0);
    lv_obj_set_style_pad_column(comp, 4, 0);
    lv_obj_set_style_border_side(comp, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(comp, 1, 0);
    lv_obj_set_style_border_color(comp, lv_color_hex(0x222b38), 0);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);

    /* Input and Send share one fixed height (line height + breathing room) so the
     * button is exactly as tall as the box rather than each sizing to its own
     * content. */
    int32_t inH = lv_font_get_line_height(kFont) + 8;

    s_compose = lv_textarea_create(comp);
    lv_textarea_set_one_line(s_compose, true);
    lv_textarea_set_placeholder_text(s_compose, "Message");
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_pad_ver(s_compose, 1, 0);
    lv_obj_set_style_pad_hor(s_compose, 4, 0);
    lv_obj_set_height(s_compose, inH);
    lv_obj_set_flex_grow(s_compose, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);                 /* Enter = Send */
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { scrollThreadBottom(false); },   /* typing -> bottom */
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* send = lv_button_create(comp);
    lv_obj_set_style_pad_ver(send, 0, 0);
    lv_obj_set_style_pad_hor(send, 8, 0);
    lv_obj_set_height(send, inH);
    lv_obj_t* sl = lv_label_create(send);
    lv_obj_set_style_text_font(sl, kFont, 0);
    lv_label_set_text(sl, "Send");
    lv_obj_center(sl);
    lv_obj_add_event_cb(send, onSend, LV_EVENT_CLICKED, nullptr);
}

/* Reflect the active identity's connection state onto the compose field. Until
 * its mailbox is up (the post-reset window while rnsd connects), history is
 * readable but a send would be held — so disable the field and say why, rather
 * than take text that can't go out. Re-run whenever `up` may have flipped. */
void composeReflectUp() {
    if (!s_compose) return;
    if (g_id >= 0 && idUp(g_id)) {
        lv_obj_remove_state(s_compose, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(s_compose, "Message");
    } else {
        lv_obj_add_state(s_compose, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(s_compose, "Waiting for initialization…");
    }
}

void openThread(const std::string& peer) {
    g_curPeer = peer;
    if (!s_thread) buildThreadShell();
    lv_label_set_text(s_threadName, peerName(peer).c_str());
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    if (s_idpick)   lv_obj_add_flag(s_idpick,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(true);             /* immersive chat: no status bar */
    markRead(peer);
    rebuildThread();
    composeReflectUp();
    deferFocus(s_compose);                  /* focus always rests on the entry box */
}

void onContactClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < g_rowPeers.size()) openThread(g_rowPeers[idx]);
}

/* A contact tapped in the nomad browser (lxmf.url_lcd) waits here until an
 * identity is active — opened immediately for a single-identity box, or after
 * the user picks one (onIdPick) when the picker is up. */
void maybeOpenPending() {
    if (g_id < 0 || g_pendingOpenPeer.empty()) return;
    std::string p = g_pendingOpenPeer;
    g_pendingOpenPeer.clear();
    openThread(p);
}

/* The on-device nomad browser tapped an lxmf@<hash> link and wrote the dest
 * hash to lxmf.url_lcd. Runs on the lcd task (subscribed via lcdRun in
 * lxmfLcdRegister), so LVGL is safe here. The core lxmf task issues the path
 * request off the same key; this is UI only. */
void onLcdOpenUrl(const char* /*key*/, const char* val) {
    if (!val || !*val) return;                 /* a clear */
    std::string peer(val);
    size_t colon = peer.find(':');             /* "<hash>[:<nonce>]" */
    if (colon != std::string::npos) peer.erase(colon);
    if (peer.size() != 32) return;             /* 16-byte dest hash = 32 hex */
    for (char c : peer)
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return;
    for (auto& c : peer) if (c>='A'&&c<='Z') c = (char)(c - 'A' + 'a');

    g_pendingOpenPeer = peer;
    lcdShowProgram("LXMF");   /* build (first open) + raise; routes to picker if >1 id */
    maybeOpenPending();       /* single id: opens now; picker: waits for onIdPick */
}

/* ---- thread rendering ---- */

/* ---- Nomad page links in message text ----
 * A message may quote a Nomad page URL "<32hex>:/path". We render those as
 * tappable links; a tap hands the URL to the Nomad browser via nomad.url_lcd
 * (nomad_lcd subscribes, brings itself forward, and navigates). This is the
 * exact reverse of nomad_lcd's lxmf@<hash> links → lxmf.url_lcd. */

/* Match a Nomad page link at line[pos]: 32 hex + ':' + '/' + a run of
 * non-space path chars, not part of a longer hex token. Fills `url` with
 * "<hash>:<path>" (hash lowercased); `len` is its byte length. */
bool matchNomadAt(const std::string& s, size_t pos, std::string& url, size_t& len) {
    auto hex = [](char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    if (pos + 34 > s.size()) return false;                  /* 32 + ':' + '/' */
    for (int k = 0; k < 32; k++) if (!hex(s[pos + k])) return false;
    if (s[pos + 32] != ':' || s[pos + 33] != '/') return false;
    if (pos > 0 && hex(s[pos - 1])) return false;           /* inside a longer hex run */
    size_t e = pos + 33;                                    /* at the '/' */
    while (e < s.size() && !isspace((unsigned char)s[e])) e++;
    while (e > pos + 34 && strchr(".,;:!?)]}'\"", s[e - 1])) e--;   /* trailing punctuation */
    url.assign(s, pos, e - pos);
    for (int k = 0; k < 32; k++) if (url[k] >= 'A' && url[k] <= 'Z') url[k] = (char)(url[k] - 'A' + 'a');
    len = e - pos;
    return true;
}

void onNomadLinkClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= g_nomadTargets.size()) return;
    /* "<hash>:<path>|<nonce>" — nonce makes repeat taps re-fire (nomad_lcd
     * strips it). Path fits comfortably; guard the buffer regardless. */
    char v[160];
    snprintf(v, sizeof v, "%s|%u", g_nomadTargets[idx].c_str(), (unsigned)millis());
    storageSet("nomad.url_lcd", v);
}

/* Render a message body into the bubble. No link → one wrapping label, exactly
 * as before. With link(s) → the body is split at link boundaries into plain
 * labels and clickable blue link labels, each wrapping within the bubble; they
 * stack in the bubble's column. (A label can't make a substring clickable, so
 * a link becomes its own widget — the same constraint nomad_lcd handles for
 * lxmf@ links.) */
void addBubbleText(lv_obj_t* bub, const std::string& content) {
    std::string body = printable(content, false);   /* drop unrenderables, keep newlines */

    auto addText = [&](size_t from, size_t to) {
        if (to <= from) return;
        lv_obj_t* l = mkLabel(bub, body.substr(from, to - from), lv_color_white());
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_width(l, 228, 0);
    };

    size_t i = 0, textStart = 0;
    bool any = false;
    while (i < body.size()) {
        std::string url; size_t len;
        if (matchNomadAt(body, i, url, len)) {
            addText(textStart, i);
            lv_obj_t* l = mkLabel(bub, url, lv_color_hex(0x6db3ff));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_max_width(l, 228, 0);
            lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(l, 4);
            size_t idx = g_nomadTargets.size();
            g_nomadTargets.push_back(url);
            lv_obj_add_event_cb(l, onNomadLinkClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
            if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), l);
            i += len;
            textStart = i;
            any = true;
        } else {
            i++;
        }
    }
    if (!any) { addText(0, body.size()); return; }   /* unchanged: single label */
    addText(textStart, body.size());
}

void addBubble(const Msg& m) {
    lv_obj_t* row = lv_obj_create(s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m.in ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bub = lv_obj_create(row);
    lv_obj_remove_style_all(bub);
    lv_obj_set_width(bub, LV_SIZE_CONTENT);
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bub, 240, 0);          /* fixed px — a pct of a content-sized chain collapses */
    lv_obj_set_style_bg_color(bub, m.in ? lv_color_hex(0x2a313a) : lv_color_hex(0x2563a0), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 6, 0);
    lv_obj_set_style_pad_ver(bub, 1, 0);
    lv_obj_set_style_pad_hor(bub, 6, 0);
    lv_obj_set_flex_flow(bub, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bub, LV_OBJ_FLAG_SCROLLABLE);

    addBubbleText(bub, m.content);                    /* text + tappable Nomad links, wrapped to 228 */

    char tbuf[8] = "";
    if (m.ts > 0) {
        time_t tt = m.ts;
        struct tm tmv {};
        localtime_r(&tt, &tmv);
        strftime(tbuf, sizeof tbuf, "%H:%M", &tmv);
    }

    /* Meta row: timestamp + (outbound only) a delivery-status glyph.
     * kFont (montserrat latin) carries the LVGL symbol set:
     *   queued/sending  "..."                 grey   (in flight)
     *   sent            one checkmark         grey   (egressed, no proof)
     *   delivered       two checkmarks        green  (cryptographic proof)
     *   failed/cancelled  X                   red
     * Stage changes re-render via the s.lxmf.id storage subscription
     * (onStorageChange → refreshMsgs + rebuildThread). */
    lv_obj_t* meta = lv_obj_create(bub);
    lv_obj_remove_style_all(meta);
    lv_obj_set_width(meta, LV_SIZE_CONTENT);
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(meta, 4, 0);
    lv_obj_remove_flag(meta, LV_OBJ_FLAG_SCROLLABLE);

    mkLabel(meta, tbuf, lv_color_hex(0xc0c8d0));
    if (!m.in) {
        const char* sym = nullptr;
        lv_color_t  col = lv_color_hex(0x8a93a0);
        if (m.stage == "sent")           { sym = LV_SYMBOL_OK; }
        else if (m.stage == "delivered") { sym = LV_SYMBOL_OK LV_SYMBOL_OK;
                                           col = lv_color_hex(0x4abf6a); }
        else if (m.stage == "failed" ||
                 m.stage == "cancelled") { sym = LV_SYMBOL_CLOSE;
                                           col = lv_color_hex(0xd9534f); }
        else if (m.stage == "queued" ||
                 m.stage == "sending")   { sym = "..."; }
        if (sym) mkLabel(meta, sym, col);
    }
}

void rebuildThread() {
    if (!s_bubbles || g_curPeer.empty()) return;
    refreshFont();
    lv_obj_clean(s_bubbles);
    g_nomadTargets.clear();          /* link widgets are gone with the cleaned bubbles */
    std::vector<const Msg*> ms;
    for (auto& m : g_msgs) if (m.peer == g_curPeer) ms.push_back(&m);
    std::sort(ms.begin(), ms.end(), [](const Msg* a, const Msg* b) { return a->ts < b->ts; });
    for (auto* m : ms) addBubble(*m);
    scrollThreadBottom(false);                          /* newest + compose at bottom */
}

/* ---- list rendering (two tabs: Contacts + On the Mesh) ---- */

struct Conv { std::string peer, preview; long ts = 0; int unread = 0; };

/* Compact "time since" badge shown at the right of every row: how long ago we
 * last heard this dest announce. Chat-app style — s / m(inutes) / h / d / w / y.
 * Empty string (drawn as nothing) when we've never heard it. */
std::string relAge(long secs) {
    if (secs < 0) secs = 0;
    char b[12];
    if      (secs < 60)          snprintf(b, sizeof b, "%lds", secs);
    else if (secs < 3600)        snprintf(b, sizeof b, "%ldm", secs / 60);
    else if (secs < 86400)       snprintf(b, sizeof b, "%ldh", secs / 3600);
    else if (secs < 7*86400L)    snprintf(b, sizeof b, "%ldd", secs / 86400L);
    else if (secs < 365*86400L)  snprintf(b, sizeof b, "%ldw", secs / (7*86400L));
    else                         snprintf(b, sizeof b, "%ldy", secs / (365*86400L));
    return b;
}

/* Last-heard-announce timestamp for a dest (0 = never). g_anns is kept sorted by
 * .last descending, so the first hit is the most recent. */
long lastAnnounce(const std::string& peer) {
    for (auto& an : g_anns) if (an.hash == peer) return an.last;
    return 0;
}

/* Populate a row host (styled by the caller) with the two-line body + a
 * right-aligned age badge: [ name / one-line subtitle ]········[ age ].
 * `host` is turned into a flex row; the text stacks in a growing left column so
 * a long subtitle ellipsizes rather than shoving the badge off-screen. */
void fillPeerContent(lv_obj_t* host, const std::string& title, const std::string& sub,
                     const std::string& age, lv_color_t titleColor) {
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* col = lv_obj_create(host);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 0);                 /* grow from 0 so the badge keeps its space */
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    /* CRITICAL: a plain lv_obj is CLICKABLE by default, and this column covers
     * nearly the whole row — it was winning the hit-test over the row button, so
     * taps landed here (no handler, no bubbling) and silently died; only the few
     * uncovered padding pixels ever reached the row. Labels aren't clickable, so
     * lists that put labels straight on the button never hit this. Make the
     * column transparent to input so presses fall through to the row. */
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    mkLabel(col, title, titleColor);

    /* LONG_DOT ellipsizes only within the label's own size, so bound both:
     * full column width + exactly one line tall → a long subtitle is clipped to
     * a single "…"-terminated line instead of wrapping. */
    lv_obj_t* pv = mkLabel(col, sub, lv_color_hex(0x8a93a0));
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pv, lv_pct(100));
    lv_obj_set_height(pv, lv_font_get_line_height(kFont));

    if (!age.empty()) {
        lv_obj_t* a = mkLabel(host, age, lv_color_hex(0x6a7280));
        lv_obj_set_style_pad_left(a, 4, 0);
    }
}

/* A plain tappable row (On-the-Mesh + the "message this address" affordance).
 * Uses the shared g_rowPeers click index → onContactClick → openThread. */
void addPeerRow(lv_obj_t* list, const std::string& peer, const std::string& title,
                const std::string& sub, const std::string& age, lv_color_t titleColor) {
    size_t idx = g_rowPeers.size();
    g_rowPeers.push_back(peer);

    lv_obj_t* row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, onContactClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    fillPeerContent(row, title, sub, age, titleColor);
}

/* ---- contact row: a plain tappable body + a fixed trashcan ----
 * The body is an ordinary button (opens the thread) — structurally identical to
 * the mesh rows, which tap reliably; the earlier swipe-to-delete foreground was
 * the one thing unique to this list and made taps miss, so it's gone. A small
 * trashcan sits at the right edge (no background). Delete is destructive, so it's
 * two-tap: the first tap arms (icon → red + grown, 3 s auto-disarm), the second writes the
 * delete sentinel (the lxmf task drops the whole conversation + contact). Per-row
 * state lives in a heap ContactDel freed on the trashcan's DELETE. */
struct ContactDel {
    std::string peer;
    lv_obj_t*   icon = nullptr;
    bool        armed = false;
    lv_timer_t* t = nullptr;
};

void contactDelDisarm(lv_timer_t* tm) {
    auto* d = static_cast<ContactDel*>(lv_timer_get_user_data(tm));
    d->armed = false; d->t = nullptr;
    if (d->icon && lv_obj_is_valid(d->icon)) {
        lv_obj_set_style_text_color(d->icon, lv_color_hex(0x8a93a0), 0);
        lv_obj_set_style_transform_scale(d->icon, 192, 0);
    }
}

void onContactTrash(lv_event_t* e) {
    auto* d = static_cast<ContactDel*>(lv_event_get_user_data(e));
    if (g_id < 0) return;
    if (!d->armed) {
        d->armed = true;
        lv_obj_set_style_text_color(d->icon, lv_color_hex(0xd9534f), 0);   /* confirm cue */
        lv_obj_set_style_transform_scale(d->icon, 320, 0);
        d->t = lv_timer_create(contactDelDisarm, 3000, d);
        lv_timer_set_repeat_count(d->t, 1);
    } else {
        if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
        /* Whole-conversation delete: lxmf wipes the msgs + contact subtree; the
         * storage change rebuilds the list without this peer (and frees this). */
        char k[48];
        snprintf(k, sizeof k, "lxmf.id.%d.cmd.delete", g_id);
        storageSet(k, d->peer.c_str());
    }
}

void onContactTrashFree(lv_event_t* e) {
    auto* d = static_cast<ContactDel*>(lv_event_get_user_data(e));
    if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
    delete d;
}

void addContactRow(lv_obj_t* list, const std::string& peer, const std::string& title,
                   const std::string& sub, const std::string& age, lv_color_t titleColor) {
    int lineH = lv_font_get_line_height(kFont);

    /* Tappable body — the same plain button the mesh tab uses (shared g_rowPeers
     * index → onContactClick → openThread). */
    size_t idx = g_rowPeers.size();
    g_rowPeers.push_back(peer);

    lv_obj_t* row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, onContactClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    fillPeerContent(row, title, sub, age, titleColor);   /* row → flex: [body][age] */

    /* Fixed trashcan at the right edge (grey icon, no background). */
    auto* d = new ContactDel();
    d->peer = peer;
    lv_obj_t* trash = lv_button_create(row);
    lv_obj_remove_style_all(trash);
    lv_obj_set_size(trash, lineH + 14, 2 * lineH);
    lv_obj_set_style_bg_opa(trash, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(trash, LV_OBJ_FLAG_SCROLLABLE);
    d->icon = mkLabel(trash, LV_SYMBOL_TRASH, lv_color_hex(0x8a93a0));
    lv_obj_center(d->icon);
    /* Resting icon is shrunk to 75%; arming grows it to 125% (scale is /256,
     * pivot centered so it grows in place). */
    lv_obj_set_style_transform_pivot_x(d->icon, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(d->icon, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(d->icon, 192, 0);
    lv_obj_add_event_cb(trash, onContactTrash,     LV_EVENT_CLICKED, d);
    lv_obj_add_event_cb(trash, onContactTrashFree, LV_EVENT_DELETE,  d);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), trash);
}

void grpLabel(lv_obj_t* list, const char* t) {
    lv_obj_t* l = mkLabel(list, t, lv_color_hex(0x6a7280));
    lv_obj_set_style_pad_top(l, 3, 0);
}

/* Rebuild both tab lists from the in-RAM stores. Cheap enough to redo both on
 * any change (contacts come from g_msgs, the mesh from the sorted g_anns); the
 * hidden tab's rows just wait. Each list keeps its own scroll across a live
 * refresh. */
/* Delete every row but keep child 0 — the search box now lives inside the scroll
 * list (so it scrolls with the rows), and must survive a rebuild with its text
 * and focus intact. */
void clearRows(lv_obj_t* list) {
    for (int32_t i = (int32_t)lv_obj_get_child_count(list) - 1; i >= 1; --i)
        lv_obj_delete(lv_obj_get_child(list, i));
}

void rebuildList(bool keepScroll) {
    if (!s_listC || !s_listM) return;
    refreshFont();
    int32_t syC = keepScroll ? lv_obj_get_scroll_y(s_listC) : 0;
    int32_t syM = keepScroll ? lv_obj_get_scroll_y(s_listM) : 0;
    clearRows(s_listC);
    clearRows(s_listM);
    g_rowPeers.clear();

    if (g_id < 0) {
        /* g_id < 0 spans two very different situations. A configured slot exists
         * in persistent storage the instant we open, but its mailbox isn't up
         * until rnsd starts and the delivery dest connects — seconds after a
         * reset. Only say "create one" when there is genuinely nothing to bring
         * up; otherwise reassure that it's coming. */
        std::vector<int> en, dis;
        configuredIds(en, dis);
        const char* msg =
            !en.empty()  ? "Waiting for initialization…"                :
            !dis.empty() ? "Identity disabled.\nEnable it in Settings." :
                           "No active identity.\nCreate one in Settings.";
        mkLabel(s_listC, msg, lv_color_hex(0x8a93a0));
        return;
    }

    long now = (long)time(nullptr);

    /* ---- Contacts tab: conversations, newest-comms first, each with a
     * last-heard-announce badge and a swipe-to-delete. ---- */
    std::string nC = lower(trim(g_qContacts));
    std::vector<Conv> convs;
    for (auto& m : g_msgs) {
        Conv* c = nullptr;
        for (auto& x : convs) if (x.peer == m.peer) { c = &x; break; }
        if (!c) { convs.push_back({ m.peer, "", 0, 0 }); c = &convs.back(); }
        if (m.ts >= c->ts) { c->ts = m.ts; c->preview = (m.in ? "" : "You: ") + m.content; }
        if (m.in && !m.read) c->unread++;
    }
    std::sort(convs.begin(), convs.end(), [](const Conv& a, const Conv& b) { return a.ts > b.ts; });

    int cRows = 0;

    /* "New": a bare 32-hex query for a peer we don't already have a thread with. */
    if (isHex32(nC)) {
        bool known = false;
        for (auto& c : convs) if (c.peer == nC) { known = true; break; }
        if (!known) {
            grpLabel(s_listC, "New");
            addPeerRow(s_listC, nC, "Message this address", nC, "", lv_color_white());
            cRows++;
        }
    }

    for (auto& c : convs) {
        std::string nm = peerName(c.peer);
        if (!qmatch(nC, nm, c.peer)) continue;
        std::string title = nm;
        if (c.unread > 0) title += " (" + std::to_string(c.unread) + ")";
        long la = lastAnnounce(c.peer);
        addContactRow(s_listC, c.peer, title, printable(c.preview, true),
                      la > 0 ? relAge(now - la) : std::string(),
                      c.unread > 0 ? lv_color_hex(0x6cc06c) : lv_color_white());
        cRows++;
    }
    if (cRows == 0)
        mkLabel(s_listC, nC.empty() ? "No conversations yet.\nSearch the mesh tab."
                                    : "No matches.", lv_color_hex(0x8a93a0));

    /* ---- On the Mesh: every dest heard within the expiry window, newest first,
     * badged by recency. No dedup with Contacts — a peer can appear in both. ---- */
    std::string nM = lower(trim(g_qMesh));
    int expire = storageGetInt("s.lxmf.on_mesh_expire", 3600);   /* 0 = show all */
    int shown = 0, total = 0;
    for (auto& an : g_anns) {
        if (expire > 0 && an.last > 0 && (now - an.last) > expire) continue;   /* too old */
        std::string nm = an.name.empty() ? peerName(an.hash) : an.name;
        if (!qmatch(nM, nm, an.hash)) continue;
        total++;
        if (shown >= MESH_ROW_CAP) continue;
        addPeerRow(s_listM, an.hash, nm, an.hash,
                   an.last > 0 ? relAge(now - an.last) : std::string(), lv_color_white());
        shown++;
    }
    if (total > shown) {
        char foot[48];
        snprintf(foot, sizeof foot, "+%d more — search to narrow", total - shown);
        grpLabel(s_listM, foot);
    }
    if (shown == 0)
        mkLabel(s_listM, nM.empty() ? "Nothing heard recently." : "No matches.",
                lv_color_hex(0x8a93a0));

    /* Hold each list's reading position across a live refresh (clamped if it
     * shrank); a fresh open / new search starts at the top (keepScroll false). */
    if (keepScroll) {
        lv_obj_update_layout(s_listC);
        lv_obj_update_layout(s_listM);
        if (syC > 0) lv_obj_scroll_to_y(s_listC, syC, LV_ANIM_OFF);
        if (syM > 0) lv_obj_scroll_to_y(s_listM, syM, LV_ANIM_OFF);
    }
}

/* ---- tabs ---- */

void styleTab(lv_obj_t* b, bool active) {
    if (!b) return;
    lv_obj_set_style_bg_color(b, active ? lv_color_hex(0x2563a0) : lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
}

void setActiveTab(int n) {
    g_activeTab = n;
    bool c = (n == 0);
    if (s_tabContacts) { if (c) lv_obj_remove_flag(s_tabContacts, LV_OBJ_FLAG_HIDDEN);
                         else    lv_obj_add_flag  (s_tabContacts, LV_OBJ_FLAG_HIDDEN); }
    if (s_tabMesh)     { if (c) lv_obj_add_flag   (s_tabMesh,     LV_OBJ_FLAG_HIDDEN);
                         else    lv_obj_remove_flag(s_tabMesh,     LV_OBJ_FLAG_HIDDEN); }
    styleTab(s_tabBtnC, c);
    styleTab(s_tabBtnM, !c);
    lv_obj_t* shown = c ? s_listC : s_listM;
    if (shown) lv_obj_scroll_to_y(shown, 0, LV_ANIM_OFF);   /* come back to a tab at the top */
    deferFocus(c ? s_searchC : s_searchM);   /* type-to-search on the shown tab */
}

void onTabClick(lv_event_t* e) {
    setActiveTab((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- search boxes (one per tab) ---- */

void searchTimerCb(lv_timer_t*) {
    g_searchTimer = nullptr;   /* one-shot: LVGL deletes it after this returns */
    rebuildList();
}

void onSearchChanged(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* t = lv_textarea_get_text(ta);
    (ta == s_searchM ? g_qMesh : g_qContacts) = t ? t : "";
    /* Postpone the rebuild to SEARCH_DEBOUNCE_MS after the last keystroke so a
     * fast typist isn't fighting a per-key rebuild (see SEARCH_DEBOUNCE_MS). */
    if (g_searchTimer) {
        lv_timer_reset(g_searchTimer);
    } else {
        g_searchTimer = lv_timer_create(searchTimerCb, SEARCH_DEBOUNCE_MS, nullptr);
        lv_timer_set_repeat_count(g_searchTimer, 1);
    }
}

void onSearchEnter(lv_event_t* e) {
    /* Enter flushes any pending debounce, then opens the thread if the query is
     * a bare 32-hex address (start a conversation with an exact dest). */
    if (g_searchTimer) {
        lv_timer_delete(g_searchTimer);
        g_searchTimer = nullptr;
        rebuildList();
    }
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    std::string needle = lower(trim(ta == s_searchM ? g_qMesh : g_qContacts));
    if (isHex32(needle)) openThread(needle);
}

/* One tab page: a single vertical scroll list whose first child is the search
 * box, so the search scrolls away with the rows instead of staying pinned. The
 * page and the list are the same object; rebuildList clears only the rows
 * (children >= 1) and leaves the search — and its text/focus — in place. */
lv_obj_t* buildTabPage(lv_obj_t*& page, lv_obj_t*& search, const char* placeholder) {
    page = lv_obj_create(s_contacts);           /* scrollable (default) — this IS the list */
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 2, 0);

    search = lv_textarea_create(page);          /* child 0 — scrolls with the rows */
    lv_textarea_set_one_line(search, true);
    lv_textarea_set_placeholder_text(search, placeholder);
    lv_obj_set_style_text_font(search, kFont, 0);
    lv_obj_set_style_pad_ver(search, 2, 0);
    lv_obj_set_style_pad_hor(search, 6, 0);
    lv_obj_set_width(search, lv_pct(100));
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), search);
    lv_obj_add_event_cb(search, onSearchChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(search, onSearchEnter,   LV_EVENT_READY,         nullptr);

    return page;   /* rows are appended after `search` */
}

lv_obj_t* mkTabButton(lv_obj_t* bar, const char* text, int idx) {
    lv_obj_t* b = lv_button_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_ver(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* l = mkLabel(b, text, lv_color_white());
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onTabClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

void buildContactsScreen() {
    s_contacts = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_contacts);
    lv_obj_set_size(s_contacts, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_contacts, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_contacts, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_contacts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_contacts, 4, 0);
    lv_obj_set_style_pad_ver(s_contacts, 1, 0);
    lv_obj_set_style_pad_row(s_contacts, 2, 0);

    /* Tab bar: two equal buttons. */
    lv_obj_t* bar = lv_obj_create(s_contacts);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_tabBtnC = mkTabButton(bar, "Contacts",    0);
    s_tabBtnM = mkTabButton(bar, "On the Mesh", 1);

    s_listC = buildTabPage(s_tabContacts, s_searchC, "Search contacts or 32-hex");
    s_listM = buildTabPage(s_tabMesh,     s_searchM, "Search the mesh");

    setActiveTab(g_activeTab);   /* Contacts by default; also styles the tab bar */
}

/* ---- identity picker ---- */

void onIdPick(lv_event_t* e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    selectId(n);
    showContacts();
    maybeOpenPending();       /* resume a nomad-tapped open under the chosen identity */
}

void showIdPicker(const std::vector<int>& ids) {
    if (!s_idpick) {
        s_idpick = lv_obj_create(s_layer);
        lv_obj_remove_style_all(s_idpick);
        lv_obj_set_size(s_idpick, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_idpick, lv_color_hex(0x10141a), 0);
        lv_obj_set_style_bg_opa(s_idpick, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(s_idpick, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(s_idpick, 6, 0);
        lv_obj_set_style_pad_ver(s_idpick, 4, 0);
        lv_obj_set_style_pad_row(s_idpick, 4, 0);
    }
    lv_obj_clean(s_idpick);
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    if (s_thread)   lv_obj_add_flag(s_thread,   LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(false);

    mkLabel(s_idpick, "Select identity", lv_color_hex(0x8a93a0));

    lv_obj_t* first = nullptr;
    for (int n : ids) {
        lv_obj_t* b = lv_button_create(s_idpick);
        lv_obj_remove_style_all(b);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(b, 1, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, onIdPick, LV_EVENT_CLICKED, (void*)(intptr_t)n);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
        if (!first) first = b;

        mkLabel(b, idLabel(n), lv_color_white());
        char dk[40];
        snprintf(dk, sizeof dk, "lxmf.id.%d.dest_hash", n);
        std::string dh = storageGetStr(dk, "");
        mkLabel(b, dh.empty() ? "(announcing…)" : (dh.substr(0, 16) + "…"), lv_color_hex(0x8a93a0));
    }
    lv_obj_remove_flag(s_idpick, LV_OBJ_FLAG_HIDDEN);
    deferFocus(first);
}

/* Route to the right first screen for the current identity set: picker (>1),
 * straight into the list (exactly 1), or the list's guidance (none). Routes on
 * *loaded* identities (dest hash known), not connected ones, so stored history
 * shows the moment the identity loads — seconds before its mailbox comes up.
 * Called on open and whenever the identity set changes while still unpicked. */
void routeByIdentity() {
    std::vector<int> ids;
    loadedIds(ids);
    if (ids.size() > 1) {
        showIdPicker(ids);
    } else if (ids.size() == 1) {
        selectId(ids[0]);
        showContacts();
    } else {
        g_id = -1;
        g_msgsPrefix.clear();
        g_msgs.clear();
        showContacts();
    }
}

/* ---- live updates: storage change -> refresh whatever's shown ---- */

/* Coalesce list rebuilds: the on-the-mesh announce stream can fire many times a
 * second, and each rebuild walks the announce catalogue + clears the list (which
 * resets the scroll). Schedule at most one rebuild per window; rebuildList keeps
 * the scroll position across it so a live update never snaps you to the top. */
void listRefreshTimerCb(lv_timer_t*) {
    g_listRefreshPending = false;
    if (s_contacts && !lv_obj_has_flag(s_contacts, LV_OBJ_FLAG_HIDDEN)) {
        refreshAnnounces();
        rebuildList(true);
    }
}
void scheduleListRefresh() {
    if (g_listRefreshPending) return;
    g_listRefreshPending = true;
    lv_timer_t* t = lv_timer_create(listRefreshTimerCb, 700, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void onStorageChange(const char* key, const char*) {
    if (!s_layer) return;
    /* Not yet committed to a slot: re-route only when the identity set itself
     * may have changed (ignore the announce firehose so the picker doesn't
     * thrash / steal focus on every heard announce). */
    if (g_id < 0) {
        if (key && (strncmp(key, "lxmf.id", 7) == 0 || strncmp(key, "s.lxmf.id", 9) == 0))
            routeByIdentity();
        return;
    }
    /* Announce-only churn touches neither messages nor the open thread — only the
     * on-the-mesh column. Skip the msg walk + thread rebuild, and always route the
     * list through the coalescing scheduler so the firehose can't stall it. */
    bool announceOnly = key && strncmp(key, "lxmf.announces", 14) == 0;
    if (!announceOnly) refreshMsgs();

    if (s_thread && !lv_obj_has_flag(s_thread, LV_OBJ_FLAG_HIDDEN)) {
        if (!announceOnly) rebuildThread();
        composeReflectUp();        /* enable/label compose the instant `up` flips */
    } else if (s_contacts && !lv_obj_has_flag(s_contacts, LV_OBJ_FLAG_HIDDEN)) {
        scheduleListRefresh();
    }
}

/* Layer eviction frees every widget; null our handles so a storage change
 * arriving after close (the subscription outlives the layer) early-returns
 * instead of touching freed objects. The next open rebuilds from scratch. */
void onLayerDelete(lv_event_t*) {
    /* App-layer objects only — s_newIdTa / s_importTa live in the Settings
     * program's layer and null themselves via their own DELETE handlers. */
    s_layer = nullptr; s_idpick = nullptr; s_contacts = nullptr;
    s_tabBtnC = nullptr; s_tabBtnM = nullptr; s_tabContacts = nullptr; s_tabMesh = nullptr;
    s_searchC = nullptr; s_searchM = nullptr; s_listC = nullptr; s_listM = nullptr;
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr;
    s_compose = nullptr; s_threadName = nullptr; g_focusTarget = nullptr;
    g_listRefreshPending = false;
    /* A queued search rebuild would touch freed widgets — drop it. */
    if (g_searchTimer) { lv_timer_delete(g_searchTimer); g_searchTimer = nullptr; }
}

/* ---- entry point (lcd task, on first open of a fresh layer) ---- */

void lxmfApp(void* arg) {
    refreshFont();   /* vector UI font live before any label / printable() */
    s_layer = static_cast<lv_obj_t*>(arg);
    s_idpick = nullptr; s_contacts = nullptr;
    s_tabBtnC = nullptr; s_tabBtnM = nullptr; s_tabContacts = nullptr; s_tabMesh = nullptr;
    s_searchC = nullptr; s_searchM = nullptr; s_listC = nullptr; s_listM = nullptr;
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr; s_compose = nullptr; s_threadName = nullptr;
    g_curPeer.clear(); g_qContacts.clear(); g_qMesh.clear();
    g_activeTab = 0;   /* Contacts tab selected by default on each fresh open */
    g_id = -1; g_msgsPrefix.clear(); g_msgs.clear();
    g_listRefreshPending = false;
    g_searchTimer = nullptr;   /* prior layer's onLayerDelete already freed it */

    lv_obj_add_event_cb(s_layer, onLayerDelete, LV_EVENT_DELETE, nullptr);

    buildContactsScreen();      /* built once; hidden/shown by the router */
    routeByIdentity();          /* picker (>1) else straight into the list */

    if (!g_subscribed) {
        storageSubscribeChanges("s.lxmf.id",      onStorageChange);   /* msgs + contacts */
        storageSubscribeChanges("lxmf.id",        onStorageChange);   /* identity up/dest edge */
        storageSubscribeChanges("lxmf.announces", onStorageChange);   /* on-the-mesh column */
        g_subscribed = true;
    }
}

/* ---- Settings → Reticulum → LXMF (admin pane, mirrors the web LxmfPanel) ---- */

void lxmfSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "LXMF");
    lcdSettingSlider (p, "Re-announce interval (s)", "s.lxmf.announce_interval_s", 0, 21600);
    lcdSettingCaption(p, "0 = announce on demand only.");
    lcdSettingSlider (p, "Announce catalogue cap", "s.lxmf.max_announces", 256, 8192);
    lcdSettingSlider (p, "On-the-mesh horizon (s)", "s.lxmf.on_mesh_expire", 0, 86400);
    lcdSettingCaption(p, "Hide mesh peers unheard for longer; 0 = show all.");
    lcdSettingSlider (p, "Advertised stamp cost", "s.lxmf.stamp_cost", 0, 18);
    lcdSettingCaption(p, "PoW cost (bits) senders pay; 0 = none.");
    lcdSettingSwitch (p, "Generate stamps", "s.lxmf.generate_stamps");
    lcdSettingCaption(p, "Pay a peer's advertised PoW cost when sending.");
    lcdSettingSwitch (p, "Require stamps", "s.lxmf.enforce_stamps");
    lcdSettingCaption(p, "Drop inbound messages without a valid stamp.");

    lcdSettingSection(p, "Identities");
    /* One block per existing slot; literal dest/enabled keys (the helpers keep
     * the key by pointer), the slot index drives name/status/Destroy. */
    if (storageExists("s.lxmf.id.0.label")) lxmfIdentityBlock(p, 0, "lxmf.id.0.dest_hash", "s.lxmf.id.0.enabled");
    if (storageExists("s.lxmf.id.1.label")) lxmfIdentityBlock(p, 1, "lxmf.id.1.dest_hash", "s.lxmf.id.1.enabled");
    if (storageExists("s.lxmf.id.2.label")) lxmfIdentityBlock(p, 2, "lxmf.id.2.dest_hash", "s.lxmf.id.2.enabled");
    if (storageExists("s.lxmf.id.3.label")) lxmfIdentityBlock(p, 3, "lxmf.id.3.dest_hash", "s.lxmf.id.3.enabled");

    lcdSettingSection(p, "Add identity");
    if (lcdHasKeyboard()) {
        /* Hardware keyboard: a name field + an explicit Create button. Enter in
         * the field commits too, but pressing it is non-obvious, so the button
         * is the discoverable affordance. */
        lv_obj_t* row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_newIdTa = lv_textarea_create(row);
        lv_textarea_set_one_line(s_newIdTa, true);
        lv_textarea_set_placeholder_text(s_newIdTa, "Name");
        lv_obj_set_style_text_font(s_newIdTa, kFont, 0);
        lv_obj_set_flex_grow(s_newIdTa, 1);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_newIdTa);
        lv_obj_add_event_cb(s_newIdTa, onAddIdentity, LV_EVENT_READY, nullptr);   /* Enter commits */
        lv_obj_add_event_cb(s_newIdTa, [](lv_event_t*){ s_newIdTa = nullptr; },   /* avoid a dangle on rebuild */
                            LV_EVENT_DELETE, nullptr);

        lv_obj_t* add = lv_button_create(row);
        lv_obj_set_style_pad_ver(add, 2, 0);
        lv_obj_set_style_pad_hor(add, 8, 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), add);
        lv_obj_t* al = lv_label_create(add);
        lv_obj_set_style_text_font(al, kFont, 0);
        lv_label_set_text(al, "Create");
        lv_obj_center(al);
        lv_obj_add_event_cb(add, onAddIdentity, LV_EVENT_CLICKED, nullptr);

        /* Second row: a 128-hex private key field + an Import button. */
        lv_obj_t* irow = lv_obj_create(p);
        lv_obj_remove_style_all(irow);
        lv_obj_set_width(irow, lv_pct(100));
        lv_obj_set_height(irow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(irow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(irow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(irow, 6, 0);
        lv_obj_remove_flag(irow, LV_OBJ_FLAG_SCROLLABLE);

        s_importTa = lv_textarea_create(irow);
        lv_textarea_set_one_line(s_importTa, true);
        lv_textarea_set_placeholder_text(s_importTa, "128-hex private key");
        lv_obj_set_style_text_font(s_importTa, kFont, 0);
        lv_obj_set_flex_grow(s_importTa, 1);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_importTa);
        lv_obj_add_event_cb(s_importTa, onImportIdentity, LV_EVENT_READY, nullptr);   /* Enter commits */
        lv_obj_add_event_cb(s_importTa, [](lv_event_t*){ s_importTa = nullptr; },     /* avoid a dangle on rebuild */
                            LV_EVENT_DELETE, nullptr);

        lv_obj_t* imp = lv_button_create(irow);
        lv_obj_set_style_pad_ver(imp, 2, 0);
        lv_obj_set_style_pad_hor(imp, 8, 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), imp);
        lv_obj_t* il = lv_label_create(imp);
        lv_obj_set_style_text_font(il, kFont, 0);
        lv_label_set_text(il, "Import");
        lv_obj_center(il);
        lv_obj_add_event_cb(imp, onImportIdentity, LV_EVENT_CLICKED, nullptr);
    } else {
        /* Touch-only: tapping a field opens the full-screen on-screen keyboard,
         * which carries its own OK (commit) button. */
        lcdSettingText(p, "New (name)",           "lxmf.cmd.identity_new");
        lcdSettingText(p, "Import (128-hex key)", "lxmf.cmd.identity_import");
    }
}

}  // namespace

/* LxmfApp — the LXMessenger launcher program as an LcdApp (and thus a Service).
 * onCreate builds the three screens; cleanup on eviction is handled by the
 * layer's own onLayerDelete (it nulls every handle so a late storage change
 * early-returns), so no onClose is needed. Declared in lxmf_app.h (global, so
 * the generated services: trampoline can `new` it); defined here where the
 * file-static UI state lives. */
LxmfApp::LxmfApp() : LcdApp({ .name = "LXMF", .iconBasename = "lxmf" }) {}

void LxmfApp::onCreate(lv_obj_t* root) { lxmfApp(root); }

/* LxmfApp::appInit — the boot-task half of bring-up, run once by LcdApp::onInit()
 * right after it hops the launcher-tile install onto the lcd task. This whole
 * file lives under conditional/spangap-lcd/, compiled only when the lcd straddle
 * is staged, so no #if is needed — no lcd, no LxmfApp, no services: registration. */
void LxmfApp::appInit() {
    lcdRegisterSettings("Mesh Network/LXMF", "LXMF Messages", lxmfSettingsPane, 2);

    /* The on-device nomad browser writes a tapped contact's dest hash to
     * lxmf.url_lcd. Subscribe ON the lcd task (via lcdRun) so the callback is
     * delivered there and may touch LVGL directly — and register here at init,
     * not in lxmfApp, so the trigger works even if LXMF was never opened. */
    lcdRun([](void*) { storageSubscribeChanges("lxmf.url_lcd", onLcdOpenUrl); });
}
