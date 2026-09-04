# Security Policy

## Supported versions

| Release line | Security support |
|---|---|
| 0.0.x | Supported |
| Earlier package releases and wire versions 2–4 | Unsupported; transcode and upgrade |

Security fixes are released as patch versions. A security release may reject a previously accepted malformed stream; accepting hostile input safely takes precedence over permissive compatibility.

## Private reporting

Use GitHub's **Security → Report a vulnerability** form at https://github.com/animish-sharma/aetherstream/security/advisories/new. This creates an encrypted, private discussion with maintainers. If GitHub is unavailable, email `security@aetherstream.dev`; encrypt sensitive attachments using the OpenPGP key advertised through WKD for that address. Verify the key through a second channel before sending embargoed material.

Do not open a public issue for a suspected memory-safety problem. Include the affected version/commit, platform, sanitizer output, reproducer or fuzz corpus input, expected impact, and whether the sample may be shared with downstream coordinators. We acknowledge reports within 48 hours and provide an initial triage result within seven days.

## Fuzzing and memory-safety reports

Preserve the exact corpus input, fuzzer command line, seed, sanitizer options, compiler revision, and symbolized stack. Minimize inputs only after retaining the original. Reports involving out-of-bounds access, use-after-free, integer overflow leading to unsafe allocation, or decoder denial of service are handled under coordinated disclosure.

For a confirmed buffer overrun, maintainers will freeze public discussion, reproduce under ASan/UBSan, audit adjacent parsing paths, prepare regression corpus entries, request a CVE through GitHub Security Advisories, notify known distributors, and publish fixed wheels/source plus an advisory. The default embargo is 90 days, shortened when exploitation is known or a coordinated release is ready.

## Scope

The C++ decoder, Python bindings, stream framing, SIMD dispatch, build scripts, and official containers are in scope. Availability issues must demonstrate a bounded input causing disproportionate CPU or memory consumption. Benchmark disagreements and malformed data that is safely rejected are ordinary bug reports.
