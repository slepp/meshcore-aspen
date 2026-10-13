local function fleet(targets, text, write)
  local peers, seen, start = {}, {}, 1
  targets = string.lower(targets)
  while start <= #targets do
    local comma = string.find(targets, ",", start, true)
    local peer = string.sub(targets, start, comma and comma - 1 or #targets)
    if #peer == 0 or seen[peer] then return "Use distinct configured repeater aliases" end
    peers[#peers + 1], seen[peer] = peer, true
    start = comma and comma + 1 or #targets + 1
  end
  if #peers < 1 or #peers > 5 or string.sub(targets, -1) == "," then
    return "Select 1..5 repeater aliases, separated by commas"
  end
  while string.sub(text, 1, 1) == " " do text = string.sub(text, 2) end
  local space = string.find(text, " ", 1, true)
  local verb = string.sub(text, 1, space and space - 1 or #text)
  local read = verb == "get" or verb == "ver" or verb == "stats" or verb == "help" or verb == "neighbors"
  if not write and not read then return "Use !rcmd-write TARGETS COMMAND to change repeater settings" end
  local lockout = verb == "setperm" or verb == "password" or verb == "erase" or verb == "reboot" or verb == "factory"
  for i = 1, 4 do
    local prefix = ({"set radio", "set freq", "set acl", "set name"})[i]
    lockout = lockout or string.sub(text, 1, #prefix) == prefix
  end
  if #peers > 1 and lockout then return "Run identity, access, radio and restart commands on one repeater at a time" end
  for i = 1, #peers do
    local peer = peers[i]
    local result = repeater.command(peer, text)
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
command("rcmd", "targets:string:84,text:text:120", "Read selected repeaters: !rcmd d1,d2 ver", "repeater_read", "owner")
command("rcmd-write", "targets:string:84,text:text:120", "Change selected repeaters; no automatic retries", "repeater_write", "owner")
