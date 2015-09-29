#include <platform.h>
#include <hostIntf.h>
#include <syscall.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <printf.h>
#include <eventQ.h>
#include <timer.h>
#include <stdio.h>
#include <seos.h>
#include <heap.h>
#include <slab.h>
#include <util.h>
#include <cpu.h>
#include <sensors.h>
#include <apInt.h>
#include <nanohubPacket.h>


/*
 * Since locking is difficult to do right for adding/removing listeners and such
 * since it can happen in interrupt context and not, and one such operation can
 * interrupt another, and we do have a working event queue, we enqueue all the
 * requests and then deal with them in the main code only when the event bubbles
 * up to the front of the quque. This allows us to not need locks around the
 * data structures.
 */

struct Task {
    /* pointers may become invalid. Tids do not. Zero tid -> not a valid task */
    uint32_t tid;

    uint16_t subbedEvtCount;
    uint16_t subbedEvtListSz;
    uint32_t *subbedEvents; /* NULL for invalid tasks */

    /* App entry points */
    const struct AppHdr *appHdr;

    /* per-platform app info */
    struct PlatAppInfo platInfo;

    /* for some basic number of subbed events, the array is stored directly here. after that, a heap chunk is used */
    uint32_t subbedEventsInt[MAX_EMBEDDED_EVT_SUBS];
};

union DeferredAction {
    struct {
        uint32_t tid;
        uint32_t evt;
    } evtSub;
    struct {
        OsDeferCbkF callback;
        void *cookie;
    } deferred;
    struct {
        uint32_t evtType;
        void *evtData;
        EventFreeF evtFreeF;
        uint32_t toTid;
    } privateEvt;
};

#define EVT_SUBSCRIBE_TO_EVT         0x00000000
#define EVT_UNSUBSCRIBE_TO_EVT       0x00000001
#define EVT_DEFERRED_CALLBACK        0x00000002
#define EVT_PRIVATE_EVT              0x00000003


static struct EvtQueue *mEvtsInternal, *mEvtsExternal;
static struct SlabAllocator* mDeferedActionsSlab;
static struct Task mTasks[MAX_TASKS];
static uint32_t mNextTid = 1;

static void osInit(void)
{
    cpuInit();
    heapInit();
    platInitialize();

    osLog(LOG_INFO, "SEOS Initializing\n");
    cpuInitLate();

    /* init task list */
    memset(mTasks, 0, sizeof(mTasks));

    /* create the queues */
    if (!(mEvtsInternal = evtQueueAlloc(512)) || !(mEvtsExternal = evtQueueAlloc(256))) {
        osLog(LOG_INFO, "events failed to init\n");
        return;
    }

    mDeferedActionsSlab = slabAllocatorNew(sizeof(union DeferredAction), 4, 32 /* for now? */);
    if (!mDeferedActionsSlab) {
        osLog(LOG_INFO, "deferred actions list failed to init\n");
        return;
    }
}

static struct Task* osFindTaskByAppID(uint64_t appID)
{
    uint32_t i;

    for (i = 0; i < MAX_TASKS && mTasks[i].tid; i++)
        if (mTasks[i].appHdr->appId == appID)
            return mTasks + i;

    return NULL;
}

static void osStartTasks(void)
{
    extern const char __code_end[];
    extern const struct AppHdr __internal_app_start, __internal_app_end, __app_start;
    const struct AppHdr *app;
    static const char magic[] = APP_HDR_MAGIC;
    uint32_t i = 0, nTasks = 0;

    osLog(LOG_INFO, "SEOS Registering tasks\n");

    for (app = &__internal_app_start; app != &__internal_app_end && nTasks < MAX_TASKS && !memcmp(magic, app->magic, sizeof(magic) - 1) && app->version == APP_HDR_VER_CUR; app++) {
        if (app->marker == APP_HDR_MARKER_INTERNAL) {

            if (osFindTaskByAppID(app->appId)) {
                osLog(LOG_ERROR, "Duplicate APP ID ignored\n");
                continue;
            }

            mTasks[nTasks].appHdr = app;
            mTasks[nTasks].subbedEvtListSz = MAX_EMBEDDED_EVT_SUBS;
            mTasks[nTasks].subbedEvents = mTasks[nTasks].subbedEventsInt;
            mTasks[nTasks].tid = mNextTid;

            if (cpuInternalAppLoad(mTasks[i].appHdr, &mTasks[i].platInfo)) {
                mNextTid++;
                nTasks++;
            }
        }
    }

    app = &__app_start;
    while (((uintptr_t)&__code_end) - ((uintptr_t)app) >= sizeof(struct AppHdr) && nTasks < MAX_TASKS && !memcmp(magic, app->magic, sizeof(magic) - 1) && app->version == APP_HDR_VER_CUR) {

        if (app->marker == APP_HDR_MARKER_VALID) {

            if (osFindTaskByAppID(app->appId)) {
                osLog(LOG_ERROR, "Duplicate APP ID ignored\n");
                continue;
            }

            mTasks[nTasks].appHdr = app;
            mTasks[nTasks].subbedEvtListSz = MAX_EMBEDDED_EVT_SUBS;
            mTasks[nTasks].subbedEvents = mTasks[nTasks].subbedEventsInt;
            mTasks[nTasks].tid = mNextTid;

            if (cpuAppLoad(mTasks[i].appHdr, &mTasks[i].platInfo)) {
                mNextTid++;
                nTasks++;
            }
        }
        app = (const struct AppHdr*)(((const uint8_t*)app) + app->rel_end);
    }

    osLog(LOG_INFO, "SEOS Starting tasks\n");
    while (i < nTasks) {
        if (cpuAppInit(mTasks[i].appHdr, &mTasks[i].platInfo, mTasks[i].tid))
            i++;
        else {
            cpuAppUnload(mTasks[i].appHdr, &mTasks[i].platInfo);
            memcpy(mTasks + i, mTasks + --nTasks, sizeof(struct Task));
        }
    }
}

static struct Task* osTaskFindByTid(uint32_t tid)
{
    uint32_t i;

    for(i = 0; i < MAX_TASKS; i++)
        if (mTasks[i].tid == tid)
            return mTasks + i;

    return NULL;
}

static void osInternalEvtHandle(uint32_t evtType, void *evtData)
{
    union DeferredAction *da = (union DeferredAction*)evtData;
    struct Task *task;
    uint32_t i;

    switch (evtType) {
    case EVT_SUBSCRIBE_TO_EVT:
    case EVT_UNSUBSCRIBE_TO_EVT:
        /* get task */
        task = osTaskFindByTid(da->evtSub.tid);
        if (!task)
            break;

        /* find if subscribed to this evt */
        for (i = 0; i < task->subbedEvtCount && task->subbedEvents[i] != da->evtSub.evt; i++);

        /* if unsub & found -> unsub */
        if (evtType == EVT_UNSUBSCRIBE_TO_EVT && i != task->subbedEvtCount)
            task->subbedEvents[i] = task->subbedEvents[--task->subbedEvtCount];
        /* if sub & not found -> sub */
        else if (evtType == EVT_SUBSCRIBE_TO_EVT && i == task->subbedEvtCount) {
            if (task->subbedEvtListSz == task->subbedEvtCount) { /* enlarge the list */
                uint32_t newSz = (task->subbedEvtListSz * 3 + 1) / 2;
                uint32_t *newList = heapAlloc(sizeof(uint32_t[newSz])); /* grow by 50% */
                if (newList) {
                    memcpy(newList, task->subbedEvents, sizeof(uint32_t[task->subbedEvtListSz]));
                    if (task->subbedEvents != task->subbedEventsInt)
                        heapFree(task->subbedEvents);
                    task->subbedEvents = newList;
                    task->subbedEvtListSz = newSz;
                }
            }
            if (task->subbedEvtListSz > task->subbedEvtCount) { /* have space ? */
                task->subbedEvents[task->subbedEvtCount++] = da->evtSub.evt;
            }
        }
        break;

    case EVT_DEFERRED_CALLBACK:
        da->deferred.callback(da->deferred.cookie);
        break;

    case EVT_PRIVATE_EVT:
        task = osTaskFindByTid(da->privateEvt.toTid);
        if (task) {
            cpuAppHandle(task->appHdr, &task->platInfo, da->privateEvt.evtType, da->privateEvt.evtData);
        }

        if (da->privateEvt.evtFreeF)
            da->privateEvt.evtFreeF(da->privateEvt.evtData);
        break;
    }
}

static void osExpApiEvtqSubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventSubscribe(tid, evtType);
}

static void osExpApiEvtqUnsubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventUnsubscribe(tid, evtType);
}

static void osExpApiEvtqEnqueue(uintptr_t *retValP, va_list args)
{
    uint32_t evtType = va_arg(args, uint32_t);
    void *evtData = va_arg(args, void*);
    EventFreeF evtFreeF = va_arg(args, EventFreeF);
    bool external = va_arg(args, int);

    //TODO: XXX: use UserspaceCallback mechanism for event freeing here!!!

    *retValP = osEnqueueEvt(evtType, evtData, evtFreeF, external);
}

static void osExpApiEvtqFuncDeferCbk(void *data)
{
    struct UserspaceCallback *ucbk = (struct UserspaceCallback*)data;

    syscallUserspaceCallbackCall(ucbk, NULL, NULL, NULL, NULL);
    syscallUserspaceCallbackFree(ucbk);
}

static void osExpApiEvtqFuncDefer(uintptr_t *retValP, va_list args)
{
    OsDeferCbkF userCbk = va_arg(args, OsDeferCbkF);
    void *userData = va_arg(args, void*);
    struct UserspaceCallback *ucbk;

    *retValP = false;
    ucbk = syscallUserspaceCallbackAlloc(userCbk, (uintptr_t)userData, 0, 0, 0);
    if (ucbk) {
        if (osDefer(osExpApiEvtqFuncDeferCbk, ucbk))
            *retValP = true;
        else
            syscallUserspaceCallbackFree(ucbk);
    }
}

static void osExpApiLogLogv(uintptr_t *retValP, va_list args)
{
    enum LogLevel level = va_arg(args, int /* enums promoted to ints in va_args in C */);
    const char *str = va_arg(args, const char*);
    va_list innerArgs = INTEGER_TO_VA_LIST(va_arg(args, uintptr_t));

    osLogv(level, str, innerArgs);
}

static void osExportApi(void)
{
    static const struct SyscallTable osMainEvtqTable = {
        .numEntries = SYSCALL_OS_MAIN_EVTQ_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVTQ_SUBCRIBE]   = { .func = osExpApiEvtqSubscribe,   },
            [SYSCALL_OS_MAIN_EVTQ_UNSUBCRIBE] = { .func = osExpApiEvtqUnsubscribe, },
            [SYSCALL_OS_MAIN_EVTQ_ENQUEUE]    = { .func = osExpApiEvtqEnqueue,     },
            [SYSCALL_OS_MAIN_EVTQ_FUNC_DEFER] = { .func = osExpApiEvtqFuncDefer,   },
        },
    };

    static const struct SyscallTable osMainLogTable = {
        .numEntries = SYSCALL_OS_MAIN_LOG_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_LOG_LOGV]   = { .func = osExpApiLogLogv,   },
        },
    };

    static const struct SyscallTable osMainTable = {
        .numEntries = SYSCALL_OS_MAIN_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVENTQ] =  { .subtable = (struct SyscallTable*)&osMainEvtqTable, },
            [SYSCALL_OS_MAIN_LOGGING] = { .subtable = (struct SyscallTable*)&osMainLogTable,  },
        },
    };
    static const struct SyscallTable osTable = {
        .numEntries = SYSCALL_OS_LAST,
        .entry = {
            [SYSCALL_OS_MAIN] = { .subtable = (struct SyscallTable*)&osMainTable, },
        },
    };

    if (!syscallAddTable(SYSCALL_DOMAIN_OS, 1, (struct SyscallTable*)&osTable))
        osLog(LOG_ERROR, "Failed to export OS base API");
}

void abort(void)
{
    /* this is necessary for va_* funcs... */
    osLog(LOG_ERROR, "Abort called");
    while(1);
}

void __attribute__((noreturn)) osMain(void)
{
    EventFreeF evtFree;
    uint32_t evtType, i, j;
    void *evtData;

    cpuIntsOff();
    timInit();
    osInit();
    sensorsInit();
    syscallInit();
    osExportApi();
    hostIntfRequest();
    apIntInit();
    cpuIntsOn();
    osStartTasks();

    //broadcast app start to all already-loaded apps
    (void)osEnqueueEvt(EVT_APP_START, NULL, NULL, false);

    while (true) {

        /* get an event */
        if (!evtQueueDequeue(mEvtsInternal, &evtType, &evtData, &evtFree, true))
            continue;

        if (evtType < EVT_NO_FIRST_USER_EVENT) { /* no need for discardable check. all internal events arent discardable */
            /* handle deferred actions and other reserved events here */
            osInternalEvtHandle(evtType, evtData);
        }
        else {
            /* send this event to all tasks who want it (decimation could happen here) */
            for (i = 0; i < MAX_TASKS; i++) {
                if (!mTasks[i].subbedEvents) /* only check real tasks */
                    continue;
                for (j = 0; j < mTasks[i].subbedEvtCount; j++) {
                    if (mTasks[i].subbedEvents[j] == evtType) {
                        cpuAppHandle(mTasks[i].appHdr, &mTasks[i].platInfo, evtType, evtData);
                        break;
                    }
                }
            }
        }

        /* free it */
        if (evtFree)
            evtFree(evtData);
    }
}

static void osDeferredActionFreeF(void* event)
{
    slabAllocatorFree(mDeferedActionsSlab, event);
}

static bool osEventSubscribeUnsubscribe(uint32_t tid, uint32_t evtType, bool sub)
{
    union DeferredAction *act = slabAllocatorAlloc(mDeferedActionsSlab);

    if (!act)
        return false;
    act->evtSub.evt = evtType;
    act->evtSub.tid = tid;

    if (osEnqueueEvt(sub ? EVT_SUBSCRIBE_TO_EVT : EVT_UNSUBSCRIBE_TO_EVT, act, osDeferredActionFreeF, false))
        return true;

    slabAllocatorFree(mDeferedActionsSlab, act);
    return false;
}

bool osEventSubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, true);
}

bool osEventUnsubscribe(uint32_t tid, uint32_t evtType)
{
    return osEventSubscribeUnsubscribe(tid, evtType, false);
}

bool osDefer(OsDeferCbkF callback, void *cookie)
{
    union DeferredAction *act = slabAllocatorAlloc(mDeferedActionsSlab);
    if (!act)
            return false;

    act->deferred.callback = callback;
    act->deferred.cookie = cookie;

    if (osEnqueueEvt(EVT_DEFERRED_CALLBACK, act, osDeferredActionFreeF, false))
        return true;

    slabAllocatorFree(mDeferedActionsSlab, act);
    return false;
}

bool osEnqueuePrivateEvt(uint32_t evtType, void *evtData, EventFreeF evtFreeF, uint32_t toTid)
{
    union DeferredAction *act = slabAllocatorAlloc(mDeferedActionsSlab);
    if (!act)
            return false;

    act->privateEvt.evtType = evtType;
    act->privateEvt.evtData = evtData;
    act->privateEvt.evtFreeF = evtFreeF;
    act->privateEvt.toTid = toTid;

    if (osEnqueueEvt(EVT_PRIVATE_EVT, act, osDeferredActionFreeF, false))
        return true;

    slabAllocatorFree(mDeferedActionsSlab, act);
    return false;
}

bool osEnqueueEvt(uint32_t evtType, void *evtData, EventFreeF evtFreeF, bool external)
{
    return evtQueueEnqueue(external ? mEvtsExternal : mEvtsInternal, evtType, evtData, evtFreeF);
}

bool osDequeueExtEvt(uint32_t *evtType, void **evtData, EventFreeF *evtFree)
{
    return evtQueueDequeue(mEvtsExternal, evtType, evtData, evtFree, false);
}

void osLogv(enum LogLevel level, const char *str, va_list vl)
{
    void *userData = platLogAllocUserData();

    platLogPutcharF(userData, level);
    cvprintf(platLogPutcharF, userData, str, vl);

    platLogFlush(userData);
}

void osLog(enum LogLevel level, const char *str, ...)
{
    va_list vl;

    va_start(vl, str);
    osLogv(level, str, vl);
    va_end(vl);
}




//Google's public key for Google's apps' signing
const uint8_t __attribute__ ((section (".pubkeys"))) _RSA_KEY_GOOGLE[] = {
    0xd9, 0xcd, 0x83, 0xae, 0xb5, 0x9e, 0xe4, 0x63, 0xf1, 0x4c, 0x26, 0x6a, 0x1c, 0xeb, 0x4c, 0x12,
    0x5b, 0xa6, 0x71, 0x7f, 0xa2, 0x4e, 0x7b, 0xa2, 0xee, 0x02, 0x86, 0xfc, 0x0d, 0x31, 0x26, 0x74,
    0x1e, 0x9c, 0x41, 0x43, 0xba, 0x16, 0xe9, 0x23, 0x4d, 0xfc, 0xc4, 0xca, 0xcc, 0xd5, 0x27, 0x2f,
    0x16, 0x4c, 0xe2, 0x85, 0x39, 0xb3, 0x0b, 0xcb, 0x73, 0xb6, 0x56, 0xc2, 0x98, 0x83, 0xf6, 0xfa,
    0x7a, 0x6e, 0xa0, 0x9a, 0xcc, 0x83, 0x97, 0x9d, 0xde, 0x89, 0xb2, 0xa3, 0x05, 0x46, 0x0c, 0x12,
    0xae, 0x01, 0xf8, 0x0c, 0xf5, 0x39, 0x32, 0xe5, 0x94, 0xb9, 0xa0, 0x8f, 0x19, 0xe4, 0x39, 0x54,
    0xad, 0xdb, 0x81, 0x60, 0x74, 0x63, 0xd5, 0x80, 0x3b, 0xd2, 0x88, 0xf4, 0xcb, 0x6b, 0x47, 0x28,
    0x80, 0xb0, 0xd1, 0x89, 0x6d, 0xd9, 0x62, 0x88, 0x81, 0xd6, 0xc0, 0x13, 0x88, 0x91, 0xfb, 0x7d,
    0xa3, 0x7f, 0xa5, 0x40, 0x12, 0xfb, 0x77, 0x77, 0x4c, 0x98, 0xe4, 0xd3, 0x62, 0x39, 0xcc, 0x63,
    0x34, 0x76, 0xb9, 0x12, 0x67, 0xfe, 0x83, 0x23, 0x5d, 0x40, 0x6b, 0x77, 0x93, 0xd6, 0xc0, 0x86,
    0x6c, 0x03, 0x14, 0xdf, 0x78, 0x2d, 0xe0, 0x9b, 0x5e, 0x05, 0xf0, 0x93, 0xbd, 0x03, 0x1d, 0x17,
    0x56, 0x88, 0x58, 0x25, 0xa6, 0xae, 0x63, 0xd2, 0x01, 0x43, 0xbb, 0x7e, 0x7a, 0xa5, 0x62, 0xdf,
    0x8a, 0x31, 0xbd, 0x24, 0x1b, 0x1b, 0xeb, 0xfe, 0xdf, 0xd1, 0x31, 0x61, 0x4a, 0xfa, 0xdd, 0x6e,
    0x62, 0x0c, 0xa9, 0xcd, 0x08, 0x0c, 0xa1, 0x1b, 0xe7, 0xf2, 0xed, 0x36, 0x22, 0xd0, 0x5d, 0x80,
    0x78, 0xeb, 0x6f, 0x5a, 0x58, 0x18, 0xb5, 0xaf, 0x82, 0x77, 0x4c, 0x95, 0xce, 0xc6, 0x4d, 0xda,
    0xca, 0xef, 0x68, 0xa6, 0x6d, 0x71, 0x4d, 0xf1, 0x14, 0xaf, 0x68, 0x25, 0xb8, 0xf3, 0xff, 0xbe,
};


#ifdef DEBUG

//debug key whose privatekey is checked in as misc/debug.privkey
const uint8_t __attribute__ ((section (".pubkeys"))) _RSA_KEY_GOOGLE_DEBUG[] = {
    0x2d, 0xff, 0xa6, 0xb5, 0x65, 0x87, 0xbe, 0x61, 0xd1, 0xe1, 0x67, 0x10, 0xa1, 0x9b, 0xc6, 0xca,
    0xc8, 0xb1, 0xf0, 0xaa, 0x88, 0x60, 0x9f, 0xa1, 0x00, 0xa1, 0x41, 0x9a, 0xd8, 0xb4, 0xd1, 0x74,
    0x9f, 0x23, 0x28, 0x0d, 0xc2, 0xc4, 0x37, 0x15, 0xb1, 0x4a, 0x80, 0xca, 0xab, 0xb9, 0xba, 0x09,
    0x7d, 0xf8, 0x44, 0xd6, 0xa2, 0x72, 0x28, 0x12, 0x91, 0xf6, 0xa5, 0xea, 0xbd, 0xf8, 0x81, 0x6b,
    0xd2, 0x3c, 0x50, 0xa2, 0xc6, 0x19, 0x54, 0x48, 0x45, 0x8d, 0x92, 0xac, 0x01, 0xda, 0x14, 0x32,
    0xdb, 0x05, 0x82, 0x06, 0x30, 0x25, 0x09, 0x7f, 0x5a, 0xbb, 0x86, 0x64, 0x70, 0x98, 0x64, 0x1e,
    0xe6, 0xca, 0x1d, 0xc1, 0xcb, 0xb6, 0x23, 0xd2, 0x62, 0x00, 0x46, 0x97, 0xd5, 0xcc, 0xe6, 0x36,
    0x72, 0xec, 0x2e, 0x43, 0x1f, 0x0a, 0xaf, 0xf2, 0x51, 0xe1, 0xcd, 0xd2, 0x98, 0x5d, 0x7b, 0x64,
    0xeb, 0xd1, 0x35, 0x4d, 0x59, 0x13, 0x82, 0x6c, 0xbd, 0xc4, 0xa2, 0xfc, 0xad, 0x64, 0x73, 0xe2,
    0x71, 0xb5, 0xf4, 0x45, 0x53, 0x6b, 0xc3, 0x56, 0xb9, 0x8b, 0x3d, 0xeb, 0x00, 0x48, 0x6e, 0x29,
    0xb1, 0xb4, 0x8e, 0x2e, 0x43, 0x39, 0xef, 0x45, 0xa0, 0xb8, 0x8b, 0x5f, 0x80, 0xb5, 0x0c, 0xc3,
    0x03, 0xe3, 0xda, 0x51, 0xdc, 0xec, 0x80, 0x2c, 0x0c, 0xdc, 0xe2, 0x71, 0x0a, 0x14, 0x4f, 0x2c,
    0x22, 0x2b, 0x0e, 0xd1, 0x8b, 0x8f, 0x93, 0xd2, 0xf3, 0xec, 0x3a, 0x5a, 0x1c, 0xba, 0x80, 0x54,
    0x23, 0x7f, 0xb0, 0x54, 0x8b, 0xe3, 0x98, 0x22, 0xbb, 0x4b, 0xd0, 0x29, 0x5f, 0xce, 0xf2, 0xaa,
    0x99, 0x89, 0xf2, 0xb7, 0x5d, 0x8d, 0xb2, 0x72, 0x0b, 0x52, 0x02, 0xb8, 0xa4, 0x37, 0xa0, 0x3b,
    0xfe, 0x0a, 0xbc, 0xb3, 0xb3, 0xed, 0x8f, 0x8c, 0x42, 0x59, 0xbe, 0x4e, 0x31, 0xed, 0x11, 0x9b,
};

#endif

