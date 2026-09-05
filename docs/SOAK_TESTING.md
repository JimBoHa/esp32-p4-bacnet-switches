# Soak testing

`tools/soak_monitor.py` writes append-only JSONL outside the repository. Every
interval it performs a directed BACnet identity probe and reads authenticated
HTTPS status/configuration. It never writes controller NVS or changes an input.

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
  --token-file /secure/path/ota_token.txt \
  --output /secure/path/soak-24h.jsonl
```

The output must not already exist. Rows are flushed after each sample; the
final summary reports availability, alerts, latency, temperature, heap change,
failures, and interruption state. Exit is zero only after the full duration
with no failures or alerts. Do not commit a report: it excludes credentials but
contains site network/configuration details.
