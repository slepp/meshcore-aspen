import {authorName, bytes, initials, mergeMessages, mergeProfiles, messageBody, postBytes, radioDeliveryLabel, validName} from "./model.js";
import {loadDevice, signChallenge} from "./device.js";

const $ = id => document.getElementById(id);
const STORAGE = "aspen.rooms.v1.";
const MAX_BYTES = 151;
const rooms = new Map();
const mobileLayout = matchMedia("(max-width: 640px)");
let active, identity, name, booted = false;
let device, accountMode = false;
class ApiError extends Error {
  constructor(status, message) {super(message); this.status = status;}
}
function read(key, fallback) {
  const raw = localStorage.getItem(STORAGE + key);
  return raw === null ? fallback : JSON.parse(raw);
}
function save(key, value) {localStorage.setItem(STORAGE + key, JSON.stringify(value));}
function report(error) {
  $("notice-text").textContent = error.message ?? String(error);
  $("notice").hidden = false;
}
async function api(path, body) {
  const response = await fetch(path, {credentials: "same-origin", cache: "no-store",
    ...(body === undefined ? {} : {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)})});
  const value = await response.json();
  if (!response.ok) throw new ApiError(response.status, value.error ?? `Room request failed (${response.status})`);
  return value;
}
const endpoint = (room, path) => `/v1/web/rooms/${encodeURIComponent(room.id)}/${path}`;
function status(room, state, text) {
  room.status = state;
  room.statusText = text;
  if (room === active) {
    $("connection").className = `connection ${state}`;
    $("connection-text").textContent = text;
    updateComposer();
  }
}
function setProfile() {
  $("profile-name").textContent = name || "Your browser";
  $("profile-avatar").textContent = name ? initials(name) : "?";
}
function drawChannels() {
  const focusedRoom = $("channels").contains(document.activeElement) ? document.activeElement.dataset.roomId : undefined;
  const fragment = document.createDocumentFragment();
  for (const room of rooms.values()) {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.roomId = room.id;
    button.className = `channel${room === active ? " active" : ""}`;
    button.setAttribute("aria-current", room === active ? "page" : "false");
    button.setAttribute("aria-label", `${room.name}${room.unread ? `, ${room.unread} unread messages` : ""}${room.session ? "" : ", not joined"}`);
    const hash = document.createElement("span");
    hash.className = "channel-hash"; hash.textContent = "#"; hash.setAttribute("aria-hidden", "true");
    const label = document.createElement("span"); label.className = "channel-label"; label.textContent = room.name;
    button.append(hash, label);
    if (room.unread) {
      const badge = document.createElement("span"); badge.className = "channel-badge"; badge.textContent = room.unread > 99 ? "99+" : String(room.unread);
      button.append(badge);
    } else if (!room.session) {
      const lock = document.createElementNS("http://www.w3.org/2000/svg", "svg");
      lock.classList.add("channel-lock"); lock.setAttribute("viewBox", "0 0 24 24"); lock.setAttribute("aria-hidden", "true");
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", "M7 11V7a5 5 0 0 1 10 0v4M5 11h14v10H5z"); lock.append(path); button.append(lock);
    }
    button.addEventListener("click", () => select(room).catch(report));
    fragment.append(button);
  }
  $("channels").replaceChildren(fragment);
  if (focusedRoom) [...$("channels").children].find(button => button.dataset.roomId === focusedRoom)?.focus({preventScroll: true});
}
function menu(open, restoreFocus = true) {
  const shown = mobileLayout.matches && open;
  const focusWasInside = $("sidebar").contains(document.activeElement) || document.activeElement === $("scrim");
  $("sidebar").classList.toggle("open", shown);
  $("scrim").hidden = !shown;
  $("mobile-menu").setAttribute("aria-expanded", String(shown));
  $("sidebar").inert = mobileLayout.matches && !shown;
  $("main-content").inert = shown;
  document.querySelector(".skip-link").inert = shown;
  if (mobileLayout.matches) $("sidebar").setAttribute("aria-hidden", String(!shown));
  else $("sidebar").removeAttribute("aria-hidden");
  if (shown) {
    $("sidebar").setAttribute("role", "dialog");
    $("sidebar").setAttribute("aria-modal", "true");
    $("channels").querySelector("button[aria-current=page]")?.focus();
  } else {
    $("sidebar").removeAttribute("role");
    $("sidebar").removeAttribute("aria-modal");
    if (mobileLayout.matches && restoreFocus && focusWasInside) $("mobile-menu").focus();
  }
}
function nearBottom() {return $("timeline").scrollHeight - $("timeline").scrollTop - $("timeline").clientHeight < 100;}
function bottom() {$("timeline").scrollTop = $("timeline").scrollHeight; $("new-messages").hidden = true;}
function drawMessages(scroll = false) {
  if (!active) return;
  const room = active;
  const wasBottom = nearBottom();
  const fragment = document.createDocumentFragment();
  let previousDay;
  const messages = [...room.messages.values()].sort((a, b) => a.seq - b.seq);
  for (const message of messages) {
    const date = new Date(message.timestamp * 1000);
    const day = date.toDateString();
    if (day !== previousDay) {
      const divider = document.createElement("div"); divider.className = "date-divider";
      divider.textContent = date.toLocaleDateString(undefined, {weekday: "short", month: "short", day: "numeric", year: "numeric"});
      fragment.append(divider); previousDay = day;
    }
    const author = authorName(message, room.profiles);
    const row = document.createElement("article");
    row.className = `message${message.author === room.session?.author ? " own" : ""}`;
    row.dataset.seq = String(message.seq);
    const avatar = document.createElement("span");
    avatar.className = `avatar swatch-${parseInt(message.author.slice(0, 2), 16) % 6}`;
    avatar.textContent = message.webName || room.profiles.has(message.author) ? initials(author) : message.author.slice(0, 2).toUpperCase();
    avatar.setAttribute("aria-hidden", "true");
    const content = document.createElement("div"); content.className = "message-content";
    const meta = document.createElement("div"); meta.className = "message-meta";
    const title = document.createElement("span"); title.className = "message-name";
    title.textContent = author; title.title = `${author} · ${message.author}`;
    const fingerprint = document.createElement("span"); fingerprint.className = "message-source";
    fingerprint.textContent = message.author.slice(0, 8); fingerprint.title = message.author;
    fingerprint.setAttribute("aria-label", `Key fingerprint ${message.author.slice(0, 8)}`);
    const time = document.createElement("time"); time.dateTime = date.toISOString(); time.title = date.toLocaleString();
    time.textContent = date.toLocaleTimeString(undefined, {hour: "2-digit", minute: "2-digit"});
    const source = document.createElement("span"); source.className = "message-source";
    source.textContent = message.webName ? "WEB" : "RADIO";
    meta.append(title, fingerprint, time, source);
    const text = document.createElement("p"); text.className = "message-text"; text.textContent = messageBody(message);
    const delivery = document.createElement("p"); delivery.className = "message-source";
    delivery.textContent = radioDeliveryLabel(message.rf);
    delivery.title = "A confirmed radio transmission is not a recipient ACK. An exhausted retry budget still accepts a late ACK; the recipient can rejoin to request history again.";
    content.append(meta, text, delivery); row.append(avatar, content); fragment.append(row);
  }
  $("messages").replaceChildren(fragment);
  $("intro-title").textContent = `Welcome to ${room.name}.`;
  $("intro-text").textContent = messages.length ? "The shared conversation, from web and radio participants." : "You're connected. Send the first message or wait for someone on the mesh.";
  $("load-older").hidden = !room.hasEarlier;
  if (scroll || wasBottom) bottom();
}
function receive(room, messages, live = false) {
  const fresh = messages.filter(message => !room.messages.has(message.seq));
  const added = fresh.length;
  mergeMessages(room.messages, messages);
  // Only consecutive stream messages can advance a catch-up cursor.
  while (room.messages.has(room.cursor + 1)) room.cursor++;
  if (room !== active && live) room.unread += added;
  if (room === active) {
    const follow = nearBottom();
    drawMessages();
    if (live && added && !follow) $("new-messages").hidden = false;
    const incoming = fresh.filter(message => message.author !== room.session?.author);
    if (live && incoming.length) {
      const last = incoming.at(-1);
      $("message-announcement").textContent = incoming.length === 1 ?
        `${authorName(last, room.profiles)} in ${room.name}: ${messageBody(last)}` :
        `${incoming.length} new messages in ${room.name}.`;
    }
  }
  if (added) drawChannels();
}
async function history(room, initial = false) {
  if (room.historyTask) return room.historyTask;
  room.historyTask = (async () => {
    let query = initial ? "" : `?after=${room.cursor}`;
    do {
      const page = await api(endpoint(room, "history") + query);
      mergeProfiles(room.profiles, page.profiles ?? []);
      receive(room, page.messages);
      if (initial) {
        room.hasEarlier = page.more;
        room.cursor = page.messages.at(-1)?.seq ?? page.floor;
        room.loaded = true;
        break;
      }
      async function refreshRadioDelivery() {
        const room = active;
        if (!room?.session || room.status !== "live" || room.rfTask || document.hidden) return;
        room.rfTask = true;
        const generation = room.generation;
        try {
          const page = await api(endpoint(room, "history") + `?before=${room.cursor + 1}`);
          if (room.generation !== generation || !room.session) return;
          mergeProfiles(room.profiles, page.profiles ?? []);
          mergeMessages(room.messages, page.messages.filter(message => room.messages.has(message.seq)));
          if (room === active) drawMessages();
        } catch (error) {
          if (room.generation !== generation) return;
          if (error.status === 401) requireLogin(room, error.message);
          else report(new Error(`RF delivery status could not be refreshed: ${error.message}`));
        } finally {room.rfTask = false;}
      }
      setInterval(() => refreshRadioDelivery(), 5000);
      room.cursor = Math.max(room.cursor, page.floor, page.messages.at(-1)?.seq ?? 0);
      if (!page.more) break;
      query = `?after=${room.cursor}`;
    } while (room.session);
    if (room === active) drawMessages(initial);
  })();
  try {await room.historyTask;}
  finally {room.historyTask = undefined;}
}
function disconnect(room) {
  room.generation++;
  clearTimeout(room.reconnect);
  room.socket?.close();
  room.socket = undefined;
}
function requireLogin(room, message) {
  disconnect(room);
  room.session = undefined;
  status(room, "", "Not joined");
  drawChannels();
  if (room === active) {
    showRoom();
    if (message) {$("join-error").textContent = message; $("join-error").hidden = false;}
  }
}
function connect(room) {
  disconnect(room);
  if (!room.session) return;
  const generation = room.generation;
  status(room, "", navigator.onLine ? "Connecting" : "Offline");
  const url = new URL(endpoint(room, "socket"), location.origin);
  url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
  url.searchParams.set("since", String(room.cursor));
  const socket = new WebSocket(url, "aspen-room.web.v1");
  room.socket = socket;
  socket.addEventListener("message", async event => {
    if (room.generation !== generation) return;
    try {
      const value = JSON.parse(event.data);
      if (value.type === "ready") {
        if (value.alias !== room.id || value.publicKey !== room.publicKey || value.version !== 1) throw new Error("The room connection returned a different radio identity");
        room.backoff = 1000;
        await history(room);
        if (room.generation === generation) status(room, "live", "Connected");
      } else if (value.type === "message") {
        receive(room, [value.message], true);
      } else if (value.type === "profile") {
        mergeProfiles(room.profiles, [value.profile]);
        if (room === active) drawMessages();
      } else if (value.type === "catchup") {
        await history(room);
        if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({op: "sync", since: room.cursor}));
      } else if (value.type === "error") throw new Error(value.error);
      else throw new Error("The room sent an unknown event");
    } catch (error) {
      if (error.status === 401) requireLogin(room, error.message);
      else {report(error); socket.close();}
    }
  });
  socket.addEventListener("close", async event => {
    if (room.generation !== generation || !room.session) return;
    status(room, "offline", navigator.onLine ? "Reconnecting" : "Offline");
    try {room.session = await api(endpoint(room, "session"));}
    catch (error) {
      if (error.status === 401) {requireLogin(room, error.message); return;}
      if (error instanceof ApiError) report(error);
    }
    if (room.generation !== generation || !room.session) return;
    room.reconnect = setTimeout(() => connect(room), room.backoff);
    room.backoff = Math.min(30000, room.backoff * 2);
  });
}
function showRoom() {
  if (!active) return;
  const room = active, joined = !!room.session;
  $("room-name").textContent = room.name;
  $("room-subtitle").textContent = joined ? "A shared conversation across IP and mesh." :
    accountMode ? "Sign in with your room account." : "Join with the room password.";
  $("join-panel").hidden = joined;
  $("timeline").hidden = !joined;
  $("composer-area").hidden = !joined;
  $("room-info").disabled = false;
  $("join-title").textContent = `Join ${room.name}.`;
  $("display-name").value = name;
  $("account-username").value = read("username", "");
  $("room-password").value = "";
  $("join-error").hidden = true;
  $("message-input").value = room.draft;
  $("message-input").placeholder = `Message ${room.name}`;
  status(room, room.status, room.statusText);
  updateComposer();
  if (joined) drawMessages(true);
}
async function select(room) {
  const focusHeading = mobileLayout.matches && $("sidebar").classList.contains("open");
  if (active) {
    active.draft = $("message-input").value;
    save(`draft.${active.id}`, active.draft);
  }
  active = room; room.unread = 0;
  save("channel", room.id);
  menu(false, !focusHeading); drawChannels(); showRoom();
  if (focusHeading) $("room-name").focus();
  if (room.session && !room.loaded) {
    await history(room, true);
    connect(room);
  }
}
function updateComposer() {
  if (!active?.session) return;
  const used = postBytes(active.session.name, $("message-input").value);
  $("byte-count").textContent = `${used} / ${MAX_BYTES} bytes`;
  $("byte-count").classList.toggle("over", used > MAX_BYTES);
  $("message-input").setAttribute("aria-invalid", String(used > MAX_BYTES));
  $("compose-error").hidden = used <= MAX_BYTES;
  $("compose-form").setAttribute("aria-busy", String(!!active.sending));
  $("send-button").disabled = !active.session || used > MAX_BYTES || !$("message-input").value.trim() || !!active.outbox || !!active.sending;
  $("message-input").readOnly = !!active.outbox || !!active.sending;
  $("pending-post").hidden = !active.outbox;
  $("pending-text").textContent = active.sending ? "Saving to the room…" : "Send result uncertain. Check or retry this same post before sending another.";
  $("retry-post").disabled = !!active.sending;
}
async function transmit(room) {
  if (room.sending || !room.outbox) return;
  if (room.outbox.author !== room.session?.author) {
    report(new Error("This pending post belongs to a different device identity. Restore its original login before checking it; no new copy was sent."));
    return;
  }
  const focusComposer = $("compose-form").contains(document.activeElement) || document.activeElement === $("retry-post");
  room.sending = true;
  if (room === active) updateComposer();
  try {
    const result = await api(endpoint(room, "posts"), {id: room.outbox.id, text: room.outbox.text});
    receive(room, [result.message], true);
    save(`outbox.${room.id}`, null);
    room.outbox = null; room.draft = "";
    save(`draft.${room.id}`, "");
    if (room === active) {
      $("message-input").value = ""; bottom();
      $("message-announcement").textContent = `Message saved to ${room.name}.`;
    }
  } catch (error) {
    if (error instanceof ApiError && [400, 413, 415].includes(error.status)) {
      save(`outbox.${room.id}`, null);
      room.outbox = null;
    }
    if (error.status === 401) requireLogin(room, error.message);
    report(error instanceof ApiError ? error : new Error(`Could not confirm the send: ${error.message}. Use Check / retry; it will not create a second copy.`));
  } finally {
    room.sending = false;
    if (room === active) {
      updateComposer();
      if (!room.outbox && room.session && focusComposer &&
          ($("composer-area").contains(document.activeElement) || document.activeElement === document.body))
        $("message-input").focus({preventScroll: true});
    }
  }
}
async function boot() {
  identity = read("identity", null);
  if (identity !== null && (typeof identity !== "string" || !/^[a-f0-9]{64}$/.test(identity))) throw new Error("This browser's saved identity is damaged. Restore its site storage before joining a room.");
  if (!identity) {
    identity = Array.from(crypto.getRandomValues(new Uint8Array(32)), byte => byte.toString(16).padStart(2, "0")).join("");
    save("identity", identity);
  }
  name = read("name", "");
  if (name && !validName(name)) throw new Error("This browser's saved display name is invalid. Update its site storage before joining a room.");
  setProfile();
  const listing = await api("/v1/web/rooms");
  if (listing.maxPostBytes !== MAX_BYTES || !Array.isArray(listing.rooms)) throw new Error("Unsupported room service response");
  accountMode = listing.loginMode === "account";
  if (accountMode) {
    if (listing.deviceProtocol !== "aspen-room.device.v1") throw new Error("Unsupported desktop identity protocol");
    device = await loadDevice();
    $("username-label").hidden = false; $("account-username").hidden = false; $("account-username").required = true;
    $("display-name-label").hidden = true; $("display-name").hidden = true; $("display-name").required = false;
    $("password-label").textContent = "Account password";
    $("login-note").textContent = "This browser holds its own private key. Your operator assigns your display name and room access. Clearing site data loses this device identity.";
    $("edit-profile").hidden = true;
  }
  const legacyAuthor = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256",
    new TextEncoder().encode(`aspen-web-author:${identity}`))), byte => byte.toString(16).padStart(2, "0")).join("");
  $("service-name").textContent = location.host;
  $("channel-count").textContent = String(listing.rooms.length);
  for (const entry of listing.rooms) {
    const room = {...entry, messages: new Map(), profiles: new Map(), cursor: 0, unread: 0, loaded: false, hasEarlier: false,
      generation: 0, backoff: 1000, status: "", statusText: "Not joined",
      draft: read(`draft.${entry.id}`, ""), outbox: read(`outbox.${entry.id}`, null)};
    if (typeof room.draft !== "string" || (room.outbox && (typeof room.outbox.id !== "string" || typeof room.outbox.text !== "string")))
      throw new Error(`The saved draft for ${room.name} is damaged`);
    if (room.outbox && !room.outbox.author) {
      room.outbox.author = legacyAuthor;
      save(`outbox.${room.id}`, room.outbox);
    }
    rooms.set(room.id, room);
  }
  drawChannels();
  if (!rooms.size) {
    $("connection-text").textContent = "No rooms";
    $("intro-title").textContent = "No rooms are configured.";
    $("intro-text").textContent = "The service operator needs to configure a room before you can join.";
    $("room-info").disabled = true; return;
  }
  await Promise.all([...rooms.values()].map(async room => {
    try {
      room.session = await api(endpoint(room, "session"));
      if (accountMode && room.session.author !== device.publicKey)
        throw new Error("The saved room session belongs to a different device key. Restore its browser profile or leave the room before signing in.");
      if (accountMode) {name = room.session.name; setProfile();}
    }
    catch (error) {if (error.status !== 401) throw error;}
  }));
  const chosen = read("channel", null);
  await select(rooms.get(chosen) ?? rooms.values().next().value);
  await Promise.all([...rooms.values()].filter(room => room.session && room !== active).map(async room => {
    await history(room, true); connect(room);
  }));
  booted = true;
}
$("join-form").addEventListener("submit", async event => {
  event.preventDefault();
  const room = active, display = $("display-name").value.trim();
  const username = $("account-username").value.trim();
  if (accountMode && !/^[a-z0-9][a-z0-9_-]{2,63}$/.test(username)) {
    $("join-error").textContent = "Use the lowercase username assigned by your room operator.";
    $("join-error").hidden = false; $("account-username").setAttribute("aria-invalid", "true");
    $("account-username").focus(); return;
  }
  if (!accountMode && !validName(display)) {
    $("join-error").textContent = "Use 1–24 UTF-8 bytes, without colons or control characters.";
    $("join-error").hidden = false;
    $("display-name").setAttribute("aria-invalid", "true"); $("display-name").focus(); return;
  }
  $("join-button").disabled = true; $("join-error").hidden = true;
  try {
    let body = {identity, name: display, password: $("room-password").value};
    if (accountMode) {
      const challenge = await api(endpoint(room, "challenge"), {username, publicKey: device.publicKey});
      const signature = await signChallenge(device, challenge, location.origin, room.id, username);
      body = {username, publicKey: device.publicKey, nonce: challenge.nonce, signature, password: $("room-password").value};
    }
    const session = await api(endpoint(room, "login"), body);
    save("name", session.name); name = session.name; setProfile();
    if (accountMode) save("username", username);
    room.session = session;
    room.messages.clear(); room.loaded = false; room.cursor = 0;
    drawChannels();
    if (room === active) showRoom();
    await history(room, true); connect(room);
    $("room-password").value = "";
    if (room === active && document.activeElement === document.body) $("message-input").focus({preventScroll: true});
  } catch (error) {
    if (room === active) {
      $("join-error").textContent = error.message; $("join-error").hidden = false;
      if (error.status === 403) {$("room-password").setAttribute("aria-invalid", "true"); $("room-password").focus();}
    } else report(new Error(`${room.name}: ${error.message}`));
  } finally {$("join-button").disabled = false; $("room-password").value = "";}
});
$("compose-form").addEventListener("submit", async event => {
  event.preventDefault();
  if (!active?.session || active.outbox || active.sending) return;
  const text = $("message-input").value;
  if (!text.trim() || postBytes(active.session.name, text) > MAX_BYTES) return;
  try {
    const post = {id: crypto.randomUUID(), text, author: active.session.author};
    save(`outbox.${active.id}`, post);
    active.outbox = post;
    await transmit(active);
  } catch (error) {report(error);}
});
$("message-input").addEventListener("input", () => {
  if (!active) return;
  active.draft = $("message-input").value;
  try {save(`draft.${active.id}`, active.draft);}
  catch (error) {report(new Error(`Draft could not be saved: ${error.message}`));}
  updateComposer();
});
$("message-input").addEventListener("keydown", event => {
  if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {event.preventDefault(); $("compose-form").requestSubmit();}
});
$("retry-post").addEventListener("click", () => transmit(active).catch(report));
$("load-older").addEventListener("click", async () => {
  const room = active;
  $("load-older").disabled = true;
  try {
    const first = Math.min(...room.messages.keys());
    const oldHeight = $("timeline").scrollHeight, oldTop = $("timeline").scrollTop;
    const page = await api(endpoint(room, "history") + `?before=${first}`);
    mergeProfiles(room.profiles, page.profiles ?? []);
    room.hasEarlier = page.more;
    receive(room, page.messages);
    if (room === active) {$("timeline").scrollTop = oldTop + $("timeline").scrollHeight - oldHeight; $("load-older").hidden = !page.more;}
  } catch (error) {report(error);}
  finally {$("load-older").disabled = false;}
});
$("new-messages").addEventListener("click", bottom);
$("timeline").addEventListener("scroll", () => {if (nearBottom()) $("new-messages").hidden = true;});
$("mobile-menu").addEventListener("click", () => menu(!$("sidebar").classList.contains("open")));
$("close-channels").addEventListener("click", () => menu(false));
$("scrim").addEventListener("click", () => menu(false));
mobileLayout.addEventListener("change", () => menu(false));
menu(false);
document.addEventListener("keydown", event => {
  if (!$("sidebar").classList.contains("open") || document.querySelector("dialog[open]")) return;
  if (event.key === "Escape") {event.preventDefault(); menu(false); return;}
  if (event.key !== "Tab") return;
  const controls = [...$("sidebar").querySelectorAll("a[href], button:not(:disabled)")].filter(element => element.getClientRects().length);
  const first = controls[0], last = controls.at(-1);
  if (event.shiftKey && document.activeElement === first) {event.preventDefault(); last?.focus();}
  else if (!event.shiftKey && document.activeElement === last) {event.preventDefault(); first?.focus();}
});
for (const id of ["account-username", "display-name", "room-password", "profile-input"]) {
  $(id).addEventListener("input", () => $(id).removeAttribute("aria-invalid"));
}
$("notice-dismiss").addEventListener("click", () => {$("notice").hidden = true;});
$("room-info").addEventListener("click", () => {
  if (!active) return;
  $("detail-name").textContent = active.name; $("detail-alias").textContent = active.id;
  $("detail-key").textContent = active.publicKey;
  $("detail-author").textContent = active.session?.author ?? device?.publicKey ?? "Join this room to create your web identity.";
  $("leave-room").hidden = !active.session; $("details-dialog").showModal();
});
$("close-details").addEventListener("click", () => $("details-dialog").close());
$("leave-room").addEventListener("click", async () => {
  const room = active;
  $("leave-room").disabled = true;
  try {
    await api(endpoint(room, "logout"), {});
    requireLogin(room);
    room.messages.clear(); room.loaded = false; room.cursor = 0;
    $("details-dialog").close();
  } catch (error) {report(error);}
  finally {$("leave-room").disabled = false;}
});
$("edit-profile").addEventListener("click", () => {
  if (mobileLayout.matches) menu(false);
  $("profile-input").value = name ?? ""; $("profile-error").hidden = true; $("profile-dialog").showModal();
});
$("close-profile").addEventListener("click", () => $("profile-dialog").close());
$("profile-form").addEventListener("submit", event => {
  event.preventDefault();
  const next = $("profile-input").value.trim();
  if (!validName(next)) {
    $("profile-error").textContent = "Use 1–24 UTF-8 bytes, without colons or control characters."; $("profile-error").hidden = false;
    $("profile-input").setAttribute("aria-invalid", "true"); $("profile-input").focus(); return;
  }
  try {save("name", next); name = next; setProfile(); $("profile-dialog").close();}
  catch (error) {report(error);}
});
window.addEventListener("online", () => {for (const room of rooms.values()) if (room.session) connect(room);});
window.addEventListener("offline", () => {for (const room of rooms.values()) if (room.session) status(room, "offline", "Offline");});
window.addEventListener("pagehide", () => {for (const room of rooms.values()) disconnect(room);});
window.addEventListener("pageshow", event => {if (event.persisted && booted) for (const room of rooms.values()) if (room.session) connect(room);});
boot().catch(error => {
  $("connection").className = "connection offline"; $("connection-text").textContent = "Unavailable";
  report(new Error(`Could not open Aspen Rooms: ${error.message}`));
});
