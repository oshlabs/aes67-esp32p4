/* Host implementation of the two address helpers aes67_sdp.c uses */
#include "aes67_net.h"
#include <arpa/inet.h>
#include <string.h>

uint32_t aes67_net_ip_to_u32(const char *ip_str)
{
    struct in_addr a;
    return inet_aton(ip_str, &a) ? a.s_addr : 0;
}

void aes67_net_u32_to_ip(uint32_t ip, char *buf, size_t len)
{
    struct in_addr a = { .s_addr = ip };
    strncpy(buf, inet_ntoa(a), len - 1);
    buf[len - 1] = '\0';
}
