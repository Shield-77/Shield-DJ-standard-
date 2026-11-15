#ifndef SHIELD_APP_WIFI_GUARD_H
#define SHIELD_APP_WIFI_GUARD_H

#include <3ds.h>
#include <string.h>

// NWM returns this exact result when wireless is already in the requested
// state. It lets us leave an originally disabled radio disabled on exit.
#define APP_WIFI_ALREADY_REQUESTED ((Result)0xC8A06C0D)

typedef struct {
    aptHookCookie hook;
    bool initialized;
    bool restore_needed;
    bool radio_disabled;
    Result last_result;
} AppWifiGuard;

static void appWifiGuardDisable(AppWifiGuard *guard) {
    if (!guard->initialized) return;
    Result result = NWMEXT_ControlWirelessEnabled(false);
    guard->last_result = result;
    if (R_SUCCEEDED(result)) {
        guard->restore_needed = true;
        guard->radio_disabled = true;
    } else if (result == APP_WIFI_ALREADY_REQUESTED) {
        // Keep ownership if this guard disabled the radio before sleep.
        guard->radio_disabled = true;
    } else {
        guard->radio_disabled = false;
    }
}

static void appWifiGuardRestore(AppWifiGuard *guard) {
    if (!guard->initialized || !guard->restore_needed) return;
    Result result = NWMEXT_ControlWirelessEnabled(true);
    guard->last_result = result;
    if (R_SUCCEEDED(result) || result == APP_WIFI_ALREADY_REQUESTED) {
        guard->restore_needed = false;
        guard->radio_disabled = false;
    }
}

static void appWifiGuardHook(APT_HookType hook, void *parameter) {
    AppWifiGuard *guard = (AppWifiGuard *)parameter;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONEXIT:
            appWifiGuardRestore(guard);
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            appWifiGuardDisable(guard);
            break;
        default:
            break;
    }
}

static bool appWifiGuardInit(AppWifiGuard *guard) {
    memset(guard, 0, sizeof(*guard));
    guard->last_result = nwmExtInit();
    if (R_FAILED(guard->last_result)) return false;
    guard->initialized = true;
    aptHook(&guard->hook, appWifiGuardHook, guard);
    appWifiGuardDisable(guard);
    return guard->radio_disabled;
}

static bool appWifiGuardExit(AppWifiGuard *guard) {
    if (!guard->initialized) return true;
    aptUnhook(&guard->hook);
    // START exits also need this: they need not trigger APTHOOK_ONEXIT.
    for (unsigned attempt = 0; attempt < 3 && guard->restore_needed; attempt++) {
        if (attempt) svcSleepThread(20 * 1000 * 1000LL);
        appWifiGuardRestore(guard);
    }
    bool restored = !guard->restore_needed;
    nwmExtExit();
    guard->initialized = false;
    return restored;
}

#endif
