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

**System-wide:**
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

Make sure `~/.local/bin` is in your `PATH` (add it to your shell's rc file, e.g. `~/.bashrc`, `~/.zshrc`, `~/.config/fish/config.fish`).

**Termux:**
```sh
git clone https://github.com/jolly-exe/base
cd base
clang -O2 -o base base.c
install -m755 base ~/../usr/bin/
```

To uninstall:
- System-wide: `sudo rm /usr/local/bin/base`
- Local: `rm ~/.local/bin/base`
- Termux: `rm ~/../usr/bin/base`

## Usage

```
base [-i BASE] [-o BASE]

  -i BASE       input base  (default: raw bytes)
  -o BASE       output base (default: raw bytes)
  -h, --help    display this help and exit
  -V, --version output version information and exit

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
