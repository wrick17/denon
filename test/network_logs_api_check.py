"""Compile and exercise the production log handler without ESP32 hardware."""

import json
from pathlib import Path
import re
import subprocess
import tempfile


source = (Path(__file__).parents[1] / "src/main.cpp").read_text()


def function(name):
    start = re.search(rf"^(?:bool|void) {name}\(", source, re.M).start()
    depth = 0
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]'
    for match in re.finditer(tokens, source[start:]):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start : start + match.end()]
    raise AssertionError(f"Unclosed production function {name}")


stub = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
bool failReserve = false;
struct String {
  std::string value;
  String() = default;
  String(const char *s) : value(s) {}
  String(std::string s) : value(s) {}
  String(uint32_t n) : value(std::to_string(n)) {}
  size_t length() const { return value.length(); }
  bool isEmpty() const { return value.empty(); }
  char operator[](size_t i) const { return value[i]; }
  bool reserve(size_t n) { if (failReserve) return false; value.reserve(n); return true; }
  String &operator+=(const String &s) { value += s.value; return *this; }
  String &operator+=(char c) { value += c; return *this; }
  friend String operator+(const String &a, const String &b) { return a.value + b.value; }
};
String apiToken = "secret";
struct Server {
  std::map<std::string, String> arguments, requestHeaders, responseHeaders;
  int status = 0;
  String body;
  bool setupAp = false;
  bool hasArg(const char *key) { return arguments.count(key); }
  String arg(const char *key) { return arguments[key]; }
  String header(const char *key) { return requestHeaders[key]; }
  void sendHeader(const char *key, const String &value) { responseHeaders[key] = value; }
  void send(int code, const char *, const String &value) { status = code; body = value; }
} server;
namespace networkLogs {
constexpr unsigned kMaxPage = 16;
struct Record { uint32_t seq, uptimeMs; uint16_t length; bool continued; char text[129]; };
struct Stats { uint32_t oldestSeq, nextSeq, dropped; };
std::vector<Record> records;
unsigned flushCalls = 0;
const char *bootId() { return "0123456789abcdef"; }
void flush() { ++flushCalls; }
Stats stats() { return {records.front().seq, records.back().seq + 1, 9}; }
bool readAfter(uint32_t after, Record &out) {
  for (const auto &record : records) if (record.seq > after) { out = record; return true; }
  return false;
}
void add(uint32_t seq, const char *text, uint16_t length, bool continued) {
  Record record = {seq, UINT32_MAX, length, continued, {}};
  memcpy(record.text, text, length);
  records.push_back(record);
}
void worstCase() {
  records.clear();
  char bytes[128]; memset(bytes, 0xff, sizeof(bytes));
  for (uint32_t seq = 1; seq <= 16; ++seq) add(seq, bytes, sizeof(bytes), true);
}
}
'''

harness = r'''
void respond(const std::string &name,
             std::map<std::string, String> arguments = {},
             const char *authorization = "Bearer secret", bool setupAp = false) {
  server = Server();
  server.arguments = arguments;
  server.setupAp = setupAp;
  server.requestHeaders["Authorization"] = authorization;
  sendLogs();
  std::cout << name << '\t' << server.status << '\t'
            << server.responseHeaders["Cache-Control"].value << '\t'
            << server.body.value << '\n';
}
int main(int argc, char **argv) {
  if (argc == 3) {
    networkLogs::worstCase();
    respond("cap", {{"after", argv[2]}});
    return 0;
  }
  const char bytes[] = {0, 1, '\n', '"', '\\', char(0x80), char(0xff)};
  networkLogs::add(10, bytes, sizeof(bytes), true);
  networkLogs::add(11, "normal\n", 7, false);
  respond("unauthorized", {}, "");
  respond("setup_unauthorized", {}, "", true);
  respond("wrong_token", {}, "Bearer wrong");
  respond("unauthorized_invalid_query", {{"after", "-1"}}, "");
  apiToken = "";
  respond("unpaired_setup", {}, "Bearer secret", true);
  apiToken = "secret";
  assert(networkLogs::flushCalls == 0);
  respond("default");
  respond("repeat");
  respond("setup_authorized", {}, "Bearer secret", true);
  respond("after10", {{"after", "10"}, {"limit", "1"}});
  respond("after11", {{"after", "11"}});
  respond("max_cursor", {{"after", "4294967295"}});
  respond("limit1", {{"limit", "1"}});
  respond("limit16", {{"limit", "16"}});
  std::vector<const char *> badAfter = {"", "-1", "+1", "1x", "1.0", " 1", "1 ", "01", "4294967296"};
  std::vector<const char *> badLimit = {"", "0", "17", "-1", "+1", "1.0", "1x", "4294967296"};
  for (size_t i = 0; i < badAfter.size(); ++i)
    respond("bad_after_" + std::to_string(i), {{"after", badAfter[i]}});
  for (size_t i = 0; i < badLimit.size(); ++i)
    respond("bad_limit_" + std::to_string(i), {{"limit", badLimit[i]}});
  assert(networkLogs::records.size() == 2);
  failReserve = true;
  respond("allocation_failed");
  failReserve = false;
  networkLogs::worstCase();
  respond("cap");
}
'''


def responses(executable, *args):
    result = subprocess.run([str(executable), *args], check=True, capture_output=True, text=True)
    parsed = {}
    for line in result.stdout.splitlines():
        name, status, cache, body = line.split("\t", 3)
        parsed[name] = (int(status), cache, body)
    return parsed


with tempfile.TemporaryDirectory(prefix="denon-log-api-") as directory:
    cpp = Path(directory) / "check.cpp"
    executable = Path(directory) / "check"
    names = ("constantTimeEquals", "hasAppAuthorization", "parseJsonUnsigned",
             "requireBackupAuthorization", "parseLogQueryUnsigned", "appendLogJsonText", "sendLogs")
    cpp.write_text(stub + "\n".join(function(name) for name in names) + harness)
    subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)], check=True)
    results = responses(executable)
    for name, (status, cache, body) in results.items():
        assert cache == "no-store", name
        if "unauthorized" in name or name in ("wrong_token", "unpaired_setup"):
            assert status == 401, name
        elif name.startswith("bad_"):
            assert status == 400, name
        elif name == "allocation_failed":
            assert status == 503, name
        else:
            assert status == 200 and len(body.encode()) <= 3072, name
            json.loads(body)
    default = json.loads(results["default"][2])
    assert default == json.loads(results["repeat"][2]) == json.loads(results["setup_authorized"][2])
    assert default["boot_id"] == "0123456789abcdef"
    assert (default["oldest_seq"], default["next_seq"], default["dropped"]) == (10, 12, 9)
    assert [record["seq"] for record in default["records"]] == [10, 11]
    assert default["records"][0]["text"].encode("latin1") == b'\x00\x01\n"\\\x80\xff'
    assert default["records"][0]["continued"] is True
    assert default["records"][1]["continued"] is False
    assert json.loads(results["after10"][2])["records"] == [default["records"][1]]
    assert json.loads(results["limit1"][2])["records"] == [default["records"][0]]
    assert json.loads(results["limit16"][2]) == default
    for name in ("after11", "max_cursor"):
        assert json.loads(results[name][2])["records"] == []
    after, collected = 0, []
    while True:
        page = responses(executable, "cap", str(after))["cap"]
        assert page == responses(executable, "cap", str(after))["cap"]
        status, cache, body = page
        assert status == 200 and cache == "no-store" and len(body.encode()) <= 3072
        payload = json.loads(body)
        assert payload["next_seq"] == 17
        if not payload["records"]:
            break
        assert 1 <= len(payload["records"]) < 16
        for record in payload["records"]:
            assert record["text"].encode("latin1") == bytes([255]) * 128
        collected.extend(record["seq"] for record in payload["records"])
        after = payload["records"][-1]["seq"]
    assert collected == list(range(1, 17))

print("Production log API authentication, query, byte escaping, response cap, and pagination checks passed")
