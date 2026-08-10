/**
 * lxmf_tiny.cpp — total-unread LXMF page on the tiny OLED.
 *
 * Storage is lxmf's API surface (see lxmf.h): unread lives per conversation
 * at s.lxmf.id.<n>.contacts.<peer>.unread, maintained by the message paths.
 * The page sums those leaves across all identity slots at draw time — cheap,
 * and only ever runs while the page is on screen. The subscription turns any
 * contact-record change into a redraw request; tinylcd drops it unless this
 * page is showing.
 */
#include "lxmf_tiny.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage.h"
#include "tinylcd.h"

static tinylcd_page_t s_page = TINYLCD_NO_PAGE;

/* Accumulator for the storageForEach walk (single-threaded: draw runs on the
 * tinylcd task only). */
static int s_unread;

static void sumUnread(const char* key, const char* val)
{
    size_t len = strlen(key);
    if (len > 7 && !strcmp(key + len - 7, ".unread")) s_unread += atoi(val);
}

static bool drawLxmfPage(tinylcd_page_t, u8g2_t* g, tinylcd_ev_t)
{
    s_unread = 0;
    /* Slot bound mirrors lxmf's private LXMF_MAX_IDENTITIES (4), the same way
     * the LCD app's identity walk does — there is no exported count. */
    for (int n = 0; n < 4; n++) {
        char prefix[40];
        snprintf(prefix, sizeof prefix, "s.lxmf.id.%d.contacts", n);
        storageForEach(prefix, sumUnread);
    }

    u8g2_SetFont(g, u8g2_font_6x10_tf);
    u8g2_DrawStr(g, 0, 17, "LXMF unread");

    char num[8];
    int n = s_unread;
    if (n < 0) n = 0;
    if (n > 999) n = 999;   /* logisoso24 fits three digits beside the label */
    snprintf(num, sizeof num, "%d", n);
    u8g2_SetFont(g, u8g2_font_logisoso24_tn);
    u8g2_uint_t w = u8g2_GetStrWidth(g, num);
    u8g2_DrawStr(g, (128 - w) / 2, 50, num);
    return false;   /* draw-only page: no events handled */
}

void LxmfTinyPage::onInit()
{
    s_page = tinylcdAddPage("lxmf", drawLxmfPage);
    tinylcdRun(ON_TINYLCD {
        /* On the tinylcd task. The whole id subtree: unread bumps arrive as
         * .unread / .read_ts writes inside the contact records. */
        storageSubscribeChanges("s.lxmf.id", ON_CHANGE { tinylcdDraw(s_page); });
    });
}
