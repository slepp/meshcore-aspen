function fleet_poll()
  local peer=repeater.next()
  if peer then return repeater.status(peer) end
end
events.every(15,"fleet_poll")
