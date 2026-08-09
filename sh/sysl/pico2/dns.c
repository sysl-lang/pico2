// Turning a name into an address, which lwIP will only do through a callback.
//
// **The callback is the whole reason this is in C.** `dns_gethostbyname` answers immediately when it
// already knows the name and otherwise returns `ERR_INPROGRESS` and calls back later, from lwIP's own
// context — an interrupt, under `pico_cyw43_arch_lwip_threadsafe_background`. That is the same
// situation `scan.c` is in and it gets the same answer: the callback writes into fixed storage here,
// and sysl reads the result out afterwards on the main thread. A sysl callback would be legal to
// declare and would be running inside an interrupt.
//
// **Only one address comes back, and that is lwIP's limit rather than this file's.** `dns.c` in the
// stack walks the answer section and returns on the first A record it matches, so the rest of the
// records are dropped before anything here could see them. Listing every address a name has means
// sending a query rather than asking the resolver, which is a different piece of work.

#include "pico/cyw43_arch.h"

#if CYW43_LWIP

#include "lwip/dns.h"
#include "lwip/ip_addr.h"

enum {
    IDLE = 0,
    PENDING,
    FOUND,
    FAILED
};

// **Written by the callback in interrupt context and read by the polling loop on the main thread**,
// so the compiler must not keep it in a register across the wait. `answer` needs no such qualifier:
// it is only read once `state` has been seen to be `FOUND`, and the callback writes it before it
// writes the state.
static volatile int state = IDLE;
static ip_addr_t answer;

// The four octets high to low, `a.b.c.d` as `0xAABBCCDD`. **The same encoding `net.c` uses**, and
// deliberately so: one sysl function takes both apart, and two spellings of this would be two chances
// to disagree.
static uint32_t packed(const ip4_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16)
        | ((uint32_t)ip4_addr3(a) << 8) | (uint32_t)ip4_addr4(a);
}

// lwIP's answer. **A null address is a failure of any kind** — no such name, or no server that would
// say — and the two are not distinguished by the API.
static void on_found(const char *name, const ip_addr_t *addr, void *env) {
    (void)name;
    (void)env;

    if (addr == NULL) {
        state = FAILED;
        return;
    }

    answer = *addr;
    state = FOUND;
}

// Begin a lookup. **Zero means the answer is already here** — a cached name, or one that was written
// as a dotted quad and needed no server at all — `1` means a query went out and the answer will
// arrive later, and a negative is lwIP's own `err_t` refusing to start.
//
// The bracketing is not optional. Under the background arch variant lwIP runs from an interrupt, and
// every call into the stack from the main thread has to be made with that interrupt held off.
// `on_found` above deliberately does *not* bracket: it is already inside.
int sysl_dns_start(const char *name) {
    err_t e;

    state = PENDING;
    ip_addr_set_zero(&answer);

    cyw43_arch_lwip_begin();
    e = dns_gethostbyname(name, &answer, on_found, NULL);
    cyw43_arch_lwip_end();

    if (e == ERR_OK) {
        state = FOUND;
        return 0;
    }

    if (e == ERR_INPROGRESS) {
        return 1;
    }

    state = FAILED;

    return e;
}

// Whether the answer is still on its way. This is what a caller polls; there is nothing to block on.
bool sysl_dns_busy(void) {
    return state == PENDING;
}

// Whether the lookup ended with an address rather than without one.
bool sysl_dns_found(void) {
    return state == FOUND;
}

uint32_t sysl_dns_address(void) {
    return state == FOUND ? packed(ip_2_ip4(&answer)) : 0;
}

#else

// **No TCP/IP stack was linked**, so there is no resolver to ask and no interface a query could leave
// by. Refusing immediately is the truth here rather than a stand-in: `-6` is lwIP's `ERR_VAL`, which
// is what the resolver itself answers when it has nothing to work with.

int sysl_dns_start(const char *name) {
    (void)name;

    return -6;
}

bool sysl_dns_busy(void) {
    return false;
}

bool sysl_dns_found(void) {
    return false;
}

uint32_t sysl_dns_address(void) {
    return 0;
}

#endif
