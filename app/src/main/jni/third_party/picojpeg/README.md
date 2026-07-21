# PicoJPEG provenance

PicoJPEG v1.1 by Rich Geldreich is public-domain software. The public-domain statement is retained at the top of both source files.

- Upstream: `https://github.com/richgel999/picojpeg`
- Pinned upstream commit: `8ab33a909b4115ace4a952b7ffcb64c350f9298d`
- Downloaded `picojpeg.c` SHA-256 before line-ending normalization: `e8cbf352a9d9aa6074259d53d9de3341b883fa52380fa94d83ce7e47b91d3a29`
- Downloaded `picojpeg.h` SHA-256 before line-ending normalization: `f0a247e2eac6ecb2d2c6f94d29b8f99eca4977c217e5d76ca1db9f4db162414c`
- Vendored `picojpeg.c` SHA-256: `55f51f4ce4ff5b9dd92dab2d13973e73e6f9bffa1148db4641c916b33e3bd00e`
- Vendored `picojpeg.h` SHA-256: `58e2a0f28f64d292d6e01f06ab582139933a95aed39df731c3d5804752d3ef11`

CRLF line endings were normalized to LF while vendoring. Four color-conversion expressions that both incremented and dereferenced the same pointer without a sequence point were split into an assignment followed by an increment. This preserves the intended current-pixel operation while removing undefined C evaluation order reported by GCC 4.9.
