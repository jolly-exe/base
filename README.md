# base

Convert between raw bytes and base 2, 8, 10, or 16.

```sh
echo -n "test" | base -o 16
# 74657374
```

## Dependencies

- Debian/Ubuntu: `apt install gcc git`
- Arch: `pacman -S gcc git`
- Fedora: `dnf install gcc git`
- Termux: `pkg install clang git`

## Install

**Prebuilt binaries** for Linux (glibc/musl, x86_64/aarch64/i686), Windows (x86_64), and macOS (aarch64) are available on the [releases page](https://github.com/jolly-exe/base/releases).

**System-wide (from source):**
```sh
git clone https://github.com/jolly-exe/base
cd base
gcc -O2 -o base base.c
sudo install -m755 base /usr/local/bin/
```

**Local (no sudo):**
```sh
git clone https://github.com/jolly-exe/base
cd base
gcc -O2 -o base base.c
mkdir -p ~/.local/bin
install -m755 base ~/.local/bin/
```

**Termux:**
```sh
git clone https://github.com/jolly-exe/base
cd base
clang -O2 -o base base.c
install -m755 base ~/../usr/bin/
```

Make sure `~/.local/bin` is in your `PATH` (add it to your shell's rc file, e.g. `~/.bashrc`, `~/.zshrc`, `~/.config/fish/config.fish`).

To uninstall:
- System-wide: `sudo rm /usr/local/bin/base`
- Local: `rm ~/.local/bin/base`
- Termux: `rm ~/../usr/bin/base`

## Usage

```
base [-i BASE] [-o BASE] [-x] [-I FILE] [-O FILE] [-X FILE]

  -i BASE           input base  (default: raw bytes)
  -o BASE           output base (default: raw bytes)
  -x                xor mode: read 'A:B' from stdin, xor decoded bytes
                    (requires -i; A and B must decode to equal length)
  -I, --input FILE      read input from FILE instead of stdin
  -O, --output FILE     write output to FILE instead of stdout
  -X, --xor-file FILE   second xor operand from FILE (requires -x; skips ':' parsing).
                        With -X, raw input is allowed (-i optional).
  -h, --help        display this help and exit
  -V, --version     output version information and exit

BASE: 2, 8, 10, 16
```

Append `le` or `be` to any base for explicit endianness (default: `be`).
Examples: `16le`, `10be`, `2le`, `8be`

Omitting `-i` or `-o` means raw bytes. Base 10 treats the whole input as a
single arbitrarily large integer.

## Examples

```sh
echo -n "test" | base -o 16           # text to hex
echo -n "test" | base -o 16le         # text to hex, little-endian
echo -n "74657374" | base -i 16       # hex to text
echo -n "hi" | base -o 2              # text to binary
echo -n "test" | base -o 10           # raw bytes to decimal (big-endian)
echo -n "test" | base -o 10le         # raw bytes to decimal (little-endian)
echo -n "ff" | base -i 16 -o 10       # hex to decimal: 255
echo -n "255" | base -i 10 -o 16      # decimal to hex: ff
echo -n "255" | base -i 10 -o 2       # decimal to binary: 11111111
cat file.bin | base -o 16             # dump binary file as hex
base -I file.bin -O file.hex -o 16    # same, no shell pipes
```

## XOR

`-x` xors two byte sequences. There are two ways to supply the operands:

**Stdin with `:` delimiter** — concatenate two encoded values separated by `:`.
Requires `-i` (raw bytes can't be split by `:` unambiguously).

**`-X FILE`** — the first operand comes from stdin (or `-I`), the second from
the file. With `-X`, raw input is allowed (`-i` becomes optional). This is the
cleanest way to xor files together.

In both cases, the two operands must decode to the same number of bytes. The
result is written raw, or re-encoded with `-o`. Only the first `:` splits; any
additional `:` is invalid for every base and is ignored along with other
invalid characters (whitespace, etc.).

```sh
# Stdin with ':' delimiter
echo -n "74657374:01020304" | base -i 16 -x -o 16      # hex xor: 75677070
echo -n "11110000:00001111" | base -i 2  -x -o 2       # binary xor: 11111111
echo -n "377:252"           | base -i 8  -x -o 8       # octal xor: 125
echo -n "255:170"           | base -i 10 -x -o 10      # decimal xor: 85
echo -n "ff:aa"             | base -i 16 -x -o 2       # cross-base: 01010101
echo -n "20:55"             | base -i 16 -x            # raw output: u

# Using -X (raw bytes, no encoding needed)
base -x -I data.bin -X key.bin -O out.bin

# Mix and match — pipe data, key from file, hex output
echo -n "deadbeef" | base -i 16 -x -X key.hex -o 16
```

## Bases

| Base | Encoding | Bytes | Roundtrips with raw |
|------|----------|-------|---------------------|
| 2    | binary, 8 chars per byte | `0110100001101001` | ✓ |
| 8    | octal, 3 chars per byte | `150151` | ✓ |
| 10   | decimal integer (whole input) | `26729` | ✗ |
| 16   | hex, 2 chars per byte | `6869` | ✓ |

Base 10 does not roundtrip because leading zero bytes are lost — `0x0068` and
`0x68` both encode to `104`, so decoding `104` back to bytes has no way to
recover the leading zero.

Endianness affects byte order. `be` (default) reads left to right; `le` reverses
the byte order. For base 10 this changes the numeric value; for 2, 8, and 16 it
changes the order bytes are encoded or decoded.


## License

GPLv3+
