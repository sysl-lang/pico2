// Scanning for networks, which is the one part of the radio that sysl cannot reach on its own.
//
// Three of the four things `15 §7` names as reachable from C and from nothing else are in this one
// call. `cyw43_wifi_scan` wants the address of a global the driver owns, an options struct the
// caller allocates and whose size only the header knows, and a callback that is handed a
// `cyw43_ev_scan_result_t` whose fields live at offsets a compiler chose. Transcribing that struct
// into sysl would compile and would be correct for exactly one build of the driver.
//
// **The whole scan happens here rather than in a sysl callback, and that is about WHERE the callback
// runs, not about what sysl can express.** sysl has `*extern(A, B) -> R` and could be handed to
// `cyw43_wifi_scan` directly. But the driver calls back from its own background context — an
// interrupt, under `pico_cyw43_arch_lwip_threadsafe_background` — and the natural sysl callback
// would build a string and push it onto a `Buf`, which is `malloc` inside an interrupt against
// newlib's non-reentrant allocator. Collecting into fixed storage here and letting sysl read it out
// afterwards, on the main thread, is the correct shape and not merely the convenient one.
//
// It also gets the deduplication somewhere sensible. A scan reports the same access point once per
// probe response, so a raw result list has a dozen copies of every network in it and is unusable as
// a list of names.

#include <string.h>

#include "pico/cyw43_arch.h"
#include "cyw43.h"

// Enough for a crowded flat. Networks past this are dropped, and `sysl_wifi_scan_overflowed` says
// so rather than letting a truncated list read as a complete one.
#define MAX_NETWORKS 32

typedef struct {
    uint8_t ssid[33];       // NUL-terminated, so sysl can read it with `from_cstring`
    int rssi;
    int channel;
    int auth_mode;
} entry_t;

static entry_t found[MAX_NETWORKS];
static int count;
static int overflowed;

// One probe response. Returning non-zero would abort the scan, and nothing here wants to.
static int on_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;

    // A hidden network answers with an empty name. There is nothing to list, and the entry would be
    // a blank line the user cannot act on.
    if (r->ssid_len == 0 || r->ssid_len > 32) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (strncmp((const char *)found[i].ssid, (const char *)r->ssid, r->ssid_len) == 0
            && found[i].ssid[r->ssid_len] == '\0') {
            // Seen already. Keep the strongest sighting, since that is the one that says whether
            // the network is actually reachable from here.
            if (r->rssi > found[i].rssi) {
                found[i].rssi = r->rssi;
                found[i].channel = r->channel;
            }
            return 0;
        }
    }

    if (count >= MAX_NETWORKS) {
        overflowed = 1;
        return 0;
    }

    memcpy(found[count].ssid, r->ssid, r->ssid_len);
    found[count].ssid[r->ssid_len] = '\0';
    found[count].rssi = r->rssi;
    found[count].channel = r->channel;
    found[count].auth_mode = r->auth_mode;
    count++;

    return 0;
}

// Begin a scan. Zero on success, and the driver's own error otherwise.
int sysl_wifi_scan_start(void) {
    cyw43_wifi_scan_options_t opts;

    // Every field of this is documented as unused, so it is zeroed rather than filled in. It cannot
    // be omitted: the driver dereferences it.
    memset(&opts, 0, sizeof opts);

    count = 0;
    overflowed = 0;

    return cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_result);
}

// Whether the scan is still running. This is the only way to know it has finished — there is no
// completion callback.
bool sysl_wifi_scan_busy(void) {
    return cyw43_wifi_scan_active(&cyw43_state);
}

int sysl_wifi_scan_count(void) {
    return count;
}

// Whether more networks were seen than there was room for.
bool sysl_wifi_scan_overflowed(void) {
    return overflowed != 0;
}

// The fields of one result, one function apiece, because a struct crossing the boundary is the thing
// this file exists to avoid. The name points into storage that stays valid until the next
// `sysl_wifi_scan_start`, which is long enough for sysl to copy it out.
const uint8_t *sysl_wifi_scan_ssid(int i) {
    return (i >= 0 && i < count) ? found[i].ssid : (const uint8_t *)"";
}

int sysl_wifi_scan_rssi(int i) {
    return (i >= 0 && i < count) ? found[i].rssi : 0;
}

int sysl_wifi_scan_channel(int i) {
    return (i >= 0 && i < count) ? found[i].channel : 0;
}

int sysl_wifi_scan_auth(int i) {
    return (i >= 0 && i < count) ? found[i].auth_mode : 0;
}
