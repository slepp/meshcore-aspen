function _failure(r)
  if not r.ok then return "HTTPS "..r.error.code end
end

function health(nonce)
  local r = rpc.call("home", "health", {})
  local error = _failure(r)
  if error then return error end
  return "Home "..nonce.." "..r.result.status
end

function echo(text)
  local r = rpc.call("home", "echo", {text=text})
  local error = _failure(r)
  if error then return error end
  return "Echo "..r.result.text
end

function probe_weather(place)
  local r = rpc.call("home", "weather", {place=place})
  local error = _failure(r)
  if error then return error end
  return tostring(r.result.temperature_c).." C; code "..
    tostring(r.result.weather_code).."; age "..
    tostring(r.result.source_age_seconds).."s"
end
