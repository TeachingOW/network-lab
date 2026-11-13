# Timeout behaviour. Start the server with all four timeouts at 1 s:
#   build/server -p 18103 --idle-timeout 1 --header-timeout 1 --body-timeout 1 --write-timeout 1
#   python3 tests/timeouts.py 18103          (exit status 0 = all passed)
import socket, sys, time, threading
port = int(sys.argv[1]); fails = 0
def conn(rcvbuf=None):
    s = socket.socket()
    if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect(("127.0.0.1", port)); return s
def wait_close(s, limit=4.0):
    """returns (seconds until EOF/RST, bytes received)"""
    t0 = time.time(); got = b""; s.settimeout(limit)
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            got += d
    except socket.timeout: return None, got
    except ConnectionResetError: pass
    return time.time() - t0, got
def check(name, ok, info):
    global fails
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {info}"); fails += not ok

s = conn(); t, _ = wait_close(s); check("idle, no request", t is not None and 0.8 < t < 1.6, f"closed after {t}")
s = conn(); s.sendall(b"GET /health HTTP/1.1\r\n\r\n"); time.sleep(0.05); s.recv(1000)
t, _ = wait_close(s); check("idle after a request", t is not None and 0.8 < t < 1.6, f"closed after {t}")

s = conn(); t0 = time.time(); got = b""
def drip(s=s):
    try:
        for ch in b"GET /health HTTP/1.1\r\nX-A: " + b"a" * 100:
            s.send(bytes([ch])); time.sleep(0.2)
    except OSError: pass
threading.Thread(target=drip, daemon=True).start()
t, got = wait_close(s); check("header sent one byte per 0.2 s (slowloris)", t is not None and t < 1.6 and b" 408 " in got, f"closed after {t}, got {got[:30]!r}")

s = conn(); s.sendall(b"PUT /items/9 HTTP/1.1\r\nContent-Length: 1000\r\n\r\n0123456789")
t, got = wait_close(s); check("body stalls", t is not None and 0.8 < t < 1.6 and b" 408 " in got, f"closed after {t}, got {got[:30]!r}")

s = conn(); s.sendall(b"PUT /items/9 HTTP/1.1\r\nContent-Length: 8\r\n\r\n"); ok = True
for i in range(8):
    time.sleep(0.5); s.send(b"x")
s.settimeout(2); r = s.recv(1000); check("body trickles every 0.5 s for 4 s (progress resets the timer)", b" 200 " in r, r[:20])

s = conn(); s.settimeout(2); ok = True
for i in range(8):
    s.sendall(b"GET /health HTTP/1.1\r\n\r\n"); ok &= b" 200 " in s.recv(1000); time.sleep(0.5)
check("active connection, a request every 0.5 s for 4 s", ok, "not closed")

s = conn(); s.sendall(b"GET /health HTTP/1.1\r\n\r\nGET /health HTTP/1.1\r\n\r\nGET /hea")
time.sleep(0.1); s.settimeout(0.2); first = s.recv(10000)
t, got = wait_close(s); check("two pipelined + partial third", first.count(b" 200 ") == 2 and t is not None and t < 1.6 and b" 408 " in got, f"closed after {t}")

# slow reader: a large response the client never reads
s = conn(); body = b'{"id":424242,"v":"' + b"q" * 1000 + b'"}\n'
s.sendall(b"POST /items/bulk HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body); s.recv(1000)
s = conn(rcvbuf=4096); ids = b"424242\n" * 20000  # ~20 MB of response
s.sendall(b"POST /items/bulk/get HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(ids) + ids)
time.sleep(2.5)
t, got = wait_close(s, 10); check("client never reads a 20 MB response", t is not None and len(got) < 15_000_000, f"closed, {len(got)} bytes had been sent")
sys.exit(1 if fails else 0)
