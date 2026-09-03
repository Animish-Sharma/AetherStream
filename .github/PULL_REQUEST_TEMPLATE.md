## Summary

Describe the problem, implementation, user-visible behavior, and compatibility impact.

## Verification

- [ ] Python tests pass (`python -m pytest -v`).
- [ ] Native tests pass (`ctest --test-dir build --output-on-failure`).
- [ ] New behavior has focused regression tests.
- [ ] ASan/UBSan tests pass for parser, allocation, or wire-format changes.
- [ ] Scalar output was compared with every affected SIMD backend.
- [ ] The 2 GB low-memory, one-job build remains functional.
- [ ] Thread-safety was considered; shared mutable state was not introduced.

## Documentation and compatibility

- [ ] Public API documentation and examples are updated.
- [ ] `CHANGELOG.md` contains a user-facing entry.
- [ ] Wire changes increment the format version and update `docs/WIRE_FORMAT_SPEC.md`.
- [ ] CMake, Python, vcpkg, Conda, and wheel packaging implications were considered.
- [ ] Benchmark claims include hardware, compiler, data provenance, and commands.

## Risk

List numerical, memory-safety, denial-of-service, ABI, and rollback risks. Link the issue being resolved.
