# SPDX-License-Identifier: Apache-2.0
"""Compact source-package metadata and optional detached Ed25519 signatures."""
import base64
from dataclasses import dataclass
import hashlib
import re

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.exceptions import InvalidSignature

SOURCE_LIMIT = 4096
PREFIX = b"--@meshcore-bot/1;"
RUNTIME = "lua-5.5.1"
API = "named-commands-v1"
WASM_RUNTIME = "wamr-2.4.1"
WASM_API = "meshcore-v1"
CAPABILITIES = frozenset({
    "cmdmeta", "events", "https", "kv", "kv.atomic", "mesh", "mesh-chan", "mesh-dest",
    "modules", "reminders", "timers", "utilities",
})
WASM_CAPABILITIES = CAPABILITIES - {"modules"}
_NAME = re.compile(r"[a-z][a-z0-9-]{0,23}\Z")
_VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
_SCHEMA = re.compile(r"(?:none|[a-z][a-z0-9-]{0,23}@[1-9][0-9]{0,4})\Z")
_SIGNATURE_HEADER = b"MCBOT-SIGNATURE/1\n"


@dataclass(frozen=True)
class Package:
    name: str
    version: str
    runtime: str
    api: str
    capabilities: tuple[str, ...]
    schema: str
    rollback: str
    source: bytes

    @property
    def digest(self):
        return hashlib.sha256(self.source).hexdigest()


def _validate_metadata(name, version, runtime, api, capabilities, schema, rollback):
    if not _NAME.fullmatch(name):
        raise ValueError("Package name must be a lowercase identifier of at most 24 bytes")
    if not _VERSION.fullmatch(version) or len(version) > 23:
        raise ValueError("Package version must be numeric major.minor.patch of at most 23 characters")
    if runtime not in (RUNTIME, WASM_RUNTIME):
        raise ValueError(f"Unsupported runtime {runtime!r}")
    expected_api = WASM_API if runtime == WASM_RUNTIME else API
    if api != expected_api:
        raise ValueError(f"Unsupported command API {api!r}; expected {expected_api}")
    if schema != "none" and not _SCHEMA.fullmatch(schema):
        raise ValueError("Data schema must be none or a lowercase name@positive-version")
    if rollback != "none" and not _SCHEMA.fullmatch(rollback):
        raise ValueError("Rollback schema must be none or a lowercase name@positive-version")
    if tuple(sorted(set(capabilities))) != tuple(capabilities):
        raise ValueError("Capabilities must be unique and sorted")
    unsupported = set(capabilities) - (WASM_CAPABILITIES if runtime == WASM_RUNTIME else CAPABILITIES)
    if unsupported:
        raise ValueError("Unsupported required capabilities: " + ", ".join(sorted(unsupported)))


def inspect(source):
    if not isinstance(source, bytes) or not 1 <= len(source) <= SOURCE_LIMIT:
        raise ValueError("Package must contain 1..4096 source bytes")
    if not source.startswith(PREFIX):
        raise ValueError("Bot package has no meshcore-bot metadata header")
    line, separator, body = source.partition(b"\n")
    if not separator or len(line) > 255 or not body:
        raise ValueError("Package metadata needs a short header and non-empty Lua source")
    try:
        fields = line[len(PREFIX):].decode("ascii").split(";")
    except UnicodeDecodeError as error:
        raise ValueError("Package metadata must be ASCII") from error
    keys = ("name", "version", "runtime", "api", "caps", "schema", "rollback")
    if len(fields) != len(keys):
        raise ValueError("Package metadata has missing or extra fields")
    values = {}
    for expected, field in zip(keys, fields):
        key, separator, value = field.partition("=")
        if not separator or key != expected or not value or value.strip() != value or any(c.isspace() for c in value):
            raise ValueError("Package metadata fields are invalid or out of order")
        values[key] = value
    caps = () if values["caps"] == "none" else tuple(values["caps"].split(","))
    _validate_metadata(values["name"], values["version"], values["runtime"], values["api"],
                       caps, values["schema"], values["rollback"])
    if values["runtime"] == WASM_RUNTIME:
        if not body.startswith(b"\0asm\1\0\0\0"):
            raise ValueError("Wasm package must contain a portable version-1 module, not AOT")
    elif source.startswith(b"\x1b") or b"\0" in source:
        raise ValueError("Lua bytecode and NUL bytes are not accepted")
    return Package(values["name"], values["version"], values["runtime"], values["api"],
                   caps, values["schema"], values["rollback"], source)


def create(source, name, version, capabilities=(), schema="none", rollback="none"):
    if not isinstance(source, bytes) or not source or len(source) > SOURCE_LIMIT:
        raise ValueError("Input must contain non-empty Lua source or portable Wasm")
    wasm = source.startswith(b"\0asm\1\0\0\0")
    if not wasm and (source.startswith(b"\x1b") or b"\0" in source):
        raise ValueError("Lua bytecode and NUL bytes are not accepted")
    if source.startswith(PREFIX):
        raise ValueError("Input is already a packaged bot module")
    caps = tuple(sorted(set(capabilities)))
    runtime, api = (WASM_RUNTIME, WASM_API) if wasm else (RUNTIME, API)
    _validate_metadata(name, version, runtime, api, caps, schema, rollback)
    metadata = (f"--@meshcore-bot/1;name={name};version={version};runtime={runtime};api={api};"
                f"caps={','.join(caps) if caps else 'none'};schema={schema};rollback={rollback}\n").encode("ascii")
    package = metadata + source
    if len(package) > SOURCE_LIMIT:
        raise ValueError(f"Package is {len(package)} bytes; the source envelope limit is {SOURCE_LIMIT}")
    return inspect(package)


def validate_transition(active, candidate):
    if active is None:
        return
    validate_schema_transition(active.schema, candidate)


def validate_schema_transition(active_schema, candidate):
    if active_schema == candidate.schema:
        return
    if candidate.rollback != active_schema:
        raise ValueError(
            f"Schema change {active_schema} -> {candidate.schema} must declare "
            f"rollback={active_schema}; destructive or incompatible migrations are not automatic"
        )


def _private_key(data):
    if len(data) == 32:
        return ed25519.Ed25519PrivateKey.from_private_bytes(data)
    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ed25519.Ed25519PrivateKey):
        raise ValueError("Signing key must be Ed25519")
    return key


def sign(source, private_key):
    inspect(source)
    key = _private_key(private_key)
    public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    signature = key.sign(source)
    digest = hashlib.sha256(source).hexdigest().encode("ascii")
    return (_SIGNATURE_HEADER + b"sha256=" + digest + b"\npublic=" +
            base64.b64encode(public) + b"\nsignature=" + base64.b64encode(signature) + b"\n")


def verify(source, signature_file, public_key):
    inspect(source)
    if len(public_key) != 32:
        try:
            public_key = serialization.load_pem_public_key(public_key).public_bytes(
                serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        except (ValueError, TypeError) as error:
            raise ValueError("Verification key must be a 32-byte Ed25519 public key or PEM") from error
    lines = signature_file.splitlines()
    if len(lines) != 4 or lines[0] + b"\n" != _SIGNATURE_HEADER:
        raise ValueError("Invalid detached package signature format")
    if not lines[1].startswith(b"sha256=") or not lines[2].startswith(b"public=") or not lines[3].startswith(b"signature="):
        raise ValueError("Invalid detached package signature fields")
    digest = lines[1][7:].decode("ascii")
    if digest != hashlib.sha256(source).hexdigest():
        raise ValueError("Signature references a different package SHA256")
    try:
        embedded = base64.b64decode(lines[2][7:], validate=True)
        signature = base64.b64decode(lines[3][10:], validate=True)
    except (ValueError, TypeError) as error:
        raise ValueError("Invalid base64 package signature data") from error
    if embedded != public_key or len(signature) != 64:
        raise ValueError("Package signature public key differs from the selected key")
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(public_key).verify(signature, source)
    except InvalidSignature as error:
        raise ValueError("Package Ed25519 signature verification failed") from error
    return digest
