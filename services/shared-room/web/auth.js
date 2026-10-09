import {startAuthentication, startRegistration} from "@simplewebauthn/browser";
import QRCode from "qrcode";
import {signAccountChallenge} from "./device.js";

export function accountAuth(api, device, onSession, report) {
  const $ = id => document.getElementById(id), root = "/v1/auth/";
  let fragment = new URLSearchParams(location.hash.slice(1));
  let enrollment = fragment.get("enroll"), pendingCode = fragment.get("link"), session, timer, request, inspected;
  if (enrollment || pendingCode) history.replaceState(null, "", location.pathname + location.search);
  if (fragment.get("user")) $("account-username").value = fragment.get("user");
  if (enrollment) {
    $("join-title").textContent = "Register your Aspen passkey.";
    $("join-button").textContent = "Register passkey and join";
  }
  const account = async (endpoint, body) => api(root + endpoint, body);
  const check = value => {
    if (value.publicKey !== device.publicKey) throw new Error("Account session belongs to a different device key. Restore its browser profile.");
    session = value; return value;
  };
  async function proof(purpose) {
    const challenge = await account("device-challenge", {publicKey: device.publicKey, purpose});
    return {id: challenge.id, signature: await signAccountChallenge(device, challenge, location.origin, purpose)};
  }
  async function passkey(username, register = false) {
    const purpose = register ? "register" : "authenticate";
    const challenge = await account(purpose + "/options", {username, publicKey: device.publicKey,
      ...(register && enrollment ? {enrollment} : {})});
    const signature = await signAccountChallenge(device, challenge, location.origin, purpose);
    const response = register ? await startRegistration({optionsJSON: challenge.options}) :
      await startAuthentication({optionsJSON: challenge.options});
    const result = check(await account(purpose + "/verify", {id: challenge.id, signature, response}));
    enrollment = null;
    $("join-title").textContent = "Come into the conversation.";
    $("join-button").textContent = "Join with passkey";
    return result;
  }
  async function restore() {
    try {return check(await account("session"));}
    catch (error) {if (error.status !== 401) throw error;}
    try {return check(await account("device-login", await proof("device-login")));}
    catch (error) {if (error.status !== 401) throw error;}
  }
  async function ensure(username) {
    if (enrollment) return passkey(username, true);
    return await restore() ?? passkey(username);
  }
  function error(error) {
    $("device-error").textContent = error.message ?? String(error); $("device-error").hidden = false;
  }
  function show(title) {
    $("device-error").hidden = true; $("device-title").textContent = title;
    $("device-request").hidden = true; $("device-approval").hidden = true; $("device-manage").hidden = true;
    if (!$("device-dialog").open) $("device-dialog").showModal();
  }
  function stop() {clearTimeout(timer); timer = undefined;}
  async function poll() {
    if (!request) return;
    const remaining = request.expires - Math.floor(Date.now() / 1000);
    $("device-expiry").textContent = remaining > 0 ? `Expires in ${remaining} seconds.` : "Device link expired. Start a new link.";
    if (remaining <= 0) {stop(); return;}
    try {
      const result = await account("link/status", {code: request.code, claim: request.claim});
      if (result.approved) {
        check(await account("link/claim", {code: request.code, claim: request.claim}));
        stop(); request = null; $("device-dialog").close();
        await onSession(session); return;
      }
      timer = setTimeout(poll, 3000);
    } catch (failure) {stop(); error(failure);}
  }
  async function startLink() {
    stop();
    show("Approve this browser on another device");
    request = await account("link/start", {...await proof("link"), label: "Aspen web browser"});
    $("device-request").hidden = false;
    $("device-link-code").value = request.code;
    $("device-link-url").href = request.url; $("device-link-url").textContent = request.url;
    $("device-key").textContent = request.publicKey;
    await QRCode.toCanvas($("device-qr"), request.url, {width: 240, margin: 2, errorCorrectionLevel: "M"});
    await poll();
  }
  async function inspect(value) {
    if (!session) session = await restore();
    if (!session) throw new Error("Join a room with your passkey first, then enter the new device's code.");
    inspected = await account("link/inspect", {code: value.trim().toUpperCase()});
    $("device-approval").hidden = false;
    $("approval-label").textContent = inspected.label;
    $("approval-key").textContent = inspected.publicKey;
    $("approval-expiry").textContent = `Expires ${new Date(inspected.expires * 1000).toLocaleTimeString()}. Compare this full device key with the requesting device before approving.`;
  }
  async function manage() {
    show("Account devices and passkeys");
    session = await restore();
    if (!session) throw new Error("Join a room with your passkey to manage or approve devices.");
    $("device-manage").hidden = false;
    const listing = await account("devices", {});
    const list = $("account-devices");
    list.replaceChildren();
    for (const device of listing.devices) {
      const row = document.createElement("li"), text = document.createElement("span");
      text.className = "key"; text.textContent = `${device.publicKey}${device.publicKey === session.publicKey ? " (this device)" : ""}${device.revoked ? " (revoked)" : ""}`;
      row.append(text);
      if (!device.revoked) {
        const button = document.createElement("button");
        button.type = "button"; button.className = "danger-button"; button.textContent = "Revoke device";
        button.addEventListener("click", async () => {
          if (!confirm(`Revoke ${device.publicKey}? This device will lose account and room access. Its saved keys and drafts will not be erased.`)) return;
          try {await account("device/revoke", {publicKey: device.publicKey}); await manage();}
          catch (failure) {error(failure);}
        });
        row.append(button);
      }
      list.append(row);
    }
    if (pendingCode) {$("approval-code").value = pendingCode; await inspect(pendingCode); pendingCode = null;}
  }
  $("link-device").addEventListener("click", () => startLink().catch(error));
  $("close-device").addEventListener("click", () => {stop(); request = null; $("device-dialog").close();});
  $("device-dialog").addEventListener("cancel", () => {stop(); request = null;});
  $("copy-link-code").addEventListener("click", () => navigator.clipboard.writeText($("device-link-code").value).catch(error));
  $("inspect-link").addEventListener("click", () => inspect($("approval-code").value).catch(error));
  $("approve-link").addEventListener("click", async () => {
    if (!inspected) return;
    $("approve-link").disabled = true;
    try {
      await account("link/approve", {code: inspected.code, publicKey: inspected.publicKey});
      inspected = null; $("device-approval").hidden = true; $("approval-code").value = "";
      $("device-title").textContent = "Device approved. It can now claim its account link.";
    } catch (failure) {error(failure);}
    finally {$("approve-link").disabled = false;}
  });
  $("add-passkey").addEventListener("click", () => passkey(session.username, true)
    .then(() => {$("device-title").textContent = "Passkey added to your account.";}).catch(error));
  $("manage-account").addEventListener("click", () => manage().catch(error));
  window.addEventListener("hashchange", () => {
    const incoming = new URLSearchParams(location.hash.slice(1));
    if (!incoming.has("link") && !incoming.has("enroll")) return;
    fragment = incoming; enrollment = incoming.get("enroll"); pendingCode = incoming.get("link");
    history.replaceState(null, "", location.pathname + location.search);
    if (enrollment) {
      $("account-username").value = incoming.get("user") ?? "";
      $("join-title").textContent = "Register your Aspen passkey.";
      $("join-button").textContent = "Register passkey and join";
      $("join-panel").hidden = false;
    } else manage().catch(report);
  });
  return {
    get username() {return session?.username ?? fragment.get("user");},
    get enrolling() {return !!enrollment;},
    ensure, restore, manage,
    async ticket(room) {
      const value = await account("room-ticket", {alias: room.id, publicKey: device.publicKey});
      if (value.publicKey !== device.publicKey || value.alias !== room.id) throw new Error("Room ticket belongs to a different device or room.");
      return {ticket: value.ticket};
    },
    async afterLogin() {if (pendingCode) await manage().catch(report);}
  };
}
