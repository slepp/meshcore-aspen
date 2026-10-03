function echo(text)
  local response = rpc.call("home", "echo", {text=text})
  if not response.ok then
    local advice = response.error.code == "unknown" and "; do not retry blindly" or ""
    return "Echo error: " .. response.error.code .. "; " ..
      rpc.text(response.error.message, 72) .. advice
  end
  return "Echo: " .. rpc.text(response.result.text, 120)
end
command("echo", "text:text:120", "Call the configured host echo", "echo", "home", "!echo hello")
