function regional_ping()
  return call_original("ping") .. " from the regional bot"
end
override_command("ping", "regional_ping")

function regional_recall(key)
  return call_original("recall")
end
override_command("recall", "regional_recall")
