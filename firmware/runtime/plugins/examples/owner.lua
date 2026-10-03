function owner_test()
  local marks, safe = kv.get("owner-runs")
  if marks and not safe then return "Error: owner counter is not RF-safe; inspect private state" end
  marks = (marks or "") .. "x"
  local ok, err = kv.put("owner-runs", marks)
  if not ok then return "Error: owner counter " .. (err or "storage unavailable") end
  return "Owner call " .. #marks
end

function owner_count()
  local marks, safe = kv.get("owner-runs")
  if marks and not safe then return "Error: owner counter is not RF-safe; inspect private state" end
  return "Owner calls " .. #(marks or "")
end

command("owner-test", "", "Run in a trusted owner's private DM", "owner_test", "owner")
command("owner-count", "", "Read this caller's private counter", "owner_count", "dm")
