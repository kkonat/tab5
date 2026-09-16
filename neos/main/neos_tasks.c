/*
 * Tasks, and the two things worth doing to one: looking at it, and ending it.
 *
 * FreeRTOS will describe every task it is running, but not in a form anything
 * outside the kernel should hold on to - what it hands over is a snapshot of
 * TCB pointers, a run-time counter that wraps, and a stack figure in whatever
 * unit the port happens to use. This file turns that into something a page of
 * rows can be drawn from twice a second: stable ids, a CPU share measured over
 * a window rather than since boot, and an honest answer to "may this one be
 * killed".
 *
 * Services live here too, and the comment in neos_tasks.h says why.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "neos_api.h"
#include "neos_audio.h"
#include "neos_tasks.h"

static const char *TAG = "tasks";

/*
 * How long a CPU figure is measured over.
 *
 * The counter FreeRTOS keeps is the microseconds a task has had since boot,
 * which as a percentage is the answer to a question nobody asked - a player
 * that has been running for an hour and a shell that started a second ago are
 * not comparable that way. What is wanted is "what is it doing now", so the
 * counters are differenced against a snapshot about a second old.
 *
 * A second rather than a tick: the page ticks at 150 ms, and a share measured
 * over 150 ms of a system with a 1 kHz scheduler is mostly quantisation noise.
 * Every call inside the window reports the window's figure, so polling this
 * faster costs nothing and changes nothing.
 */
#define WINDOW_US 1000000

/** Stack and priority a service gets when it does not ask for its own. */
#define SVC_STACK_DEFAULT 4096
/*
 * Above the app, below input.
 *
 * An app runs on the boot task at priority 1 and some apps busy-poll, so a
 * service at or below that would be starved by exactly the app that started it.
 * The other side of it is that the glass must stay ahead of the sound: touch is
 * 5 and the panel runner is 4, so 3 is the one slot that is above everything an
 * app can be and below everything a finger goes through.
 */
#define SVC_PRIO 3

/** How long a service is given to notice it has been asked to stop. */
#define SVC_STOP_MS 500

/* ------------------------------------------------------------------ */
/* Ids                                                                 */
/* ------------------------------------------------------------------ */

/*
 * A task handle is the address of its control block, and that address comes
 * back: a task that exits frees the block, and the next task to start may well
 * be handed the same memory. A list drawn a second ago would then have a row
 * whose handle points at a task nobody was looking at, and a kill button on
 * that row would end it.
 *
 * So nothing outside this file ever sees a handle. Ids come from a counter that
 * only goes up, and the table below remembers which handle each one was for,
 * along with the name and the kernel's own task number so that a handle which
 * has been recycled can be told from one that has not.
 */
#define ID_MAX 64

typedef struct {
    bool         used;
    TaskHandle_t handle;
    uint32_t     id;
    uint32_t     tasknum;        /* the kernel's uxTCBNumber: creation order */
    char         name[NEOS_TASK_NAME];
    uint32_t     rt_mark;        /* run time at the last window boundary */
    uint16_t     permille;       /* what that window worked out to */
    bool         seen;           /* in the enumeration going on right now */
} entry_t;

static entry_t  s_ent[ID_MAX];
static uint32_t s_next_id = 1;
static int64_t  s_window_t0;

/*
 * One lock over the id table and the service registry.
 *
 * They are read from the app's task, written from the boot task when an app
 * exits, and written from a service's own task as it retires - three tasks, one
 * table. A mutex rather than a spinlock because the slow paths here take
 * milliseconds (a task being deleted, an ELF image being freed) and blocking is
 * the right thing to do about that.
 */
static SemaphoreHandle_t s_lock;

static void lock_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/*
 * Waits as long as it takes, and every caller checks the answer anyway.
 *
 * No path below holds this across anything slow - the one that waits half a
 * second for a service to notice it has been asked to stop gives the mutex back
 * first, precisely so that the service can take it to retire itself. With no
 * timeout there is no "I did not get the lock so the table is now half updated"
 * case to reason about, which for a table that decides when an ELF image is
 * freed is worth more than a bounded wait.
 *
 * False only when there is no mutex at all, which is a machine that ran out of
 * memory before the first app started.
 */
static bool lock(void)
{
    lock_init();
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Which tasks are NeOS's                                              */
/* ------------------------------------------------------------------ */

/*
 * The tasks that may not be killed, by name prefix.
 *
 * Three groups, and every one of them is fatal rather than inconvenient: the
 * kernel's own (the idle tasks hold the delete queue, the timer task runs half
 * the system's callbacks, ipc is how the other core is asked for anything),
 * NeOS's own (no touch is no input, no ui is no keyboard and no way out of a
 * panel), and the Wi-Fi transport, which is a chain of tasks over SDIO to a
 * second chip and comes apart if any link in it goes.
 *
 * Prefixes rather than exact names because several of these are numbered per
 * core, and because the transport's are a family with one job. Anything not
 * matched here is killable, which is the point of the list being this short: a
 * component that spawned a worker nobody thought about is exactly what somebody
 * would be looking at the page to find.
 */
static const char *const PROTECTED[] = {
    /* the kernel and the IDF */
    "IDLE", "ipc", "esp_timer", "main", "tiT", "sys_evt", "Tmr Svc",
    /* NeOS */
    "touch", "ui", "status", "net", "orient", "upload", "weather",
    "cardwatch", "click",
    /* the radio, its transport, and the RPC over it */
    "wifi", "sdio", "spi_", "rpc", "recv_task", "send_task", "flow_ctrl",
    "host_reset", "power_save", "ps_alert", "pserial", "hci", "btC",
};

static bool name_protected(const char *name)
{
    for (size_t i = 0; i < sizeof(PROTECTED) / sizeof(PROTECTED[0]); i++) {
        const size_t n = strlen(PROTECTED[i]);
        if (strncmp(name, PROTECTED[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Services                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    bool            used;
    TaskHandle_t    handle;
    uint32_t        id;
    char            name[NEOS_TASK_NAME];
    volatile bool   stopping;
    int             image;          /* keeper slot, or -1 while the app is resident */
    neos_service_fn fn;
    void           *arg;
} svc_t;

static svc_t s_svc[NEOS_SERVICES_MAX];

/*
 * The images services are executing out of.
 *
 * One slot per app that left something behind, with a count of how many of its
 * services are still running. The struct is a copy of the loader's own handle
 * to the image - see neos_service_adopt_image() - and the only thing done with
 * it is esp_elf_deinit(), once, when the count reaches zero.
 */
typedef struct {
    bool      used;
    esp_elf_t elf;
    int       refs;
} image_t;

static image_t s_img[NEOS_SERVICES_MAX];

static int svc_live_locked(void)
{
    int n = 0;
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (s_svc[i].used) {
            n++;
        }
    }
    return n;
}

int neos_service_count(void)
{
    if (!lock()) {
        return 0;
    }
    const int n = svc_live_locked();
    unlock();
    return n;
}

static svc_t *svc_by_handle_locked(TaskHandle_t h)
{
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (s_svc[i].used && s_svc[i].handle == h) {
            return &s_svc[i];
        }
    }
    return NULL;
}

bool neos_service_stopping(void)
{
    /*
     * No lock. This is polled from inside a service's own loop, possibly every
     * few milliseconds, and the worst a race can do is answer with the value
     * from a moment ago - which is the same answer the next poll gives. Taking
     * a mutex on the audio path to read one bool would be the more expensive
     * mistake.
     */
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (s_svc[i].used && s_svc[i].handle == me) {
            return s_svc[i].stopping;
        }
    }
    return false;
}

/*
 * Give back everything one service was holding. Exactly once per service,
 * whether it returned on its own or was deleted from under itself.
 *
 * The image is the part that matters. A service is app code, so the last one
 * out of an image is what makes those pages free to give back - and the count
 * has to be decremented here rather than in the trampoline, because a service
 * that was deleted outright never reaches its trampoline's last line.
 */
static void svc_retire_locked(svc_t *s)
{
    if (!s->used) {
        return;
    }
    const int img = s->image;
    ESP_LOGI(TAG, "service \"%s\" gone", s->name);
    memset(s, 0, sizeof(*s));

    if (img >= 0 && img < NEOS_SERVICES_MAX && s_img[img].used) {
        if (--s_img[img].refs <= 0) {
            ESP_LOGI(TAG, "last service out of image %d - freeing it", img);
            esp_elf_deinit(&s_img[img].elf);
            memset(&s_img[img], 0, sizeof(s_img[img]));
        }
    }

    /*
     * With nothing left running, whatever the last app took and its services
     * kept has to go back. The speaker is the one that matters: the boot chain
     * skips its usual release while services live, precisely so that a player
     * keeps playing, which makes this the other end of that decision.
     */
    if (svc_live_locked() == 0) {
        neos_audio_app_release();
    }
}

static void svc_trampoline(void *arg)
{
    svc_t *s = (svc_t *)arg;

    /*
     * Wait to be told which task we are.
     *
     * A service runs at priority 3 and is started from an app at priority 1, so
     * xTaskCreate() hands control straight to it and this body can be several
     * lines in before the starter gets to write down the handle. Everything that
     * finds a service by handle - the stop flag, the kill button, the row in the
     * list - would miss it until then, and a service that finished inside that
     * window would retire a slot the starter was still filling in.
     */
    while (!s->handle) {
        vTaskDelay(1);
    }

    s->fn(s->arg);

    if (lock()) {
        svc_retire_locked(s);
        unlock();
    }
    vTaskDelete(NULL);
}

uint32_t neos_service_start(const char *name, neos_service_fn fn, void *arg,
                            uint32_t stack)
{
    if (!fn || !name || !name[0]) {
        return 0;
    }
    if (stack == 0) {
        stack = SVC_STACK_DEFAULT;
    }
    if (!lock()) {
        return 0;
    }

    svc_t *s = NULL;
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (!s_svc[i].used) {
            s = &s_svc[i];
            break;
        }
    }
    if (!s) {
        unlock();
        ESP_LOGW(TAG, "no free service slot for \"%s\"", name);
        return 0;
    }

    memset(s, 0, sizeof(*s));
    s->used  = true;          /* before the task exists: the trampoline reads it */
    s->image = -1;
    s->fn    = fn;
    s->arg   = arg;
    strlcpy(s->name, name, sizeof(s->name));

    TaskHandle_t h = NULL;
    if (xTaskCreate(svc_trampoline, s->name, stack, s, SVC_PRIO, &h) != pdPASS) {
        memset(s, 0, sizeof(*s));
        unlock();
        ESP_LOGE(TAG, "could not start service \"%s\" (%u B stack)",
                 name, (unsigned)stack);
        return 0;
    }
    s->handle = h;

    /*
     * An id now rather than at the next enumeration, because the app is told one
     * and may well stop the service before anything has drawn a list.
     */
    entry_t *e = NULL;
    for (int i = 0; i < ID_MAX && !e; i++) {
        if (!s_ent[i].used) {
            e = &s_ent[i];
        }
    }
    if (e) {
        memset(e, 0, sizeof(*e));
        e->used   = true;
        e->handle = h;
        e->id     = s_next_id++;
        strlcpy(e->name, s->name, sizeof(e->name));
        s->id = e->id;
    }

    ESP_LOGI(TAG, "service \"%s\" up, id %u, %u B stack",
             s->name, (unsigned)s->id, (unsigned)stack);
    const uint32_t id = s->id;
    unlock();
    return id;
}

bool neos_service_adopt_image(esp_elf_t *elf)
{
    if (!elf || !lock()) {
        return false;
    }

    int refs = 0;
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (s_svc[i].used && s_svc[i].image < 0) {
            refs++;
        }
    }
    if (refs == 0) {
        unlock();
        return false;           /* nothing to keep it for */
    }

    int slot = -1;
    for (int i = 0; i < NEOS_SERVICES_MAX && slot < 0; i++) {
        if (!s_img[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        unlock();
        ESP_LOGE(TAG, "no free image slot - %d service(s) cannot be kept", refs);
        return false;
    }

    s_img[slot].used = true;
    s_img[slot].elf  = *elf;
    s_img[slot].refs = refs;
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (s_svc[i].used && s_svc[i].image < 0) {
            s_svc[i].image = slot;
        }
    }

    /*
     * Zeroed, not just copied. The caller goes on to call esp_elf_deinit() on
     * its own struct as it does after every app, and that call has to become a
     * no-op instead of freeing the text these services are running out of.
     */
    memset(elf, 0, sizeof(*elf));

    ESP_LOGI(TAG, "image kept in slot %d for %d service(s)", slot, refs);
    unlock();
    return true;
}

/* ------------------------------------------------------------------ */
/* The list                                                            */
/* ------------------------------------------------------------------ */

/*
 * uxTaskGetSystemState() wants somewhere to put a snapshot of every task, and
 * it is called from a page that ticks, so the buffer is allocated once and
 * kept. PSRAM: it is two kilobytes that are touched a few times a second, and
 * internal RAM on this board is the scarce pool - see neos_main.c.
 */
static TaskStatus_t *s_snap;

/*
 * And a second one for the kill path, which checks a handle is still live
 * without holding this file's mutex - see still_live(). Sharing one buffer
 * would mean two tasks filling it at once, which is the one way a snapshot can
 * lie about what is running.
 */
static TaskStatus_t *s_snap_kill;

static TaskStatus_t *snap_alloc(TaskStatus_t **slot)
{
    if (!*slot) {
        *slot = heap_caps_malloc(NEOS_TASKS_MAX * sizeof(TaskStatus_t),
                                 MALLOC_CAP_SPIRAM);
    }
    return *slot;
}

static uint8_t state_of(eTaskState s)
{
    switch (s) {
    case eRunning:   return NEOS_TASK_RUNNING;
    case eReady:     return NEOS_TASK_READY;
    case eBlocked:   return NEOS_TASK_BLOCKED;
    case eSuspended: return NEOS_TASK_SUSPENDED;
    case eDeleted:   return NEOS_TASK_DELETED;
    default:         return NEOS_TASK_UNKNOWN;
    }
}

const char *neos_task_state_name(uint8_t state)
{
    switch (state) {
    case NEOS_TASK_RUNNING:   return "running";
    case NEOS_TASK_READY:     return "ready";
    case NEOS_TASK_BLOCKED:   return "blocked";
    case NEOS_TASK_SUSPENDED: return "suspended";
    case NEOS_TASK_DELETED:   return "deleted";
    default:                  return "?";
    }
}

/**
 * The id for a task, minting one if this is the first time it has been seen.
 *
 * A handle that has come back on a different task - same address, different
 * name or a different kernel task number - is a new task and gets a new id,
 * which is what stops a stale row killing an innocent bystander.
 */
static entry_t *entry_for_locked(const TaskStatus_t *st)
{
    entry_t *free_slot = NULL;

    for (int i = 0; i < ID_MAX; i++) {
        entry_t *e = &s_ent[i];
        if (!e->used) {
            if (!free_slot) {
                free_slot = e;
            }
            continue;
        }
        if (e->handle != st->xHandle) {
            continue;
        }
        const bool same = strncmp(e->name, st->pcTaskName, NEOS_TASK_NAME - 1) == 0
                          && (e->tasknum == 0 || e->tasknum == (uint32_t)st->xTaskNumber);
        if (same) {
            e->tasknum = (uint32_t)st->xTaskNumber;
            return e;
        }
        /* The address is being reused by something else. Retire the old row. */
        memset(e, 0, sizeof(*e));
        if (!free_slot) {
            free_slot = e;
        }
    }

    if (!free_slot) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used    = true;
    free_slot->handle  = st->xHandle;
    free_slot->id      = s_next_id++;
    free_slot->tasknum = (uint32_t)st->xTaskNumber;
    strlcpy(free_slot->name, st->pcTaskName, sizeof(free_slot->name));
    return free_slot;
}

int neos_tasks(neos_task_t *out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    TaskStatus_t *snap = snap_alloc(&s_snap);
    if (!snap || !lock()) {
        return 0;
    }

    const UBaseType_t n = uxTaskGetSystemState(snap, NEOS_TASKS_MAX, NULL);
    if (n == 0) {
        /*
         * Not "no tasks" - that is impossible, this call is running on one. The
         * kernel fills nothing at all rather than truncating when the array is
         * too small for the system, so the only way to read a zero here is
         * NEOS_TASKS_MAX being too low. Said out loud because the symptom is an
         * empty page, which looks like a broken page.
         */
        ESP_LOGW(TAG, "%u tasks will not fit in %d - raise NEOS_TASKS_MAX",
                 (unsigned)uxTaskGetNumberOfTasks(), NEOS_TASKS_MAX);
        unlock();
        return 0;
    }

    /*
     * Creation order, which is the kernel's own task number.
     *
     * The order uxTaskGetSystemState() reports in is the order of its internal
     * ready and blocked lists, which is to say it changes as tasks run. A list
     * with a kill button on every row cannot be drawn from that: the row under
     * a finger would be a different task by the time the finger came down.
     * Insertion sort because n is thirty-odd and already nearly sorted.
     */
    for (UBaseType_t i = 1; i < n; i++) {
        const TaskStatus_t t = snap[i];
        UBaseType_t j = i;
        while (j > 0 && snap[j - 1].xTaskNumber > t.xTaskNumber) {
            snap[j] = snap[j - 1];
            j--;
        }
        snap[j] = t;
    }

    /* One boundary per window, and every caller inside it sees the same
       figures - see WINDOW_US. */
    const int64_t now = esp_timer_get_time();
    const bool    boundary = (s_window_t0 == 0) || (now - s_window_t0 >= WINDOW_US);
    const uint32_t elapsed = boundary && s_window_t0
                           ? (uint32_t)(now - s_window_t0) : 0;

    for (int i = 0; i < ID_MAX; i++) {
        s_ent[i].seen = false;
    }

    int wrote = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t *st = &snap[i];
        entry_t *e = entry_for_locked(st);
        if (!e) {
            continue;           /* more tasks than the id table holds */
        }
        e->seen = true;

        if (boundary) {
            /*
             * Unsigned arithmetic on purpose: the counter is 32-bit
             * microseconds and wraps every 71 minutes, and the difference is
             * still right across the wrap as long as the window is shorter
             * than that. It is one second.
             */
            const uint32_t rt = (uint32_t)st->ulRunTimeCounter;
            if (elapsed > 0) {
                const uint32_t used = rt - e->rt_mark;
                uint32_t pm = (uint32_t)(((uint64_t)used * 1000u) / elapsed);
                if (pm > 1000) {
                    pm = 1000;      /* two cores, rounding, a task that moved */
                }
                e->permille = (uint16_t)pm;
            }
            e->rt_mark = rt;
        }

        if (wrote >= max) {
            continue;           /* keep accounting for the rest, just do not write it */
        }

        neos_task_t *o = &out[wrote++];
        memset(o, 0, sizeof(*o));
        o->id = e->id;
        strlcpy(o->name, st->pcTaskName, sizeof(o->name));
        o->state      = state_of(st->eCurrentState);
        o->prio       = (uint8_t)st->uxCurrentPriority;
        o->stack_free = (uint32_t)st->usStackHighWaterMark;   /* bytes: see below */
        o->cpu_permille = e->permille;

        const BaseType_t core = xTaskGetCoreID(st->xHandle);
        o->core = (core == tskNO_AFFINITY) ? -1 : (int8_t)core;

        if (name_protected(o->name)) {
            o->flags |= NEOS_TASK_PROTECTED;
        }
        if (svc_by_handle_locked(st->xHandle)) {
            o->flags |= NEOS_TASK_SERVICE;
        }
        if (st->xHandle == xTaskGetCurrentTaskHandle()) {
            o->flags |= NEOS_TASK_PROTECTED;    /* the caller cannot kill itself */
        }
    }

    /* A task that was not in this enumeration has gone. Dropping the row is
       what makes its id match nothing ever again. */
    for (int i = 0; i < ID_MAX; i++) {
        if (s_ent[i].used && !s_ent[i].seen) {
            memset(&s_ent[i], 0, sizeof(s_ent[i]));
        }
    }

    if (boundary) {
        s_window_t0 = now;
    }
    unlock();
    return wrote;
}

/*
 * usStackHighWaterMark is in bytes here and in words on most other ports: the
 * RISC-V port defines portSTACK_TYPE as uint8_t, so the kernel's division by
 * sizeof(StackType_t) is a division by one. Worth saying out loud because a
 * figure four times too small reads as a task about to overflow.
 */

/* ------------------------------------------------------------------ */
/* Killing                                                             */
/* ------------------------------------------------------------------ */

/**
 * Is this handle still the task it was?
 *
 * Asked immediately before a delete, and not the same question as "is the id in
 * the table" - the table is only as fresh as the last neos_tasks(), and the tap
 * being acted on now was drawn from that. A task that exited in between has to
 * be found out about here rather than by calling into a freed control block.
 *
 * Takes no lock of its own, and must not: it is called both with this file's
 * mutex held (from the kill path) and without it (from the wait loop, which
 * releases the mutex so the service can retire itself). uxTaskGetSystemState()
 * does its own locking against the kernel, and the buffer is this path's alone.
 */
static bool still_live(TaskHandle_t h, const char *name)
{
    TaskStatus_t *snap = snap_alloc(&s_snap_kill);
    if (!snap) {
        return false;
    }
    const UBaseType_t n = uxTaskGetSystemState(snap, NEOS_TASKS_MAX, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        if (snap[i].xHandle == h &&
            strncmp(snap[i].pcTaskName, name, NEOS_TASK_NAME - 1) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Ask a service to stop, wait, and delete it if it did not.
 *
 * The wait is what a service is for: it is holding a codec, a socket, a file,
 * and the only thing that can put those back is the service itself. Half a
 * second is long enough for a loop that checks between blocks of audio and
 * short enough that the button does not feel broken.
 */
static void svc_stop(svc_t *s)
{
    const TaskHandle_t h = s->handle;
    char name[NEOS_TASK_NAME];
    strlcpy(name, s->name, sizeof(name));

    s->stopping = true;
    unlock();

    for (int waited = 0; waited < SVC_STOP_MS; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!still_live(h, name)) {
            return;         /* it returned, and its trampoline retired it */
        }
    }

    ESP_LOGW(TAG, "service \"%s\" did not stop when asked - deleting it", name);
    if (!lock()) {
        return;
    }
    /* Re-found rather than reused: half a second is long enough for the slot to
       have been retired and handed to another service. */
    svc_t *again = svc_by_handle_locked(h);
    if (again && still_live(h, name)) {
        vTaskDelete(h);
        svc_retire_locked(again);
    }
    unlock();
}

bool neos_task_kill(uint32_t id)
{
    if (id == 0 || !lock()) {
        return false;
    }

    entry_t *e = NULL;
    for (int i = 0; i < ID_MAX && !e; i++) {
        if (s_ent[i].used && s_ent[i].id == id) {
            e = &s_ent[i];
        }
    }
    if (!e) {
        unlock();
        ESP_LOGW(TAG, "kill %u: no such task", (unsigned)id);
        return false;       /* a stale row: the task has already gone */
    }

    const TaskHandle_t h = e->handle;
    char name[NEOS_TASK_NAME];
    strlcpy(name, e->name, sizeof(name));

    if (name_protected(name) || h == xTaskGetCurrentTaskHandle()) {
        unlock();
        ESP_LOGW(TAG, "kill \"%s\": refused, NeOS needs it", name);
        return false;
    }

    svc_t *s = svc_by_handle_locked(h);
    if (s) {
        svc_stop(s);            /* unlocks */
        return true;
    }

    if (!still_live(h, name)) {
        memset(e, 0, sizeof(*e));
        unlock();
        return false;
    }

    ESP_LOGW(TAG, "deleting task \"%s\"", name);
    vTaskDelete(h);
    memset(e, 0, sizeof(*e));
    unlock();
    return true;
}

void neos_service_stop_all(void)
{
    /*
     * By index, not "find the first one still running" in a loop. The two are
     * the same as long as stopping a service always frees its slot - and this is
     * called on the way out of a case where something has already not gone the
     * way it should, which is the wrong moment to depend on that.
     */
    for (int i = 0; i < NEOS_SERVICES_MAX; i++) {
        if (!lock()) {
            return;
        }
        if (s_svc[i].used) {
            svc_stop(&s_svc[i]);        /* unlocks */
        } else {
            unlock();
        }
    }
}
