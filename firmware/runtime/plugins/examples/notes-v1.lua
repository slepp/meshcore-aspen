function save_note(text)
  local result = kv.transaction({
    {key="example:note-v1", value=text},
    {key="example:schema", value="1"},
  })
  if not result.ok then return "Note write "..result.status.."; read before retrying" end
  return "Saved"
end
command("note-save", "text:text:120", "Save a private note", "save_note", "dm", "!note-save lunch")

function read_note()
  return kv.get("example:note-v1") or "No note"
end
command("note-read", "", "Read the private note", "read_note", "dm")
