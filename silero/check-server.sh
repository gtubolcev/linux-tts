#!/bin/sh
# Manual check of silero-server against a planted runtime directory: run in a
# container with the runtime at /rt (read-only) and linux-tts at /src, e.g.
#   docker run --rm --network none -v $PWD:/src:ro -v ~/.local/share/silero-tts/v5_6_ru:/rt:ro IMAGE sh /src/silero/check-server.sh
# Clean and with numpy.py / onnxruntime/ beside it: loads; a .so, a sourceless
# .pyc or a symbolic link in the package: refused.
S=/src/silero/silero-server; M=/src/silero/silero_onnx.sha256
health() { python3 -I - "$1" <<'P'
import socket,sys,time
for i in range(300):
    try:
        s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"GET /v2/health HTTP/1.1\r\nHost: x\r\n\r\n")
        r=s.recv(64).decode(); s.close()
        if " 200 " in r: print("health 200"); sys.exit(0)
    except OSError: pass
    time.sleep(0.2)
print("no health"); sys.exit(1)
P
}
gen() { python3 -I - "$1" <<'P'
import socket,sys,json
b=json.dumps({"text":"Привет, это проверка.","speaker":"xenia"}).encode()
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(b"POST /v2/generate HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(b) + b)
d=b""
while True:
    c=s.recv(65536)
    if not c: break
    d+=c
h,_,body=d.partition(b"\r\n\r\n"); print(h.split(b"\r\n")[0].decode(), len(body), "bytes")
P
}
run() {  # dir
  rm -f /tmp/s.sock
  $S --dir "$1" --socket /tmp/s.sock --manifest $M > /tmp/log 2>&1 & P=$!
  health /tmp/s.sock && gen /tmp/s.sock
  kill $P 2>/dev/null; wait $P 2>/dev/null
  grep -E "loaded|cannot|onnxruntime|EVIL" /tmp/log | cut -c1-220
}
echo "== clean"; run /rt
echo "== numpy.py and onnxruntime/ next to the runtime"
mkdir -p /tmp/a && cp -a /rt/. /tmp/a/ && echo 'raise SystemExit("EVIL numpy")' > /tmp/a/numpy.py
mkdir -p /tmp/a/onnxruntime && echo 'raise SystemExit("EVIL ort")' > /tmp/a/onnxruntime/__init__.py
run /tmp/a
echo "== a .so in the package"
mkdir -p /tmp/b && cp -a /rt/. /tmp/b/ && cp /tmp/b/silero_onnx/engine.py /tmp/b/silero_onnx/engine.cpython-312-x86_64-linux-gnu.so
run /tmp/b
echo "== a sourceless .pyc"
mkdir -p /tmp/c && cp -a /rt/. /tmp/c/ && : > /tmp/c/silero_onnx/evil.pyc
run /tmp/c
echo "== a symlinked directory"
mkdir -p /tmp/d && cp -a /rt/. /tmp/d/ && ln -s /tmp /tmp/d/silero_onnx/sub
run /tmp/d
