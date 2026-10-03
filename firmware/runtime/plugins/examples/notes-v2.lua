local function migrate()
  local schema = kv.get("example:schema")
  if schema == "2" then return true end
  if schema and schema ~= "1" then return false,"unsupported saved schema" end
  local old = kv.get("example:note-v1") or false
  if not schema and (old or kv.get("example:note-v2")) then
    return false,"unmarked data; inspect with notes-v1 before changing it"
  end
  local result = kv.transaction({
    {key="example:schema", expect=schema or false, value="2"},
    {key="example:note-v1", expect=old, value=old},
    {key="example:note-v2", value=old},
  })
  if not result.ok then return false,result.status.."; read before retrying" end
  return true
end

function save_note(text)
  local ok,state=migrate()
  if not ok then return "Note migration: "..state end
  local result = kv.transaction({
    {key="example:schema", expect="2", value="2"},
    {key="example:note-v1", value=text},
    {key="example:note-v2", value=text},
  })
  if not result.ok then return "Note write "..result.status.."; read before retrying" end
  return "Saved"
end
command("note-save", "text:text:120", "Save a rollback-compatible private note", "save_note", "dm", "!note-save lunch")

function read_note()
  local ok,state=migrate()
  if not ok then return "Note migration: "..state end
  return kv.get("example:note-v2") or "No note"
end
command("note-read", "", "Read the private note", "read_note", "dm")
