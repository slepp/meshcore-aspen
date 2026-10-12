# Hew compiler and standard-library probes

Run a probe from `hosts/willow` with `hew check tests/probes/NAME.hew`.
These small programs retain compiler and API examples independently of the
production host checks. A successful type check does not establish runtime
behaviour; the networking runtime checks remain in `make library-test`.

The following type-check results apply to Hew
`0.6.0-rc8-dev.230+3e9267d355` and
`0.6.0-rc8-dev.1+017db3b`, each with its matching standard library.

| Probe | Result on both revisions |
| --- | --- |
| `feasibility.hew` | Pass |
| `range_cast_probe.hew` | Pass |
| `select_none_probe.hew` | Pass, unused-variable warnings |
| `std_net_select_probe.hew` | Pass |
| `std_tcp_write_probe.hew` | Pass |
| `process_pipe_probe.hew` | Fails: `std.process.start_argv` is unavailable |
| `select_send_probe.hew` | Fails: a sink send is not a supported select source |
| `std_tls_deadline_probe.hew` | Fails: its two-argument `tls.connect` call differs from the current three-argument API |
| `std_websocket_binary_probe.hew` | Fails: the send result must be handled |

The supervision resource contract remains at
[`../supervised_resource_contract.hew`](../supervised_resource_contract.hew).
It has also been executed successfully with revision `017db3b`; this does not
imply that the full Willow application has been built with that revision.
