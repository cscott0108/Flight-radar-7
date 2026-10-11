#include "web_access.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

const char *WebIf_Name(WebIf w)
{
    switch (w) {
    case WEBIF_STATION: return "your Wi-Fi network (station)";
    case WEBIF_SETUP_AP: return "the open setup access point";
    default: return "an unidentified interface";
    }
}

WebIf WebAccess_Classify(const void *sa, size_t len, uint32_t staIp, uint32_t apIp)
{
    if (!sa || len < sizeof(sa_family_t))
        return WEBIF_UNKNOWN;
    uint32_t local = 0; /* network byte order */
    const sa_family_t fam = ((const struct sockaddr *)sa)->sa_family;
    if (fam == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        memcpy(&local, &in->sin_addr, 4);
    } else if (fam == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, mapped, sizeof(mapped)) != 0)
            return WEBIF_UNKNOWN; /* native IPv6: not used by this firmware's WebUI; fail closed */
        memcpy(&local, b + 12, 4);
    } else {
        return WEBIF_UNKNOWN;
    }
    if (local == 0)
        return WEBIF_UNKNOWN;
    if (apIp != 0 && local == apIp)
        return WEBIF_SETUP_AP;
    if (staIp != 0 && local == staIp)
        return WEBIF_STATION;
    return WEBIF_UNKNOWN;
}

#ifdef ESP_PLATFORM
static uint32_t NetifIp(const char *key)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(key);
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK)
        return ip.ip.addr;
    return 0;
}

WebIf WebAccess_RequestInterface(struct httpd_req *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0)
        return WEBIF_UNKNOWN;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0)
        return WEBIF_UNKNOWN;
    return WebAccess_Classify(&ss, (size_t)len, NetifIp("WIFI_STA_DEF"), NetifIp("WIFI_AP_DEF"));
}
#endif
