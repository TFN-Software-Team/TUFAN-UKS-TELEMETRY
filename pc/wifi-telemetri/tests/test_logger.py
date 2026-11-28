"""logger.py testleri — kalıcı kayıt, tekrar koruması, since API'si, HTTP uç
noktaları ve doğrudan-ESP32 kaynağı (sahte ESP32 HTTP sunucusuna karşı).
Çalıştırma:  cd pc/wifi-telemetri && python -m pytest -q tests"""

import http.server
import json
import os
import sys
import threading
import time
import urllib.request

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import logger  # noqa: E402


def tel(seq, ts=None, spd=452):
    ts = seq * 200 if ts is None else ts
    return (f"TEL,2,{seq},428,790,0,1,0,33150,33030,31,28,1,795,-1650,8950,1,"
            f"{ts},{spd}")


@pytest.fixture
def store(tmp_path):
    s = logger.LogStore(str(tmp_path))
    yield s
    s.close()


def read_rows(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    assert lines[0] == logger.LOG_HEADER
    return [l.split(";", 3) for l in lines[1:]]


def test_is_tel_line():
    assert logger.is_tel_line(tel(1))
    assert not logger.is_tel_line("TEL,2,1")
    assert not logger.is_tel_line(tel(1) + ",9")
    assert not logger.is_tel_line(tel(1).replace("428", "4x8"))
    assert not logger.is_tel_line("CSV,1,2,3,4,5,6")


def test_append_writes_to_disk_immediately(store):
    assert store.append(tel(0) + "\r\n") == 1
    assert store.append(tel(1)) == 2
    rows = read_rows(store.path)  # dosya kapatılmadan okunur: anında flush
    assert [r[0] for r in rows] == ["1", "2"]
    assert rows[1][3] == tel(1)
    assert store.stats["lines"] == 2


def test_duplicate_line_is_not_written_twice(store):
    store.append(tel(5))
    assert store.append(tel(5)) is None
    assert store.stats["dup"] == 1
    assert len(read_rows(store.path)) == 1


def test_bad_lines_are_counted_and_logged_not_stored(store):
    assert store.append("garbage") is None
    assert store.append("") is None
    assert store.stats["bad"] == 1
    assert read_rows(store.path) == []
    with open(store.events_path, encoding="utf-8") as f:
        assert "HATALI SATIR: garbage" in f.read()


def test_seq_gap_and_reboot_detection(store):
    store.append(tel(10))
    store.append(tel(13))          # 11, 12 kayıp
    assert store.stats["seq_gap"] == 2
    store.append(tel(0, ts=150))   # ESP32 yeniden başladı
    assert store.stats["esp_reboot"] == 1
    with open(store.events_path, encoding="utf-8") as f:
        assert "ESP32 YENIDEN BASLADI" in f.read()


def test_since_returns_only_newer_rows_with_limit(store):
    for i in range(10):
        store.append(tel(i))
    assert [n for n, _, _ in store.since(0)] == list(range(1, 11))
    assert [n for n, _, _ in store.since(7)] == [8, 9, 10]
    assert [n for n, _, _ in store.since(0, limit=3)] == [1, 2, 3]
    assert store.since(10) == []


def test_since_older_than_memory_starts_from_oldest(tmp_path):
    s = logger.LogStore(str(tmp_path), max_mem=5)
    try:
        for i in range(12):
            s.append(tel(i))
        assert [n for n, _, _ in s.since(0)] == [8, 9, 10, 11, 12]
        assert len(read_rows(s.path)) == 12  # disk hepsini tutar
    finally:
        s.close()


def test_each_run_gets_new_file_and_run_id(tmp_path):
    a = logger.LogStore(str(tmp_path))
    b = logger.LogStore(str(tmp_path))
    try:
        assert a.path != b.path and a.run_id != b.run_id
        assert os.path.basename(b.events_path).startswith("wifi_events_")
    finally:
        a.close(); b.close()


# ---------------------------------------------------------------------------
# HTTP API
# ---------------------------------------------------------------------------
class _FakeSource:
    def __init__(self):
        self.state = logger.SourceState("mqtt", "mqtts://test:8883 · tufan/aks")
        self.state.connected = True
        self.state.detail = "broker'a bağlı"


@pytest.fixture
def api(store):
    httpd = logger.ThreadingHTTPServer(("127.0.0.1", 0), logger.make_handler(store, _FakeSource()))
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}", store
    httpd.shutdown()
    httpd.server_close()


def get(url):
    with urllib.request.urlopen(url, timeout=3) as r:
        return r.status, r.headers, r.read()


def test_api_lines_and_status(api):
    base, store = api
    for i in range(3):
        store.append(tel(i))
    store.set_esp_status({"online": True, "ip": "192.168.43.7", "sim": True})
    _, h, body = get(base + "/api/lines?since=1")
    j = json.loads(body)
    assert h["Access-Control-Allow-Origin"] == "*"
    assert j["run_id"] == store.run_id and j["last_no"] == 3
    assert [r["no"] for r in j["lines"]] == [2, 3]
    assert j["lines"][0]["line"] == tel(1)
    s = json.loads(get(base + "/api/status")[2])
    assert s["source"]["connected"] is True
    assert s["esp_status"]["ip"] == "192.168.43.7"
    assert s["stats"]["lines"] == 3


def test_api_logs_listing_and_download(api):
    base, store = api
    store.append(tel(0))
    j = json.loads(get(base + "/api/logs")[2])
    name = os.path.basename(store.path)
    assert j["current"] == name and any(f["name"] == name for f in j["files"])
    _, h, body = get(base + "/logs/" + name)
    assert "attachment" in h["Content-Disposition"]
    assert tel(0) in body.decode()


def test_api_rejects_path_traversal_and_bad_params(api):
    base, _ = api
    for path in ("/logs/..%2F..%2Fetc%2Fpasswd", "/logs/secret.txt", "/api/lines?since=x"):
        with pytest.raises(urllib.error.HTTPError) as e:
            get(base + path)
        assert e.value.code == 400


def test_index_served(api):
    base, _ = api
    status, h, body = get(base + "/")
    assert status == 200 and b"TUFAN Wi-Fi Telemetri" in body


# ---------------------------------------------------------------------------
# Doğrudan ESP32 kaynağı — sahte ESP32 (/telemetry.csv?since=, /status)
# ---------------------------------------------------------------------------
class _FakeEsp:
    def __init__(self):
        self.lines = []
        self.requests = []
        outer = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                if self.path.startswith("/status"):
                    body = json.dumps({"online": True, "ip": "10.0.0.9", "sim": True}).encode()
                else:
                    since = None
                    if "since=" in self.path:
                        since = int(self.path.split("since=")[1])
                    rows = [l for s, l in outer.lines if since is None or s > since]
                    body = "".join(l + "\r\n" for l in rows).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = logger.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def wait_for(cond, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def test_esp_poll_source_logs_all_lines_once(store):
    esp = _FakeEsp()
    src = logger.EspPollSource(store, esp.url, period=0.05)
    try:
        esp.lines = [(i, tel(i)) for i in range(5)]
        src.start()
        assert wait_for(lambda: store.stats["lines"] == 5)
        esp.lines += [(i, tel(i)) for i in range(5, 8)]
        assert wait_for(lambda: store.stats["lines"] == 8)
        time.sleep(0.3)
        assert store.stats["lines"] == 8 and store.stats["dup"] == 0
        assert any("since=4" in r for r in esp.requests)  # kaldığı yerden ister
        assert wait_for(lambda: store.esp_status is not None)
        assert src.state.connected
    finally:
        src.stop(); esp.close()


def test_esp_poll_source_survives_unreachable_esp(store):
    src = logger.EspPollSource(store, "http://127.0.0.1:9", period=0.05)
    try:
        src.start()
        assert wait_for(lambda: "ulaşılamıyor" in src.state.detail)
        assert not src.state.connected
    finally:
        src.stop()
