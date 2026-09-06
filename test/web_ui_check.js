#!/usr/bin/env node

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(
  process.argv[2] || path.join(__dirname, "../src/main.cpp"),
  "utf8",
);
const page = source.match(/const char kPage\[\][\s\S]*?R"HTML\(([\s\S]*?)\)HTML";/);
assert(page, "embedded web page not found");
const script = page[1].match(/<script>([\s\S]*?)<\/script>/);
assert(script, "embedded web UI script not found");

let activeRequests = 0;
let maximumActiveRequests = 0;
const intervals = [];
const pendingFetch = () => {
  activeRequests += 1;
  maximumActiveRequests = Math.max(maximumActiveRequests, activeRequests);
  return new Promise(() => {});
};

vm.runInNewContext(script[1], {
  AbortController,
  alert() {},
  clearTimeout() {},
  confirm: () => false,
  document: {
    addEventListener() {},
    querySelector() {},
    querySelectorAll: () => [],
  },
  fetch: pendingFetch,
  setInterval: callback => intervals.push(callback),
  setTimeout() {},
});

assert.equal(intervals.length, 1, "web UI must register one background poll");
for (let tick = 0; tick < 3; tick += 1) intervals[0]();
assert.equal(
  maximumActiveRequests,
  1,
  `background polling accumulated ${maximumActiveRequests} pending requests`,
);

async function checkStalledBodyTimeout() {
  let abort;
  let timeoutCleared = false;
  const element = {
    classList: {remove() {}, toggle() {}},
    replaceChildren() {},
  };
  const context = {
    AbortController,
    alert() {},
    clearTimeout() {
      timeoutCleared = true;
    },
    confirm: () => false,
    document: {
      addEventListener() {},
      querySelector: () => element,
      querySelectorAll: () => [],
    },
    fetch(_url, options) {
      return Promise.resolve({
        ok: true,
        status: 200,
        json: () =>
          new Promise((_resolve, reject) => {
            options.signal.addEventListener("abort", () => reject(Error("aborted")));
          }),
      });
    },
    setInterval() {},
    setTimeout(callback) {
      abort = callback;
      return 1;
    },
  };

  vm.runInNewContext(script[1], context);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(timeoutCleared, false, "body read lost its request timeout");
  abort();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(timeoutCleared, true, "request timeout was not cleared after abort");
}

checkStalledBodyTimeout().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
