local function fleet(targets, text, write)
  local peers, err = repeater.targets(targets)
  if not peers then return err end
  while string.sub(text, 1, 1) == " " do text = string.sub(text, 2) end
  local space = string.find(text, " ", 1, true)
  local verb = string.sub(text, 1, space and space - 1 or #text)
  local read = verb == "get" or verb == "ver" or verb == "stats" or verb == "help" or verb == "neighbors" or text == "gps" or text == "gps diag"
  if not write and not read then return "Use !rcmd-write TARGETS COMMAND to change repeater settings" end
  local lockout = verb == "setperm" or verb == "password" or verb == "erase" or verb == "reboot" or verb == "factory"
  for i = 1, 4 do
    local prefix = ({"set radio", "set freq", "set acl", "set name"})[i]
    lockout = lockout or string.sub(text, 1, #prefix) == prefix
  end
  if #peers > 1 and lockout then return "Run identity, access, radio and restart commands on one repeater at a time" end
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
