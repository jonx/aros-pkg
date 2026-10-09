#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# The progress events, as a program receives them through pkg_sink.event:
# build/test_events drives libpkg through pkg.h and checks every event of
# every operation it runs (see the top of tests/test_events.c). This script
# gives it what it cannot make itself: an HTTP server that answers from the
# test's directory in three ways, chosen by the first part of the path,
#
#   /known/<path>     with a Content-Length
#   /chunked/<path>   chunked, so the whole is not known
#   /slow/<path>      with a Content-Length, after holding the headers back
#
# and a 404 for a file that is not there.

set -u
TEST_EVENTS=${TEST_EVENTS:-./build/test_events}
[ -x "$TEST_EVENTS" ] || { echo "events: missing $TEST_EVENTS (make build/test_events)" >&2; exit 69; }
command -v python3 > /dev/null 2>&1 || { echo "events: needs python3" >&2; exit 69; }
T=$(mktemp -d "${TMPDIR:-/tmp}/pkg-events.XXXXXX")
server=
stop() {
    [ -n "$server" ] && { kill $server 2>/dev/null; wait $server 2>/dev/null; }
    chmod -R u+w "$T" 2>/dev/null
    rm -rf "$T"
}
trap stop EXIT

python3 - "$T" > "$T/server.log" 2>&1 <<'PY' &
import http.server, os, sys, time
top = os.path.realpath(sys.argv[1])
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a):
        pass
    def do_GET(self):
        mode, _, rel = self.path.lstrip("/").partition("/")
        path = os.path.realpath(os.path.join(top, "srv", rel))
        if mode not in ("known", "chunked", "slow") or not path.startswith(top) or not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        data = open(path, "rb").read()
        if mode == "slow":
            time.sleep(0.3)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        if mode == "chunked":
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if mode == "chunked":
            for at in range(0, len(data), 4096):
                piece = data[at:at + 4096]
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.wfile.write(data)
        self.wfile.flush()
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(os.path.join(top, "port"), "w").write(str(s.server_port))
s.serve_forever()
PY
server=$!
n=0
while [ ! -s "$T/port" ] && [ "$n" -lt 100 ]; do n=$((n + 1)); sleep 0.1; done
[ -s "$T/port" ] || { echo "events: the test server did not start" >&2; cat "$T/server.log" >&2; exit 69; }

mkdir -p "$T/srv"
EVENTS_DIR="$T/srv" EVENTS_HTTP="http://127.0.0.1:$(cat "$T/port")" PKG_CACHE="$T/cache" \
    "$TEST_EVENTS"
