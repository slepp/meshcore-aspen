# Review reader-facing copy

Run from the repository root:

```sh
python3 tools/check_copy.py
python3 tools/check_copy.py --inventory
python3 tools/check_copy.py hosts/willow firmware/esp32 dashboards/grafana
```

For the website checkout, use its path and select its public pages:

```sh
python3 tools/check_copy.py --root /path/to/ve6slp.ca public/projects/meshcore
```

The command finds phrases worth reading in context: vague state claims,
routine qualifications, contrast clauses and development-process language.
Matches are review prompts. They do not fail a build or require replacing
every occurrence. A successful run means the search completed, not that the
copy has been reviewed.

Read the whole passage or interaction before editing. Name what the user
can do, what the state means and any useful next step. Keep real security,
compatibility and destructive-action warnings, as well as established terms
such as MQTT retained messages. Do not change machine-readable fields or
strengthen delivery guarantees while improving prose.

When a match needs to stay, explain why on the same or preceding line using
the file's ordinary comment syntax:

```html
<!-- copy-check: allow vague-state -- MQTT retained messages is the protocol term. -->
<p>Clear the retained messages before changing the topic.</p>
```

Use `vague-state`, `qualification`, `contrast` or `process`, as printed by the
command. Exceptions remain visible in the output with their reasons. Avoid
file-wide exclusions for a phrase that needs one local explanation.
For JSON and other formats without comments, leave the prompt visible and
record the reason in the copy review or PR. Do not add review metadata to
product text or machine-readable fields.

## Surfaces to inspect

Use `--inventory` to list candidate files before a cleanup. The normal scan
uses Git-tracked files; explicit paths also include new files.

| Surface | Main locations |
| --- | --- |
| Discovery and setup | Root README, firmware and host READMEs, setup and installation guides |
| Firmware controls and status | `firmware/esp32/MastAdminPage.h`, admin handlers, `firmware/runtime`, `firmware/nrf52840` |
| Willow controls and status | `hosts/willow/src`, `hosts/willow/native`, `hosts/willow/docs`, launch tools |
| Birch controls and status | `cmd`, `internal/app/admin_page.html`, `internal` handlers |
| Mesh commands | Native command handlers, `firmware/runtime/plugins`, `command-library` |
| Shared room and terminal client | `services/shared-room`, `clients/room-tui` |
| Monitoring | `dashboards/grafana` JSON titles, descriptions, legends and value mappings |
| Publishing and operator tools | Help and errors in `tools`, firmware setup/update helpers |
| Website | The separate website checkout's `public/projects/meshcore` pages |
| Embedding packages | `packages` READMEs and examples |

Tests, fixtures, vendored code, generated build directories, history,
protocol references and named maintainer guides are excluded. Markdown code
fences and inline code are skipped. Source scans inspect string literals,
including C++ raw strings, Hew strings and Lua long strings, rather than
identifiers or comments. JSON field names are skipped. HTML scanning covers
text, attributes and inline JavaScript strings while skipping styles and
comments.

This is a small search aid, not a language parser or rendered-interface
check. Some source strings contain protocol data; classify those in context.
Computed or translated text, split phrases across lines, unusual string
syntax and dynamically loaded copy need a manual pass. Search excluded
reference material separately when it contains a reader-facing section.
Use existing browser checks for changed pages, including layout, keyboard
access, links and errors. Report the surfaces reviewed and any remaining gaps.
