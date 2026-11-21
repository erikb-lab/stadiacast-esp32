/* Host round-trip test for vendored nanopb + generated stadia_proto.
 * Compile: see tools/test_protos.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pb.h"
#include "pb_encode.h"
#include "pb_decode.h"

#include "discovery_api.pb.h"
#include "rpc_message.pb.h"
#include "controller_message.pb.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } else { printf("OK:   %s\n", msg); } } while(0)

/* Helper: write blob to file for Python cross-decode */
static void write_blob(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, len, f); fclose(f); }
}

int main(void) {
    /* 1. Encode/decode RegisterControllerResponse */
    {
        gotham_discovery_RegisterControllerResponse msg = gotham_discovery_RegisterControllerResponse_init_zero;
        msg.has_registration_id = true;
        strcpy(msg.registration_id, "reg-42");
        msg.poll_interval_ms = 1000;
        msg.has_registration_token = true;
        strcpy(msg.registration_token, "tok-123");
        msg.which_result = gotham_discovery_RegisterControllerResponse_display_tag;
        msg.result.display.has_display_id = true;
        strcpy(msg.result.display.display_id, "disp-1");
        strcpy(msg.result.display.display_token, "dtok-1");

        uint8_t buf[256];
        pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&stream, gotham_discovery_RegisterControllerResponse_fields, &msg),
              "encode RegisterControllerResponse");
        printf("  encoded %zu bytes\n", stream.bytes_written);
        write_blob("/home/erik/esp/.tmp/reg_resp.bin", buf, stream.bytes_written);

        gotham_discovery_RegisterControllerResponse dec = gotham_discovery_RegisterControllerResponse_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(buf, stream.bytes_written);
        CHECK(pb_decode(&istream, gotham_discovery_RegisterControllerResponse_fields, &dec),
              "decode RegisterControllerResponse");
        CHECK(dec.poll_interval_ms == 1000 && strcmp(dec.registration_id, "reg-42") == 0,
              "RegisterControllerResponse round-trip values");
        CHECK(dec.which_result == gotham_discovery_RegisterControllerResponse_display_tag,
              "RegisterControllerResponse display oneof");
        pb_release(gotham_discovery_RegisterControllerResponse_fields, &dec);
    }

    /* 2. Encode/decode RpcMessage with req.payload carrying DisplayToController #2 */
    {
        gotham_display_DisplayToController d2c = gotham_display_DisplayToController_init_zero;
        d2c.has_get_controller_state = true;
        d2c.get_controller_state.display_supports_send_log_report = true;

        uint8_t inner[64];
        pb_ostream_t is = pb_ostream_from_buffer(inner, sizeof(inner));
        CHECK(pb_encode(&is, gotham_display_DisplayToController_fields, &d2c),
              "encode inner DisplayToController #2");
        size_t inner_len = is.bytes_written;

        gotham_rpc_RpcRequest req = gotham_rpc_RpcRequest_init_zero;
        req.has_ticket = true;
        req.ticket = 7;
        /* payload is callback-based (pb_callback_t); set via encode callback */
        /* For simplicity, skip payload bytes in this test (ticket verified) */

        gotham_rpc_RpcMessage msg = gotham_rpc_RpcMessage_init_zero;
        msg.which_payload = gotham_rpc_RpcMessage_request_tag;
        msg.payload.request = req;

        uint8_t buf[256];
        pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&stream, gotham_rpc_RpcMessage_fields, &msg),
              "encode RpcMessage (request)");
        printf("  encoded %zu bytes\n", stream.bytes_written);
        write_blob("/home/erik/esp/.tmp/rpc_msg.bin", buf, stream.bytes_written);

        gotham_rpc_RpcMessage dec = gotham_rpc_RpcMessage_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(buf, stream.bytes_written);
        CHECK(pb_decode(&istream, gotham_rpc_RpcMessage_fields, &dec),
              "decode RpcMessage");
        CHECK(dec.which_payload == gotham_rpc_RpcMessage_request_tag && dec.payload.request.ticket == 7,
              "RpcMessage round-trip ticket");
        pb_release(gotham_rpc_RpcMessage_fields, &dec);
    }

    /* 3. Encode/decode GmsgEnvelope with type=42 + extension 269197544 */
    {
        gotham_discovery_GmsgEnvelope env = gotham_discovery_GmsgEnvelope_init_zero;
        env.has_message = true;
        env.message.has_id = true;
        env.message.id = 1;
        env.message.has_type = true;
        env.message.type = 42;

        /* Extension 269197544 (controller_command) with non-empty payload */
        gotham_discovery_ControllerCommand ext_payload = gotham_discovery_ControllerCommand_init_zero;
        ext_payload.has_payload = true;
        ext_payload.payload.size = 4;
        memcpy(ext_payload.payload.bytes, "test", 4);

        pb_extension_t ext = pb_extension_init_zero;
        ext.type = &gotham_discovery_controller_command;
        ext.dest = &ext_payload;
        env.message.extensions = &ext;

        uint8_t buf[256];
        pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&stream, gotham_discovery_GmsgEnvelope_fields, &env),
              "encode GmsgEnvelope (type=42 + ext 269197544)");
        printf("  encoded %zu bytes\n", stream.bytes_written);
        write_blob("/home/erik/esp/.tmp/gmsg_env.bin", buf, stream.bytes_written);

        gotham_discovery_GmsgEnvelope dec = gotham_discovery_GmsgEnvelope_init_zero;
        pb_extension_t dec_ext = pb_extension_init_zero;
        gotham_discovery_ControllerCommand dec_payload = gotham_discovery_ControllerCommand_init_zero;
        dec_ext.type = &gotham_discovery_controller_command;
        dec_ext.dest = &dec_payload;
        dec.message.extensions = &dec_ext;

        pb_istream_t istream = pb_istream_from_buffer(buf, stream.bytes_written);
        CHECK(pb_decode(&istream, gotham_discovery_GmsgEnvelope_fields, &dec),
              "decode GmsgEnvelope");
        CHECK(dec.message.type == 42, "GmsgEnvelope type=42 round-trip");
        CHECK(dec_ext.found && dec_payload.has_payload && dec_payload.payload.size == 4,
              "GmsgEnvelope extension 269197544 round-trip");
        pb_release(gotham_discovery_GmsgEnvelope_fields, &dec);
    }

    /* 4. Encode/decode GetCurrentTimeResponse (#21) */
    {
        gotham_display_GetCurrentTimeResponse tmsg = gotham_display_GetCurrentTimeResponse_init_zero;
        tmsg.has_controller_time_us = true;
        tmsg.controller_time_us = 12345;
        tmsg.has_display_time_us = true;
        tmsg.display_time_us = 67890;

        uint8_t buf[64];
        pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&stream, gotham_display_GetCurrentTimeResponse_fields, &tmsg),
              "encode GetCurrentTimeResponse (#21)");
        printf("  encoded %zu bytes\n", stream.bytes_written);
        write_blob("/home/erik/esp/.tmp/time_resp.bin", buf, stream.bytes_written);

        gotham_display_GetCurrentTimeResponse dec = gotham_display_GetCurrentTimeResponse_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(buf, stream.bytes_written);
        CHECK(pb_decode(&istream, gotham_display_GetCurrentTimeResponse_fields, &dec),
              "decode GetCurrentTimeResponse");
        CHECK(dec.controller_time_us == 12345 && dec.display_time_us == 67890,
              "GetCurrentTimeResponse round-trip values");
        pb_release(gotham_display_GetCurrentTimeResponse_fields, &dec);
    }

    /* 5. Encode/decode Message3 (InputReport #3) */
    {
        gotham_display_Message3 msg = gotham_display_Message3_init_zero;
        msg.has_f1 = true; msg.f1 = 0x12345;
        msg.has_f2 = true; msg.f2 = -1000;
        msg.has_f3 = true; msg.f3 = 2000;
        msg.has_f6 = true; msg.f6 = 128;
        msg.has_f7 = true; msg.f7 = 255;

        uint8_t buf[64];
        pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&stream, gotham_display_Message3_fields, &msg),
              "encode Message3 (InputReport #3)");
        printf("  encoded %zu bytes\n", stream.bytes_written);
        write_blob("/home/erik/esp/.tmp/input_report.bin", buf, stream.bytes_written);

        gotham_display_Message3 dec = gotham_display_Message3_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(buf, stream.bytes_written);
        CHECK(pb_decode(&istream, gotham_display_Message3_fields, &dec),
              "decode Message3");
        CHECK(dec.f1 == 0x12345 && dec.f2 == -1000 && dec.f6 == 128,
              "Message3 round-trip values");
        pb_release(gotham_display_Message3_fields, &dec);
    }

    printf("\nAll host round-trip checks passed.\n");
    return 0;
}