idf_component_register(
    SRCS "discovery_api.pb.c" "webrtc_sdp.pb.c" "rpc_message.pb.c" "controller_message.pb.c" "streamer_message.pb.c" "input.pb.c"
    INCLUDE_DIRS "."
    REQUIRES nanopb
)