# HTTP/2 wire conformance methodology

The test organization in `http2_wire_conformance.py` is inspired by h2spec's
connection-per-case, handcrafted-frame, and wire-level error-verification approach.
The implementation and RFC 9113 expectations in this repository are independently
maintained; no h2spec source is embedded.

h2spec is copyright (c) 2014 Moto Ishizawa and distributed under the MIT License:
https://github.com/summerwind/h2spec
