# Soak testing

`tools/soak_monitor.py` writes append-only JSONL to the selected private path
(prefer outside the repository). Every interval it performs a directed BACnet
identity probe and reads HTTPS status/configuration; an optional viewer token
can authenticate those reads. It never writes controller NVS or changes an input.

The monitor alerts on unavailable/malformed BACnet or HTTPS, reboot or firmware
identity drift, IP/configuration changes, unhealthy Ethernet/tasks/input fault,
unexpected COV subscribers, heap/temperature thresholds, new protocol errors,
counter regression, or decreasing uptime. Physical input state changes are
logged but are not failures unless the input reports a fault.

```sh
python3 tools/soak_monitor.py \
  --device-address DEVICE_IP \
  --device-instance 599152 \
  --duration 86400 \
  --interval 60 \
  --timeout 5 \
  --minimum-heap-bytes 1000000 \
  --maximum-temperature-c 85 \
  --summary-every 15 \
  --certificate main/ota_server_cert.pem \
  --token-file /secure/path/ota_viewer_token.txt \
  --output /secure/path/soak-24h.jsonl
```

The output must not already exist. Rows are flushed after each sample; the
final summary reports availability, alerts, latency, temperature, heap change,
failures, and interruption state. Exit is zero only after the full duration
with no failures or alerts. Do not commit a report: it excludes credentials but
contains site network/configuration details.

Run finite HIL/negative tests before starting the soak. They can consume COV
slots and increase protocol/rate-limit counters even when every assertion
passes. Do not run them concurrently with a clean endurance experiment.
Avoid overlapping browsers/pollers beyond the server's two TLS sockets.

Before starting another monitor, check whether a session already owns an active
run. Do not kill/restart it, reboot/flash the controller, change configuration,
or reuse/truncate its output just to merge code or refresh a report. Preserve
the PID, start time, private output path and command options in the local
handoff; never paste credentials or raw site configuration into GitHub.

An interrupted, alerting, in-progress, or short run is **not a 24-hour pass**.
After completion, add a sanitized summary to
[VALIDATION_HISTORY.md](VALIDATION_HISTORY.md): tested source/version/image,
requested and elapsed duration, successful/failed samples, alerts and final
exit/interruption state. Keep raw JSONL private. See [TESTING.md](TESTING.md)
for the complete evidence and maintenance rules.
