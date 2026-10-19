-- Read-only repeater CLI commands. !rcmd sends only these, and a query can be
-- retried once after route repair. Commands are compared in lower case.
local READS = {
  ["ver"] = true, ["board"] = true, ["clock"] = true, ["neighbors"] = true,
  ["gps"] = true, ["gps diag"] = true, ["sensor list"] = true, ["region"] = true,
  ["powersaving"] = true, ["ota status"] = true, ["log"] = true, ["help"] = true,
  ["stats"] = true, ["stats-core"] = true, ["stats-radio"] = true, ["stats-packets"] = true,
}
local READ_PREFIXES = {"get ", "sensor get ", "stats ", "help "}

-- Changes that are safe to send to several repeaters at once: a wrong value
-- can be corrected over the air afterwards. Radio, power, routing mode,
-- identity, access, restart and OTA commands can strand a repeater, so they
-- run on one repeater at a time.
local FLEET_WRITES = {
  ["advert"] = true, ["advert.zerohop"] = true, ["clock sync"] = true,
  ["clear stats"] = true, ["discover.neighbors"] = true,
}
local FLEET_SETTINGS = {
  ["flood.max"] = true, ["flood.max.advert"] = true, ["flood.max.unscoped"] = true,
  ["txdelay"] = true, ["direct.txdelay"] = true, ["rxdelay"] = true, ["af"] = true,
  ["advert.interval"] = true, ["flood.advert.interval"] = true,
}

local function blank(c) return c == " " or c == "\t" end

local function trim(text)
  local first, last = 1, #text
  while first <= last and blank(string.sub(text, first, first)) do first = first + 1 end
  while last >= first and blank(string.sub(text, last, last)) do last = last - 1 end
  return string.sub(text, first, last)
end

local function starts(text, prefix) return string.sub(text, 1, #prefix) == prefix end

local function is_read(command)
  if READS[command] then return true end
  for i = 1, #READ_PREFIXES do
    if starts(command, READ_PREFIXES[i]) then return true end
  end
  return false
end

local function fleet_write(command)
  if FLEET_WRITES[command] then return true end
  if not starts(command, "set ") then return false end
  local space = string.find(command, " ", 5, true)
  return space ~= nil and FLEET_SETTINGS[string.sub(command, 5, space - 1)] == true
end

local function fleet(targets, text, write)
  local peers, err = repeater.targets(targets)
  if not peers then return err end
  text = trim(text)
  if text == "" then return "Add a repeater CLI command: !rcmd TARGETS ver" end
  local command = string.lower(text)
  local read = is_read(command)
  if not write and not read then return "Use !rcmd-write TARGETS COMMAND to change repeater settings" end
  if #peers > 1 and not read and not fleet_write(command) then
    return "Run this command on one repeater at a time; group writes allow flood, delay, advert and stats settings"
  end
  for i = 1, #peers do
    local peer = peers[i]
    local result = repeater.command(peer, text, read)
    local answer = result.ok and result.reply or result.error
    if result.truncated then answer = answer .. " (truncated)" end
    local width = ctx.limits.reply_bytes - #peer - 2
    for offset = 1, #answer, width do
      local message = peer .. ": " .. string.sub(answer, offset, offset + width - 1)
      if i == #peers and offset + width > #answer then reply(message)
      else mesh.send(mesh.compose{text=message}) end
    end
  end
end

function repeater_read(targets, text) return fleet(targets, text, false) end
function repeater_write(targets, text) return fleet(targets, text, true) end
command("rcmd", "targets:string:159,text:text:120", "Read repeaters or groups: !rcmd d1,all ver", "repeater_read", "owner")
command("rcmd-write", "targets:string:159,text:text:120", "Change selected repeaters; writes sent once", "repeater_write", "owner")
