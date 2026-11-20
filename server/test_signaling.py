import http.client
import json
import threading
import unittest
from unittest.mock import patch

from voice_gate_signaling import (
    MAX_BODY_BYTES,
    MAX_SDP_BYTES,
    OPEN_PATH,
    LiveSignaling,
    OpenAIRequest,
    OpeningOffer,
    SignalingError,
    create_server,
)


OFFER = "v=0\r\ns=voice-gate-offer\r\n"
ANSWER = "v=0\r\ns=voice-gate-answer\r\n"
PROVIDER_ANSWER = {
    "session": {"id": "live_new", "internal": "excluded"},
    "transport": {"type": "webrtc", "sdp": ANSWER},
}


class SignalingContract(unittest.TestCase):
    def setUp(self):
        self.requests = []

    def post(self, path, payload):
        self.requests.append((path, payload))
        return PROVIDER_ANSWER

    def test_create_and_fork_use_their_respective_contracts(self):
        signaling = LiveSignaling("fixture-model", True, self.post)
        fresh = signaling.open(OpeningOffer.parse({"sdp": OFFER}))
        resumed = signaling.open(OpeningOffer.parse({"sdp": OFFER, "resume_from": "live_saved"}))
        for result in (fresh, resumed):
            self.assertEqual(result, {"session_id": "live_new", "sdp": ANSWER})

        (create_path, create), (fork_path, fork) = self.requests
        self.assertEqual(create_path, "/v1/live/sessions")
        self.assertEqual(fork_path, "/v1/live/sessions/live_saved/fork")
        self.assertEqual(create["session"]["model"], "fixture-model")
        self.assertNotIn("model", fork["session"])
        for request in (create, fork):
            self.assertTrue(request["session"]["store"])
            self.assertEqual(request["transport"], {"type": "webrtc", "sdp": OFFER})
            self.assertEqual(request["session"]["client"]["data_channel"]["allowed_client_events"],
                             ["session.close"])

    def test_storage_disabled_keeps_the_request_fresh(self):
        signaling = LiveSignaling("fixture-model", False, self.post)
        signaling.open(OpeningOffer.parse({"sdp": OFFER}))
        self.assertFalse(self.requests[0][1]["session"]["store"])
        with self.assertRaisesRegex(SignalingError, "storage_disabled"):
            signaling.open(OpeningOffer.parse({"sdp": OFFER, "resume_from": "live_saved"}))
        self.assertEqual(len(self.requests), 1)

    def test_invalid_offer_is_rejected_before_calling_provider(self):
        for offer in ([], {"sdp": []}, {"sdp": " "}, {"sdp": OFFER, "model": "override"},
                      {"sdp": OFFER, "resume_from": "../wrong"}, {"sdp": OFFER, "resume_from": False},
                      {"sdp": "x" * (MAX_SDP_BYTES + 1)}, {"sdp": "\ud800"}):
            with self.subTest(offer_type=type(offer)):
                with self.assertRaisesRegex(SignalingError, "invalid_offer"):
                    OpeningOffer.parse(offer)

    def test_missing_provider_answer_preserves_the_unknown_outcome(self):
        signaling = LiveSignaling("fixture-model", False, lambda *_: {"session": {"id": "live_new"}})
        with self.assertRaisesRegex(SignalingError, "provider_outcome_unknown"):
            signaling.open(OpeningOffer.parse({"sdp": OFFER}))

    @patch("voice_gate_signaling.http.client.HTTPSConnection")
    def test_provider_authentication_stays_in_one_https_request(self, connect):
        connection = connect.return_value
        response = connection.getresponse.return_value
        response.status = 201
        response.read.return_value = json.dumps(PROVIDER_ANSWER).encode("utf-8")
        post = OpenAIRequest("fixture-provider-key")

        self.assertEqual(post("/path", {"transport": "fixture"}), PROVIDER_ANSWER)
        connect.assert_called_once_with("api.openai.com", timeout=10)
        connection.request.assert_called_once()
        args, keywords = connection.request.call_args
        self.assertEqual(args, ("POST", "/path"))
        self.assertEqual(keywords["headers"]["Authorization"], "Bearer fixture-provider-key")
        self.assertEqual(json.loads(keywords["body"]), {"transport": "fixture"})
        connection.close.assert_called_once()

    @patch("voice_gate_signaling.http.client.HTTPSConnection")
    def test_missing_provider_response_does_not_retry(self, connect):
        connection = connect.return_value
        connection.getresponse.side_effect = TimeoutError()
        with self.assertRaises(SignalingError) as failure:
            OpenAIRequest("fixture-key")("/path", {})
        self.assertEqual(failure.exception.status, 504)
        self.assertEqual(failure.exception.payload(), {"error": "provider_outcome_unknown"})
        connection.request.assert_called_once()
        connection.close.assert_called_once()

    @patch("voice_gate_signaling.http.client.HTTPSConnection")
    def test_provider_rejection_preserves_status_without_remote_body(self, connect):
        response = connect.return_value.getresponse.return_value
        response.status = 404
        with self.assertRaises(SignalingError) as failure:
            OpenAIRequest("fixture-key")("/path", {})
        self.assertEqual(failure.exception.status, 502)
        self.assertEqual(failure.exception.payload(), {"error": "provider_rejected", "provider_status": 404})
        response.read.assert_not_called()


class LocalHttpBoundary(unittest.TestCase):
    def setUp(self):
        self.requests = []

        def post(path, payload):
            self.requests.append((path, payload))
            return PROVIDER_ANSWER

        self.server = create_server(0, "local-test-token", LiveSignaling("test", True, post))
        self.addCleanup(self.server.server_close)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02})
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.thread.join()

    def request_offer(self, body, token="local-test-token", content_length=None):
        headers = {"Content-Type": "application/json", "Authorization": f"Bearer {token}"}
        if content_length is not None:
            headers["Content-Length"] = str(content_length)
        host, port = self.server.server_address
        connection = http.client.HTTPConnection(host, port, timeout=2)
        try:
            connection.request("POST", OPEN_PATH, body, headers)
            response = connection.getresponse()
            return response.status, response.getheader("Cache-Control"), json.loads(response.read())
        finally:
            connection.close()

    def test_authorized_offer_returns_only_the_id_and_answer(self):
        status, cache, result = self.request_offer(json.dumps({"sdp": OFFER}))
        self.assertEqual((status, cache), (201, "no-store"))
        self.assertEqual(result, {"session_id": "live_new", "sdp": ANSWER})
        self.assertEqual(len(self.requests), 1)

    def test_invalid_requests_cannot_reach_provider(self):
        self.assertEqual(self.request_offer("{}", token="wrong")[0], 401)
        self.assertEqual(self.request_offer("{bad JSON")[0], 400)
        self.assertEqual(self.request_offer("", content_length=MAX_BODY_BYTES + 1)[0], 413)
        self.assertEqual(self.requests, [])


if __name__ == "__main__":
    unittest.main()
