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
| `std_websocket_binary_probe.hew` | Pass, unused-function warning |

`process_pipe_probe.hew` and `select_send_probe.hew` are expected negative
feature queries; their rejection records the unavailable operations.

`std.net.tls.connect(host, port, tls.options())` completes a verified handshake
before returning. Its default handshake deadline is 10 seconds; subsequent
read/write timeouts are configured separately. Run `make outbound-test` for
Willow's TLS certificate, deadline, write-failure and supervision checks.

The supervision resource contract remains at
[`../supervised_resource_contract.hew`](../supervised_resource_contract.hew).
It has also been executed successfully with revision `017db3b`; this does not
imply that the full Willow application has been built with that revision.
