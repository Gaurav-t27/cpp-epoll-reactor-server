# Demo

Demonstrates the server's key capabilities with coloured terminal output.

## Prerequisites

The server must be running before launching the demo.

**Local build:**
```bash
./build/bin/tcp_server
```

**Docker:**
```bash
docker compose up -d
```

## Run

```bash
python3 demo/demo.py              # default: 127.0.0.1:8080
python3 demo/demo.py HOST PORT    # custom host/port
```

## What it shows

| Demo | What it demonstrates |
|---|---|
| Basic echo | Three messages sent and echoed back as uppercase |
| 10 concurrent clients | All connections opened simultaneously, handled in a single event loop iteration |
| 1 MB payload | Large data transfer with correct uppercase transformation and throughput measurement |


