-- Configure home_status (GET /v1/example/status), home_echo
-- (POST /v1/example/echo), and home_rpc sum (POST /v1/rpc) first.
-- Lua cannot select a URL or credential.
local function failure(name,response)
  local advice=response.error.code=="unknown" and "; do not retry blindly" or ""
  return name.." error: "..response.error.code.."; "..
    rpc.text(response.error.message,72)..advice
end
function net_health()
  local response = rpc.call("home", "health", {})
  if not response.ok then return failure("Health",response) end
  return "Home: " .. rpc.text(response.result.status,100)
end

function net_status()
  local response = http.get("home_status")
  if not response.ok then return failure("Status",response) end
  if not response.body.service or not response.body.status then
    return "Error: status response requires service and status fields"
  end
  return rpc.text(response.body.service,48) .. ": " .. rpc.text(response.body.status,72)
end

function net_echo(text)
  local response = http.post("home_echo", {text = text})
  if not response.ok then return failure("Echo",response) end
  return rpc.text(response.body.text, 100)
end

function net_sum(a, b)
  local response = rpc.call("home_rpc", "sum", {a = a, b = b})
  if not response.ok then return failure("Sum",response) end
  if response.result.sum==nil then return "Error: sum response missing sum" end
  return "Sum: " .. tostring(response.result.sum)
end

function net_digest(text)
  local response = rpc.call("home_rpc", "digest", {text = text, rounds = 100000})
  if not response.ok then return failure("Digest",response) end
  if not response.result.sha256 then return "Error: digest response missing sha256" end
  return rpc.text(response.result.sha256,64)
end

command("net_health", "", "Configured health RPC; private DM/home grant","net_health","home")
command("net_status", "", "Configured status GET; private DM/home grant","net_status","home")
command("net_echo", "text:text:100", "Configured echo POST; private DM/home grant","net_echo","home","!net_echo hello")
command("net_sum", "a:int:-1000000:1000000,b:int:-1000000:1000000", "Configured sum RPC; private DM/home grant","net_sum","home","!net_sum 2 3")
command("net_digest", "text:text:100", "Configured digest RPC; private DM/home grant","net_digest","home","!net_digest hello")
