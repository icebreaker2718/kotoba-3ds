"""The plugin's HTTPS client (mbedTLS) against a local TLS server with its own test CA:
Content-Length and chunked answers, HTTP errors, and a server the CA didn't sign.
    make -C tests test_lens && python3 tests/test_https.py"""
import os, ssl, subprocess, sys, tempfile, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0
def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    fails += 0 if cond else 1

d = tempfile.mkdtemp()
def sh(*a): subprocess.run(a, cwd=d, check=True, capture_output=True)
for name in ("ca", "other"):
    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", f"/CN=Test {name} CA",
       "-keyout", f"{name}.key", "-out", f"{name}.pem")
open(os.path.join(d, "ext"), "w").write("subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n")
sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost", "-keyout", "srv.key", "-out", "srv.csr")
sh("openssl", "x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
   "-days", "2", "-extfile", "ext", "-out", "srv.pem")

seen = {}
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        seen["body"], seen["ctype"], seen["host"] = body, self.headers["Content-Type"], self.headers["Host"]
        if self.path == "/echo":
            out = b"got:" + body
            self.send_response(200); self.send_header("Content-Length", str(len(out))); self.end_headers()
            self.wfile.write(out)
        elif self.path == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for part in (b"\x12\x03ab", b"c" * 5000, b"end"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            out = b"no"
            self.send_response(403); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(out)
        self.close_connection = True

srv = HTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(os.path.join(d, "srv.pem"), os.path.join(d, "srv.key"))
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = str(srv.server_port)
probe = os.path.join(HERE, "test_lens")

def run(roots, path="/echo", host="localhost"):
    r = subprocess.run([probe, "--https", host, port, os.path.join(d, roots), path], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.strip()

code, out = run("ca.pem")
check(code == 0 and "body=[got:hello lens]" in out, "TLS POST with Content-Length: " + out)
check(seen.get("ctype") == "application/x-protobuf" and seen.get("host") == "localhost", f"headers sent {seen.get('host')}")
code, out = run("ca.pem", "/chunked")
check(code == 0 and "body=[\x12\x03ab" + "c" * 5000 + "end]" in out, "chunked answer decoded")
code, out = run("ca.pem", "/forbidden")
check(code != 0 and "status=403" in out and "HTTP 403" in out, "HTTP error reported: " + out[-80:])
code, out = run("other.pem")
check(code != 0 and "status=0" in out and "certificate" in out, "server not signed by our roots is refused: " + out[-120:])
code, out = run("ca.pem", host="127.0.0.1")
check(code != 0 and "certificate" in out, "host name must match the certificate: " + out[-100:])

print("\nALL PASSED" if not fails else f"\n{fails} FAILED")
sys.exit(1 if fails else 0)
