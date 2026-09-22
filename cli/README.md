# cli

A single Rust front end is planned and not yet written. Until it exists there
are two executables:

- the C `abicase` tool in [../tools](../tools): `dump`, `verify`, `id`, `digest`
  and `selftest`;
- `abi-coordinator` in [../coordinator](../coordinator), which runs cases
  against workers in isolated processes and writes the run directory.
