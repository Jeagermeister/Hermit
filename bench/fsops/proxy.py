"""A recording proxy between Hermes and Ollama.

Why this exists: the default run log captures only Hermes' final reply. When
`qwen3.5:4b` wrote `5|2026-08-12 shipped` into tally.txt on 2026-08-12, the log said
`DONE` and nothing else - there was no way to tell whether the `5|` was a rendered
line-number prefix echoed back as literal content (a tool-design bug, fixable) or the
model inventing it (a model defect, must be designed around). Those need very
different responses from Hermit, and guessing between them is not acceptable.

Hermes' own `request_dump_*.json` files do not help: that path fires only on API
errors, so a successful-but-wrong run produces nothing.

Sitting on the wire gives ground truth - the exact bytes of every request the model
received (including how file content was rendered to it) and every response it
produced (including tool calls). It needs no change to ~/.hermes/config.yaml, because
CUSTOM_BASE_URL takes precedence over the config file's base_url:

    base_url = explicit_base_url or env_custom_base_url or cfg_base_url or ...
    (hermes_cli/runtime_provider.py)

so the redirect is per-subprocess and leaves the user's global Hermes setup alone.
"""
from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
import urllib.request
import urllib.error

# Serial runs, so a module-level sink is safe and keeps the handler simple.
_sink: Path | None = None
_lock = threading.Lock()
_upstream = "http://localhost:11434"


def set_sink(path: Path | None) -> None:
    """Direct subsequent transcript records to `path` (JSONL), or discard if None."""
    global _sink
    with _lock:
        _sink = path


def _record(rec: dict) -> None:
    with _lock:
        if _sink is None:
            return
        with _sink.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")


def _trim_request(body: bytes) -> dict:
    """Keep what explains model behaviour; drop what would bloat the transcript.

    `messages` is the whole point - it is where a rendered `5|` prefix would show up.
    Tool *definitions* are large and identical on every turn, so only their names are
    kept.
    """
    try:
        req = json.loads(body)
    except Exception:
        return {"unparsed": body[:2000].decode("utf-8", "replace")}
    tools = req.get("tools")
    if isinstance(tools, list):
        req["tools"] = [t.get("function", {}).get("name", "?") for t in tools]
        req["_tools_trimmed"] = True
    return req


def _summarise_stream(raw: str) -> dict:
    """Reassemble an SSE stream into the content and tool calls the model emitted."""
    content, reasoning, calls = [], [], {}
    for line in raw.splitlines():
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if not payload or payload == "[DONE]":
            continue
        try:
            delta = json.loads(payload)["choices"][0].get("delta", {})
        except Exception:
            continue
        if delta.get("content"):
            content.append(delta["content"])
        # Thinking models emit their reasoning in a SEPARATE delta field, and Ollama
        # names it `reasoning` on the OpenAI-compatible endpoint. Dropping it made a
        # model that burned 3,489 characters of reasoning to produce 8 characters of
        # answer look like a model that had simply stalled.
        for key in ("reasoning", "reasoning_content"):
            if delta.get(key):
                reasoning.append(delta[key])
        for tc in delta.get("tool_calls") or []:
            slot = calls.setdefault(tc.get("index", 0), {"name": "", "arguments": ""})
            fn = tc.get("function") or {}
            if fn.get("name"):
                slot["name"] = fn["name"]
            if fn.get("arguments"):
                slot["arguments"] += fn["arguments"]
    joined = "".join(reasoning)
    return {"content": "".join(content),
            "reasoning": joined,
            "reasoning_chars": len(joined),
            "tool_calls": [calls[k] for k in sorted(calls)]}


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args) -> None:  # keep the benchmark's stdout clean
        pass

    def _proxy(self, method: str) -> None:
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        url = _upstream + self.path
        headers = {k: v for k, v in self.headers.items()
                   if k.lower() not in ("host", "content-length", "connection")}
        req = urllib.request.Request(url, data=body or None, headers=headers, method=method)

        try:
            with urllib.request.urlopen(req, timeout=600) as resp:
                status, resp_headers = resp.status, resp.headers
                self.send_response(status)
                for k, v in resp_headers.items():
                    if k.lower() in ("transfer-encoding", "content-length", "connection"):
                        continue
                    self.send_header(k, v)
                self.send_header("Connection", "close")
                self.end_headers()
                # Tee: forward each chunk immediately so streaming is not buffered
                # (buffering would distort the per-run timings this benchmark records),
                # while accumulating a copy for the transcript.
                chunks = []
                while True:
                    chunk = resp.read(8192)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    try:
                        self.wfile.write(chunk)
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        break
                raw = b"".join(chunks).decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            status = exc.code
            self.send_response(status)
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw.encode())
        except Exception as exc:  # upstream unreachable
            raw, status = f"proxy error: {exc}", 502
            self.send_response(502)
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(raw.encode())

        if self.path.endswith("/chat/completions"):
            if raw.lstrip().startswith("data:"):
                response = _summarise_stream(raw)
            else:
                try:
                    response = json.loads(raw)
                except Exception:
                    response = {"unparsed": raw[:4000]}
            _record({"path": self.path, "status": status,
                     "request": _trim_request(body), "response": response})

    def do_POST(self) -> None:
        self._proxy("POST")

    def do_GET(self) -> None:
        self._proxy("GET")


def start(upstream: str = "http://localhost:11434") -> tuple[ThreadingHTTPServer, int]:
    """Bind an ephemeral port, serve in a daemon thread, return (server, port)."""
    global _upstream
    _upstream = upstream.rstrip("/")
    server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]
