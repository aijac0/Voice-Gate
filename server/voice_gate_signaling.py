"""Local trusted signaling service for a single Voice Gate user."""

from __future__ import annotations

import argparse
import hmac
import http.client
import json
import os
import re
import socket
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable


OPEN_PATH = "/segments/open"
PROVIDER_CREATE_PATH = "/v1/live/sessions"
MAX_SDP_BYTES = 64 * 1024
MAX_BODY_BYTES = 256 * 1024
SESSION_ID = re.compile(r"[A-Za-z0-9_-]{1,256}")


class SignalingError(Exception):
    def __init__(self, status: HTTPStatus, code: str, provider_status: int | None = None):
        super().__init__(code)
        self.status = status
        self.code = code
        self.provider_status = provider_status

    def payload(self) -> dict:
        result = {"error": self.code}
        if self.provider_status is not None:
            result["provider_status"] = self.provider_status
        return result


def valid_sdp(value: object) -> bool:
    if not isinstance(value, str) or not value.strip():
        return False
    try:
        return len(value.encode("utf-8")) <= MAX_SDP_BYTES
    except UnicodeEncodeError:
        return False


def valid_id(value: object) -> bool:
    return isinstance(value, str) and SESSION_ID.fullmatch(value) is not None


@dataclass(frozen=True)
class OpeningOffer:
    sdp: str
    resume_from: str | None

    @classmethod
    def parse(cls, payload: object) -> OpeningOffer:
        if not isinstance(payload, dict) or payload.keys() - {"sdp", "resume_from"}:
            raise SignalingError(HTTPStatus.BAD_REQUEST, "invalid_offer")
        sdp = payload.get("sdp")
        source = payload.get("resume_from")
        if not valid_sdp(sdp) or (source is not None and not valid_id(source)):
            raise SignalingError(HTTPStatus.BAD_REQUEST, "invalid_offer")
        return cls(sdp, source)


class OpenAIRequest:
    def __init__(self, api_key: str):
        self._api_key = api_key

    def __call__(self, path: str, payload: dict) -> object:
        connection = http.client.HTTPSConnection("api.openai.com", timeout=10)
        try:
            connection.request("POST", path, body=json.dumps(payload).encode("utf-8"), headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
                "Accept": "application/json",
            })
            response = connection.getresponse()
            if response.status != HTTPStatus.CREATED:
                rejected = 400 <= response.status < 500 and response.status != HTTPStatus.REQUEST_TIMEOUT
                code = "provider_rejected" if rejected else "provider_outcome_unknown"
                raise SignalingError(HTTPStatus.BAD_GATEWAY, code, response.status)
            body = response.read(MAX_BODY_BYTES + 1)
            if len(body) > MAX_BODY_BYTES:
                raise SignalingError(HTTPStatus.BAD_GATEWAY, "provider_outcome_unknown")
            return json.loads(body)
        except (TimeoutError, socket.timeout) as error:
            raise SignalingError(HTTPStatus.GATEWAY_TIMEOUT, "provider_outcome_unknown") from error
        except (OSError, http.client.HTTPException, ValueError) as error:
            raise SignalingError(HTTPStatus.BAD_GATEWAY, "provider_outcome_unknown") from error
        finally:
            connection.close()


class LiveSignaling:
    def __init__(self, model: str, store: bool, post_json: Callable[[str, dict], object]):
        self._model = model
        self._store = store
        self._post_json = post_json

    def open(self, offer: OpeningOffer) -> dict:
        path = PROVIDER_CREATE_PATH
        session = {"store": self._store, "client": {"data_channel": {
            "allowed_client_events": ["session.close"],
        }}}
        if offer.resume_from is None:
            session["model"] = self._model
        else:
            if not self._store:
                raise SignalingError(HTTPStatus.BAD_REQUEST, "storage_disabled")
            path = f"{path}/{offer.resume_from}/fork"

        response = self._post_json(path, {
            "session": session,
            "transport": {"type": "webrtc", "sdp": offer.sdp},
        })
        try:
            session_id = response["session"]["id"]
            transport = response["transport"]
            sdp = transport["sdp"]
            if transport["type"] == "webrtc" and valid_id(session_id) and valid_sdp(sdp):
                return {"session_id": session_id, "sdp": sdp}
        except (KeyError, TypeError):
            pass
        raise SignalingError(HTTPStatus.BAD_GATEWAY, "provider_outcome_unknown")


def create_server(port: int, client_token: str, signaling: LiveSignaling) -> ThreadingHTTPServer:
    expected_auth = f"Bearer {client_token}".encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        timeout = 5

        def log_message(self, format, *args):
            # SDP and credentials are excluded from the prototype's HTTP logs.
            return

        def respond(self, status: HTTPStatus, payload: dict) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def read_offer(self) -> OpeningOffer:
            if self.headers.get_content_type() != "application/json":
                raise SignalingError(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "expect_json")
            length = self.headers.get("Content-Length")
            if length is None:
                raise SignalingError(HTTPStatus.LENGTH_REQUIRED, "expect_content_length")
            try:
                size = int(length)
                if size <= 0:
                    raise ValueError()
                if size > MAX_BODY_BYTES:
                    raise SignalingError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "body_too_large")
                body = self.rfile.read(size)
                if len(body) != size:
                    raise ValueError()
                return OpeningOffer.parse(json.loads(body))
            except (TimeoutError, socket.timeout) as error:
                raise SignalingError(HTTPStatus.REQUEST_TIMEOUT, "offer_read_timeout") from error
            except (ValueError, RecursionError) as error:
                raise SignalingError(HTTPStatus.BAD_REQUEST, "invalid_offer") from error

        def do_POST(self) -> None:
            if self.path != OPEN_PATH:
                self.respond(HTTPStatus.NOT_FOUND, {"error": "unknown_route"})
                return
            authorization = self.headers.get("Authorization", "").encode("utf-8")
            if not hmac.compare_digest(authorization, expected_auth):
                self.respond(HTTPStatus.UNAUTHORIZED, {"error": "unauthorized"})
                return
            try:
                answer = signaling.open(self.read_offer())
                self.respond(HTTPStatus.CREATED, answer)
            except SignalingError as error:
                self.respond(error.status, error.payload())

    return ThreadingHTTPServer(("127.0.0.1", port), Handler)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, help="Live model for a fresh segment")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--store", action="store_true", help="Enable storage and resumed segments")
    args = parser.parse_args()
    api_key = os.environ.get("OPENAI_API_KEY", "")
    client_token = os.environ.get("VOICE_GATE_CLIENT_TOKEN", "")
    if (not api_key or not client_token.isascii() or not client_token.isprintable() or
            any(character.isspace() for character in client_token) or client_token == api_key):
        parser.error("Set OPENAI_API_KEY and a distinct ASCII VOICE_GATE_CLIENT_TOKEN without spaces")
    if not args.model.strip() or not 1 <= args.port <= 65535:
        parser.error("Supply a model and a port between 1 and 65535")

    signaling = LiveSignaling(args.model, args.store, OpenAIRequest(api_key))
    with create_server(args.port, client_token, signaling) as server:
        print(f"Voice Gate signaling on http://127.0.0.1:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
