/* Phase 1: DNS Sink (UDP 53) + SNTP Server (UDP 123) in one select() task. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#define TAG "NET"
#define DNS_PORT 53
#define SNTP_PORT 123

/* DNS QTYPEs */
#define QTYPE_A  1
#define QTYPE_AAAA 28

/* Redirect all A queries to the SoftAP IP */
static const uint8_t SOFTAP_IP[4] = { 192, 168, 4, 1 };

/* RFC 1035: build a DNS response for a query */
static size_t dns_build_response(const uint8_t *query, size_t qlen, int rcode_nxdomain,
                                 const uint8_t *answer_ip, uint8_t *resp) {
    /* Copy header (12 bytes) */
    memcpy(resp, query, 12);
    /* Clean flags: QR=1, RD echoed from query; RA=1. */
    resp[2] = 0x81 | (query[2] & 0x01);
    resp[3] = rcode_nxdomain ? 0x83 : 0x80; /* NOERROR (0) or NXDOMAIN (3) */
    /* QDCOUNT = 1, ANCOUNT = 1 if answer, NSCOUNT = 0, ARCOUNT = 0 */
    resp[4] = 0; resp[5] = 1; /* QDCOUNT = 1 */
    resp[6] = 0; resp[7] = (answer_ip && !rcode_nxdomain) ? 1 : 0; /* ANCOUNT */
    resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0;

    /* Copy the question section (from offset 12) */
    size_t pos = 12;
    size_t q_end = pos;
    {
        size_t off = pos;
        while (off < qlen && query[off] != 0) {
            if (query[off] & 0xC0) { off += 2; break; }
            off += 1 + query[off];
        }
        if (off < qlen) off++;
        q_end = off + 4; /* QTYPE + QCLASS */
        if (q_end > qlen) q_end = qlen; /* bounds check */
    }
    if (q_end > pos) memcpy(resp + pos, query + pos, q_end - pos);
    pos += q_end - pos;

    /* Append answer RR if applicable */
    if (answer_ip && !rcode_nxdomain) {
        resp[pos++] = 0xC0; resp[pos++] = 0x0C; /* Name: pointer to offset 12 */
        resp[pos++] = 0x00; resp[pos++] = 0x01; /* TYPE=A */
        resp[pos++] = 0x00; resp[pos++] = 0x01; /* CLASS=IN */
        resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x3C; /* TTL=60 */
        resp[pos++] = 0x00; resp[pos++] = 0x04; /* RDLENGTH=4 */
        memcpy(resp + pos, answer_ip, 4);
        pos += 4;
    }
    return pos;
}

/* Extract hostname from DNS query */
static void dns_log_hostname(const uint8_t *query, size_t qlen, char *out, size_t out_len) {
    size_t off = 12, hpos = 0;
    out[0] = 0;
    while (off < qlen && hpos < out_len - 1) {
        uint8_t len = query[off++];
        if (len == 0) break;
        if (off + len > qlen) break;
        if (hpos > 0 && hpos < out_len - 1) out[hpos++] = '.';
        memcpy(out + hpos, query + off, len);
        hpos += len;
        off += len;
    }
    out[hpos] = 0;
}

/* SNTP: build a 48-byte NTP server response */
static void sntp_build_response(const uint8_t *req, uint8_t *resp) {
    memset(resp, 0, 48);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t sec_be = htonl((uint32_t)tv.tv_sec + 2208988800UL);
    uint32_t frac_be = htonl((uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000ULL));

    resp[0] = 0x24; /* LI=0, VN=4, Mode=4 */
    resp[1] = 2;    /* Stratum = 2 */
    resp[2] = 4;    /* Poll */
    resp[3] = 0xEC; /* Precision */

    /* Copy client transmit timestamp (req + 40) to our originate (resp + 24) */
    memcpy(resp + 24, req + 40, 8);

    /* Reference (16), Receive (32), Transmit (40) = sec + frac big-endian */
    memcpy(resp + 16, &sec_be, 4); memcpy(resp + 20, &frac_be, 4);
    memcpy(resp + 32, &sec_be, 4); memcpy(resp + 36, &frac_be, 4);
    memcpy(resp + 40, &sec_be, 4); memcpy(resp + 44, &frac_be, 4);
}

static void net_services_task(void *arg) {
    int dns_sock, sntp_sock;

    /* Seed system clock to 2026-11-21 00:00:00 UTC */
    struct timeval tv = { .tv_sec = 1795219200, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "Clock set to %" PRId64 " (2026-11-21 00:00:00 UTC)", (int64_t)tv.tv_sec);

    /* Create UDP sockets */
    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) { ESP_LOGE(TAG, "DNS socket create failed"); vTaskDelay(1000); return; }
    sntp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sntp_sock < 0) { ESP_LOGE(TAG, "SNTP socket create failed"); vTaskDelay(1000); return; }

    struct sockaddr_in dns_addr = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(dns_sock, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed on %d", DNS_PORT);
        vTaskDelay(1000); return;
    }
    struct sockaddr_in sntp_addr = { .sin_family = AF_INET, .sin_port = htons(SNTP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sntp_sock, (struct sockaddr *)&sntp_addr, sizeof(sntp_addr)) < 0) {
        ESP_LOGE(TAG, "SNTP bind failed on %d", SNTP_PORT);
        vTaskDelay(1000); return;
    }
    ESP_LOGI(TAG, "DNS sink on UDP 53, SNTP on UDP 123");

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(dns_sock, &rfds);
        FD_SET(sntp_sock, &rfds);
        int maxfd = (dns_sock > sntp_sock) ? dns_sock : sntp_sock;
        struct timeval to = { 1, 0 }; /* 1s timeout */
        int r = select(maxfd + 1, &rfds, NULL, NULL, &to);
        if (r <= 0) continue;

        if (FD_ISSET(dns_sock, &rfds)) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            uint8_t buf[512];
            int n = recvfrom(dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
            if (n > 12 && FD_ISSET(dns_sock, &rfds)) {
                /* Parse QTYPE */
                uint8_t resp[512];
                size_t rlen;
                /* Read QTYPE from offset 32? Actually QTYPE is at offset 12 + name_len + ... */
                /* We parse the name to find QTYPE. For simplicity, read first 2 bytes after question. */
                /* Find end of question name */
                size_t off = 12;
                while (off < n && buf[off] != 0) {
                    if (buf[off] & 0xC0) { off += 2; break; }
                    off += 1 + buf[off];
                }
                if (off < n) off++;
                if (off + 4 <= n) {
                    uint16_t qtype = ntohs(*(uint16_t *)(buf + off));
                    char hostname[128];
                    dns_log_hostname(buf, n, hostname, sizeof(hostname));
                    if (qtype == QTYPE_A) {
                        rlen = dns_build_response(buf, n, 0, SOFTAP_IP, resp);
                        ESP_LOGI(TAG, "DNS A %s -> 192.168.4.1", hostname);
                    } else if (qtype == QTYPE_AAAA) {
                        rlen = dns_build_response(buf, n, 0, NULL, resp);
                        ESP_LOGI(TAG, "DNS AAAA %s -> empty (no IPv6)", hostname);
                    } else {
                        rlen = dns_build_response(buf, n, 1, NULL, resp);
                        ESP_LOGI(TAG, "DNS %s type %d -> NXDOMAIN", hostname, qtype);
                    }
                    sendto(dns_sock, resp, rlen, 0, (struct sockaddr *)&src, sizeof(src));
                }
            }
        }

        if (FD_ISSET(sntp_sock, &rfds)) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            uint8_t buf[48];
            int n = recvfrom(sntp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
            if (n >= 48) {
                uint8_t resp[48];
                sntp_build_response(buf, resp);
                sendto(sntp_sock, resp, 48, 0, (struct sockaddr *)&src, sizeof(src));
                ESP_LOGI(TAG, "SNTP query from %s -> replied", inet_ntoa(src.sin_addr));
            }
        }
    }
}

void net_services_init(void) {
    xTaskCreate(&net_services_task, "net_services", 4096, NULL, 5, NULL);
}
