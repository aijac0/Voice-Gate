"""Run the native client's integration test against the actual local HTTP service."""

import os
import subprocess
import sys
import threading
from http import HTTPStatus

from voice_gate_signaling import LiveSignaling, SignalingError, create_server


def main():
    requests = []
    responders = []

    def post(path, payload):
        requests.append(path)
        offer = payload["transport"]["sdp"]
        if "a=fingerprint:" in offer and path == "/v1/live/sessions":
            peer = subprocess.Popen([sys.argv[3], "--answer-stdio"], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE)
            responders.append(peer)
            encoded = offer.encode("utf-8")
            peer.stdin.write(f"{len(encoded)}\n".encode("ascii") + encoded)
            peer.stdin.flush()
            size = int(peer.stdout.readline())
            answer = peer.stdout.read(size)
            if not answer or len(answer) != size:
                raise AssertionError("The fixture peer did not supply a complete SDP answer")
            return {"session": {"id": "local-peer"},
                    "transport": {"type": "webrtc", "sdp": answer.decode("utf-8")}}
        if payload["transport"] != {"type": "webrtc", "sdp": "v=0\r\ns=cpp-client\r\n"}:
            raise AssertionError("The C++ SDP offer changed on the way to the provider")
        if path == "/v1/live/sessions":
            return {"session": {"id": "live_fresh"},
                    "transport": {"type": "webrtc", "sdp": "v=0\r\ns=fresh-answer\r\n"}}
        if path == "/v1/live/sessions/live_saved/fork":
            return {"session": {"id": "live_resumed"},
                    "transport": {"type": "webrtc", "sdp": "v=0\r\ns=fork-answer\r\n"}}
        raise SignalingError(HTTPStatus.BAD_GATEWAY, "provider_outcome_unknown", 503)

    with create_server(0, "test-client-token", LiveSignaling("fixture", True, post)) as server:
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
        worker.start()
        try:
            subprocess.run([sys.argv[1], str(server.server_port)], check=True, timeout=20)
            env = dict(os.environ, VOICE_GATE_CLIENT_TOKEN="test-client-token")
            subprocess.run([sys.argv[2], "--open-and-close", "--port", str(server.server_port),
                            "--bind-address", "127.0.0.1"], env=env, check=True, timeout=25)
            if len(responders) != 1:
                raise AssertionError("The live probe must complete one local opening and closure")
            responders[0].stdin.write(b"\n")
            responders[0].stdin.close()
            if responders[0].wait(timeout=6) != 0:
                raise AssertionError("The fixture peer did not confirm the probe exchange")
        finally:
            server.shutdown()
            worker.join()
            for peer in responders:
                if peer.poll() is None:
                    peer.kill()
                    peer.wait()
                peer.stdin.close()
                peer.stdout.close()

    if requests != ["/v1/live/sessions", "/v1/live/sessions/live_saved/fork",
                    "/v1/live/sessions/live_unknown/fork", "/v1/live/sessions"]:
        raise AssertionError("Unexpected provider requests from the native client")


if __name__ == "__main__":
    main()
