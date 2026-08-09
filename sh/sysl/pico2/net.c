// Where the board is on the network, which lwIP keeps somewhere sysl cannot reach.
//
// **Two of the three things `15 §7` names as C-only are in one line of this file.** The station
// interface is a field of `cyw43_state`, a global whose address sysl cannot take, and lwIP reads an
// address out of it with `netif_ip4_addr` — a *macro* over a `struct netif`, so there is no symbol to
// declare even if the struct could be spelled. What crosses the boundary here is four octets in a
// number.
//
// **The octets are packed high to low, so `a.b.c.d` arrives as `0xAABBCCDD`.** lwIP holds an address
// in network byte order, and handing its `u32_t` over unchanged would make the sysl side ask which
// way round this machine is — a question it has no way to answer and no business asking. Packing it
// here costs three shifts and settles it.
//
// **A build with no TCP/IP stack has no interface to read**, and the driver says so with
// `CYW43_LWIP`: `pico_cyw43_arch_none` sets it to 0 and `netif` is compiled out of `cyw43_t`
// altogether. Without the guard below this package would stop compiling in any project that only
// wants the LED — `blink/` is one — so the guard is not defensive, it is the difference between a
// package that composes and one that dictates a link line. What it answers there is true rather than
// a stand-in: a program with nothing to run TCP/IP on cannot have an address, and its link can never
// come up, because coming up is a thing only lwIP can report.

#include "pico/cyw43_arch.h"
#include "cyw43.h"

#if CYW43_LWIP

#include "lwip/netif.h"

static uint32_t packed(const ip4_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16)
        | ((uint32_t)ip4_addr3(a) << 8) | (uint32_t)ip4_addr4(a);
}

// The station interface. `cyw43_state.netif` is indexed by interface rather than being two named
// fields, and the access-point half is `CYW43_ITF_AP` — not read here, since nothing in this package
// hands out addresses for it to have one from.
static struct netif *station(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

uint32_t sysl_wifi_ip4_address(void) {
    return packed(netif_ip4_addr(station()));
}

uint32_t sysl_wifi_ip4_netmask(void) {
    return packed(netif_ip4_netmask(station()));
}

uint32_t sysl_wifi_ip4_gateway(void) {
    return packed(netif_ip4_gw(station()));
}

// `CYW43_LINK_*`, straight through. The translation into something a caller can match on happens in
// sysl, where the rest of this package's translation happens.
int sysl_wifi_link_status(void) {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
}

#else

uint32_t sysl_wifi_ip4_address(void) {
    return 0;
}

uint32_t sysl_wifi_ip4_netmask(void) {
    return 0;
}

uint32_t sysl_wifi_ip4_gateway(void) {
    return 0;
}

// `CYW43_LINK_DOWN`, which is where a link with no stack under it stays.
int sysl_wifi_link_status(void) {
    return 0;
}

#endif
