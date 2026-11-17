#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"

// IDF's dual-stack HTTP listener returns IPv4 clients as mapped IPv6 sockets.
// Normalize only that representation; native IPv6 is outside this IPv4 API.
static inline bool passport_http_local_ipv4(int socket, struct in_addr *address)
{
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    if (getsockname(socket, (struct sockaddr *)&local, &length)) return false;
    if (length == sizeof(struct sockaddr_in) && local.ss_family == AF_INET) {
        struct sockaddr_in ipv4;
        memcpy(&ipv4, &local, sizeof(ipv4));
        *address = ipv4.sin_addr;
        return true;
    }
#if !defined(LWIP_IPV6) || LWIP_IPV6
    if (length == sizeof(struct sockaddr_in6) && local.ss_family == AF_INET6) {
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        struct sockaddr_in6 ipv6;
        memcpy(&ipv6, &local, sizeof(ipv6));
        if (memcmp(ipv6.sin6_addr.s6_addr, mapped_prefix, sizeof(mapped_prefix))) return false;
        memcpy(&address->s_addr, ipv6.sin6_addr.s6_addr + sizeof(mapped_prefix), sizeof(address->s_addr));
        return true;
    }
#endif
    return false;
}
