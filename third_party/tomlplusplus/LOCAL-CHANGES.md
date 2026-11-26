# Local changes to toml++ 3.4.0

`toml++/toml.hpp`, `is_non_ascii_horizontal_whitespace()`: a code point in the highest
lookup bucket other than U+FEFF (for example U+FD3F) fell through to `TOML_UNREACHABLE`,
which is undefined behaviour in an optimised build. It now returns `false` (not
whitespace). Found by fuzzing framelighter's config and settings parsers with the input
`p` U+FD3F.

`toml++/toml.hpp`, the formatter's `calc_weight` for a floating-point value: for a value
between -1 and 1, or infinity, `static_cast<size_t>(log10(val))` converted a negative or
infinite number to an unsigned integer, which is undefined behaviour (it only feeds the
choice of how to lay out an array, so no wrong file came of it). The number of digits is
now taken as 0 there. Found by running the unit tests with `-fsanitize=float-cast-overflow`:
every calibration with a small number in it went through it when the settings were written.
