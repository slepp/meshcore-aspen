import {test} from "node:test";
import assert from "node:assert/strict";
import {bytes, initials, mergeMessages, messageBody, postBytes, validName} from "../web/model.js";

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
