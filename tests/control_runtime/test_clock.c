// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
int test_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void test_freeaddrinfo(struct addrinfo *);
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
#include "../../firmware/main/app_clock.c"
/* Compile the actual station-owner entry barrier alongside the actual clock.
 * Only SDK TCP/IP execution and global DNS storage are replaced below. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#include "clock_network_fixture.inc"

static uint64_t clock_ms = 100;
static unsigned critical, created, initialized, started, stopped, step;
static unsigned legacy_creates, caps_creates;
static bool fail_create;
static void (*callback)(struct timeval *);
static bool sync_before_lock;
static jmp_buf finish;
static unsigned resolutions;
typedef enum {
    BASE, NETWORK_REPLACE, CONFIG_REPLACE, KNOWN_NETWORK_REPLACE,
    ALTERNATE, MULTIPLE, DNS_FAILURE, DNS_TIMEOUT, INIT_REPLACE, START_REPLACE
} scenario_t;
static scenario_t scenario;
static const char *retained[APP_CLOCK_SERVER_CAPACITY];
static char configured[APP_CLOCK_SERVER_CAPACITY][INET6_ADDRSTRLEN];
static unsigned retained_count;
static bool cached_old_answer;
static uint64_t cache_expires;
static unsigned cache_hits;
static bool tcpip_owner;
static unsigned dns_clears, pending_dns, cancelled_dns, dns_sockets_closed;
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn callback_fn, void *context)
{
    assert(!critical && !tcpip_owner);
    tcpip_owner = true;
    esp_err_t result = callback_fn(context);
    tcpip_owner = false;
    return result;
}
void dns_clear_cache(void)
{
    assert(tcpip_owner && !critical);
    ++dns_clears;
    cached_old_answer = false;
    /* Pinned global clear fails every pending lookup and removes its DNS UDP
     * ownership; an already-returned result is still local to its old waiter. */
    cancelled_dns += pending_dns;
    if (pending_dns) ++dns_sockets_closed;
    pending_dns = 0;
}
static void replace_network(void)
{
    portENTER_CRITICAL(&s_lock);
    station_network_lost_locked();
    portEXIT_CRITICAL(&s_lock);
    assert(station_network_enter(s_network.generation) == ESP_OK);
}
int test_getaddrinfo(const char *name, const char *service, const struct addrinfo *hints,
                            struct addrinfo **out)
{
    static struct sockaddr_in address;
    static struct sockaddr_in6 address6;
    static struct addrinfo answer;
    assert(!critical && name && !service && hints->ai_family == AF_UNSPEC &&
           hints->ai_socktype == SOCK_DGRAM);
    ++resolutions;
    *out = NULL;
    bool cache_hit = scenario == NETWORK_REPLACE && cached_old_answer && clock_ms < cache_expires;
    if (cache_hit) ++cache_hits;
    if (scenario == DNS_FAILURE || !strcmp(name, "missing.example")) return EAI_NONAME;
    if ((scenario == NETWORK_REPLACE && resolutions == 1) ||
        (scenario == KNOWN_NETWORK_REPLACE && resolutions == 2)) {
        uint32_t before = state.sync_generation;
        clock_ms += 2000;
        portENTER_CRITICAL(&s_lock);
        station_network_lost_locked();
        portEXIT_CRITICAL(&s_lock);
        clock_ms += 1000;
        if (scenario == NETWORK_REPLACE) {
            /* Positive-TTL completion happens before the old caller wakes,
             * after loss but before admission of the newly acquired LAN. */
            cached_old_answer = true;
            cache_expires = clock_ms + 120000;
            pending_dns = 2; /* Other old-network waiters are cancelled too. */
        }
        clock_ms += 2000; /* The old lookup completes on the replacement LAN. */
        assert(station_network_enter(s_network.generation) == ESP_OK);
        if (scenario == KNOWN_NETWORK_REPLACE) assert(state.sync_generation == before);
        assert(!state.active);
    }
    if (scenario == CONFIG_REPLACE && resolutions == 1) {
        clock_ms += 2000;
        app_clock_config_t replacement = {.servers = {"replacement.example"},
            .maximum_age_ms = 60000, .refresh_interval_ms = 60000};
        assert(app_clock_init(&replacement) == ESP_OK);
    }
    if (scenario == DNS_TIMEOUT) clock_ms += 30000;
    if (!strcmp(name, "ipv6.example")) {
        address6 = (struct sockaddr_in6){.sin6_family = AF_INET6};
        assert(inet_pton(AF_INET6, "2001:db8::20", &address6.sin6_addr) == 1);
        answer = (struct addrinfo){.ai_family = AF_INET6, .ai_addr = (struct sockaddr *)&address6,
                                  .ai_addrlen = sizeof(address6)};
        *out = &answer;
        return 0;
    }
    if (scenario == CONFIG_REPLACE && resolutions > 1) assert(!strcmp(name, "replacement.example"));
    address = (struct sockaddr_in){.sin_family = AF_INET};
    const char *numeric = cache_hit || resolutions == 1 ? "192.0.2.10" : "192.0.2.20";
    assert(inet_pton(AF_INET, numeric, &address.sin_addr) == 1);
    answer = (struct addrinfo){.ai_family = AF_INET, .ai_addr = (struct sockaddr *)&address,
                               .ai_addrlen = sizeof(address)};
    *out = &answer;
    return 0;
}
void test_freeaddrinfo(struct addrinfo *answer) { assert(!critical && answer); }
void test_enter(void)
{
    if (sync_before_lock) {
        assert(!critical);
        sync_before_lock = false;
        ++clock_ms;
        struct timeval tv = {.tv_sec = 1900000000};
        callback(&tv);
    }
    ++critical;
}
void test_leave(void)
{
    assert(critical);
    --critical;
}
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static int create_clock(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                        unsigned priority, void *out)
{
    assert(fn == worker && !strcmp(name, "network_clock") && stack == 4096 && priority == 1 &&
           !arg && !out);
    if (fail_create) {
        return 0;
    }
    ++created;
    return pdPASS;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                void *out)
{
    assert(!APP_NETWORK_EXTERNAL_STACK);
    ++legacy_creates;
    return create_clock(fn, name, stack, arg, priority, out);
}
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                        unsigned priority, void *out, unsigned caps)
{
    assert(APP_NETWORK_EXTERNAL_STACK && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++caps_creates;
    return create_clock(fn, name, stack, arg, priority, out);
}
void vTaskDelete(void *task)
{
    (void)task;
    assert(!"Clock task must remain resident");
}
void vTaskDeleteWithCaps(void *task)
{
    (void)task;
    assert(!"Clock task must remain resident");
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c)
{
    assert(!critical && !retained_count && !c->start && !c->wait_for_sync && !c->server_from_dhcp &&
           !c->renew_servers_after_new_IP && c->num_of_servers &&
           c->num_of_servers <= APP_CLOCK_SERVER_CAPACITY && c->sync_cb);
    struct in_addr ipv4;
    struct in6_addr ipv6;
    /* Pinned lwIP parses these synchronously. A hostname here would leave an
     * unlabelled asynchronous DNS callback that can target a replacement PCB. */
    for (unsigned i = 0; i < c->num_of_servers; ++i) {
        assert(c->servers[i] && (inet_pton(AF_INET, c->servers[i], &ipv4) == 1 ||
                               inet_pton(AF_INET6, c->servers[i], &ipv6) == 1));
        retained[i] = c->servers[i];
        snprintf(configured[i], sizeof(configured[i]), "%s", retained[i]);
    }
    retained_count = (unsigned)c->num_of_servers;
    callback = c->sync_cb;
    ++initialized;
    if (scenario == INIT_REPLACE && initialized == 1) {
        replace_network();
    }
    return ESP_OK;
}
esp_err_t esp_netif_sntp_start(void)
{
    assert(!critical && retained_count);
    ++started;
    if (scenario == START_REPLACE && started == 1) {
        replace_network();
        struct timeval tv = {.tv_sec = 1900000000};
        callback(&tv);
        uint64_t wall;
        assert(!app_clock_wall_time(&wall));
    }
    return ESP_OK;
}
void esp_netif_sntp_deinit(void)
{
    assert(!critical && retained_count);
    for (unsigned i = 0; i < retained_count; ++i) assert(!strcmp(retained[i], configured[i]));
    retained_count = 0;
    ++stopped;
}
static void scenario_step(const app_clock_snapshot_t *view)
{
    struct timeval tv = {.tv_sec = 1900000000};
    uint64_t wall;
    ++step;
    if (scenario == NETWORK_REPLACE || scenario == CONFIG_REPLACE ||
        scenario == INIT_REPLACE || scenario == START_REPLACE) {
        if (step == 1) {
            assert(!view->known && !view->synchronizing && resolutions == 1);
            assert(initialized == (scenario == INIT_REPLACE || scenario == START_REPLACE ? 1U : 0U));
            assert(started == (scenario == START_REPLACE ? 1U : 0U));
        } else if (step == 2) {
            assert(view->synchronizing && resolutions == 2 && !view->known);
            assert(!strcmp(configured[0], "192.0.2.20"));
            if (scenario == NETWORK_REPLACE) {
                assert(dns_clears == 1 && !cache_hits && cache_expires > clock_ms);
                assert(cancelled_dns == 2 && dns_sockets_closed == 1);
            }
            callback(&tv);
            assert(app_clock_wall_time(&wall));
        } else {
            assert(view->known && !view->synchronizing && created == 1);
            longjmp(finish, 1);
        }
    } else if (scenario == KNOWN_NETWORK_REPLACE) {
        if (step == 1) {
            assert(view->synchronizing); callback(&tv);
        } else if (step == 2) {
            assert(view->known && stopped == 1); clock_ms += 60000;
        } else {
            assert(view->known && !view->synchronizing && resolutions == 2 && initialized == 1 && started == 1);
            if (step == 4) longjmp(finish, 1);
        }
    } else if (scenario == ALTERNATE || scenario == MULTIPLE) {
        assert(resolutions == 2 && initialized == 1 && started == 1);
        assert(retained_count == (scenario == MULTIPLE ? 2U : 1U) || step == 2);
        if (step == 1) {
            assert(!strcmp(configured[scenario == MULTIPLE ? 1 : 0], "2001:db8::20"));
            callback(&tv);
        } else {
            assert(view->known && stopped == 1); longjmp(finish, 1);
        }
    } else {
        assert(!view->known && !view->synchronizing && resolutions == 1 && !initialized && !started);
        assert(view->error == (scenario == DNS_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL));
        if (step == 3) longjmp(finish, 1);
    }
    clock_ms += 100;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 100 && critical == 0);
    app_clock_snapshot_t view;
    uint64_t wall = 0;
    app_clock_snapshot(&view);
    if (scenario != BASE) { scenario_step(&view); return; }
    struct timeval tv = {.tv_sec = 1900000000, .tv_usec = 0};
    switch (++step) {
    case 1:
        assert(view.synchronizing && !view.known && view.unix_seconds == 0 &&
               view.age_ms == UINT32_MAX);
        app_clock_station_ready(true);
        assert(!app_clock_wall_time(&wall));
        tv.tv_sec = 1;
        callback(&tv);
        assert(!app_clock_wall_time(&wall));
        tv.tv_sec = 1900000000;
        callback(&tv);
        assert(app_clock_wall_time(&wall) && wall == 1900000000);
        /* The callback can publish a newer anchor while the worker is waiting
         * to take its first state snapshot on the next turn. */
        sync_before_lock = true;
        break;
    case 2:
        assert(stopped == 1 && view.known && !view.synchronizing &&
               !view.cryptographically_authenticated);
        tv.tv_sec = 2000000000;
        callback(&tv);
        assert(app_clock_wall_time(&wall) && wall == 1900000000);
        app_clock_station_ready(false);
        app_clock_station_ready(true);
        clock_ms += 100;
        break;
    case 3:
        assert(view.known && !view.synchronizing && initialized == 1 && stopped == 1);
        clock_ms += 60001;
        break;
    case 4:
        assert(!view.known && view.synchronizing && initialized == 2);
        app_clock_station_ready(false);
        callback(&tv);
        assert(!app_clock_wall_time(&wall));
        break;
    case 5:
        assert(!view.station_ready && !view.synchronizing && !view.known && stopped == 2);
        app_clock_station_ready(true);
        /* The new network can reach a time source even when the old one could
         * not. The regular refresh deadline is still nearly a minute away. */
        clock_ms += 100;
        break;
    case 6:
        assert(view.synchronizing && initialized == 3);
        sync_before_lock = true;
        assert(app_clock_wall_time(&wall));
        clock_ms = 99;
        assert(!app_clock_wall_time(&wall));
        break;
    case 7: {
        assert(!view.known);
        app_clock_config_t invalid = {.servers = {"clock.example/attack"}};
        assert(app_clock_init(&invalid) == ESP_ERR_INVALID_ARG);
        app_clock_config_t replacement = {.servers = {"replacement.example"},
                                          .maximum_age_ms = 60000,
                                          .refresh_interval_ms = 60000};
        assert(app_clock_init(&replacement) == ESP_OK);
        clock_ms += 100;
        break;
    }
    case 8:
        assert(view.synchronizing && !view.known && created == 1);
        callback(&tv);
        assert(app_clock_wall_time(&wall));
        ++clock_ms;
        break;
    default:
        assert(view.known && !view.cryptographically_authenticated && created == 1 && started == 4);
        longjmp(finish, 1);
    }
}
int main(void)
{
    /* Existing system time is deliberately irrelevant: no API reads it as
     * evidence. Only the registered SDK callback can establish an anchor. */
    uint64_t wall = 0;
    assert(!app_clock_wall_time(&wall));
    app_clock_config_t c = {
        .servers = {"clock.example"}, .maximum_age_ms = 60000, .refresh_interval_ms = 60000};
    fail_create = true;
    assert(app_clock_init(&c) == ESP_ERR_NO_MEM);
    app_clock_snapshot_t unavailable;
    app_clock_snapshot(&unavailable);
    assert(!unavailable.initialized && !unavailable.known && !state.worker_started && !created);
    assert((APP_NETWORK_EXTERNAL_STACK ? caps_creates : legacy_creates) == 1 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_creates : caps_creates) == 0);
    fail_create = false;
    assert(app_clock_init(&c) == ESP_OK);
    app_clock_station_ready(true);
    if (setjmp(finish) == 0) {
        worker(NULL);
    }
    assert((APP_NETWORK_EXTERNAL_STACK ? caps_creates : legacy_creates) == 2 &&
           (APP_NETWORK_EXTERNAL_STACK ? legacy_creates : caps_creates) == 0);
    assert(critical == 0);
    for (scenario_t test = NETWORK_REPLACE; test <= START_REPLACE; ++test) {
        memset(&state, 0, sizeof(state));
        memset(&s_network, 0, sizeof(s_network));
        memset(retained, 0, sizeof(retained));
        memset(configured, 0, sizeof(configured));
        retained_count = resolutions = initialized = started = stopped = step = created = 0;
        cached_old_answer = false; cache_expires = 0; cache_hits = 0;
        dns_clears = pending_dns = cancelled_dns = dns_sockets_closed = 0;
        callback = NULL; clock_ms = 100; scenario = test;
        app_clock_config_t config = {.servers = {"clock.example"},
            .maximum_age_ms = test == KNOWN_NETWORK_REPLACE ? 120000 : 60000,
            .refresh_interval_ms = 60000};
        if (test == ALTERNATE || test == MULTIPLE) {
            config.servers[0] = test == ALTERNATE ? "missing.example" : "clock.example";
            config.servers[1] = "ipv6.example";
        }
        assert(app_clock_init(&config) == ESP_OK);
        assert(station_network_enter(s_network.generation) == ESP_OK && !dns_clears);
        if (setjmp(finish) == 0) worker(NULL);
        assert(!critical && !retained_count && created == 1);
        if (test == CONFIG_REPLACE || test == ALTERNATE || test == MULTIPLE ||
            test == DNS_FAILURE || test == DNS_TIMEOUT) assert(!dns_clears);
    }
    puts("PASS: clock callback evidence, unknown boot/date, stale and regressed time, disconnect, "
         "late callback, numeric IPv4/IPv6 alternatives, delayed DNS replacement/configuration, "
         "attempt deadline, no busy retry and SDK admission rechecks");
    return 0;
}
