#!/usr/bin/env python3
"""Prints the fall alerts sent by the dot, to test them without a real server.

    python3 tools/alert_receiver.py [port]

Then set the alert URL in menuconfig to http://<your PC's IP>:8000/
"""
import json
import sys
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    seen = set()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            alert = json.loads(body)
            event_id = alert["event_id"]
            fell_at = datetime.now() - timedelta(milliseconds=alert["age_ms"])
        except (ValueError, KeyError, TypeError):
            self.send_response(400)
            self.end_headers()
            print(f"Rejected: {body!r}")
            return

        # A retry can arrive after a lost answer: it still gets a 200 so the board stops
        duplicate = event_id in Handler.seen
        Handler.seen.add(event_id)
        self.send_response(200)
        self.end_headers()
        label = "duplicate" if duplicate else "FALL"
        print(f"{label} at {fell_at:%H:%M:%S} from {alert.get('device')}: "
              f"peak {alert.get('peak_g')} g, posture changed {alert.get('angle_deg')} degrees "
              f"[{event_id}]", flush=True)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"Listening on port {port}", flush=True)
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
