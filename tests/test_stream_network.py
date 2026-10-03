#!/usr/bin/env python3
"""Private RTMP/RTMPS fixtures; no devices, public endpoints, or persistent keys."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import select
import socket
import ssl
import subprocess
import tempfile
import threading
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_client(binary, url, trust="", mode="accept", env=None):
    result = subprocess.run([str(binary), "--fixture", url, str(trust), mode], env=env,
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stderr
    assert "rtmp://" not in result.stderr and "rtmps://" not in result.stderr
    print(result.stderr.strip())


class Proxy:
    def __init__(self, backend, certificate=None, key=None, stall_tls=False, deny_publish=False):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.backend = backend
        self.stop = threading.Event()
        self.errors = []
        self.certificate = certificate
        self.key = key
        self.stall_tls = stall_tls
        self.forwarded = 0
        self.deny_publish = deny_publish
        self.denials = 0
        self.thread = threading.Thread(target=self.forward, daemon=True)
        self.thread.start()

    def forward(self):
        try:
            with self.listener.accept()[0] as incoming:
                incoming.settimeout(4)
                if self.stall_tls:
                    self.stop.wait(4)
                    return
                if self.certificate:
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(self.certificate, self.key)
                    try:
                        client = context.wrap_socket(incoming, server_side=True)
                    except (ssl.SSLError, ConnectionResetError):
                        return  # Expected for the untrusted-certificate fixture.
                else:
                    client = incoming
                try:
                    first = client.recv(4096)
                except (ssl.SSLError, ConnectionResetError):
                    client.close()
                    return
                if not first:
                    client.close()
                    return
                self.forwarded += len(first)
                with client, socket.create_connection(("127.0.0.1", self.backend), timeout=3) as target:
                    target.sendall(first)
                    client.setblocking(False)
                    target.setblocking(False)
                    peers = {client: target, target: client}
                    while not self.stop.is_set():
                        ready, _, _ = select.select(list(peers), [], [], 0.02)
                        if isinstance(client, ssl.SSLSocket) and client.pending() and client not in ready:
                            ready.append(client)
                        for source in ready:
                            try:
                                data = source.recv(65536)
                            except (ssl.SSLWantReadError, BlockingIOError):
                                continue
                            if not data:
                                return
                            if source is client:
                                self.forwarded += len(data)
                            elif self.deny_publish and b"NetStream.Publish.Start" in data:
                                # Same-size AMF status replacement preserves FFmpeg's
                                # RTMP framing while making this private server deny publish.
                                data = data.replace(b"NetStream.Publish.Start", b"NetStream.Publish.Deny!")
                                data = data.replace(b"\x00\x06status", b"\x00\x06error\x00")
                                # The deliberately URL/key-bearing server diagnostic
                                # reaches FFmpeg, which must never forward it to logs.
                                data = data.replace(b"is now published", b"access denied!!!")
                                self.denials += 1
                            destination = peers[source]
                            view = memoryview(data)
                            while view and not self.stop.is_set():
                                try:
                                    count = destination.send(view)
                                    view = view[count:]
                                except (ssl.SSLWantWriteError, BlockingIOError):
                                    select.select([], [destination], [], 0.02)
        except (ConnectionResetError, BrokenPipeError):
            pass  # SIGKILL cancellation closes the test client immediately.
        except Exception as error:
            self.errors.append(error)
        finally:
            self.listener.close()

    def close(self):
        self.stop.set()
        self.thread.join(5)
        assert not self.thread.is_alive()
        assert not self.errors, self.errors


def receiver(port, output):
    process = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "quiet", "-y", "-listen", "1",
                                "-i", f"rtmp://127.0.0.1:{port}/ingest", "-c", "copy", "-f", "flv", str(output)],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.2)
    assert process.poll() is None
    return process


def finish_receiver(process):
    try:
        process.wait(3)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    arguments = parser.parse_args()
    binary = arguments.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="cast-stream-network-") as temporary:
        directory = Path(temporary)
        certificate, key = directory / "certificate.pem", directory / "tls-key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(certificate)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        backend_port = free_port()
        output = directory / "trusted.flv"
        ingest = receiver(backend_port, output)
        proxy = Proxy(backend_port, certificate, key)
        try:
            run_client(binary, f"rtmps://localhost:{proxy.port}/ingest", certificate)
        finally:
            proxy.close()
            finish_receiver(ingest)
        information = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-count_packets",
                                                        "-show_streams", "-of", "json", str(output)], text=True))
        streams = {stream["codec_type"]: stream for stream in information["streams"]}
        assert streams["video"]["codec_name"] == "h264" and int(streams["video"]["nb_read_packets"]) > 10
        assert streams["audio"]["codec_name"] == "aac" and int(streams["audio"]["nb_read_packets"]) > 10
        proxy = Proxy(free_port(), certificate, key)
        try:
            run_client(binary, f"rtmps://localhost:{proxy.port}/ingest", mode="reject")
        finally:
            proxy.close()
        assert proxy.forwarded == 0, "untrusted certificate accepted application data"
        # A trusted issuer does not authorize a mismatching server name.
        proxy = Proxy(free_port(), certificate, key)
        try:
            run_client(binary, f"rtmps://127.0.0.1:{proxy.port}/ingest", certificate, "reject")
        finally:
            proxy.close()
        assert proxy.forwarded == 0, "mismatching hostname accepted application data"
        proxy = Proxy(free_port(), stall_tls=True)
        try:
            run_client(binary, f"rtmps://localhost:{proxy.port}/ingest", mode="stall")
        finally:
            proxy.close()
        backend_port = free_port()
        ingest = receiver(backend_port, directory / "denied.flv")
        proxy = Proxy(backend_port, deny_publish=True)
        try:
            run_client(binary, f"rtmp://localhost:{proxy.port}/ingest", mode="reject-key")
        finally:
            proxy.close()
            finish_receiver(ingest)
        assert proxy.denials == 1, "fixture did not deny the publish command"
        interposer = directory / "block-dns.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-o", str(interposer),
                        "tests/fixtures/stream_block_dns.c", "-ldl"], check=True)
        environment = os.environ.copy()
        # Preserve sanitizer preload ordering when used under make sanitize.
        prior = environment.get("LD_PRELOAD", "")
        if environment.get("ASAN_OPTIONS") and not prior:
            prior = subprocess.check_output(["cc", "-print-file-name=libasan.so"], text=True).strip()
        environment["LD_PRELOAD"] = f"{prior}:{interposer}" if prior else str(interposer)
        environment["CAST_TEST_DNS_MARKER"] = str(directory / "dns-entered")
        run_client(binary, "rtmp://cast-blocked.invalid:1935/ingest", mode="blocked", env=environment)
    print("streaming TLS trust/hostname, blocked TLS negotiation and actual blocked DNS fixtures passed")


if __name__ == "__main__":
    main()
