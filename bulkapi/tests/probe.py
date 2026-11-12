# Usage: python3 tests/probe.py PORT > out.txt; run against two builds and diff the outputs.
# Sends a fixed sequence of requests on one connection (plus some on fresh ones) and prints
# every response byte-for-byte, so two servers can be diffed.
import socket, sys, time
port = int(sys.argv[1])
def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(3); return s
def req(m, path, body=b"", extra=b""):
    h = f"{m} {path} HTTP/1.1\r\nHost: x\r\n".encode() + extra
    if body or m in ("POST", "PUT"): h += f"Content-Length: {len(body)}\r\n".encode()
    return h + b"\r\n" + body
def read_all(s, n_resp):
    buf = b""; out = []
    while len(out) < n_resp:
        try: d = s.recv(1 << 20)
        except socket.timeout: out.append(b"<timeout>"); break
        if not d: out.append(b"<eof>"); break
        buf += d
        while True:
            he = buf.find(b"\r\n\r\n")
            if he < 0: break
            hdr = buf[:he].decode()
            cl = 0
            for line in hdr.split("\r\n")[1:]:
                k, _, v = line.partition(":")
                if k.lower() == "content-length": cl = int(v.strip())
            if len(buf) < he + 4 + cl: break
            out.append(buf[:he + 4 + cl]); buf = buf[he + 4 + cl:]
    return out
s = conn()
seq = [
    req("GET", "/health"), req("GET", "/items/1"),
    req("PUT", "/items/1", b"hello"), req("GET", "/items/1"),
    req("PUT", "/items/2", b'quote " back \\ nl \n tab \t ctl \x01\x1f end'), req("GET", "/items/2"),
    req("PUT", "/items/3", b"x" * 48), req("GET", "/items/3"),
    req("PUT", "/items/4", b"y" * 49), req("GET", "/items/4"),
    req("PUT", "/items/4", b"short"), req("GET", "/items/4"),
    req("PUT", "/items/5", b"z" * 5000), req("GET", "/items/5"),
    req("PUT", "/items/0", b"zero"), req("GET", "/items/0"),
    req("PUT", "/items/18446744073709551615", b"max"), req("GET", "/items/18446744073709551615"),
    req("DELETE", "/items/1"), req("GET", "/items/1"), req("DELETE", "/items/1"),
    req("POST", "/items/bulk", b'{"id":10,"v":"a"}\n{"id":11,"v":"b\\"q\\nz"}\nbad line\n{"id":12, "v" : "c"}\r\n'),
    req("POST", "/items/bulk/get", b"10 11 12 13\n2\n0"),
    req("POST", "/items/bulk/get", "\n".join(str(i) for i in range(10, 400)).encode()),
    req("POST", "/items/bulk/delete", b"10,11,99"), req("POST", "/items/bulk/get", b"10 11 12"),
    req("GET", "/items/abc"), req("GET", "/nope"), req("POST", "/health"),
    req("GET", "/items/bulk"), req("PATCH", "/items/1"),
    req("GET", "/health", extra=b"connection: keep-alive\r\n"),
]
s.sendall(b"".join(seq))
for r in read_all(s, len(seq)): sys.stdout.buffer.write(r + b"\n----\n")
s.close()
# many inserts then delete half then get all (exercises table growth + backward-shift delete)
s = conn()
body = "".join(f'{{"id":{i * 7919},"v":"v{i}"}}\n' for i in range(20000)).encode()
s.sendall(req("POST", "/items/bulk", body)); [sys.stdout.buffer.write(r + b"\n----\n") for r in read_all(s, 1)]
s.sendall(req("POST", "/items/bulk/delete", " ".join(str(i * 7919) for i in range(0, 20000, 2)).encode())); [sys.stdout.buffer.write(r + b"\n----\n") for r in read_all(s, 1)]
s.sendall(req("POST", "/items/bulk/get", " ".join(str(i * 7919) for i in range(20000)).encode()))
r = read_all(s, 1)[0]; import hashlib; print("bulkget20000", len(r), hashlib.sha256(r).hexdigest())
s.close()
# errors that close
for raw in [b"GARBAGE\r\n\r\n", req("POST", "/items/bulk", extra=b"Transfer-Encoding: chunked\r\n"),
            b"PUT /items/1 HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", req("GET", "/health", extra=b"Connection: close\r\n"),
            b"GET /health HTTP/1.0\r\n\r\n"]:
    s = conn(); s.sendall(raw)
    for r in read_all(s, 2): sys.stdout.buffer.write(r + b"\n----\n")
    s.close()
# split request arriving in pieces
s = conn(); r = req("PUT", "/items/77", b"piecewise-body")
for i in range(0, len(r), 7): s.sendall(r[i:i+7]); time.sleep(0.002)
s.sendall(req("GET", "/items/77"))
for x in read_all(s, 2): sys.stdout.buffer.write(x + b"\n----\n")
