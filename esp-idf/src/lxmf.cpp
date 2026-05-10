/**
 * lxmf — LXMF messaging protocol task. Stub.
 *
 * Phase 4 work; this is just the scaffold so main.cpp compiles cleanly.
 */
#include "lxmf.h"
#include "diptych.h"
#include "ports.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "lxmf";

static TaskHandle_t s_task = nullptr;

static TickType_t nextDeadline(void)
{
    /* TODO: send-queue retries, propagation-pull timer, inbox prune. */
    return portMAX_DELAY;
}

static void lxmfTaskMain(void*)
{
    info("[%s] task up", TAG);

    /* TODO: itsConnect("rnsd", RNSD_PORT_DEST, dest_hash, ...) once
     *       rnsd is up; subsequent inbound packets arrive on the handle. */

    for (;;) {
        itsPoll(nextDeadline());
        /* TODO: drain dest stream, run send queue, update inbox. */
    }
}

void lxmfInit(void)
{
    /* Core 1, prio 1, 8 KB PSRAM stack per §4. Lighter than rnsd. */
    s_task = spawnTask(lxmfTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
}
