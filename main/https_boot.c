/* Phase 1: HTTPS boot server on port 443 + lightweight HTTP on port 80 (1..4 controllers). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "mbedtls/ssl.h"
#include "pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "discovery_api.pb.h"
#include "webrtc_sdp.pb.h"
#include "stadia_signaling.h"
#include "stadia_transport.h"
#include "web_ui.h"
#include "mbedtls/sha256.h"

#define TAG "HTTPS"

extern const char fullchain_pem_start[] asm("_binary_fullchain_pem_start");
extern const char fullchain_pem_end[]   asm("_binary_fullchain_pem_end");
extern const char privkey_pem_start[]   asm("_binary_privkey_pem_start");
extern const char privkey_pem_end[]     asm("_binary_privkey_pem_end");

static mbedtls_x509_crt sp_cert;

static uint32_t get_req_peer_ip(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0) return 0;
    if (ss.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        return s6->sin6_addr.un.u32_addr[3];
    }
    return 0;
}

/* Format the cert DER SHA-256 as "sha-256 AA:BB:CC:..." (like Stadiacast). */
static void cert_fingerprint_hex(char *buf, size_t len) {
    uint8_t hash[32];
    mbedtls_sha256(sp_cert.raw.p, sp_cert.raw.len, hash, 0);
    for (size_t i = 0; i < 32 && len > 2; i++) {
        snprintf(buf, len, "%s%02X", i ? ":" : "", hash[i]);
        size_t l = strlen(buf); buf += l; len -= l;
    }
    *buf = 0;
}

/* ---------- Response helpers ---------- */
static void http_date_header(char *buf, size_t len) {
    struct tm utc_time;
    time_t now = time(NULL);
    gmtime_r(&now, &utc_time);
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &utc_time);
}

static esp_err_t send_response(httpd_req_t *req, const char *status, const char *body,
                               size_t body_len, const char *content_type, const char *extra_hdr) {
    char date[64];
    http_date_header(date, sizeof(date));
    httpd_resp_set_status(req, status);
    if (content_type) httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Date", date);
    if (extra_hdr) httpd_resp_set_hdr(req, "Connection", extra_hdr);
    if (body_len > 0) {
        httpd_resp_send(req, body, body_len);
    } else {
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

/* ---------- Handlers ---------- */
static esp_err_t get_generate_204(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /generate_204");
    char date[64]; http_date_header(date, sizeof(date));
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Date", date);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_stadiaconfig(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /cast/chromecast/device/stadiaconfig");
    return send_response(req, "200 OK", "{}", 2, "application/json", NULL);
}

static esp_err_t post_update2(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /service/update2");
    const char *xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gupdate protocol=\"3.0\"><app><updatecheck status=\"noupdate\"/></app></gupdate>";
    return send_response(req, "200 OK", xml, strlen(xml), "text/xml", NULL);
}

static esp_err_t post_clearcut(httpd_req_t *req) {
    /* Drain request body cleanly before closing so lwIP never sends a TCP RST */
    char drain[512];
    int rem = req->content_len;
    while (rem > 0) {
        int r = httpd_req_recv(req, drain, rem > (int)sizeof(drain) ? (int)sizeof(drain) : rem);
        if (r <= 0) break;
        rem -= r;
    }
    ESP_LOGI(TAG, "POST /log/batch");
    return send_response(req, "200 OK", "", 0, NULL, "close");
}

static esp_err_t post_firmware_check(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /v1:FirmwareVersionCheck");
    gotham_discovery_FirmwareVersionCheckResponse *resp = calloc(1, sizeof(*resp));
    if (!resp) return send_response(req, "503", "", 0, "application/x-protobuf", NULL);
    resp->has_requires_update = true;
    resp->requires_update = false;

    uint8_t buf[64];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, gotham_discovery_FirmwareVersionCheckResponse_fields, resp)) {
        free(resp);
        return send_response(req, "500 Internal Server Error", "encode fail", 0, NULL, NULL);
    }
    free(resp);
    return send_response(req, "200 OK", (char *)buf, stream.bytes_written, "application/x-protobuf", NULL);
}

static void build_sdp_answer(int slot,
                             chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates *bundle,
                             const uint8_t *offer_bytes, size_t offer_len);

static esp_err_t post_register_controller(httpd_req_t *req) {
    static char body[4096];
    size_t body_len = req->content_len;
    if (body_len > sizeof(body) - 1) body_len = sizeof(body) - 1;
    ssize_t read = 0;
    while (read < (ssize_t)body_len) {
        ssize_t r = httpd_req_recv(req, body + read, body_len - read);
        if (r <= 0) break;
        read += r;
    }
    if (read > 0) body[read] = 0;

    char dev_id[64] = {0}, cloud_id[64] = {0};
    httpd_req_get_hdr_value_str(req, "controller-device-id", dev_id, sizeof(dev_id));
    httpd_req_get_hdr_value_str(req, "controller-cloud-device-id", cloud_id, sizeof(cloud_id));

    int slot = stadia_signaling_resolve_slot(dev_id, NULL, get_req_peer_ip(req));
    stadia_session_t *sess = &stadia_sessions[slot];

    ESP_LOGI(TAG, "[P%d] POST /v1:RegisterController dev_id=%s cloud_id=%s body_len=%u read=%d",
             slot + 1, dev_id, cloud_id, (unsigned)req->content_len, (int)read);

    gotham_discovery_RegisterControllerRequest *req_msg = calloc(1, sizeof(*req_msg));
    if (!req_msg) return send_response(req, "503", "", 0, NULL, NULL);
    gotham_discovery_RegisterControllerResponse *resp = calloc(1, sizeof(*resp));
    if (!resp) { free(req_msg); return send_response(req, "503", "", 0, NULL, NULL); }

    if (read > 0) {
        pb_istream_t istream = pb_istream_from_buffer((const uint8_t *)body, (size_t)read);
        if (!pb_decode(&istream, gotham_discovery_RegisterControllerRequest_fields, req_msg)) {
            char errmsg[128] = "(no errmsg)";
            if (istream.errmsg) snprintf(errmsg, sizeof(errmsg), "%s", istream.errmsg);
            ESP_LOGW(TAG, "[P%d] RegisterController: decode failed: %s", slot + 1, errmsg);
        }
    }

    resp->has_registration_id = true;
    snprintf(resp->registration_id, sizeof(resp->registration_id), "local-%.40s", dev_id[0] ? dev_id : "controller");
    resp->poll_interval_ms = 250;
    resp->has_registration_token = true;
    snprintf(resp->registration_token, sizeof(resp->registration_token), "stadia-esp32");
    if (dev_id[0]) snprintf(sess->device_id, sizeof(sess->device_id), "%s", dev_id);
    snprintf(sess->registration_id, sizeof(sess->registration_id), "%.63s", resp->registration_id);
    sess->registered = true;

    if (req_msg->has_discovery) {
        stadia_signaling_auto_link(sess);
        resp->which_result = gotham_discovery_RegisterControllerResponse_display_tag;
        snprintf(resp->result.display.display_id, sizeof(resp->result.display.display_id), "%s", sess->display_id);
        snprintf(resp->result.display.display_token, sizeof(resp->result.display.display_token), "%s", sess->display_token);

        if (req_msg->discovery.sdp_offer_bundle.size > 0) {
            ESP_LOGI(TAG, "[P%d] Autolink offer bundle: %u bytes -> queue SDP answer on GMMsg",
                     slot + 1, (unsigned)req_msg->discovery.sdp_offer_bundle.size);
            chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates *bundle = calloc(1, sizeof(*bundle));
            if (bundle) {
                build_sdp_answer(slot, bundle, req_msg->discovery.sdp_offer_bundle.bytes, req_msg->discovery.sdp_offer_bundle.size);
                static uint8_t bundle_buf[2048];
                pb_ostream_t ostream = pb_ostream_from_buffer(bundle_buf, sizeof(bundle_buf));
                if (pb_encode(&ostream, chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates_fields, bundle)) {
                    ESP_LOGI(TAG, "[P%d] Queued SDP answer bundle: %zu bytes", slot + 1, ostream.bytes_written);
                    stadia_signaling_queue_gmsg(sess, bundle_buf, ostream.bytes_written);
                }
                free(bundle);
            }
        }
    }

    static uint8_t resp_buf[512];
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_buf, sizeof(resp_buf));
    bool ok = pb_encode(&ostream, gotham_discovery_RegisterControllerResponse_fields, resp);
    free(req_msg); free(resp);
    if (!ok) {
        return send_response(req, "500 Internal Server Error", "", 0, NULL, NULL);
    }
    return send_response(req, "200 OK", (char *)resp_buf, ostream.bytes_written, "application/x-protobuf", NULL);
}

static void build_sdp_answer(int slot,
                             chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates *bundle,
                             const uint8_t *offer_bytes, size_t offer_len) {
    memset(bundle, 0, sizeof(*bundle));
    bundle->has_sdp = true;
    chrome_cloudcast_proto_api_common_WebRtcSdp *sdp = &bundle->sdp;
    sdp->has_type = true;
    sdp->type = chrome_cloudcast_proto_api_common_WebRtcSdp_Type_SDP_ANSWER;
    sdp->has_sdp_answer = true;
    chrome_cloudcast_proto_api_common_RtcSdpAnswer *a = &sdp->sdp_answer;

    bool raw_dtls = true;
    bool delayed_acks = false;
    bool use_rtp_ts = false;
    if (offer_bytes && offer_len > 0) {
        chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates *ob = calloc(1, sizeof(*ob));
        if (ob) {
            pb_istream_t is = pb_istream_from_buffer(offer_bytes, offer_len);
            if (pb_decode(&is, chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates_fields, ob)) {
                const chrome_cloudcast_proto_api_common_RtcSdpOffer *o = &ob->sdp.sdp_offer;
                if (o->has_raw_dtls) raw_dtls = o->raw_dtls;
                if (o->has_delayed_acks) delayed_acks = o->delayed_acks;
                if (o->has_use_rtp_timestamps) use_rtp_ts = o->use_rtp_timestamps;
                const char *cand_ip = NULL;
                uint16_t cand_port = 0;
                for (pb_size_t i = 0; i < ob->ice_candidate_count; i++) {
                    const chrome_cloudcast_proto_api_common_RtcIceCandidate *ic = &ob->ice_candidate[i].ice_candidate;
                    if (ic->has_local_endpoint_ip && strchr(ic->local_endpoint_ip, ':') == NULL && ic->local_endpoint_port > 0) {
                        cand_ip = ic->local_endpoint_ip;
                        cand_port = (uint16_t)ic->local_endpoint_port;
                        break;
                    }
                }
                ESP_LOGI(TAG, "[P%d] Decoded offer: ufrag=%s raw_dtls=%d delayed_acks=%d rtp_ts=%d cand=%s:%u",
                         slot + 1, o->ice_ufrag, (int)raw_dtls, (int)delayed_acks, (int)use_rtp_ts,
                         cand_ip ? cand_ip : "(none)", (unsigned)cand_port);
                stadia_transport_set_remote_slot(slot, o->ice_ufrag, o->ice_password, cand_ip, cand_port);
            } else {
                ESP_LOGW(TAG, "[P%d] Offer bundle decode failed: %s", slot + 1, is.errmsg ? is.errmsg : "(null)");
            }
            free(ob);
        }
    }

    a->has_ice_ufrag = true;
    snprintf(a->ice_ufrag, sizeof(a->ice_ufrag), "stadia"); /* MUST be <= 7 chars for controller nanopb */
    a->has_ice_password = true;
    snprintf(a->ice_password, sizeof(a->ice_password), "stadiaesp32pwd");
    a->has_fingerprint = true;
    {
        char fp[96];
        cert_fingerprint_hex(fp, sizeof(fp));
        snprintf(a->fingerprint, sizeof(a->fingerprint), "sha-256 %s", fp);
    }
    a->has_raw_dtls = true;
    a->raw_dtls = raw_dtls;
    a->has_delayed_acks = true;
    a->delayed_acks = delayed_acks;
    a->has_use_rtp_timestamps = true;
    a->use_rtp_timestamps = use_rtp_ts;
    a->has_network_type = true;
    a->network_type = chrome_cloudcast_proto_api_common_RtcSdpNetworkType_NETWORK_TYPE_ETHERNET;
    a->has_disable_game_stats = true;
    a->disable_game_stats = true;
    a->has_disable_frame_tokens = true;
    a->disable_frame_tokens = true;

    /* Add ICE host candidate: our WiFi IP + slot Display UDP port (50000 + slot*4) */
    {
        chrome_cloudcast_proto_api_common_WebRtcSdp *cand = &bundle->ice_candidate[bundle->ice_candidate_count];
        cand->has_type = true;
        cand->type = chrome_cloudcast_proto_api_common_WebRtcSdp_Type_ICE_CANDIDATE;
        cand->has_ice_candidate = true;
        chrome_cloudcast_proto_api_common_RtcIceCandidate *c = &cand->ice_candidate;
        c->has_sdp_mid = true;
        snprintf(c->sdp_mid, sizeof(c->sdp_mid), "0");
        c->has_sdp_mline_index = true;
        c->sdp_mline_index = 0;
        c->has_foundation = true;
        snprintf(c->foundation, sizeof(c->foundation), "1");
        c->has_priority = true;
        c->priority = (126u << 24) | (65535u << 8) | 255u;
        c->has_local_endpoint_ip = true;
        snprintf(c->local_endpoint_ip, sizeof(c->local_endpoint_ip), "192.168.4.1");
        c->has_local_endpoint_port = true;
        c->local_endpoint_port = STADIA_SLOT_DISP_PORT(slot);
        bundle->ice_candidate_count++;
    }
}

static esp_err_t post_discover_display(httpd_req_t *req) {
    static char body[2048];
    size_t body_len = req->content_len;
    if (body_len > sizeof(body) - 1) body_len = sizeof(body) - 1;
    ssize_t read = 0;
    while (read < (ssize_t)body_len) {
        ssize_t r = httpd_req_recv(req, body + read, body_len - read);
        if (r <= 0) break;
        read += r;
    }
    if (read > 0) body[read] = 0;

    int slot = stadia_signaling_resolve_slot(NULL, NULL, get_req_peer_ip(req));
    stadia_session_t *sess = &stadia_sessions[slot];
    ESP_LOGI(TAG, "[P%d] POST /v1:DiscoverDisplay body_len=%u read=%d", slot + 1, (unsigned)read, (int)read);

    gotham_discovery_DiscoverDisplayRequest *req_msg = calloc(1, sizeof(*req_msg));
    if (!req_msg) return send_response(req, "503", "", 0, NULL, NULL);
    if (read > 0) {
        pb_istream_t istream = pb_istream_from_buffer((const uint8_t *)body, (size_t)read);
        if (!pb_decode(&istream, gotham_discovery_DiscoverDisplayRequest_fields, req_msg)) {
            char errmsg[128] = "(no errmsg)";
            if (istream.errmsg) snprintf(errmsg, sizeof(errmsg), "%s", istream.errmsg);
            ESP_LOGW(TAG, "[P%d] DiscoverDisplay: decode failed: %s", slot + 1, errmsg);
        }
    }

    if (req_msg->has_sdp_offer_bundle && req_msg->sdp_offer_bundle.size > 0) {
        chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates *bundle = calloc(1, sizeof(*bundle));
        if (bundle) {
            build_sdp_answer(slot, bundle, req_msg->sdp_offer_bundle.bytes, req_msg->sdp_offer_bundle.size);
            static uint8_t bundle_buf[2048];
            pb_ostream_t ostream = pb_ostream_from_buffer(bundle_buf, sizeof(bundle_buf));
            if (pb_encode(&ostream, chrome_cloudcast_proto_api_common_WebRtcSdpAndIceCandidates_fields, bundle)) {
                ESP_LOGI(TAG, "[P%d] Queued SDP answer bundle: %zu bytes", slot + 1, ostream.bytes_written);
                stadia_signaling_queue_gmsg(sess, bundle_buf, ostream.bytes_written);
            }
            free(bundle);
        }
    }
    free(req_msg);

    gotham_discovery_DiscoverDisplayResponse *resp = calloc(1, sizeof(*resp));
    if (!resp) return send_response(req, "503", "", 0, NULL, NULL);
    snprintf(resp->display_id, sizeof(resp->display_id), "%s", sess->display_id);
    snprintf(resp->display_token, sizeof(resp->display_token), "%s", sess->display_token);
    if (!sess->has_display) stadia_signaling_auto_link(sess);

    static uint8_t resp_buf[512];
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_buf, sizeof(resp_buf));
    bool ok = pb_encode(&ostream, gotham_discovery_DiscoverDisplayResponse_fields, resp);
    free(resp);
    if (!ok) return send_response(req, "500 Internal Server Error", "", 0, NULL, NULL);
    return send_response(req, "200 OK", (char *)resp_buf, ostream.bytes_written, "application/x-protobuf", NULL);
}

static esp_err_t post_resolve_discovery(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /v1:ResolveDiscovery");
    return send_response(req, "200 OK", "", 0, NULL, NULL);
}

static void gmsg_send_envelope(httpd_req_t *req, const uint8_t *msg_buf, size_t msg_len, uint32_t msg_id) {
    uint8_t *env_buf = malloc(2048);
    gotham_discovery_GmsgEnvelope *env = calloc(1, sizeof(*env));
    gotham_discovery_ControllerCommand *cmd = calloc(1, sizeof(*cmd));
    pb_extension_t *ext = calloc(1, sizeof(*ext));
    if (!env_buf || !env || !cmd || !ext) {
        free(env_buf); free(env); free(cmd); free(ext);
        send_response(req, "503 Service Unavailable", "", 0, "application/x-protobuf", NULL);
        return;
    }

    env->has_message = true;
    env->message.has_id = true;
    env->message.id = msg_id;
    env->message.has_type = true;
    env->message.type = 42;

    cmd->has_payload = true;
    cmd->payload.size = msg_len;
    memcpy(cmd->payload.bytes, msg_buf, msg_len);

    ext->type = &gotham_discovery_controller_command;
    ext->dest = cmd;
    ext->found = true;
    ext->next = NULL;
    env->message.extensions = ext;

    pb_ostream_t ostream = pb_ostream_from_buffer(env_buf, 2048);
    if (!pb_encode(&ostream, gotham_discovery_GmsgEnvelope_fields, env)) {
        ESP_LOGE(TAG, "gmsg: encode failed: %s", ostream.errmsg ? ostream.errmsg : "(none)");
        send_response(req, "200 OK", "", 0, "application/x-protobuf", NULL);
    } else {
        ESP_LOGI(TAG, "gmsg: sending envelope (%zu bytes, id=%lu, type=42)",
                 ostream.bytes_written, (unsigned long)msg_id);
        send_response(req, "200 OK", (char *)env_buf, ostream.bytes_written, "application/x-protobuf", NULL);
    }
    free(env_buf); free(env); free(cmd); free(ext);
}

static void gmsg_worker_task(void *arg) {
    httpd_req_t *req = (httpd_req_t *)arg;
    uint8_t *msg_buf = malloc(GMMSG_MAX);
    if (!msg_buf) {
        send_response(req, "503 Service Unavailable", "", 0, "application/x-protobuf", NULL);
        httpd_req_async_handler_complete(req);
        vTaskDelete(NULL);
        return;
    }

    int slot = stadia_signaling_resolve_slot(NULL, NULL, get_req_peer_ip(req));
    stadia_session_t *sess = &stadia_sessions[slot];

    size_t msg_len = 0;
    uint32_t msg_id = 1;
    bool has = stadia_signaling_gmsg_wait(sess, 25000, msg_buf, GMMSG_MAX, &msg_len, &msg_id);
    if (!has) {
        send_response(req, "200 OK", "", 0, "application/x-protobuf", NULL);
    } else {
        gmsg_send_envelope(req, msg_buf, msg_len, msg_id);
    }
    free(msg_buf);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t gmsg_poll(httpd_req_t *req) {
    char *body = malloc(1024);
    if (!body) return send_response(req, "503 Service Unavailable", "", 0, "application/x-protobuf", NULL);

    size_t body_len = req->content_len;
    if (body_len > 1023) body_len = 1023;
    ssize_t read = 0;
    while (read < (ssize_t)body_len) {
        ssize_t r = httpd_req_recv(req, body + read, body_len - read);
        if (r <= 0) break;
        read += r;
    }

    int slot = 0;
    gotham_discovery_GmsgPollRequest *poll_req = calloc(1, sizeof(*poll_req));
    if (poll_req && read > 0) {
        pb_istream_t istream = pb_istream_from_buffer((const uint8_t *)body, (size_t)read);
        if (!pb_decode(&istream, gotham_discovery_GmsgPollRequest_fields, poll_req)) {
            ESP_LOGW(TAG, "gmsg: decode failed: %s", istream.errmsg ? istream.errmsg : "(none)");
        }
        slot = stadia_signaling_resolve_slot(NULL, poll_req->subscription, get_req_peer_ip(req));
        ESP_LOGI(TAG, "[P%d] POST /gmsg poll: subscription=%s has_ack=%d ack_id=%lld",
                 slot + 1, poll_req->subscription, poll_req->has_last_message, (long long)poll_req->last_message.id);
        stadia_signaling_mark_acked(&stadia_sessions[slot], poll_req->last_message.id);
    } else {
        slot = stadia_signaling_resolve_slot(NULL, NULL, get_req_peer_ip(req));
    }
    free(poll_req);
    free(body);

    stadia_session_t *sess = &stadia_sessions[slot];

    /* Fast synchronous path: reply immediately if SDP answer is already queued (zero task spawn) */
    uint8_t *msg_buf = malloc(GMMSG_MAX);
    if (msg_buf) {
        size_t msg_len = 0;
        uint32_t msg_id = 1;
        if (stadia_signaling_gmsg_wait(sess, 0, msg_buf, GMMSG_MAX, &msg_len, &msg_id)) {
            gmsg_send_envelope(req, msg_buf, msg_len, msg_id);
            free(msg_buf);
            return ESP_OK;
        }
        free(msg_buf);
    }

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        ESP_LOGE(TAG, "gmsg: httpd_req_async_handler_begin failed");
        return send_response(req, "503 Service Unavailable", "", 0, "application/x-protobuf", NULL);
    }

    if (xTaskCreate(gmsg_worker_task, "gmsg_worker", 8192, async_req, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "gmsg: xTaskCreate failed");
        send_response(async_req, "503 Service Unavailable", "", 0, "application/x-protobuf", NULL);
        httpd_req_async_handler_complete(async_req);
    }
    return ESP_OK;
}

/* ---------- HTTP on port 80 (connectivity check) ---------- */
static esp_err_t http80_generate_204(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /generate_204 (HTTP 80)");
    return send_response(req, "204 No Content", "", 0, NULL, NULL);
}

static const httpd_uri_t http80_uri = {
    .uri = "/generate_204",
    .method = HTTP_GET,
    .handler = http80_generate_204,
};

static httpd_handle_t http80 = NULL;

static void https_boot_init_http80(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;
    if (httpd_start(&http80, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed on :80");
        return;
    }
    httpd_register_uri_handler(http80, &http80_uri);
    web_ui_register(http80);
    ESP_LOGI(TAG, "HTTP server + Web UI/OTA on :80");
}

static const httpd_uri_t uris[] = {
    { .uri = "/generate_204",                          .method = HTTP_GET,  .handler = get_generate_204 },
    { .uri = "/cast/chromecast/device/stadiaconfig",   .method = HTTP_GET,  .handler = get_stadiaconfig },
    { .uri = "/service/update2",                       .method = HTTP_POST, .handler = post_update2 },
    { .uri = "/log/batch",                             .method = HTTP_POST, .handler = post_clearcut },
    { .uri = "/v1:FirmwareVersionCheck",               .method = HTTP_POST, .handler = post_firmware_check },
    { .uri = "/v1:RegisterController",                 .method = HTTP_POST, .handler = post_register_controller },
    { .uri = "/v1:DiscoverDisplay",                    .method = HTTP_POST, .handler = post_discover_display },
    { .uri = "/v1:ResolveDiscovery",                   .method = HTTP_POST, .handler = post_resolve_discovery },
    { .uri = "/gmsg",                                  .method = HTTP_POST, .handler = gmsg_poll },
};

static const size_t NUM_URIS = sizeof(uris) / sizeof(uris[0]);

static void https_boot_task(void *arg) {
    mbedtls_x509_crt_init(&sp_cert);
    int cert_rc = mbedtls_x509_crt_parse(&sp_cert, (const unsigned char *)fullchain_pem_start,
                           (size_t)(fullchain_pem_end - fullchain_pem_start));
    ESP_LOGI(TAG, "cert parse rc=%d, first_cert_ok=%d", cert_rc, sp_cert.next != NULL);

    https_boot_init_http80();

    httpd_ssl_config_t httpd_ssl = HTTPD_SSL_CONFIG_DEFAULT();
    httpd_ssl.httpd.stack_size = 20480;
    httpd_ssl.httpd.task_priority = 6;
    httpd_ssl.httpd.max_uri_handlers = 16;
    httpd_ssl.httpd.max_open_sockets = 10;
    httpd_ssl.httpd.lru_purge_enable = true;
    httpd_ssl.httpd.ctrl_port = 32769;
    httpd_ssl.port_secure = 443;
    httpd_ssl.servercert = (const uint8_t *)fullchain_pem_start;
    httpd_ssl.servercert_len = (size_t)(fullchain_pem_end - fullchain_pem_start);
    httpd_ssl.prvtkey_pem = (const uint8_t *)privkey_pem_start;
    httpd_ssl.prvtkey_len = (size_t)(privkey_pem_end - privkey_pem_start);
    httpd_ssl.cert_select_cb = NULL;

    httpd_handle_t server = NULL;
    if (httpd_ssl_start(&server, &httpd_ssl) != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS start failed on :443");
        vTaskDelay(1000); return;
    }

    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTPS server on :443, HTTP server on :80");
    while (1) vTaskDelay(1000);
}

void https_boot_init(void) {
    xTaskCreate(&https_boot_task, "https_boot", 16384, NULL, 6, NULL);
}
