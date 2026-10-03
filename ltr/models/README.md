# Model artifacts

Generated model weights are ignored by Git. Keep only explicitly released,
versioned artifacts in source control or an external model registry.

`lambdarank-v1/` is the first explicitly released artifact. Its model and
metadata are tracked so the C++ runtime and Python parity tests use the same
immutable input.
