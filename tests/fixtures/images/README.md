# Ordinary image fixtures

Generated original landscape artwork and simple transparency/color examples,
licensed under the BaseOS project MIT license. No downloaded photograph,
private content, fuzz input, exploit sample, or intentionally faulting file is
included.

`tools/make_image_examples.py` generates each standard image using Pillow and
records independent decoded pixels as compressed `.rgba.zlib` references.
`manifest.json` records dimensions, detected-format IDs, and file hashes.
The BOS1 reference holds palette bytes, not RGBA. PNG16 references use the
documented high-byte reduction to 8-bit output. The two `*_limit` files are
valid normal images whose resource needs exceed BaseOS's explicit limits.

Pillow is needed only to regenerate; ordinary tests use the checked-in bytes
with Python's standard library. JPEG references allow small differences between
integer IDCT implementations. GIF fully transparent pixels may have different
unused RGB bytes; alpha and visible colors must match.
