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
 *   - List: a search box on top (focused on entry so a hardware keyboard types
 *     straight into it) over two sections, mirroring the web rail —
 *       Contacts:    peers you've messaged, name + last-message preview.
 *       On the Mesh:  announced peers you haven't talked to yet. The announce
 *                     catalogue can hold thousands, so the column is sorted by
 *                     recency and capped (MESH_ROW_CAP); a footer notes the
 *                     remainder and the search box narrows it. A bare 32-hex
 *                     query offers "message this address".
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

/* Compact + accented: smaller than the 14 default, and (unlike LVGL's stock
 * montserrat) carries umlauts/accents so message text isn't reduced to boxes. */
const lv_font_t* const kFont = &lv_font_montserrat_12_latin;

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
std::string       g_query;              /* current search-box text */
bool              g_subscribed = false;
bool              g_listRefreshPending = false;   /* a coalesced list rebuild is queued */
lv_timer_t*       g_searchTimer = nullptr;        /* one-shot search debounce (null = idle) */

lv_obj_t* s_layer    = nullptr;
lv_obj_t* s_idpick   = nullptr;         /* identity picker (first screen, >1 ident) */
lv_obj_t* s_contacts = nullptr;         /* list screen: search box + scroll list */
lv_obj_t* s_search   = nullptr;         /* search textarea atop the list */
lv_obj_t* s_list     = nullptr;         /* scroll container of rows (cleaned+rebuilt) */
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

/* Usable = brought up by the firmware AND announcing a dest (mirrors the web's
 * usableIdentities). A config-only slot can't send/receive, so it's excluded. */
void usableIds(std::vector<int>& out) {
    out.clear();
    for (int n = 0; n < 4; n++) {
        char k[40];
        snprintf(k, sizeof k, "lxmf.id.%d.up", n);
        if (storageGetInt(k, 0) != 1) continue;
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        if (storageGetStr(k, "").empty()) continue;
        out.push_back(n);
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
    if (g_id < 0 || peer.empty() || text.empty()) return;
    static unsigned seq = 0;
    char key[40];
    snprintf(key, sizeof key, "o_%u_%u", (unsigned)millis(), ++seq);
    char base[120];
    snprintf(base, sizeof base, "s.lxmf.id.%d.msgs.%s.%s", g_id, peer.c_str(), key);
    char k[200];
    auto setf = [&](const char* f, const char* v) {
        snprintf(k, sizeof k, "%s.%s", base, f);
        storageSet(k, v);
    };
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
    storageSet(k, (int)time(nullptr));
    /* Send sentinel (ephemeral): "<peer>/<key>". The lxmf task drives the stage. */
    char sentinel[48], payload[80];
    snprintf(sentinel, sizeof sentinel, "lxmf.id.%d.cmd.send", g_id);
    snprintf(payload, sizeof payload, "%s/%s", peer.c_str(), key);
    storageSet(sentinel, payload);
}

void onSend(lv_event_t*) {
    if (!s_compose || g_curPeer.empty()) return;
    const char* t = lv_textarea_get_text(s_compose);
    if (t && *t) {
        sendMessage(g_curPeer, t);
        lv_textarea_set_text(s_compose, "");
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
        deferFocus(s_search);               /* type-to-search on entry */
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

    /* Scroll area (grows to fill): bubble column + the compose row at its end. */
    s_msgList = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_msgList);
    lv_obj_set_width(s_msgList, lv_pct(100));
    lv_obj_set_flex_grow(s_msgList, 1);
    lv_obj_set_flex_flow(s_msgList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_msgList, 4, 0);
    lv_obj_set_style_pad_ver(s_msgList, 1, 0);
    lv_obj_set_style_pad_row(s_msgList, 3, 0);   /* gap between last message and the input */

    s_bubbles = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, lv_pct(100));
    lv_obj_set_height(s_bubbles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bubbles, 1, 0);
    lv_obj_remove_flag(s_bubbles, LV_OBJ_FLAG_SCROLLABLE);

    /* Compose row: textarea + Send. Last child of the scroll area, so it follows
     * the final message and scrolls off the top rather than sticking to the bottom. */
    lv_obj_t* comp = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(comp);
    lv_obj_set_width(comp, lv_pct(100));
    lv_obj_set_height(comp, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(comp, 1, 0);
    lv_obj_set_style_pad_column(comp, 4, 0);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);

    s_compose = lv_textarea_create(comp);
    lv_textarea_set_one_line(s_compose, true);
    lv_textarea_set_placeholder_text(s_compose, "Message");
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_pad_ver(s_compose, 1, 0);
    lv_obj_set_style_pad_hor(s_compose, 4, 0);
    lv_obj_set_flex_grow(s_compose, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);                 /* Enter sends */
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { scrollThreadBottom(false); },   /* typing -> bottom */
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* send = lv_button_create(comp);
    lv_obj_set_style_pad_ver(send, 1, 0);
    lv_obj_set_style_pad_hor(send, 6, 0);
    lv_obj_t* sl = lv_label_create(send);
    lv_obj_set_style_text_font(sl, kFont, 0);
    lv_label_set_text(sl, "Send");
    lv_obj_center(sl);
    lv_obj_add_event_cb(send, onSend, LV_EVENT_CLICKED, nullptr);
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
    deferFocus(s_compose);                  /* type-to-reply on open */
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
    lv_obj_clean(s_bubbles);
    g_nomadTargets.clear();          /* link widgets are gone with the cleaned bubbles */
    std::vector<const Msg*> ms;
    for (auto& m : g_msgs) if (m.peer == g_curPeer) ms.push_back(&m);
    std::sort(ms.begin(), ms.end(), [](const Msg* a, const Msg* b) { return a->ts < b->ts; });
    for (auto* m : ms) addBubble(*m);
    scrollThreadBottom(false);                          /* newest + compose at bottom */
}

/* ---- list rendering (search + Contacts + On the Mesh) ---- */

struct Conv { std::string peer, preview; long ts = 0; int unread = 0; };

/* A two-line tappable row (name + one-line subtitle). Stacking (not inline)
 * keeps a tall, reliable tap target even when the subtitle is short. */
void addPeerRow(const std::string& peer, const std::string& title,
                const std::string& sub, lv_color_t titleColor) {
    size_t idx = g_rowPeers.size();
    g_rowPeers.push_back(peer);

    lv_obj_t* row = lv_button_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 1, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, onContactClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    mkLabel(row, title, titleColor);

    /* LONG_DOT ellipsizes only within the label's own size, so bound both:
     * full row width + exactly one line tall → a long subtitle is clipped to a
     * single "…"-terminated line instead of wrapping. */
    lv_obj_t* pv = mkLabel(row, sub, lv_color_hex(0x8a93a0));
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pv, lv_pct(100));
    lv_obj_set_height(pv, lv_font_get_line_height(kFont));
}

void grpLabel(const char* t) {
    lv_obj_t* l = mkLabel(s_list, t, lv_color_hex(0x6a7280));
    lv_obj_set_style_pad_top(l, 3, 0);
}

void rebuildList(bool keepScroll) {
    if (!s_list) return;
    int32_t savedY = keepScroll ? lv_obj_get_scroll_y(s_list) : 0;
    lv_obj_clean(s_list);
    g_rowPeers.clear();

    if (g_id < 0) {
        mkLabel(s_list, "No active identity.\nCreate one in Settings.", lv_color_hex(0x8a93a0));
        return;
    }

    std::string needle = lower(trim(g_query));

    /* Conversations (Contacts) from the message store. */
    std::vector<Conv> convs;
    for (auto& m : g_msgs) {
        Conv* c = nullptr;
        for (auto& x : convs) if (x.peer == m.peer) { c = &x; break; }
        if (!c) { convs.push_back({ m.peer, "", 0, 0 }); c = &convs.back(); }
        if (m.ts >= c->ts) { c->ts = m.ts; c->preview = (m.in ? "" : "You: ") + m.content; }
        if (m.in && !m.read) c->unread++;
    }
    std::sort(convs.begin(), convs.end(), [](const Conv& a, const Conv& b) { return a.ts > b.ts; });
    std::set<std::string> convSet;
    for (auto& c : convs) convSet.insert(c.peer);

    /* "New": a bare 32-hex query for a peer we don't already list. */
    if (isHex32(needle) && !convSet.count(needle)) {
        bool heard = false;
        for (auto& an : g_anns) if (an.hash == needle) { heard = true; break; }
        if (!heard) {
            grpLabel("New");
            addPeerRow(needle, "Message this address", needle, lv_color_white());
        }
    }

    /* Contacts (filtered). */
    bool anyC = false;
    for (auto& c : convs) {
        std::string nm = peerName(c.peer);
        if (!qmatch(needle, nm, c.peer)) continue;
        if (!anyC) { grpLabel("Contacts"); anyC = true; }
        std::string title = nm;
        if (c.unread > 0) title += " (" + std::to_string(c.unread) + ")";
        addPeerRow(c.peer, title, printable(c.preview, true),
                   c.unread > 0 ? lv_color_hex(0x6cc06c) : lv_color_white());
    }

    /* On the Mesh: announced peers we haven't messaged, filtered + capped. */
    bool anyM = false;
    int shown = 0, total = 0;
    for (auto& an : g_anns) {
        if (convSet.count(an.hash)) continue;
        std::string nm = an.name.empty() ? peerName(an.hash) : an.name;
        if (!qmatch(needle, nm, an.hash)) continue;
        total++;
        if (shown >= MESH_ROW_CAP) continue;
        if (!anyM) { grpLabel("On the Mesh"); anyM = true; }
        addPeerRow(an.hash, nm, an.hash, lv_color_white());
        shown++;
    }
    if (total > shown) {
        char foot[48];
        snprintf(foot, sizeof foot, "+%d more — search to narrow", total - shown);
        grpLabel(foot);
    }

    if (g_rowPeers.empty()) {
        mkLabel(s_list, needle.empty() ? "No conversations yet.\nSearch the mesh above."
                                       : "No matches.", lv_color_hex(0x8a93a0));
    }

    /* Hold the reading position across a live refresh (clamped if the list
     * shrank); a fresh open / new search starts at the top (keepScroll false). */
    if (keepScroll && savedY > 0) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, savedY, LV_ANIM_OFF);
    }
}

/* ---- search box ---- */

void searchTimerCb(lv_timer_t*) {
    g_searchTimer = nullptr;   /* one-shot: LVGL deletes it after this returns */
    rebuildList();
}

void onSearchChanged(lv_event_t*) {
    if (!s_search) return;
    const char* t = lv_textarea_get_text(s_search);
    g_query = t ? t : "";
    /* Postpone the rebuild to SEARCH_DEBOUNCE_MS after the last keystroke so a
     * fast typist isn't fighting a per-key rebuild (see SEARCH_DEBOUNCE_MS). */
    if (g_searchTimer) {
        lv_timer_reset(g_searchTimer);
    } else {
        g_searchTimer = lv_timer_create(searchTimerCb, SEARCH_DEBOUNCE_MS, nullptr);
        lv_timer_set_repeat_count(g_searchTimer, 1);
    }
}

void onSearchEnter(lv_event_t*) {
    /* Enter acts on the current query — flush any pending debounce first so
     * g_rowPeers reflects what's typed, not the last settled rebuild. */
    if (g_searchTimer) {
        lv_timer_delete(g_searchTimer);
        g_searchTimer = nullptr;
        rebuildList();
    }
    std::string needle = lower(trim(g_query));
    if (isHex32(needle)) { openThread(needle); return; }
    if (g_rowPeers.size() == 1) openThread(g_rowPeers[0]);
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

    s_search = lv_textarea_create(s_contacts);
    lv_textarea_set_one_line(s_search, true);
    lv_textarea_set_placeholder_text(s_search, "Search or 32-hex address");
    lv_obj_set_style_text_font(s_search, kFont, 0);
    lv_obj_set_style_pad_ver(s_search, 2, 0);
    lv_obj_set_style_pad_hor(s_search, 6, 0);
    lv_obj_set_width(s_search, lv_pct(100));
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_search);
    lv_obj_add_event_cb(s_search, onSearchChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_search, onSearchEnter,   LV_EVENT_READY,         nullptr);

    s_list = lv_obj_create(s_contacts);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 1, 0);
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
 * straight into the list (exactly 1), or the list's guidance (none). Called on
 * open and whenever the identity set changes while still unpicked. */
void routeByIdentity() {
    std::vector<int> ids;
    usableIds(ids);
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
    s_layer = nullptr; s_idpick = nullptr; s_contacts = nullptr; s_search = nullptr;
    s_list = nullptr; s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr;
    s_compose = nullptr; s_threadName = nullptr; g_focusTarget = nullptr;
    g_listRefreshPending = false;
    /* A queued search rebuild would touch freed widgets — drop it. */
    if (g_searchTimer) { lv_timer_delete(g_searchTimer); g_searchTimer = nullptr; }
}

/* ---- entry point (lcd task, on first open of a fresh layer) ---- */

void lxmfApp(void* arg) {
    s_layer = static_cast<lv_obj_t*>(arg);
    s_idpick = nullptr; s_contacts = nullptr; s_search = nullptr; s_list = nullptr;
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr; s_compose = nullptr; s_threadName = nullptr;
    g_curPeer.clear(); g_query.clear();
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

/* LxmfApp — onCreate builds the screens; cleanup on eviction is handled by the
 * layer's own onLayerDelete (it nulls every handle so a late storage change
 * early-returns), so no onClose is needed. */
class LxmfApp : public LcdApp {
public:
    LxmfApp() : LcdApp({ .name = "LXMF", .iconBasename = "lxmf" }) {}
    void onCreate(lv_obj_t* root) override { lxmfApp(root); }
};

}  // namespace

/* Register the LXMessenger launcher program — a when:-gated init: hook
 * (spangap/spangap-lcd). This whole file lives under conditional/spangap-lcd/,
 * compiled only when the lcd straddle is staged, so no #if is needed. Plain
 * C++ linkage to match the generated dispatcher's forward decl. */
void lxmfLcdRegister(void) {
    lcdRun([](void*) { lcdInstall(new LxmfApp()); });   /* tile build is LVGL: on the lcd task */
    lcdRegisterSettings("Mesh Network/LXMF", "LXMF Messages", lxmfSettingsPane, 2);

    /* The on-device nomad browser writes a tapped contact's dest hash to
     * lxmf.url_lcd. Subscribe ON the lcd task (via lcdRun) so the callback is
     * delivered there and may touch LVGL directly — and register here at init,
     * not in lxmfApp, so the trigger works even if LXMF was never opened. */
    lcdRun([](void*) { storageSubscribeChanges("lxmf.url_lcd", onLcdOpenUrl); });
}
