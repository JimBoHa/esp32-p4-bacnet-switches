"""Validate the real C serializer and GPIO mock output for every failure mode."""
import json
import subprocess
import sys


for scenario in range(5):
    result = subprocess.run([sys.argv[1], str(scenario)], check=True,
                            capture_output=True, text=True)
    before, normal, changed = [json.loads(line) for line in result.stdout.splitlines()]
    assert before["initialized"] is False and before["readable_count"] == 0
    assert all(pin["raw_level"] is None for pin in before["pins"])
    assert normal["initialized"] is True
    assert normal["initialization_ok"] is (scenario == 0)
    assert normal["readable_count"] == (25 if scenario == 0 else 24)
    assert normal["captured_uptime_ms"] == normal["completed_uptime_ms"] == 5000000000
    assert normal["sample_mode"] == "sequential-on-request"
    assert normal["position_count"] == len(normal["pins"]) == 40
    assert normal["gpio_count"] == 27
    assert [p["position"] for p in normal["pins"]] == list(range(1, 41))
    assert [p["gpio"] for p in normal["pins"] if p["raw_level"] is True] == [54, 47]
    for pin in normal["pins"]:
        if pin["gpio"] is None:
            assert pin["status"] == "non-gpio" and pin["raw_level"] is None and pin["pad"] is None
        elif pin["gpio"] in (24, 25):
            assert pin["status"] == "reserved-usb" and pin["raw_level"] is None and pin["pad"] is None
        elif pin["gpio"] == 2 and scenario:
            assert pin["status"] == "initialization-error" and pin["raw_level"] is None
        else:
            assert pin["status"] == "readable" and type(pin["raw_level"]) is bool
            assert pin["pad"]["input_enabled"] is True
            assert pin["initialization_preserved_config"] is True
    assert changed["readable_count"] == normal["readable_count"] - 2
    assert changed["pins"][11]["status"] == "input-disabled"
    assert changed["pins"][13]["status"] == "configuration-error"
    assert changed["pins"][11]["raw_level"] is None
    assert changed["pins"][13]["raw_level"] is None
print("All header GPIO safety and JSON scenarios passed")
