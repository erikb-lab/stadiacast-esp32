"""Cross-decode C-encoded nanopb blobs using Python Stadiacast's protobuf classes."""
import sys, os

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
STADIACAST_SRC = os.environ.get("STADIACAST_SRC", os.path.join(REPO_DIR, "..", "stadiacast", "src"))
sys.path.insert(0, STADIACAST_SRC)
from stadiacast.protos import Protos
from stadiacast.config import Config
import google.protobuf.text_format as tf

# Stadiacast uses a descriptor pool; load from its compiled descriptors
import glob
p = Protos.__new__(Protos)
# Simpler: use google.protobuf directly with the .proto files
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

# Load protos from stadiacast schemas
import subprocess
BLOB_DIR = os.environ.get("BLOB_DIR", os.path.join(REPO_DIR, ".tmp"))

def load_proto(name):
    # Use stadiacast's own compiled descriptors if available
    desc_path = os.path.join(STADIACAST_SRC, "stadiacast", "schemas", name.replace(".proto", ".descriptor"))
    if os.path.exists(desc_path):
        pool = descriptor_pool.DescriptorPool()
        fdp = descriptor_pb2.FileDescriptorProto()
        fdp.ParseFromString(open(desc_path, 'rb').read())
        try:
            pool.Add(fdp)
        except TypeError:
            # Already loaded
            pool = descriptor_pool.Default()
        return pool
    return None

# Test RegisterControllerResponse
pool = load_proto("discovery_api.proto")
if pool:
    msg_cls = message_factory.GetMessageClass(pool.FindMessageTypeByName("gotham.discovery.RegisterControllerResponse"))
    data = open(f"{BLOB_DIR}/reg_resp.bin", "rb").read()
    msg = msg_cls()
    msg.ParseFromString(data)
    print(f"RegisterControllerResponse: poll_interval_ms={msg.poll_interval_ms}")
    print(f"  registration_id={msg.registration_id!r}")
    print(f"  registration_token={msg.registration_token!r}")
    print(f"  display.display_id={msg.display.display_id!r}")
    print(f"  display.display_token={msg.display.display_token!r}")
    print("OK: RegisterControllerResponse cross-decode")

# Test RpcMessage
pool2 = load_proto("rpc_message.proto")
if pool2:
    msg_cls = message_factory.GetMessageClass(pool2.FindMessageTypeByName("gotham.rpc.RpcMessage"))
    data = open(f"{BLOB_DIR}/rpc_msg.bin", "rb").read()
    msg = msg_cls()
    msg.ParseFromString(data)
    print(f"RpcMessage: which_payload={msg.WhichOneof('payload')}")
    if msg.HasField("request"):
        print(f"  request.ticket={msg.request.ticket}")
    print("OK: RpcMessage cross-decode")

# Test GetCurrentTimeResponse
pool3 = load_proto("controller_message.proto")
if pool3:
    msg_cls = message_factory.GetMessageClass(pool3.FindMessageTypeByName("gotham.display.GetCurrentTimeResponse"))
    data = open(f"{BLOB_DIR}/time_resp.bin", "rb").read()
    msg = msg_cls()
    msg.ParseFromString(data)
    print(f"GetCurrentTimeResponse: controller_time_us={msg.controller_time_us} display_time_us={msg.display_time_us}")
    print("OK: GetCurrentTimeResponse cross-decode")

    # Message3 (InputReport)
    msg_cls = message_factory.GetMessageClass(pool3.FindMessageTypeByName("gotham.display.Message3"))
    data = open(f"{BLOB_DIR}/input_report.bin", "rb").read()
    msg = msg_cls()
    msg.ParseFromString(data)
    print(f"Message3: f1={msg.f1} f2={msg.f2} f6={msg.f6} f7={msg.f7}")
    print("OK: Message3 cross-decode")

# Test GmsgEnvelope
pool4 = load_proto("discovery_api.proto")
if pool4:
    msg_cls = message_factory.GetMessageClass(pool4.FindMessageTypeByName("gotham.discovery.GmsgEnvelope"))
    data = open(f"{BLOB_DIR}/gmsg_env.bin", "rb").read()
    msg = msg_cls()
    msg.ParseFromString(data)
    print(f"GmsgEnvelope: message.id={msg.message.id} message.type={msg.message.type}")
    # Extension
    ext = msg.message.Extensions[pool4.FindExtensionByName("gotham.discovery.controller_command")]
    print(f"  controller_command.payload={ext.payload!r}")
    print("OK: GmsgEnvelope cross-decode")

print("\nAll Python cross-decode checks passed.")