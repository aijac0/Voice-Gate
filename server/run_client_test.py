"""Run the native client's integration test against the actual local HTTP service."""

import subprocess
import sys
import threading
from http import HTTPStatus

from voice_gate_signaling import LiveSignaling, SignalingError, create_server


def main():
    requests = []

    def post(path, payload):
        requests.append(path)
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
        finally:
            server.shutdown()
            worker.join()

    if requests != ["/v1/live/sessions", "/v1/live/sessions/live_saved/fork",
                    "/v1/live/sessions/live_unknown/fork"]:
        raise AssertionError("Unexpected provider requests from the native client")


if __name__ == "__main__":
    main()
