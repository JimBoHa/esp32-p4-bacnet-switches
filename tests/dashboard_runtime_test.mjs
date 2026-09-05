import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import vm from "node:vm";


class FakeElement {
  constructor(id = "") {
    this.id = id;
    this.checked = id === "autoRefresh";
    this.children = [];
    this.className = "";
    this.hidden = id === "dashboard";
    this.listeners = new Map();
    this.textContent = "";
    this.value = "";
  }

  get firstChild() {
    return this.children[0] ?? null;
  }

  addEventListener(type, listener) {
    this.listeners.set(type, listener);
  }

  append(...children) {
    this.children.push(...children);
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }

  focus() {}

  click() { this.clicked = true; }

  remove() { this.removed = true; }

  removeChild(child) {
    const index = this.children.indexOf(child);
    assert.notEqual(index, -1);
    this.children.splice(index, 1);
    return child;
  }
}


const elements = new Map();
const element = (id) => {
  if (!elements.has(id)) elements.set(id, new FakeElement(id));
  return elements.get(id);
};
const document = {
  body: new FakeElement("body"),
  createElement: (tag) => new FakeElement(tag),
  getElementById: element,
};

let finishFirstFetch;
let firstOptions;
let fetchImplementation = (url, options) => {
  assert.equal(url, "/ota/status");
  firstOptions = options;
  return new Promise((resolve) => { finishFirstFetch = resolve; });
};
let nextTimer = 1;
const timers = new Map();
let downloadedBlob;
const revokedUrls = [];
const pageListeners = new Map();
const requests = [];
const window = {
  addEventListener: (type, callback) => pageListeners.set(type, callback),
  URL: {
    createObjectURL: (blob) => { downloadedBlob = blob; return "blob:test-report"; },
    revokeObjectURL: (url) => revokedUrls.push(url),
  },
  clearTimeout: (id) => timers.delete(id),
  fetch: (url, options) => {
    assert.equal(options.method, "GET");
    assert.equal(options.headers.Authorization, undefined);
    assert.equal(options.headers.Accept, "application/json");
    assert.equal(options.credentials, "omit");
    assert.equal(options.cache, "no-store");
    assert.equal(options.redirect, "error");
    requests.push({url, options});
    return fetchImplementation(url, options);
  },
  setTimeout: (callback, delay) => {
    const id = nextTimer++;
    timers.set(id, {callback, delay});
    return id;
  },
};

const source = readFileSync(
  new URL("../main/diagnostics_dashboard.js", import.meta.url),
  "utf8",
);
vm.runInNewContext(source, {AbortController, Blob, document, Error, window});

const listener = (id, type) => {
  const value = element(id).listeners.get(type);
  assert.equal(typeof value, "function", `${id} has no ${type} listener`);
  return value;
};
const flush = () => new Promise((resolve) => setImmediate(resolve));
const pageHide = () => pageListeners.get("pagehide")();
const pageShow = () => pageListeners.get("pageshow")({persisted: true});
const fireTimer = (delay) => {
  const match = [...timers].find(([, timer]) => timer.delay === delay);
  assert(match, `no ${delay} ms timer`);
  const [id, timer] = match;
  timers.delete(id);
  timer.callback();
};

// First request starts automatically, with no form, token, or user interaction.
assert.equal(requests.length, 1);
assert(firstOptions.signal instanceof AbortSignal);
await listener("refresh", "click")();
assert.equal(requests.length, 1, "overlapping refresh was not suppressed");
pageHide();
assert.equal(firstOptions.signal.aborted, true);
finishFirstFetch({
  ok: true,
  status: 200,
  json: async () => ({state: "valid"}),
});
await flush();
await flush();
assert.equal(element("dashboard").hidden, true);
assert.equal(element("connectedControls").hidden, false);
assert.equal(element("overallHealth").textContent, "Not connected");
assert.equal(timers.size, 0);

const healthyStatus = {
  state: "valid",
  access_role: "anonymous",
  security: {software_signature_verification: true, anonymous_read_only: true},
  clock: {utc_unix_ms: 1577836800000, clock_quality: "synchronized",
          configured_server: "site.ntp", sync_count: 1, rejected_sync_count: 0},
  version: "1.28.0",
  system: {
    chip_temperature_c: 36.4,
    task_watchdog: {
      bacnet: {healthy: true, subscribed: true},
      switch_inputs: {healthy: true, subscribed: true},
    },
  },
  network: {ipv4: "192.168.75.152", link_up: true},
  configuration: {restart_required: false},
  network_configuration: {restart_required: false, trial_active: false},
  gpio_diagnostics: [20, 21, 22].map((gpio) => ({
    current: {raw: false, stable: false},
    fault: false,
    gpio,
    signal: {chattering: false},
  })),
  bacnet: {},
  header_diagnostics: {
    gpio_count: 27,
    pins: [54, 19, null, 18, 17, 16, 15, null, 14, 6, 5, 4, null, 3, 2, 8, 7,
           null, 24, 25, 48, 47, null, 46, 33, 32, 27, null, 26, null, 23, 22,
           null, 21, 20, null, null, null, null, null].map((gpio, index) => ({
      position: index + 1, gpio, label: gpio === null ? "GND" : `GPIO${gpio}`,
      status: gpio === null ? "non-gpio" : [24, 25].includes(gpio) ? "reserved-usb" : "readable",
      raw_level: gpio === null || [24, 25].includes(gpio) ? null : gpio === 47,
      pad: gpio === null ? null : {pull_up: false, pull_down: false,
                                  output_enable_controlled_by_peripheral: false, output_enabled: false},
      usage: "Header GPIO",
    })),
  },
  input_history: {
    overwritten_events: 0,
    events: [
      {sequence: 1, gpio: 20, uptime_ms: 20, type: "initial-state", active: false, pulse_width_ms: 0},
      {sequence: 2, gpio: 20, uptime_ms: 70, type: "rejected-pulse", active: false, pulse_width_ms: 20},
    ],
  },
  fault_log: [{sequence: 1, boot_count: 1, uptime_ms: 1000,
               utc_unix_ms: 1577836800000, clock_quality: "synchronized", type: "boot", code: 3}],
};
fetchImplementation = async () => ({
  ok: true,
  status: 200,
  json: async () => healthyStatus,
});
pageShow();
await flush();
await flush();
assert.equal(element("dashboard").hidden, false);
assert.equal(element("overallHealth").textContent, "Healthy");
assert.match(element("connectionStatus").textContent, /Read-only access. No login required/);
assert.match(element("signaturePolicy").textContent, /Signed OTA required/);
assert.equal(element("inputHistoryRows").children.length, 2);
assert.equal(element("inputHistoryRows").children[0].children[0].textContent, "2");
assert.equal(element("inputHistoryRows").children[0].children[6].textContent, "20 ms");
assert.match(element("inputHistoryRows").children[0].children[3].textContent, /Unknown.*unsynchronized/);
assert.match(element("faultRows").children[0].children[3].textContent, /2020-01-01T00:00:00.000Z.*synchronized/);
assert(element("clockDetails").children.some(child => child.textContent === "2020-01-01T00:00:00.000Z"));
assert.match(element("inputHistoryStatus").textContent, /2 events shown/);
assert.equal(element("headerPinRows").children.length, 40);
assert.match(element("headerPinsStatus").textContent, /25 of 27 GPIOs readable/);
assert.match(element("headerPinsStatus").textContent, /HIGH: P1-22 \/ GPIO47/);
assert.equal(element("headerPinRows").children[21].children[3].textContent, "HIGH");
assert.equal(element("headerPinRows").children[34].children[3].textContent, "LOW");
assert.equal(element("headerPinRows").children[2].children[3].textContent, "N/A");
assert.equal(element("headerPinRows").children[18].children[3].textContent, "N/A");
assert.equal(element("headerPinRows").children[18].children[4].textContent, "reserved-usb");
assert.equal(element("headerPinRows").children[21].children[5].textContent, "None (may float)");

// Polling can be disabled; manual refresh works without restarting it.
assert.equal([...timers.values()].filter(timer => timer.delay === 5000).length, 1);
element("autoRefresh").checked = false;
listener("autoRefresh", "change")();
assert.equal(timers.size, 0);
await listener("refresh", "click")();
assert.equal(timers.size, 0);
element("autoRefresh").checked = true;
listener("autoRefresh", "change")();
const countBeforePoll = requests.length;
fireTimer(5000);
await flush();
assert.equal(requests.length, countBeforePoll + 1);

const diagnosticReport = {schema: 1, report_type: "esp32-p4-diagnostics", status: healthyStatus};
fetchImplementation = async (url, options) => {
  assert.equal(url, "/diagnostics/report");
  assert.equal(options.headers.Authorization, undefined);
  assert.equal(options.redirect, "error");
  return {ok: true, status: 200, json: async () => diagnosticReport};
};
await listener("downloadReport", "click")();
assert.equal(downloadedBlob.type, "application/json");
assert.deepEqual(JSON.parse(await downloadedBlob.text()), diagnosticReport);
const downloadLink = document.body.children.at(-1);
assert.equal(downloadLink.clicked, true);
assert.equal(downloadLink.removed, true);
assert.match(downloadLink.download, /^esp32-p4-diagnostics-.*\.json$/);
for (const [id, timer] of timers) {
  if (timer.delay === 1000) { timer.callback(); timers.delete(id); }
}
assert.deepEqual(revokedUrls, ["blob:test-report"]);

let finishReport;
let reportOptions;
fetchImplementation = (_url, options) => {
  reportOptions = options;
  return new Promise(resolve => { finishReport = resolve; });
};
const cancelledDownload = listener("downloadReport", "click")();
assert.equal(element("downloadReport").disabled, true);
const countBeforeDuplicate = requests.length;
await listener("downloadReport", "click")();
assert.equal(requests.length, countBeforeDuplicate);
pageHide();
assert.equal(reportOptions.signal.aborted, true);
finishReport({ok: true, status: 200, json: async () => diagnosticReport});
await cancelledDownload;
assert.equal(document.body.children.length, 1);
assert.equal(element("downloadReport").disabled, false);

fetchImplementation = async () => ({ok: true, status: 200, json: async () => healthyStatus});
pageShow();
await flush();
await flush();
fetchImplementation = async () => ({ok: false, status: 401});
await listener("downloadReport", "click")();
assert.equal(element("dashboard").hidden, false);
assert.match(element("reportStatus").textContent, /HTTP 401/);

// Failed reads hide stale live data and remain retryable without a login form.
fetchImplementation = async () => ({ok: false, status: 401});
await listener("refresh", "click")();
assert.equal(element("dashboard").hidden, true);
assert.equal(element("connectedControls").hidden, false);
assert.equal(element("overallHealth").textContent, "Connection error");
assert.match(element("connectionStatus").textContent, /HTTP 401/);
fetchImplementation = async () => ({ok: true, status: 200, json: async () => healthyStatus});
fireTimer(5000);
await flush();
assert.equal(element("dashboard").hidden, false);

fetchImplementation = async () => { throw new Error("network unavailable"); };
await listener("refresh", "click")();
assert.equal(element("dashboard").hidden, true);
assert.match(element("connectionStatus").textContent, /network unavailable/);

// A late JSON body cannot restore live data after the request deadline.
let finishStatusBody;
let timedStatusOptions;
fetchImplementation = async (_url, options) => {
  timedStatusOptions = options;
  return {ok: true, status: 200, json: () => new Promise(resolve => { finishStatusBody = resolve; })};
};
const timedStatus = listener("refresh", "click")();
await flush();
fireTimer(10000);
assert.equal(timedStatusOptions.signal.aborted, true);
finishStatusBody(healthyStatus);
await timedStatus;
assert.equal(element("dashboard").hidden, true);
assert.match(element("connectionStatus").textContent, /request timed out/);

let finishReportBody;
fetchImplementation = async () => ({
  ok: true, status: 200,
  json: () => new Promise(resolve => { finishReportBody = resolve; }),
});
const timedReport = listener("downloadReport", "click")();
await flush();
fireTimer(15000);
finishReportBody(diagnosticReport);
await timedReport;
assert.equal(document.body.children.length, 1);
assert.equal(element("downloadReport").disabled, false);
assert.match(element("reportStatus").textContent, /request timed out/);

fetchImplementation = async () => ({ok: true, status: 200, json: async () => []});
await listener("refresh", "click")();
assert.match(element("connectionStatus").textContent, /invalid status document/);
await listener("downloadReport", "click")();
assert.match(element("reportStatus").textContent, /invalid report document/);

pageHide();
const finalRequestCount = requests.length;
await listener("refresh", "click")();
await listener("downloadReport", "click")();
assert.equal(requests.length, finalRequestCount);
assert.equal(timers.size, 0);

console.log("dashboard automatic anonymous reads, polling, downloads, errors, and cancellation checks passed");
