// Package state owns private durable host identity and role-state authority.
// Callers hold Lock for the root directory for the host's entire lifetime.
//
// identity-state.json version 1 optionally contains pending_identity, a base64
// encoding of exactly 64 native scalar-prefix bytes. StageRoleIdentity accepts
// only room/repeater roles, leaves active identity/document untouched and keeps
// pending in that same authority. WriteRoleJSON preserves it on ordinary saves.
// Legacy identity/document files remain non-authoritative once an envelope exists.
//
// ActivateRoleIdentity takes the pure roles.RebindStateIdentity-compatible
// callback. It must run with the target application and source fully retired.
// Rebound document, activated identity and cleared pending field are committed
// together. No pending key means no writes. Existing ResetRole and active
// ImportRoleIdentity replace the envelope without retaining stale pending keys.
//
// One process-wide coordination lock serializes envelope changes and collision
// decisions against all five host roles' canonical current and pending keys.
// It is not an identity registry; each decision reads storage. Context-aware
// stage/import admission can cancel while waiting or before commit. Once a
// write begins its actual commit result is authoritative, not a late context
// error. ErrCommitIndeterminate must remain fail-closed: never compensate with
// an old document or blindly activate/reopen after an uncertain result.
package state
