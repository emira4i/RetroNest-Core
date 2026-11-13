#!/usr/bin/env python3
"""Generates the tiny test programs used by the on-device core smoke test (:corehost androidTest).

Every byte here is written by this script: no third-party ROM, no BIOS, no boot code. Each program is
the smallest thing a core accepts as content for its system: a header (where the core reads one), a
few instructions that set the background colour to a known non-black value, and an endless loop.

Run from anywhere (standard library only):
    python tools/testroms/make_test_roms.py           # (re)write the assets
    python tools/testroms/make_test_roms.py --check   # exit 1 if the checked-in assets are stale

Outputs go to corehost/src/androidTest/assets/testroms/<system>.bin. They are checked in (a few KB in
total) so the Gradle build and the test need no Python; the extension the core expects is chosen by
the test when it copies the file out (the repo .gitignore blocks real ROM extensions on purpose).

Systems and what the core needs to accept the content:
  nes   fceumm      iNES header, mapper 0, 16 KB PRG + CHR RAM. Backdrop colour $21 (light blue).
  gb    mgba        32 KB ROM-only cart. mGBA's GB detection reads ONLY the first 4 bytes of the
                    header logo field (CE ED 66 66); the other 44 logo bytes are left zero. No boot
                    ROM runs (no BIOS file, mgba_skip_bios=ON), so nothing checks the logo.
                    BGP=$55 (light grey instead of the post-boot white).
  gbc   mgba        Same, CGB flag $80, BG palette 0 colour 0 = green.
  gba   mgba        Header with the ARM branch at 0 (mGBA checks byte 3 = $EA) and the fixed $96 at
                    $B2; the logo area is zero (HLE BIOS, no logo check). Backdrop = blue.
  snes  bsnes       32 KB LoROM with a valid internal header + checksum. Backdrop = red.
  md    clownmdemu  68000 vectors + standard header, VDP mode 5 display on, CRAM 0 = green.
  sms   gearsystem  32 KB, no mapper, no "TMR SEGA" header (gearsystem does not need it).
                    Mode 4 display on, CRAM 0 and 16 = red.
  a26   stella      4 KB cart, NTSC kernel (VSYNC / 37 VBLANK / 192 visible / 30 overscan),
                    COLUBK = $44 (red).
  psx   pcsx_rearmed  PS-X EXE run on the core's HLE BIOS: GPU display on, VRAM filled red.
  nds   melondsds   Homebrew-style header (ARM9 at $200, ARM7 at $400, unencrypted), direct boot on
                    the core's built-in FreeBIOS: POWCNT on, DISPCNT mode 1, backdrop red.
N64 is not generated: booting a cartridge needs the console's IPL3 boot code (copyrighted), which the
core verifies by checksum/CIC detection, so there is no honest self-made N64 program.
"""
import os
import struct
import sys

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "corehost", "src", "androidTest", "assets", "testroms")


def pad(data, size, fill=0):
    assert len(data) <= size, (len(data), size)
    return bytes(data) + bytes([fill]) * (size - len(data))


def place(rom, offset, data):
    rom[offset:offset + len(data)] = data


# ------------------------------------------------------------------ NES (6502)

def nes():
    org = 0xC000
    code = bytes([
        0x78,                    # SEI
        0xD8,                    # CLD
        0xA2, 0xFF, 0x9A,        # LDX #$FF / TXS
        0xAD, 0x02, 0x20,        # vbl1: LDA $2002
        0x10, 0xFB,              # BPL vbl1
        0xAD, 0x02, 0x20,        # vbl2: LDA $2002
        0x10, 0xFB,              # BPL vbl2
        0xA9, 0x3F, 0x8D, 0x06, 0x20,  # PPUADDR = $3F00 (palette)
        0xA9, 0x00, 0x8D, 0x06, 0x20,
        0xA9, 0x21, 0xA2, 0x20,  # every palette entry = $21 (backdrop included)
        0x8D, 0x07, 0x20,        # pal: STA $2007
        0xCA, 0xD0, 0xFA,        # DEX / BNE pal
        0xA9, 0x00, 0x8D, 0x06, 0x20,  # PPUADDR = 0, scroll = 0
        0x8D, 0x06, 0x20,
        0x8D, 0x05, 0x20,
        0x8D, 0x05, 0x20,
        0xA9, 0x0A, 0x8D, 0x01, 0x20,  # PPUMASK: show background
    ])
    loop = org + len(code)
    code += bytes([0x4C, loop & 0xFF, loop >> 8])  # JMP loop
    rti = org + len(code)
    code += bytes([0x40])  # RTI
    prg = bytearray(pad(code, 0x4000))
    place(prg, 0x3FFA, struct.pack("<HHH", rti, org, rti))  # NMI, RESET, IRQ
    header = b"NES\x1a" + bytes([1, 0, 0, 0]) + bytes(8)  # 1x16 KB PRG, CHR RAM, mapper 0
    return header + bytes(prg)


# ------------------------------------------------------------------ GB / GBC (SM83)

def gb(cgb):
    rom = bytearray(0x8000)
    place(rom, 0x100, bytes([0x00, 0xC3, 0x50, 0x01]))  # NOP / JP $0150
    place(rom, 0x104, bytes([0xCE, 0xED, 0x66, 0x66]))  # what mGBA's GBIsROM compares (4 of 48 bytes)
    place(rom, 0x134, b"RNTEST")
    rom[0x143] = 0x80 if cgb else 0x00
    rom[0x147] = 0x00  # ROM only
    rom[0x148] = 0x00  # 32 KB
    rom[0x149] = 0x00  # no RAM
    code = bytes([
        0xF3,                    # DI
        0x31, 0xFE, 0xFF,        # LD SP,$FFFE
        0xF0, 0x44,              # vbl: LDH A,(LY)
        0xFE, 0x90,              # CP 144
        0x20, 0xFA,              # JR NZ,vbl
        0xAF, 0xE0, 0x40,        # XOR A / LCDC = 0 (LCD off: palette RAM writable)
        0x3E, 0x55, 0xE0, 0x47,  # BGP = $55
    ])
    if cgb:
        code += bytes([
            0x3E, 0x80, 0xE0, 0x68,  # BCPS = index 0, auto-increment
            0x3E, 0xE0, 0xE0, 0x69,  # BCPD = $03E0 (green), low byte
            0x3E, 0x03, 0xE0, 0x69,  # high byte
        ])
    code += bytes([0x3E, 0x91, 0xE0, 0x40])  # LCDC = $91 (LCD + BG on)
    code += bytes([0x18, 0xFE])  # JR -2
    place(rom, 0x150, code)
    chk = 0
    for b in rom[0x134:0x14D]:
        chk = (chk - b - 1) & 0xFF
    rom[0x14D] = chk
    total = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
    rom[0x14E:0x150] = struct.pack(">H", total)
    return bytes(rom)


# ------------------------------------------------------------------ GBA (ARM)

def gba():
    rom = bytearray(0x400)
    place(rom, 0x00, struct.pack("<I", 0xEA00002E))  # B $C0 (byte 3 = $EA)
    place(rom, 0xA0, b"RNTEST")
    place(rom, 0xAC, b"RNTE")
    place(rom, 0xB0, b"00")
    rom[0xB2] = 0x96
    chk = 0
    for b in rom[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    rom[0xBD] = (chk - 0x19) & 0xFF
    code = [
        0xE3A00301,  # MOV r0,#0x04000000
        0xE3A01000,  # MOV r1,#0
        0xE1C010B0,  # STRH r1,[r0]       DISPCNT = 0 (mode 0, no layers: backdrop only)
        0xE3A00405,  # MOV r0,#0x05000000
        0xE3A01C7C,  # MOV r1,#0x7C00     blue (BGR555)
        0xE1C010B0,  # STRH r1,[r0]       palette 0
        0xEAFFFFFE,  # B .
    ]
    place(rom, 0xC0, b"".join(struct.pack("<I", w) for w in code))
    return bytes(rom)


# ------------------------------------------------------------------ SNES (65816)

def snes():
    rom = bytearray(0x8000)
    code = bytes([
        0x78,                    # SEI
        0x18, 0xFB,              # CLC / XCE (native mode, 8-bit A/X)
        0xA9, 0x8F, 0x8D, 0x00, 0x21,  # INIDISP = $8F (forced blank)
        0x9C, 0x21, 0x21,        # CGADD = 0
        0xA9, 0x1F, 0x8D, 0x22, 0x21,  # CGDATA = $001F (red), low
        0xA9, 0x00, 0x8D, 0x22, 0x21,  # high
        0xA9, 0x0F, 0x8D, 0x00, 0x21,  # INIDISP = $0F (display on, full brightness)
        0x80, 0xFE,              # BRA -2
    ])
    place(rom, 0x0000, code)
    rti = 0x8000 + len(code)
    rom[len(code)] = 0x40  # RTI
    place(rom, 0x7FC0, b"RETRONEST SMOKE TEST ")  # 21 bytes
    rom[0x7FD5] = 0x20  # LoROM
    rom[0x7FD6] = 0x00  # ROM only
    rom[0x7FD7] = 0x05  # 32 KB
    rom[0x7FD8] = 0x00  # no SRAM
    rom[0x7FD9] = 0x01  # North America
    rom[0x7FDA] = 0x00
    rom[0x7FDB] = 0x00
    place(rom, 0x7FDC, struct.pack("<HH", 0xFFFF, 0x0000))
    for off in range(0x7FE4, 0x7FF0, 2):  # native COP/BRK/ABORT/NMI/-/IRQ
        place(rom, off, struct.pack("<H", rti))
    for off in range(0x7FF4, 0x8000, 2):  # emulation COP/-/ABORT/NMI/RESET/IRQ
        place(rom, off, struct.pack("<H", rti))
    place(rom, 0x7FFC, struct.pack("<H", 0x8000))  # RESET
    total = sum(rom) & 0xFFFF
    place(rom, 0x7FDC, struct.pack("<HH", total ^ 0xFFFF, total))
    return bytes(rom)


# ------------------------------------------------------------------ Mega Drive / Genesis (68000)

def md():
    rom = bytearray(0x2000)
    place(rom, 0x000, struct.pack(">II", 0x00FFFE00, 0x00000200))  # SSP, reset PC
    rte = 0x200 + 0x40
    for vec in range(2, 64):
        place(rom, vec * 4, struct.pack(">I", rte))
    place(rom, 0x100, b"SEGA MEGA DRIVE ")
    place(rom, 0x110, b"(C)RNST 2026.NOV")
    place(rom, 0x120, b"RETRONEST SMOKE TEST".ljust(48))
    place(rom, 0x150, b"RETRONEST SMOKE TEST".ljust(48))
    place(rom, 0x180, b"GM 00000000-00")
    place(rom, 0x190, b"J".ljust(16))
    place(rom, 0x1A0, struct.pack(">II", 0, len(rom) - 1))
    place(rom, 0x1A8, struct.pack(">II", 0x00FF0000, 0x00FFFFFF))
    place(rom, 0x1F0, b"JUE".ljust(16))
    code = bytes.fromhex(
        "23FC5345474100A14000"   # move.l #'SEGA',$A14000 (TMSS unlock; ignored without TMSS)
        + "33FC800400C00004"     # move.w #$8004,$C00004   reg 0
        + "33FC814400C00004"     # move.w #$8144,$C00004   reg 1: display on, mode 5
        + "33FC870000C00004"     # move.w #$8700,$C00004   reg 7: backdrop = CRAM 0
        + "23FCC000000000C00004"  # move.l #$C0000000,$C00004  CRAM write, address 0
        + "33FC00E000C00000"     # move.w #$00E0,$C00000   green
        + "60FE"                 # bra.s *
    )
    place(rom, 0x200, code)
    place(rom, rte, bytes.fromhex("4E73"))  # RTE
    words = struct.unpack(">%dH" % ((len(rom) - 0x200) // 2), bytes(rom[0x200:]))
    place(rom, 0x18E, struct.pack(">H", sum(words) & 0xFFFF))
    return bytes(rom)


# ------------------------------------------------------------------ Master System (Z80)

def sms():
    rom = bytearray(0x8000)

    def out(value, port):
        return bytes([0x3E, value, 0xD3, port])  # LD A,n / OUT (port),A

    code = (bytes([0xF3, 0xED, 0x56, 0x31, 0xF0, 0xDF])  # DI / IM 1 / LD SP,$DFF0
            + out(0x04, 0xBF) + out(0x80, 0xBF)        # reg 0: mode 4
            + out(0xFF, 0xBF) + out(0x82, 0xBF)        # reg 2: name table $3800
            + out(0x00, 0xBF) + out(0x87, 0xBF)        # reg 7: backdrop = sprite colour 0
            + out(0x00, 0xBF) + out(0xC0, 0xBF) + out(0x03, 0xBE)  # CRAM 0 = red
            + out(0x10, 0xBF) + out(0xC0, 0xBF) + out(0x03, 0xBE)  # CRAM 16 = red
            + out(0x40, 0xBF) + out(0x81, 0xBF)        # reg 1: display on
            + bytes([0x18, 0xFE]))                     # JR -2
    place(rom, 0x0000, bytes([0xF3, 0xC3, 0x00, 0x01]))  # DI / JP $0100 (past the RST/NMI vectors)
    place(rom, 0x0100, code)
    place(rom, 0x0038, bytes([0xED, 0x4D]))  # RETI
    place(rom, 0x0066, bytes([0xED, 0x45]))  # RETN
    return bytes(rom)


# ------------------------------------------------------------------ Atari 2600 (6507)

def a26():
    org = 0xF000
    code = bytearray([0x78, 0xD8, 0xA2, 0xFF, 0x9A])  # SEI / CLD / LDX #$FF / TXS
    frame = org + len(code)

    def lines(count):
        # LDX #count / loop: STA WSYNC / DEX / BNE loop
        return bytes([0xA2, count, 0x85, 0x02, 0xCA, 0xD0, 0xFB])

    code += bytes([0xA9, 0x02, 0x85, 0x00, 0x85, 0x02, 0x85, 0x02, 0x85, 0x02, 0xA9, 0x00, 0x85, 0x00])  # VSYNC 3 lines
    code += bytes([0xA9, 0x02, 0x85, 0x01]) + lines(37)          # VBLANK on, 37 lines
    code += bytes([0xA9, 0x00, 0x85, 0x01, 0xA9, 0x44, 0x85, 0x09]) + lines(192)  # VBLANK off, COLUBK red
    code += bytes([0xA9, 0x02, 0x85, 0x01]) + lines(30)          # overscan
    code += bytes([0x4C, frame & 0xFF, frame >> 8])              # JMP frame
    rom = bytearray(pad(code, 0x1000))
    place(rom, 0xFFC, struct.pack("<HH", org, org))  # RESET, IRQ/BRK
    return bytes(rom)


# ------------------------------------------------------------------ PlayStation (MIPS R3000, PS-X EXE)

def psx():
    load = 0x80010000
    words = [0x3C081F80]  # lui t0,0x1F80

    def store(value, port_off):
        words.extend([
            0x3C090000 | (value >> 16),     # lui t1,hi
            0x35290000 | (value & 0xFFFF),  # ori t1,t1,lo
            0xAD090000 | port_off,          # sw t1,off(t0)
        ])

    GP0, GP1 = 0x1810, 0x1814
    store(0x00000000, GP1)  # reset GPU
    store(0x08000001, GP1)  # display mode 320x240 NTSC 15-bit
    store(0x05000000, GP1)  # display area start 0,0
    store(0x06C60260, GP1)  # horizontal range
    store(0x07040010, GP1)  # vertical range
    store(0xE1000400, GP0)  # draw mode: drawing to the display area allowed
    store(0xE3000000, GP0)  # drawing area top-left 0,0
    store(0xE403BD3F, GP0)  # drawing area bottom-right 319,239
    store(0xE5000000, GP0)  # drawing offset 0,0
    store(0x020000FF, GP0)  # fill rectangle, red
    store(0x00000000, GP0)  #   at 0,0
    store(0x00F00140, GP0)  #   320x240
    store(0x03000000, GP1)  # display enable
    loop = load + len(words) * 4
    words.extend([0x08000000 | ((loop >> 2) & 0x03FFFFFF), 0x00000000])  # j loop / nop
    text = pad(b"".join(struct.pack("<I", w) for w in words), 0x800)
    header = bytearray(0x800)
    place(header, 0x00, b"PS-X EXE")
    place(header, 0x10, struct.pack("<IIII", load, 0, load, len(text)))  # pc0, gp0, t_addr, t_size
    place(header, 0x30, struct.pack("<II", 0x801FFF00, 0))              # s_addr, s_size
    return bytes(header) + text


# ------------------------------------------------------------------ Nintendo DS (ARM9 + ARM7)

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def nds():
    rom = bytearray(0x20000)
    arm9 = [
        0xE3A00301,  # MOV r0,#0x04000000
        0xE3A01C80,  # MOV r1,#0x8000
        0xE3811003,  # ORR r1,r1,#3
        0xE2802FC1,  # ADD r2,r0,#0x304
        0xE1C210B0,  # STRH r1,[r2]      POWCNT1 = LCD on, 2D engine A, swap
        0xE3A01801,  # MOV r1,#0x10000
        0xE5801000,  # STR r1,[r0]       DISPCNT A = display mode 1, no layers (backdrop)
        0xE3A00405,  # MOV r0,#0x05000000
        0xE3A0101F,  # MOV r1,#0x1F      red (BGR555)
        0xE1C010B0,  # STRH r1,[r0]
        0xEAFFFFFE,  # B .
    ]
    arm7 = [0xEAFFFFFE]  # B .
    arm9_bin = pad(b"".join(struct.pack("<I", w) for w in arm9), 0x100)
    arm7_bin = pad(b"".join(struct.pack("<I", w) for w in arm7), 0x100)
    place(rom, 0x200, arm9_bin)
    place(rom, 0x400, arm7_bin)
    place(rom, 0x00, b"RNSMOKETEST\0")
    place(rom, 0x0C, b"####")
    place(rom, 0x10, b"00")
    rom[0x14] = 0x00  # device capacity: 128 KB
    place(rom, 0x20, struct.pack("<IIII", 0x200, 0x02000000, 0x02000000, len(arm9_bin)))
    place(rom, 0x30, struct.pack("<IIII", 0x400, 0x037F8000, 0x037F8000, len(arm7_bin)))
    place(rom, 0x80, struct.pack("<II", len(rom), 0x200))  # used ROM size, header size
    place(rom, 0x15E, struct.pack("<H", crc16(rom[0:0x15E])))
    return bytes(rom)


ROMS = {
    "nes": nes,
    "gb": lambda: gb(False),
    "gbc": lambda: gb(True),
    "gba": gba,
    "snes": snes,
    "md": md,
    "sms": sms,
    "a26": a26,
    "psx": psx,
    "nds": nds,
}


def main(argv):
    check = "--check" in argv
    out_dir = os.path.normpath(OUT_DIR)
    stale = []
    if not check:
        os.makedirs(out_dir, exist_ok=True)
    for name, make in ROMS.items():
        data = make()
        path = os.path.join(out_dir, name + ".bin")
        if check:
            try:
                with open(path, "rb") as f:
                    if f.read() != data:
                        stale.append(name)
            except FileNotFoundError:
                stale.append(name)
        else:
            with open(path, "wb") as f:
                f.write(data)
            print("%-5s %6d bytes  %s" % (name, len(data), path))
    if check:
        if stale:
            print("stale test ROMs (re-run tools/testroms/make_test_roms.py): " + ", ".join(stale))
            return 1
        print("test ROMs up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
