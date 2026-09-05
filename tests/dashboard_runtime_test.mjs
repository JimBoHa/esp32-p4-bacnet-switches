import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import vm from "node:vm";


class FakeElement {
  constructor(id = "") {
    this.id = id;
    this.checked = id === "autoRefresh";
    this.children = [];
    this.className = "";
    this.hidden = id === "connectedControls" || id === "dashboard";
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

let fetchImplementation = () => {
  throw new Error("unexpected fetch");
};
let nextTimer = 1;
const timers = new Map();
let downloadedBlob;
const revokedUrls = [];
const window = {
  URL: {
    createObjectURL: (blob) => { downloadedBlob = blob; return "blob:test-report"; },
    revokeObjectURL: (url) => revokedUrls.push(url),
  },
  clearTimeout: (id) => timers.delete(id),
  fetch: (...arguments_) => fetchImplementation(...arguments_),
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
vm.runInNewContext(source, {AbortController, Blob, document, window});

const listener = (id, type) => {
  const value = element(id).listeners.get(type);
  assert.equal(typeof value, "function", `${id} has no ${type} listener`);
  return value;
};
const submit = (token) => {
  element("token").value = token;
  listener("connectForm", "submit")({preventDefault() {}});
};
const flush = () => new Promise((resolve) => setImmediate(resolve));

let finishFirstFetch;
let firstOptions;
fetchImplementation = (_url, options) => {
  firstOptions = options;
  return new Promise((resolve) => {
    finishFirstFetch = resolve;
  });
};
submit("A".repeat(32));
assert(firstOptions.signal instanceof AbortSignal);
listener("disconnect", "click")();
assert.equal(firstOptions.signal.aborted, true);
finishFirstFetch({
  ok: true,
  status: 200,
  json: async () => ({state: "valid"}),
});
await flush();
await flush();
assert.equal(element("dashboard").hidden, true);
assert.equal(element("connectedControls").hidden, true);
assert.equal(element("overallHealth").textContent, "Not connected");

const healthyStatus = {
  state: "valid",
  access_role: "viewer",
  security: {software_signature_verification: true},
  clock: {utc_unix_ms: 1577836800000, clock_quality: "synchronized",
          configured_server: "site.ntp", sync_count: 1, rejected_sync_count: 0},
  version: "1.20.0",
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
submit("B".repeat(32));
await flush();
await flush();
assert.equal(element("dashboard").hidden, false);
assert.equal(element("overallHealth").textContent, "Healthy");
assert.match(element("connectionStatus").textContent, /Authenticated as viewer/);
assert.match(element("signaturePolicy").textContent, /Signed OTA required/);
assert.equal(element("inputHistoryRows").children.length, 2);
assert.equal(element("inputHistoryRows").children[0].children[0].textContent, "2");
assert.equal(element("inputHistoryRows").children[0].children[6].textContent, "20 ms");
assert.match(element("inputHistoryRows").children[0].children[3].textContent, /Unknown.*unsynchronized/);
assert.match(element("faultRows").children[0].children[3].textContent, /2020-01-01T00:00:00.000Z.*synchronized/);
assert(element("clockDetails").children.some(child => child.textContent === "2020-01-01T00:00:00.000Z"));
assert.match(element("inputHistoryStatus").textContent, /2 events shown/);

const diagnosticReport = {schema: 1, report_type: "esp32-p4-diagnostics", status: healthyStatus};
fetchImplementation = async (url, options) => {
  assert.equal(url, "/diagnostics/report");
  assert.equal(options.headers.Authorization, `Bearer ${"B".repeat(32)}`);
  assert.equal(options.redirect, "error");
  return {ok: true, status: 200, json: async () => diagnosticReport};
};
await listener("downloadReport", "click")();
assert.equal(downloadedBlob.type, "application/json");
assert.deepEqual(JSON.parse(await downloadedBlob.text()), diagnosticReport);
assert.equal((await downloadedBlob.text()).includes("B".repeat(32)), false);
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
listener("disconnect", "click")();
assert.equal(reportOptions.signal.aborted, true);
finishReport({ok: true, status: 200, json: async () => diagnosticReport});
await cancelledDownload;
assert.equal(document.body.children.length, 1);
assert.equal(element("downloadReport").disabled, false);

fetchImplementation = async () => ({ok: true, status: 200, json: async () => healthyStatus});
submit("B".repeat(32));
await flush();
await flush();
fetchImplementation = async () => ({ok: false, status: 401});
await listener("downloadReport", "click")();
assert.equal(element("dashboard").hidden, true);
assert.match(element("reportStatus").textContent, /HTTP 401/);

fetchImplementation = async () => ({ok: true, status: 200, json: async () => healthyStatus});
submit("B".repeat(32));
await flush();
await flush();

fetchImplementation = async () => ({ok: false, status: 401});
await listener("refresh", "click")();
assert.equal(element("dashboard").hidden, true);
assert.equal(element("connectedControls").hidden, true);
assert.equal(element("overallHealth").textContent, "Not connected");
assert.match(element("connectionStatus").textContent, /HTTP 401/);

console.log("dashboard cancellation and authentication runtime checks passed");
