"""Checks a server's responses as TechEmpower's verification does, for bench/web/run.sh.

usage: python3 -I verify.py PORT

For /plaintext and /json: a 200, the expected body, Content-Type, a Content-Length that matches the body, and Server
and Date headers. Also that three pipelined requests get three responses.
"""

import email.utils
import json
import socket
import sys

REQUEST = "GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n"


def read_response(conn, buffered):
    """Reads one response from conn, after buffered bytes: returns (status, headers, body, the bytes after it)."""
    while b"\r\n\r\n" not in buffered:
        chunk = conn.recv(65536)
        if not chunk:
            raise ValueError("connection closed before the end of the headers")
        buffered += chunk

    head, buffered = buffered.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()

    if "content-length" not in headers:
        raise ValueError("no Content-Length")
    length = int(headers["content-length"])
    while len(buffered) < length:
        chunk = conn.recv(65536)
        if not chunk:
            raise ValueError("connection closed before the end of the body")
        buffered += chunk

    return status, headers, buffered[:length], buffered[length:]


def check(port, path, content_type, check_body):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as conn:
        conn.sendall(REQUEST.format(path=path).encode())
        status, headers, body, _ = read_response(conn, b"")

    problems = []
    if status != 200:
        problems.append(f"status {status}")
    if not headers.get("content-type", "").startswith(content_type):
        problems.append(f"Content-Type {headers.get('content-type')!r}, not {content_type}")
    if "server" not in headers:
        problems.append("no Server")
    if "date" not in headers or email.utils.parsedate_to_datetime(headers["date"]) is None:
        problems.append(f"Date {headers.get('date')!r}")
    if not check_body(body):
        problems.append(f"body {body!r}")
    return [f"{path}: {p}" for p in problems]


def check_pipelining(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as conn:
        conn.sendall(REQUEST.format(path="/plaintext").encode() * 3)
        buffered = b""
        for i in range(3):
            status, _, body, buffered = read_response(conn, buffered)
            if status != 200 or body != b"Hello, World!":
                return [f"pipelined response {i + 1}: status {status}, body {body!r}"]
    return []


def main():
    port = int(sys.argv[1])
    problems = []
    try:
        problems += check(port, "/plaintext", "text/plain", lambda b: b == b"Hello, World!")
        problems += check(port, "/json", "application/json", lambda b: json.loads(b) == {"message": "Hello, World!"})
        problems += check_pipelining(port)
    except (OSError, ValueError) as err:
        problems.append(str(err))

    for problem in problems:
        print(f"  verify: {problem}")
    sys.exit(1 if problems else 0)


main()
