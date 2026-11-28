#!/usr/bin/env python3
"""TUFAN Wi-Fi telemetri LOGGER'ı (TEST yolu).

Zincir:
    ESP32 (AKS veya S3 test kartı) ──MQTT/TLS──> HiveMQ Cloud ──> BU SERVİS
                                   └─HTTP (aynı ağ)────────────────┘
    BU SERVİS ──> logs/wifi_*.csv   (KALICI kayıt, her satır anında diske)
              └─> http://<pc>:8080  (kayıtlı verinin web adresi)
                    /                 canlı sayfa (index.html, "Logger" modu)
                    /api/lines?since= kayıtlı satırlar (JSON) — TUFAN-monitor
                                      Wi-Fi kaynağı buradan çeker
                    /api/status       logger + ESP32 durumu
                    /api/logs         log dosyaları listesi
                    /logs/<dosya>     log dosyası indirme

Neden ayrı bir servis: HiveMQ veriyi yalnız İLETİR, saklamaz; tarayıcı
sayfası da yalnız açıkken gelenleri görür. Kayıt, tarayıcıdan ve
arayüzlerden bağımsız olarak burada tutulur.

Bağımlılık: yalnız MQTT kaynağı için `paho-mqtt` (pip install paho-mqtt).
Doğrudan ESP32 kaynağı yalnız Python standart kütüphanesini kullanır.

Bu servis jüri kaydının (TUFAN-monitor, LoRa→UKS, 9.2.f) yerini TUTMAZ.
"""

from __future__ import annotations

import argparse
import collections
import datetime as _dt
import http.server
import json
import os
import re
import socketserver
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TEL_RE = re.compile(r"^TEL(,-?\d+){18}$")  # 19 alanlı TEL v2 (AKS Telemetry::formatFrame)
LOG_HEADER = "no;pc_zaman;pc_ms;tel_satiri"
LOG_NAME_RE = re.compile(r"^wifi_[0-9A-Za-z_\-]+\.(csv|log)$")


def now_ms() -> int:
    return int(time.time() * 1000)


def is_tel_line(line: str) -> bool:
    return bool(TEL_RE.match(line))


def tel_seq(line: str) -> int | None:
    try:
        return int(line.split(",")[2])
    except (IndexError, ValueError):
        return None


# ---------------------------------------------------------------------------
# Kalıcı kayıt
# ---------------------------------------------------------------------------
class LogStore:
    """TEL satırlarını diske (anında flush) ve belleğe (son `max_mem` satır)
    yazar. Her satıra bu oturumda artan bir `no` verilir; istemciler
    `since(no)` ile kaldıkları yerden devam eder. `run_id` her logger
    açılışında değişir — istemci bunu görünce baştan (since=0) okur.

    Tekrar koruması: aynı TEL satırı (seq+ts içerdiği için benzersizdir)
    son `dedup_window` satır içinde görüldüyse ikinci kez yazılmaz
    (MQTT yeniden gönderimi / iki kaynağın çakışması)."""

    def __init__(self, out_dir: str, max_mem: int = 200_000, dedup_window: int = 2000,
                 fsync_every_sec: float = 2.0):
        os.makedirs(out_dir, exist_ok=True)
        self.out_dir = out_dir
        self.run_id = uuid.uuid4().hex[:12]
        stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.path = self._unique(os.path.join(out_dir, f"wifi_{stamp}"), ".csv")
        base = os.path.basename(self.path)[len("wifi_"):-len(".csv")]
        self.events_path = os.path.join(out_dir, f"wifi_events_{base}.log")
        self._f = open(self.path, "x", encoding="utf-8", newline="\n")
        self._f.write(LOG_HEADER + "\n")
        self._f.flush()
        self._ev = open(self.events_path, "a", encoding="utf-8")
        self._lock = threading.Lock()
        self._mem: collections.deque = collections.deque(maxlen=max_mem)
        self._recent: collections.deque = collections.deque(maxlen=dedup_window)
        self._recent_set: set[str] = set()
        self._next_no = 1
        self._last_fsync = time.monotonic()
        self._fsync_every = fsync_every_sec
        self.stats = {"lines": 0, "dup": 0, "bad": 0, "seq_gap": 0, "esp_reboot": 0}
        self._last_seq: int | None = None
        self.last_line_pc_ms: int | None = None
        self.esp_status: dict | None = None
        self.esp_status_pc_ms: int | None = None

    @staticmethod
    def _unique(stem: str, ext: str) -> str:
        path, n = stem + ext, 1
        while os.path.exists(path):
            path = f"{stem}_{n}{ext}"
            n += 1
        return path

    def event(self, msg: str) -> None:
        line = f"{_dt.datetime.now().isoformat(timespec='milliseconds')} {msg}"
        with self._lock:
            try:
                self._ev.write(line + "\n")
                self._ev.flush()
            except ValueError:
                pass  # kapanmış
        print(line, flush=True)

    def append(self, raw: str, pc_ms: int | None = None) -> int | None:
        """Bir satırı kaydeder; kaydedildiyse `no` döner, değilse None
        (boş / TEL değil / tekrar)."""
        line = raw.strip()
        if not line:
            return None
        if not is_tel_line(line):
            with self._lock:
                self.stats["bad"] += 1
            self.event(f"HATALI SATIR: {line[:200]}")
            return None
        pc_ms = now_ms() if pc_ms is None else pc_ms
        reboot_msg = None
        with self._lock:
            if line in self._recent_set:
                self.stats["dup"] += 1
                return None
            if len(self._recent) == self._recent.maxlen:
                self._recent_set.discard(self._recent[0])
            self._recent.append(line)
            self._recent_set.add(line)

            seq = tel_seq(line)
            if seq is not None and self._last_seq is not None:
                if seq < self._last_seq:
                    self.stats["esp_reboot"] += 1
                    reboot_msg = f"ESP32 YENIDEN BASLADI (seq {self._last_seq} -> {seq})"
                elif seq > self._last_seq + 1:
                    self.stats["seq_gap"] += seq - self._last_seq - 1
            if seq is not None:
                self._last_seq = seq

            no = self._next_no
            self._next_no += 1
            iso = _dt.datetime.fromtimestamp(pc_ms / 1000).isoformat(timespec="milliseconds")
            self._f.write(f"{no};{iso};{pc_ms};{line}\n")
            self._f.flush()
            if time.monotonic() - self._last_fsync >= self._fsync_every:
                os.fsync(self._f.fileno())
                self._last_fsync = time.monotonic()
            self._mem.append((no, pc_ms, line))
            self.stats["lines"] += 1
            self.last_line_pc_ms = pc_ms
        if reboot_msg:
            self.event(reboot_msg)
        return no

    def set_esp_status(self, status: dict) -> None:
        with self._lock:
            self.esp_status = status
            self.esp_status_pc_ms = now_ms()

    def since(self, after_no: int, limit: int = 1000) -> list[tuple[int, int, str]]:
        """`no` > after_no olan satırlar (en fazla `limit`). after_no bellekte
        tutulandan eskiyse tutulan en eski satırdan başlar."""
        limit = max(1, min(limit, 20000))
        with self._lock:
            if not self._mem:
                return []
            first_no = self._mem[0][0]
            start = max(0, after_no - first_no + 1)
            out = []
            for i in range(start, len(self._mem)):
                out.append(self._mem[i])
                if len(out) >= limit:
                    break
            return out

    def last_no(self) -> int:
        with self._lock:
            return self._next_no - 1

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "run_id": self.run_id,
                "file": os.path.basename(self.path),
                "last_no": self._next_no - 1,
                "stats": dict(self.stats),
                "last_line_pc_ms": self.last_line_pc_ms,
                "esp_status": self.esp_status,
                "esp_status_pc_ms": self.esp_status_pc_ms,
            }

    def close(self) -> None:
        with self._lock:
            for f in (self._f, self._ev):
                try:
                    f.flush()
                    os.fsync(f.fileno())
                    f.close()
                except (ValueError, OSError):
                    pass


# ---------------------------------------------------------------------------
# Kaynaklar
# ---------------------------------------------------------------------------
class SourceState:
    def __init__(self, kind: str, target: str):
        self.kind = kind
        self.target = target
        self.connected = False
        self.detail = "başlatılıyor"


class MqttSource:
    """HiveMQ Cloud (veya herhangi bir broker) <base>/tel ve <base>/status
    konularına abone olur. paho kendi thread'inde çalışır, kopunca kendisi
    yeniden bağlanır."""

    def __init__(self, store: LogStore, host: str, port: int, user: str, password: str,
                 topic_base: str, tls: bool = True):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            sys.exit("MQTT kaynağı için paho-mqtt gerekli:  pip install paho-mqtt")
        self._mqtt = mqtt
        self.store = store
        self.base = topic_base.rstrip("/")
        self.state = SourceState("mqtt", f"{'mqtts' if tls else 'mqtt'}://{host}:{port} · {self.base}")
        client_id = "tufan-logger-" + uuid.uuid4().hex[:8]
        if hasattr(mqtt, "CallbackAPIVersion"):  # paho-mqtt >= 2.0
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        else:
            self.client = mqtt.Client(client_id=client_id)
        if user:
            self.client.username_pw_set(user, password)
        if tls:
            self.client.tls_set()
        self.client.reconnect_delay_set(min_delay=1, max_delay=10)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.host, self.port = host, port

    def start(self) -> None:
        self.store.event(f"KAYNAK MQTT: {self.state.target}")
        self.client.connect_async(self.host, self.port, keepalive=30)
        self.client.loop_start()

    def stop(self) -> None:
        self.client.loop_stop()
        try:
            self.client.disconnect()
        except Exception:
            pass

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        rc = getattr(reason_code, "value", reason_code)
        if rc == 0:
            self.state.connected = True
            self.state.detail = "broker'a bağlı"
            client.subscribe([(self.base + "/tel", 0), (self.base + "/status", 0)])
            self.store.event("MQTT BAGLANDI")
        else:
            self.state.connected = False
            self.state.detail = f"broker reddetti ({reason_code})"
            self.store.event(f"MQTT BAGLANTI REDDI: {reason_code}")

    def _on_disconnect(self, client, userdata, *args):
        self.state.connected = False
        self.state.detail = "broker bağlantısı yok (yeniden deneniyor)"
        self.store.event("MQTT KOPTU")

    def _on_message(self, client, userdata, msg):
        text = msg.payload.decode("utf-8", errors="replace")
        if msg.topic.endswith("/status"):
            try:
                self.store.set_esp_status(json.loads(text))
            except ValueError:
                pass
            return
        for line in text.splitlines():
            self.store.append(line)


class EspPollSource:
    """ESP32'ye aynı ağdan doğrudan HTTP ile bağlanır:
    GET /telemetry.csv?since=<son seq> (0,5 sn) + GET /status (5 sn)."""

    def __init__(self, store: LogStore, base_url: str, period: float = 0.5):
        if not re.match(r"^https?://", base_url):
            base_url = "http://" + base_url
        self.base = base_url.rstrip("/")
        self.store = store
        self.period = period
        self.state = SourceState("esp", self.base)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="esp-poll", daemon=True)
        self._last_seq: int | None = None

    def start(self) -> None:
        self.store.event(f"KAYNAK DOGRUDAN ESP32: {self.base}")
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=3)

    def _get(self, path: str, timeout: float = 3.0) -> bytes:
        with urllib.request.urlopen(self.base + path, timeout=timeout) as r:
            return r.read()

    def _run(self) -> None:
        last_status = 0.0
        was_up = None
        while not self._stop.is_set():
            try:
                q = "" if self._last_seq is None else f"?since={self._last_seq}"
                body = self._get("/telemetry.csv" + q).decode("utf-8", errors="replace")
                for line in body.splitlines():
                    if self.store.append(line) is not None or is_tel_line(line.strip()):
                        s = tel_seq(line.strip())
                        if s is not None:
                            self._last_seq = s
                if time.monotonic() - last_status >= 5.0:
                    last_status = time.monotonic()
                    try:
                        self.store.set_esp_status(json.loads(self._get("/status")))
                    except ValueError:
                        pass
                self.state.connected, self.state.detail = True, "ESP32'ye bağlı"
                if was_up is not True:
                    self.store.event("ESP32 BAGLANDI")
                was_up = True
            except Exception as exc:  # ağ hatası: logla, beklemeye devam et
                self.state.connected, self.state.detail = False, f"ESP32'ye ulaşılamıyor ({exc})"
                if was_up is not False:
                    self.store.event(f"ESP32 ULASILAMIYOR: {exc}")
                was_up = False
                self._stop.wait(2.0)
                continue
            self._stop.wait(self.period)


# ---------------------------------------------------------------------------
# HTTP (kayıtlı verinin web adresi)
# ---------------------------------------------------------------------------
def make_handler(store: LogStore, source):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "TufanWifiLogger/1"

        def log_message(self, fmt, *args):  # istek başına konsol gürültüsü yapma
            pass

        def _send(self, code: int, body: bytes, ctype: str, extra: dict | None = None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                       "application/json; charset=utf-8")

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path in ("/", "/index.html"):
                try:
                    with open(os.path.join(HERE, "index.html"), "rb") as f:
                        self._send(200, f.read(), "text/html; charset=utf-8")
                except OSError:
                    self._send(404, b"index.html yok", "text/plain; charset=utf-8")
            elif u.path == "/api/lines":
                try:
                    since = int(q.get("since", ["0"])[0])
                    limit = int(q.get("limit", ["1000"])[0])
                except ValueError:
                    return self._json({"error": "since/limit sayi olmali"}, 400)
                rows = store.since(since, limit)
                self._json({
                    "run_id": store.run_id,
                    "last_no": store.last_no(),
                    "lines": [{"no": n, "pc_ms": t, "line": l} for n, t, l in rows],
                })
            elif u.path == "/api/status":
                snap = store.snapshot()
                snap["source"] = {"kind": source.state.kind, "target": source.state.target,
                                  "connected": source.state.connected,
                                  "detail": source.state.detail}
                snap["pc_ms"] = now_ms()
                self._json(snap)
            elif u.path == "/api/logs":
                files = []
                for name in sorted(os.listdir(store.out_dir)):
                    if LOG_NAME_RE.match(name):
                        p = os.path.join(store.out_dir, name)
                        files.append({"name": name, "bytes": os.path.getsize(p),
                                      "mtime_ms": int(os.path.getmtime(p) * 1000)})
                self._json({"files": files, "current": os.path.basename(store.path)})
            elif u.path.startswith("/logs/"):
                name = urllib.parse.unquote(u.path[len("/logs/"):])
                if not LOG_NAME_RE.match(name):  # yol gezinme (../) engeli
                    return self._send(400, b"gecersiz dosya adi", "text/plain; charset=utf-8")
                p = os.path.join(store.out_dir, name)
                if not os.path.isfile(p):
                    return self._send(404, b"yok", "text/plain; charset=utf-8")
                with open(p, "rb") as f:
                    data = f.read()
                self._send(200, data, "text/csv; charset=utf-8",
                           {"Content-Disposition": f'attachment; filename="{name}"'})
            else:
                self._send(404, b"bulunamadi", "text/plain; charset=utf-8")

    return Handler


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---------------------------------------------------------------------------
# Ayarlar / main
# ---------------------------------------------------------------------------
DEFAULTS = {
    "source": "mqtt",           # "mqtt" | "esp"
    "broker": "",               # örn. xxxxxxxx.s1.eu.hivemq.cloud
    "mqtt_port": 8883,
    "tls": True,
    "user": "",
    "password": "",             # ya da TUFAN_MQTT_PASSWORD ortam değişkeni
    "topic": "tufan/aks",
    "esp": "",                  # örn. http://192.168.43.120
    "listen": "127.0.0.1",      # 0.0.0.0 = aynı ağdaki başka PC'ler de erişir (kimlik doğrulama YOK)
    "http_port": 8080,
    "out_dir": os.path.join(HERE, "logs"),
}


def load_settings(argv=None) -> dict:
    ap = argparse.ArgumentParser(description="TUFAN Wi-Fi telemetri logger'ı (TEST yolu)")
    ap.add_argument("--config", default=os.path.join(HERE, "logger_ayarlar.json"),
                    help="JSON ayar dosyası (varsayılan: logger_ayarlar.json, git'e girmez)")
    ap.add_argument("--source", choices=["mqtt", "esp"])
    ap.add_argument("--broker")
    ap.add_argument("--mqtt-port", type=int, dest="mqtt_port")
    ap.add_argument("--no-tls", action="store_true", help="TLS'siz broker (yalnız yerel test)")
    ap.add_argument("--user")
    ap.add_argument("--topic")
    ap.add_argument("--esp", help="ESP32 adresi (source=esp)")
    ap.add_argument("--listen")
    ap.add_argument("--http-port", type=int, dest="http_port")
    ap.add_argument("--out-dir", dest="out_dir")
    a = ap.parse_args(argv)

    cfg = dict(DEFAULTS)
    if os.path.isfile(a.config):
        with open(a.config, encoding="utf-8") as f:
            cfg.update(json.load(f))
    for k in ("source", "broker", "mqtt_port", "user", "topic", "esp", "listen", "http_port", "out_dir"):
        v = getattr(a, k)
        if v is not None:
            cfg[k] = v
    if a.no_tls:
        cfg["tls"] = False
    if os.environ.get("TUFAN_MQTT_PASSWORD"):
        cfg["password"] = os.environ["TUFAN_MQTT_PASSWORD"]
    return cfg


def main(argv=None) -> int:
    cfg = load_settings(argv)
    store = LogStore(cfg["out_dir"])
    if cfg["source"] == "mqtt":
        if not cfg["broker"]:
            sys.exit("MQTT broker adresi yok: logger_ayarlar.json 'broker' veya --broker")
        source = MqttSource(store, cfg["broker"], int(cfg["mqtt_port"]), cfg["user"],
                            cfg["password"], cfg["topic"], tls=bool(cfg["tls"]))
    else:
        if not cfg["esp"]:
            sys.exit("ESP32 adresi yok: logger_ayarlar.json 'esp' veya --esp")
        source = EspPollSource(store, cfg["esp"])

    httpd = ThreadingHTTPServer((cfg["listen"], int(cfg["http_port"])), make_handler(store, source))
    store.event(f"LOGGER BASLADI run_id={store.run_id} dosya={store.path}")
    shown = "localhost" if cfg["listen"] in ("127.0.0.1", "0.0.0.0") else cfg["listen"]
    print(f"Arayüz / kayıtlı veri:  http://{shown}:{cfg['http_port']}/", flush=True)
    source.start()
    try:
        httpd.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        store.event("LOGGER DURDU")
        source.stop()
        httpd.server_close()
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
