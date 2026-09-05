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
  createElement: (tag) => new FakeElement(tag),
  getElementById: element,
};

let fetchImplementation = () => {
  throw new Error("unexpected fetch");
};
let nextTimer = 1;
const timers = new Map();
const window = {
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
vm.runInNewContext(source, {AbortController, document, window});

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
  fault_log: [],
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
assert.equal(element("inputHistoryRows").children.length, 2);
assert.equal(element("inputHistoryRows").children[0].children[0].textContent, "2");
assert.equal(element("inputHistoryRows").children[0].children[5].textContent, "20 ms");
assert.match(element("inputHistoryStatus").textContent, /2 events shown/);

fetchImplementation = async () => ({ok: false, status: 401});
await listener("refresh", "click")();
assert.equal(element("dashboard").hidden, true);
assert.equal(element("connectedControls").hidden, true);
assert.equal(element("overallHealth").textContent, "Not connected");
assert.match(element("connectionStatus").textContent, /HTTP 401/);

console.log("dashboard cancellation and authentication runtime checks passed");
