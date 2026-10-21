# Local changes to toml++ 3.4.0

`toml++/toml.hpp`, `is_non_ascii_horizontal_whitespace()`: a code point in the highest
lookup bucket other than U+FEFF (for example U+FD3F) fell through to `TOML_UNREACHABLE`,
which is undefined behaviour in an optimised build. It now returns `false` (not
whitespace). Found by fuzzing framelighter's config and settings parsers with the input
`p` U+FD3F.
