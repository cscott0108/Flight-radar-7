#pragma once
/* 0.1.8: which network interface an HTTP request arrived on.
 *
 * The device always runs the open "Flight-Radar-Setup" access point (APSTA, WIFI_AUTH_OPEN) next to its
 * station connection, and the WebUI has no login. Features that expose captured console output (log files) are
 * therefore limited to requests that arrived on the STATION interface, i.e. from the user's own Wi-Fi network.
 *
 * Decision is made from the socket's LOCAL address (getsockname on the request socket): the device address the
 * client connected to. The httpd socket is dual-stack (CONFIG_LWIP_IPV6), so IPv4 clients appear as IPv4-mapped
 * IPv6 addresses; both forms are handled. Anything that is not exactly the current station IPv4 address is
 * treated as "not trusted" (fails closed): the setup-AP address, native IPv6, no station IP yet, lookup errors. */
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WEBIF_UNKNOWN = 0, /* could not be determined, or an address that is neither interface: not trusted */
    WEBIF_STATION,     /* the user's Wi-Fi network: trusted */
    WEBIF_SETUP_AP,    /* the open setup access point: not trusted */
} WebIf;

/* Pure classifier (host-tested). sa/len: a struct sockaddr_in or sockaddr_in6 as returned by getsockname.
 * staIp/apIp: IPv4 addresses in network byte order as stored by lwIP (0 = interface has no address). */
WebIf WebAccess_Classify(const void *sa, size_t len, uint32_t staIp, uint32_t apIp);
const char *WebIf_Name(WebIf w);

/* The interface this request arrived on (ESP: getsockname + esp_netif addresses; host tests stub it). */
struct httpd_req;
WebIf WebAccess_RequestInterface(struct httpd_req *req);
