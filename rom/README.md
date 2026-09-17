# The cartridge is not in this repository

`Video Olympics` is Atari's, 1977, written by Joe Decuir (CX2621). This project
is a patch and a server; it is not a place to redistribute the game. Supply your
own dump:

| File | Size | md5 |
|---|---|---|
| `Video-Olympics.bin` | 2048 | `60e0ea3cbe0913d39803477945e9e5ec` |

`video-olympics.asm` is **generated** from that dump by `make disasm`, which
runs DiStella over it with the code/data map in `tools/vo.cfg`. It is not
checked in either, for the same reason the dump is not: it is the game.

Unlike Combat, there was no published commented disassembly of Video Olympics to
start from -- not in the `milnak/atari-vcs-disassembly` collection, not anywhere
else. `tools/vo.cfg` is this project's own work, and `make verify-org` is what
makes it trustworthy: it reassembles the generated source through
`tools/dasm2as.py` and requires the result to be the cartridge dump, byte for
byte. Nothing downstream is worth running until that passes.
