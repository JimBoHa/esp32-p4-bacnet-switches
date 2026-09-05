"use strict";

(() => {
  const element = (id) => document.getElementById(id);
  const connectForm = element("connectForm");
  const tokenInput = element("token");
  const connectedControls = element("connectedControls");
  const dashboard = element("dashboard");
  const connectionStatus = element("connectionStatus");
  const overallHealth = element("overallHealth");
  const autoRefresh = element("autoRefresh");

  let bearerToken = "";
  let refreshTimer = 0;
  let activeRequest = null;

  const text = (id, value) => {
    element(id).textContent = value;
  };

  const finiteNumber = (value) =>
    typeof value === "number" && Number.isFinite(value) ? value : null;

  const formatInteger = (value) => {
    const number = finiteNumber(value);
    return number === null ? "—" : Math.trunc(number).toLocaleString();
  };

  const formatBytes = (value) => {
    const number = finiteNumber(value);
    if (number === null || number < 0) return "—";
    const units = ["B", "KiB", "MiB", "GiB"];
    let scaled = number;
    let unit = 0;
    while (scaled >= 1024 && unit < units.length - 1) {
      scaled /= 1024;
      unit += 1;
    }
    return `${scaled.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
  };

  const formatDuration = (value) => {
    const number = finiteNumber(value);
    if (number === null || number < 0) return "—";
    let seconds = Math.floor(number / 1000);
    const days = Math.floor(seconds / 86400);
    seconds %= 86400;
    const hours = Math.floor(seconds / 3600);
    seconds %= 3600;
    const minutes = Math.floor(seconds / 60);
    seconds %= 60;
    return `${days}d ${hours}h ${minutes}m ${seconds}s`;
  };

  const yesNo = (value) => value === true ? "Yes" : value === false ? "No" : "—";
  const onOff = (value) => value === true ? "ON" : value === false ? "OFF" : "—";

  const clearChildren = (node) => {
    while (node.firstChild) node.removeChild(node.firstChild);
  };

  const appendCell = (row, value, className = "") => {
    const cell = document.createElement("td");
    cell.textContent = String(value);
    if (className) cell.className = className;
    row.appendChild(cell);
  };

  const pill = (label, condition, warning = false) => {
    const span = document.createElement("span");
    span.className = `pill ${condition ? "pill-good" : warning ? "pill-warn" : "pill-bad"}`;
    span.textContent = label;
    return span;
  };

  const renderDetails = (id, rows) => {
    const list = element(id);
    clearChildren(list);
    rows.forEach(([label, value]) => {
      const term = document.createElement("dt");
      const detail = document.createElement("dd");
      term.textContent = label;
      detail.textContent = String(value ?? "—");
      list.append(term, detail);
    });
  };

  const renderInputs = (inputs) => {
    const body = element("inputRows");
    clearChildren(body);
    (Array.isArray(inputs) ? inputs : []).forEach((input) => {
      const item = input && typeof input === "object" ? input : {};
      const signal = item.signal && typeof item.signal === "object" ? item.signal : {};
      const current = item.current && typeof item.current === "object" ? item.current : {};
      const selfTest = item.self_test && typeof item.self_test === "object" ? item.self_test : {};
      const row = document.createElement("tr");
      appendCell(row, item.gpio ?? "—", "number");
      const state = document.createElement("td");
      state.appendChild(pill(onOff(current.stable), current.stable === false, current.stable === true));
      row.appendChild(state);
      appendCell(row, onOff(current.raw));
      appendCell(row, selfTest.run === true ? (selfTest.classification ?? "unknown") : "not run");
      appendCell(row, formatInteger(signal.raw_edge_count), "number");
      appendCell(row, formatInteger(signal.accepted_transition_count), "number");
      appendCell(row, formatInteger(signal.rejected_pulse_count), "number");
      const chatter = document.createElement("td");
      chatter.appendChild(pill(signal.chattering === true ? "ACTIVE" : "Quiet", signal.chattering !== true));
      row.appendChild(chatter);
      body.appendChild(row);
    });
  };

  const renderWatchdogs = (watchdogs) => {
    const body = element("watchdogRows");
    clearChildren(body);
    Object.entries(watchdogs && typeof watchdogs === "object" ? watchdogs : {}).forEach(([name, value]) => {
      const item = value && typeof value === "object" ? value : {};
      const row = document.createElement("tr");
      appendCell(row, name);
      appendCell(row, yesNo(item.subscribed));
      const health = document.createElement("td");
      health.appendChild(pill(item.healthy === true ? "Healthy" : "Fault", item.healthy === true));
      row.appendChild(health);
      appendCell(row, formatDuration(item.last_heartbeat_ms), "number");
      body.appendChild(row);
    });
  };

  const renderFaultLog = (events) => {
    const body = element("faultRows");
    clearChildren(body);
    const ordered = Array.isArray(events) ? [...events].reverse() : [];
    ordered.forEach((event) => {
      const item = event && typeof event === "object" ? event : {};
      const row = document.createElement("tr");
      appendCell(row, formatInteger(item.sequence), "number");
      appendCell(row, formatInteger(item.boot_count), "number");
      appendCell(row, formatDuration(item.uptime_ms), "number");
      appendCell(row, item.type ?? "unknown");
      appendCell(row, formatInteger(item.code), "number");
      body.appendChild(row);
    });
  };

  const renderInputHistory = (history) => {
    const body = element("inputHistoryRows");
    clearChildren(body);
    const events = history && Array.isArray(history.events)
      ? history.events.slice(-64).reverse() : [];
    text("inputHistoryStatus", `${events.length} events shown · ${formatInteger(history && history.overwritten_events)} older events overwritten`);
    if (events.length === 0) {
      const row = document.createElement("tr");
      const cell = document.createElement("td");
      cell.colSpan = 6;
      cell.textContent = "No input events recorded for this boot.";
      row.appendChild(cell);
      body.appendChild(row);
      return;
    }
    events.forEach((event) => {
      const item = event && typeof event === "object" ? event : {};
      const row = document.createElement("tr");
      appendCell(row, formatInteger(item.sequence), "number");
      appendCell(row, item.gpio ?? "—", "number");
      appendCell(row, `${formatDuration(item.uptime_ms)} + ${formatInteger(finiteNumber(item.uptime_ms) === null ? null : item.uptime_ms % 1000)} ms`, "number");
      appendCell(row, item.type ?? "unknown");
      appendCell(row, onOff(item.active));
      appendCell(row, item.type === "rejected-pulse" ? `${formatInteger(item.pulse_width_ms)} ms` : "—");
      body.appendChild(row);
    });
  };

  const render = (status) => {
    const system = status.system && typeof status.system === "object" ? status.system : {};
    const network = status.network && typeof status.network === "object" ? status.network : {};
    const config = status.configuration && typeof status.configuration === "object" ? status.configuration : {};
    const networkConfig = status.network_configuration && typeof status.network_configuration === "object" ? status.network_configuration : {};
    const bacnet = status.bacnet && typeof status.bacnet === "object" ? status.bacnet : {};
    const watchdogs = system.task_watchdog && typeof system.task_watchdog === "object" ? system.task_watchdog : {};
    const inputs = Array.isArray(status.gpio_diagnostics) ? status.gpio_diagnostics : [];
    const allWatchdogsHealthy = Object.keys(watchdogs).length >= 2 && Object.values(watchdogs).every((item) => item && item.healthy === true);
    const allInputsHealthy = inputs.length === 3 && inputs.every((item) => item && item.fault === false && item.signal && item.signal.chattering !== true);
    const configurationHealthy = config.restart_required === false && networkConfig.restart_required === false && networkConfig.trial_active === false;
    const healthy = status.state === "valid" && network.link_up === true && Boolean(network.ipv4) && allWatchdogsHealthy && allInputsHealthy && configurationHealthy;

    overallHealth.textContent = healthy ? "Healthy" : "Attention required";
    overallHealth.className = `health ${healthy ? "health-good" : "health-bad"}`;
    text("firmware", status.version ?? "—");
    text("source", status.git_revision ?? "—");
    text("uptime", formatDuration(system.uptime_ms));
    text("bootCount", `Boot ${formatInteger(system.boot_count)}`);
    text("temperature", finiteNumber(system.chip_temperature_c) === null ? "—" : `${system.chip_temperature_c.toFixed(1)} °C`);
    text("freeHeap", formatBytes(system.free_heap_bytes));
    text("minimumHeap", `minimum ${formatBytes(system.minimum_free_heap_bytes)}`);

    renderDetails("identityDetails", [
      ["Project", status.project],
      ["Firmware state", status.state],
      ["ESP-IDF", status.idf_version],
      ["OTA partition", status.partition],
      ["Image SHA-256", status.image_sha256],
      ["Reset reason", system.reset_reason && system.reset_reason.name],
      ["Last OTA result", system.last_ota_result],
      ["Config revision", `${config.active_database_revision ?? "—"} active / ${config.saved_database_revision ?? "—"} saved`],
    ]);
    renderDetails("networkDetails", [
      ["Link", network.link_up === true ? `${network.speed_mbps ?? "—"} Mb/s ${network.full_duplex === true ? "full duplex" : "half duplex"}` : "Down"],
      ["IPv4", network.ipv4],
      ["Netmask", network.netmask],
      ["Gateway", network.gateway],
      ["MAC", network.mac],
      ["Address mode", networkConfig.mode],
      ["Hostname", networkConfig.hostname],
      ["Link losses", formatInteger(network.link_down_count)],
      ["Reconnects", formatInteger(network.reconnect_count)],
      ["Address age", formatDuration(network.address_age_ms)],
    ]);
    renderDetails("bacnetDetails", [
      ["Device instance", formatInteger(bacnet.device_instance)],
      ["Vendor identifier", formatInteger(bacnet.vendor_identifier)],
      ["UDP port", formatInteger(bacnet.port ?? 47808)],
      ["Received packets", formatInteger(bacnet.rx)],
      ["Responses", formatInteger(bacnet.responses)],
      ["Who-Is", formatInteger(bacnet.who_is)],
      ["Who-Has", formatInteger(bacnet.who_has)],
      ["ReadProperty", formatInteger(bacnet.read_property)],
      ["ReadPropertyMultiple", formatInteger(bacnet.read_property_multiple)],
      ["COV subscriptions", formatInteger(bacnet.active_cov_subscriptions)],
      ["Malformed packets", formatInteger(bacnet.malformed)],
      ["Protocol errors", formatInteger(bacnet.errors)],
      ["Rate limited", formatInteger(bacnet.rate_limited)],
    ]);
    renderInputs(inputs);
    renderInputHistory(status.input_history);
    renderWatchdogs(watchdogs);
    renderFaultLog(status.fault_log);
    text("lastUpdated", `Updated ${new Date().toLocaleTimeString()}`);
    dashboard.hidden = false;
  };

  const scheduleRefresh = () => {
    window.clearTimeout(refreshTimer);
    if (bearerToken && autoRefresh.checked) {
      refreshTimer = window.setTimeout(refresh, 5000);
    }
  };

  const refresh = async () => {
    if (!bearerToken || activeRequest) return;
    const tokenForRequest = bearerToken;
    const controller = new AbortController();
    let requestTimedOut = false;
    const requestTimeout = window.setTimeout(() => {
      requestTimedOut = true;
      controller.abort();
    }, 10000);
    activeRequest = controller;
    connectionStatus.className = "status-line";
    connectionStatus.textContent = "Refreshing authenticated diagnostics…";
    try {
      const response = await window.fetch("/ota/status", {
        method: "GET",
        headers: {"Authorization": `Bearer ${tokenForRequest}`, "Accept": "application/json"},
        cache: "no-store",
        credentials: "omit",
        signal: controller.signal,
      });
      if (activeRequest !== controller || bearerToken !== tokenForRequest) return;
      if (!response.ok) {
        if (response.status === 401) bearerToken = "";
        throw new Error(`HTTP ${response.status}`);
      }
      const status = await response.json();
      if (activeRequest !== controller || bearerToken !== tokenForRequest) return;
      if (!status || typeof status !== "object" || Array.isArray(status)) {
        throw new Error("invalid status document");
      }
      render(status);
      connectedControls.hidden = false;
      connectionStatus.textContent = "Authenticated. Device data is read-only.";
    } catch (error) {
      if (activeRequest !== controller) return;
      if (controller.signal.aborted && !requestTimedOut) return;
      connectionStatus.className = "status-line error";
      connectionStatus.textContent = `Diagnostics unavailable: ${requestTimedOut ? "request timed out" : error instanceof Error ? error.message : "request failed"}`;
      if (!bearerToken) {
        dashboard.hidden = true;
        connectedControls.hidden = true;
        overallHealth.textContent = "Not connected";
        overallHealth.className = "health health-idle";
      } else {
        overallHealth.textContent = "Connection error";
        overallHealth.className = "health health-bad";
      }
    } finally {
      window.clearTimeout(requestTimeout);
      if (activeRequest === controller) {
        activeRequest = null;
        scheduleRefresh();
      }
    }
  };

  connectForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const candidate = tokenInput.value.trim();
    if (!/^[\x21-\x7e]{32,128}$/.test(candidate)) {
      connectionStatus.className = "status-line error";
      connectionStatus.textContent = "Token must contain 32–128 printable characters without spaces.";
      return;
    }
    window.clearTimeout(refreshTimer);
    if (activeRequest) {
      activeRequest.abort();
      activeRequest = null;
    }
    bearerToken = candidate;
    tokenInput.value = "";
    refresh();
  });

  element("refresh").addEventListener("click", refresh);
  autoRefresh.addEventListener("change", scheduleRefresh);
  element("disconnect").addEventListener("click", () => {
    bearerToken = "";
    window.clearTimeout(refreshTimer);
    if (activeRequest) {
      activeRequest.abort();
      activeRequest = null;
    }
    dashboard.hidden = true;
    connectedControls.hidden = true;
    overallHealth.textContent = "Not connected";
    overallHealth.className = "health health-idle";
    connectionStatus.className = "status-line";
    connectionStatus.textContent = "Token cleared from page memory.";
    tokenInput.focus();
  });
})();
