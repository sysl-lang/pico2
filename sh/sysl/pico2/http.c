// Fetching a document over HTTP, with or without TLS.
//
// **lwIP's HTTP client is used unchanged for both schemes**, which is the whole reason this file is
// as short as it is. `http_client.c` opens its connection through `altcp_new(settings->
// altcp_allocator)`, and `altcp` is an indirection over TCP: leave the allocator null and the
// request goes out in the clear, hand it one that makes TLS connections and the same client speaks
// https. There is no second code path here and no second client.
//
// **The callbacks are why this is C**, as with `scan.c` and `dns.c`. Both the body callback and the
// completion callback run in lwIP's own context — an interrupt, under
// `pico_cyw43_arch_lwip_threadsafe_background` — and both are handed a `struct pbuf` chain that must
// be walked and released there. The body is copied into fixed storage here and sysl reads it out
// afterwards on the main thread.
//
// **Three things had to be supplied that neither library provides**, and each is a line or two that
// is invisible until it is missing:
//
// - `psa_crypto_init`, which mbedtls 3.6 requires before a TLS context is set up and which lwIP's
//   TLS layer never calls.
// - `mbedtls_ms_time`, which mbedtls implements over `clock_gettime` and `GetSystemTimeAsFileTime`
//   and for nothing else.
// - **The server name**, for SNI and for checking the certificate belongs to the host asked for.
//   `mbedtls_ssl_set_hostname` is called nowhere in lwIP or in the SDK's patched TLS layer, so a
//   request to any host sharing an address with others — which is most of the web — would be
//   answered with the wrong certificate.

#include "pico/cyw43_arch.h"

#if CYW43_LWIP && LWIP_ALTCP_TLS_MBEDTLS

#include <string.h>

#include "pico/time.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

// How much of a body is kept. Anything past this is counted and dropped, and
// `sysl_http_truncated` says so — a short answer that reads as the whole document is the one
// outcome worth ruling out.
#define BODY_MAX 4096

// The longest host name that will be asked for. It has to outlive the call that starts the request,
// because SNI reads it again when the connection is allocated.
#define HOST_MAX 128

enum {
    IDLE = 0,
    RUNNING,
    DONE
};

// **`state` is written by the completion callback in interrupt context** and read by the polling
// loop on the main thread, so it must not be kept in a register across the wait. The fields beside
// it are only read once `state` has been seen to be `DONE`, and the callback writes them first.
static volatile int state = IDLE;
static volatile int outcome;
static volatile uint32_t http_status;
static volatile uint32_t body_len;
static volatile int truncated;

static uint8_t body[BODY_MAX + 1];
static char host[HOST_MAX];

static struct altcp_tls_config *tls_config;
static altcp_allocator_t allocator;
static httpc_connection_t settings;
static httpc_state_t *request;

// The certificate authorities this board trusts, which is the whole of its opinion about who is who
// on the internet.
//
// **Two roots rather than a bundle**, because a bundle is 200 KB of certificates for sites nobody
// here visits and the interesting failure is worth being able to see. A host whose chain ends
// somewhere else is refused, and that refusal is correct rather than a bug — it is what
// `MBEDTLS_SSL_VERIFY_REQUIRED` means. Adding a root is appending its PEM to this string.
//
// GTS Root R4 (Google Trust Services, to 2036) and ISRG Root X1 (Let's Encrypt, to 2035).
static const char trusted_roots[] =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\r\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\r\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\r\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\r\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\r\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\r\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\r\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\r\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\r\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\r\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\r\n"
    "-----END CERTIFICATE-----\r\n"
    "-----BEGIN CERTIFICATE-----\r\n"
    "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\r\n"
    "-----END CERTIFICATE-----\r\n";

// The millisecond clock mbedtls asks for, which it cannot get anywhere on a board without an
// operating system. It measures intervals with it, so time since boot is exactly right and the fact
// that this board does not know the date does not matter here.
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(to_us_since_boot(get_absolute_time()) / 1000);
}

// A TLS connection that knows which host it is for.
//
// **This function exists because lwIP never calls `mbedtls_ssl_set_hostname`.** Without it the
// ClientHello carries no server name, so a server hosting many sites at one address has no way to
// know which certificate to send, and mbedtls has no name to check the certificate it does get
// against. The first failure is loud and the second is silent, which is the worse of the two.
static struct altcp_pcb *tls_alloc_for_host(void *config, u8_t ip_type) {
    struct altcp_pcb *pcb = altcp_tls_alloc((struct altcp_tls_config *)config, ip_type);

    if (pcb != NULL) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);

        if (ssl != NULL) {
            mbedtls_ssl_set_hostname(ssl, host);
        }
    }

    return pcb;
}

// One pbuf chain of body. Headers have already been taken off by the client.
static err_t on_body(void *env, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)env;
    (void)err;

    if (p == NULL) {
        return ERR_OK;
    }

    u16_t room = (u16_t)(BODY_MAX - body_len);
    u16_t take = p->tot_len < room ? p->tot_len : room;

    if (take < p->tot_len) {
        truncated = 1;
    }

    if (take > 0) {
        pbuf_copy_partial(p, body + body_len, take, 0);
        body_len += take;
        body[body_len] = '\0';
    }

    // **Both of these are the caller's job here and neither is optional.** The window has to be
    // reopened or the server stops sending after one bufferful, and the chain is ours to release.
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

static void on_finished(void *env, httpc_result_t result, u32_t received, u32_t server_status,
                        err_t err) {
    (void)env;
    (void)received;
    (void)err;

    outcome = (int)result;
    http_status = server_status;
    state = DONE;
}

// Begin a request. **Zero means it started**, and a negative is either lwIP's `err_t` or one of the
// two refusals below that have no lwIP equivalent.
//
// The bracketing is the same rule `dns.c` follows: under the background arch variant lwIP runs from
// an interrupt, and a call into the stack from the main thread has to hold that off. Neither
// callback brackets, both being already inside.
int sysl_http_start(const char *server, uint16_t port, const char *uri, bool secure) {
    err_t e;

    // One request at a time. The body buffer and the host name are single, so a second request while
    // one is in flight would quietly overwrite both.
    if (state == RUNNING) {
        return -100;
    }

    strncpy(host, server, HOST_MAX - 1);
    host[HOST_MAX - 1] = '\0';

    body_len = 0;
    body[0] = '\0';
    truncated = 0;
    outcome = 0;
    http_status = 0;

    memset(&settings, 0, sizeof settings);
    settings.result_fn = on_finished;

    if (secure) {
        if (tls_config == NULL) {
            // **mbedtls 3.6 will not set up a TLS context until PSA has been initialised**, and
            // nothing in lwIP does it. Once is enough for the life of the program.
            if (psa_crypto_init() != PSA_SUCCESS) {
                return -101;
            }

            // The length passed must include the terminating NUL: mbedtls decides between PEM and
            // DER by looking for one, and a PEM bundle counted without it is read as DER and
            // rejected.
            tls_config = altcp_tls_create_config_client((const u8_t *)trusted_roots,
                                                        sizeof trusted_roots);

            if (tls_config == NULL) {
                return -102;
            }
        }

        allocator.alloc = tls_alloc_for_host;
        allocator.arg = tls_config;
        settings.altcp_allocator = &allocator;
    }

    state = RUNNING;

    cyw43_arch_lwip_begin();
    e = httpc_get_file_dns(host, port, uri, &settings, on_body, NULL, &request);
    cyw43_arch_lwip_end();

    if (e != ERR_OK) {
        state = DONE;
        outcome = HTTPC_RESULT_ERR_UNKNOWN;

        return e;
    }

    return 0;
}

bool sysl_http_busy(void) {
    return state == RUNNING;
}

// `httpc_result_t`: `0` succeeded, and the rest name a stage that failed — `2` could not connect,
// `3` could not resolve, `4` the server hung up, `5` timed out, `7` out of memory.
int sysl_http_outcome(void) {
    return outcome;
}

// The HTTP status line's code, which is a separate question from whether the transfer worked: a
// `404` that arrives intact is a successful fetch of an error page.
uint32_t sysl_http_status(void) {
    return http_status;
}

uint32_t sysl_http_body_len(void) {
    return body_len;
}

bool sysl_http_truncated(void) {
    return truncated != 0;
}

// NUL-terminated, and valid until the next `sysl_http_start`.
const uint8_t *sysl_http_body(void) {
    return body;
}

#else

// **Either no TCP/IP stack or no TLS was linked**, so there is nothing to fetch with. `-103` is not
// an lwIP code; it says the build has no HTTP client in it, which is a different thing from a
// request that failed.

int sysl_http_start(const char *server, uint16_t port, const char *uri, bool secure) {
    (void)server;
    (void)port;
    (void)uri;
    (void)secure;

    return -103;
}

bool sysl_http_busy(void) {
    return false;
}

int sysl_http_outcome(void) {
    return 1;
}

uint32_t sysl_http_status(void) {
    return 0;
}

uint32_t sysl_http_body_len(void) {
    return 0;
}

bool sysl_http_truncated(void) {
    return false;
}

const uint8_t *sysl_http_body(void) {
    return (const uint8_t *)"";
}

#endif
