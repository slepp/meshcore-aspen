import {test} from "node:test";
import assert from "node:assert/strict";
import {authorName, bytes, initials, mergeMessages, mergeProfiles, messageBody, postBytes, radioDeliveryIndicator, radioDeliveryLabel, validName} from "../web/model.js";

test("counts the actual RF text budget including UTF-8 display name", () => {
  assert.equal(bytes("Zoë"), 4);
  assert.equal(postBytes("Zoë", "🙂"), 10);
  assert.equal(postBytes("Alice", "x".repeat(144)), 151);
  assert.equal(postBytes("Alice", "x".repeat(145)), 152);
  for (const name of ["", "x".repeat(25), "Bad: name", " spaced ", "\0", "\ud800"])
    assert.equal(validName(name), false);
  assert.equal(validName("Zoë"), true);
  assert.equal(initials("Alice Smith"), "AS");
});

test("merges live/replayed messages once in canonical order without hiding conflicts", () => {
  const first = {seq: 1, timestamp: 100, author: "aa".repeat(32), text: "Alice: hello", webName: "Alice"};
  const second = {...first, seq: 2, timestamp: 101, text: "Alice: again"};
  const map = new Map();
  assert.deepEqual(mergeMessages(map, [second, first, first]), [first, second]);
  assert.equal(messageBody(first), "hello");
  assert.equal(messageBody({...first, webName: null}), "Alice: hello");
  assert.throws(() => mergeMessages(map, [{...first, text: "changed"}]), /conflicting/);
  assert.throws(() => mergeMessages(map, [{...first, seq: 0}]), /invalid/);
});

test("maps names by full key, keeps fingerprints separate and rejects invalid profile data", () => {
  const profiles = new Map(), author = "aa".repeat(32), other = "aaaaaaaa" + "bb".repeat(28);
  const profile = {publicKey: author, name: "Alice", advertType: 1, timestamp: 100, source: "radio"};
  mergeProfiles(profiles, [profile, {...profile, publicKey: other, name: "Bob"}]);
  assert.equal(authorName({author, webName: null}, profiles), "Alice");
  assert.equal(authorName({author: other, webName: null}, profiles), "Bob");
  assert.equal(authorName({author: "cc".repeat(32), webName: null}, profiles), "cccccccc…");
  assert.equal(authorName({author, webName: "Original web name"}, profiles), "Original web name");
  mergeProfiles(profiles, [{...profile, timestamp: 99, name: "Old"}, {...profile, timestamp: 101, name: "New"}]);
  assert.equal(authorName({author}, profiles), "New");
  for (const bad of [{name: "\u001b[31m"}, {source: "unverified"}, {publicKey: "aaaaaaaa"}, {timestamp: -1}, {advertType: 15}])
    assert.throws(() => mergeProfiles(profiles, [{...profile, ...bad}]), /invalid participant/);
});

test("shows RF transmission separately from recipient ACKs and refreshes status without changing history", () => {
  const rf = {recipients: 1, queued: 0, sent: 1, acknowledged: 0, uncertain: 0, failed: 0,
    paused: 0, retrying: 1, exhausted: 1, attempts: 4, nextRetryAt: null};
  assert.match(radioDeliveryLabel(rf), /Radio ACK 0\/1 sessions.*transmitted, awaiting ACK.*retry budget exhausted/);
  const message = {seq: 1, timestamp: 100, author: "aa".repeat(32), text: "hello", rf};
  const map = new Map();
  mergeMessages(map, [message]);
  const acknowledged = {...message, rf: {...rf, sent: 0, acknowledged: 1, retrying: 0, exhausted: 0}};
  mergeMessages(map, [acknowledged]);
  assert.equal(map.size, 1);
  assert.match(radioDeliveryLabel(map.get(1).rf), /Radio ACK 1\/1/);
  assert.match(radioDeliveryLabel(undefined), /unavailable/);
  for (const bad of [{sent: -1}, {recipients: 0}, {acknowledged: 1}, {paused: 2}, {nextRetryAt: -1}])
    assert.throws(() => mergeMessages(map, [{...message, rf: {...rf, ...bad}}]), /invalid RF/);
});

test("keeps waiting and paused counts in details, shows only ACK evidence or exceptional delivery", () => {
  const rf = {recipients: 10, queued: 10, sent: 0, acknowledged: 0, uncertain: 0, failed: 0,
    paused: 6, retrying: 0, exhausted: 0, attempts: 0, nextRetryAt: null};
  assert.deepEqual(radioDeliveryIndicator(rf), {count: 0, attention: false, label: "0 radio acknowledgements"});
  assert.match(radioDeliveryLabel(rf), /10 waiting for transmission.*6 paused/);
  const sent = {...rf, queued: 9, sent: 1, paused: 5, attempts: 1};
  assert.equal(radioDeliveryIndicator(sent).count, 0);
  const ack = {...sent, sent: 0, acknowledged: 1};
  assert.deepEqual(radioDeliveryIndicator(ack), {count: 1, attention: false, label: "1 radio acknowledgement"});
  assert.match(radioDeliveryLabel(ack), /Radio ACK 1\/10 sessions.*9 waiting for transmission.*5 paused/);
  for (const field of ["uncertain", "failed"]) {
    const status = {...rf, queued: 9, [field]: 1};
    assert.equal(radioDeliveryIndicator(status).attention, true);
    assert.doesNotMatch(radioDeliveryIndicator(status).label, /seen|read|queued|paused/i);
  }
  assert.equal(radioDeliveryIndicator({...sent, exhausted: 1}).attention, true);
  assert.equal(radioDeliveryIndicator(undefined).label, "Radio delivery status unavailable");
});
