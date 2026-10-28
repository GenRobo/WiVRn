# Test TCP sink for the server's "app-data" relay (see docs/windows.md), used by app-data-test.ps1.
#
#   app_data_sink.py serve <port> <payload> <out-prefix> [--restart-after S] [--chunk N] [--rate N]
#       Listens on 127.0.0.1:<port>, accepts one connection at a time, sends <payload> to it in <chunk>
#       byte writes at <rate> writes per second and records what it receives in <out-prefix>-<n>.bin.
#       With --restart-after, the first connection and the listener are closed after S seconds and the
#       sink listens again 3 s later, to test the relay's reconnection.
#       With --stall-first S, the first connection neither sends nor reads for S seconds;
#       the relay must end that stream on queue overflow and reconnect without splicing byte gaps.
#   app_data_sink.py check <sent> <chunk> <received>...
#       Checks that each <received> file is a contiguous, <chunk> aligned slice of <sent> repeated,
#       i.e. what a sink sees of a probe replaying <sent> with PROBE_APP_DATA_REPEAT=1.
import socket
import sys
import time


def serve(args):
    port = int(args[0])
    payload = open(args[1], "rb").read()
    prefix = args[2]
    restart_after = None
    stall_first = 0
    chunk = 65536
    rate = 100
    i = 3
    while i < len(args):
        if args[i] == "--restart-after":
            restart_after = float(args[i + 1])
        elif args[i] == "--stall-first":
            stall_first = float(args[i + 1])
        elif args[i] == "--chunk":
            chunk = int(args[i + 1])
        elif args[i] == "--rate":
            rate = int(args[i + 1])
        i += 2

    connection = 0
    while True:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(1)
        print(f"sink: listening on 127.0.0.1:{port}", flush=True)
        client, address = listener.accept()
        connection += 1
        client.setblocking(False)
        print(f"sink: connection {connection} from {address}", flush=True)
        received = 0
        sent = 0
        started = time.monotonic()
        next_send = started
        out = open(f"{prefix}-{connection}.bin", "wb")
        while True:
            now = time.monotonic()
            if restart_after is not None and connection == 1 and now - started >= restart_after:
                print(f"sink: closing connection {connection} after {restart_after} s to test reconnection", flush=True)
                break
            if sent < len(payload) and now >= next_send and not (stall_first and connection == 1):
                next_send += 1 / rate
                piece = payload[sent:sent + chunk]
                client.setblocking(True)
                client.sendall(piece)
                client.setblocking(False)
                sent += len(piece)
                if sent == len(payload):
                    print(f"sink: payload sent, {sent} bytes", flush=True)
            if connection == 1 and now - started < stall_first:
                time.sleep(0.001)
                continue
            try:
                data = client.recv(1 << 20)
            except BlockingIOError:
                time.sleep(0.001)
                continue
            except ConnectionError as e:
                print(f"sink: connection {connection} error: {e}", flush=True)
                break
            if not data:
                print(f"sink: connection {connection} closed by peer", flush=True)
                break
            out.write(data)
            received += len(data)
        out.close()
        client.close()
        listener.close()
        print(f"sink: connection {connection} done, sent {sent} received {received}", flush=True)
        if restart_after is not None and connection == 1:
            time.sleep(3)
            continue
        if stall_first and connection == 1:
            continue
        return 0


def check(args):
    sent = open(args[0], "rb").read()
    chunk = int(args[1])
    ok = True
    for path in args[2:]:
        received = open(path, "rb").read()
        if not received:
            print(f"{path}: empty")
            ok = False
            continue
        repeated = sent * (len(received) // len(sent) + 2)
        # The relay drops whole packets before the sink is connected: the first byte is chunk aligned
        offsets = [o for o in range(0, len(sent), chunk) if repeated[o:o + len(received)] == received]
        if offsets:
            print(f"{path}: {len(received)} bytes, contiguous slice of the sent stream from offset {offsets[0]}")
        else:
            print(f"{path}: {len(received)} bytes, NOT a contiguous slice of the sent stream")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit({"serve": serve, "check": check}[sys.argv[1]](sys.argv[2:]))
