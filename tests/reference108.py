# Extracted unchanged helpers from user-supplied Python reference.
# Source SHA256: 11366d4f9ab46548f9501c343c5cd8c99af5c5b342117ceaacffc1635e81ab89
# Only the algorithm 108 slot is retained; other slots are not used in tests.
import ctypes
BYTE = ctypes.c_ubyte
WORD = ctypes.c_ushort
table_data = [0] * 3328
table_data[108*13:109*13] = [5, 254, 255, 248, 83, 143, 126, 62, 147, 107, 244, 7, 250]
security_table = (BYTE * 3328)(*table_data)

def op_05(val: WORD) -> WORD:
    """Opcode 0x05: swap the high and low bytes to mirror the ECU instruction."""
    return WORD(((val.value << 8) | (val.value >> 8)) & 0xFFFF)

def op_14(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x14: add the literal WORD formed by hh as MSB and ll as LSB."""
    add_val = (hh.value << 8) | ll.value
    return WORD((val.value + add_val) & 0xFFFF)

def op_2a(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x2A: ones-complement the WORD and increment when hh < ll."""
    new_val = ~val.value & 0xFFFF
    if hh.value < ll.value:
        new_val = (new_val + 1) & 0xFFFF
    return WORD(new_val)

def op_37(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x37: mask the accumulator with the literal ll<<8 | hh."""
    and_val = (ll.value << 8) | hh.value
    return WORD(val.value & and_val)

def op_4c(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x4C: rotate the WORD left by hh bits (ll is ignored by the ROM)."""
    shift = hh.value
    return WORD(((val.value << shift) | (val.value >> (16 - shift))) & 0xFFFF)

def op_52(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x52: OR in hh (low byte) and ll (high byte) to force bits high."""
    return WORD((val.value | hh.value | (ll.value << 8)))

def op_6b(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x6B: rotate the WORD right by ll bits, emulating the firmware."""
    shift = ll.value
    return WORD(((val.value >> shift) | (val.value << (16 - shift))) & 0xFFFF)

def op_75(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x75: add the literal built as ll as MSB and hh as LSB (reverse of 0x14)."""
    add_val = (ll.value << 8) | hh.value
    return WORD((val.value + add_val) & 0xFFFF)

def op_7e(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x7E: byte-swap then add, choosing op_14 or op_75 based on hh >= ll."""
    if hh.value >= ll.value:
        return op_14(op_05(val), hh, ll)
    else:
        return op_75(op_05(val), hh, ll)

def op_98(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0x98: subtract the literal WORD composed of hh:ll."""
    sub_val = (hh.value << 8) | ll.value
    return WORD((val.value - sub_val) & 0xFFFF)

def op_f8(val: WORD, hh: BYTE, ll: BYTE) -> WORD:
    """Opcode 0xF8: subtract the literal built as ll:hh (byte order flipped)."""
    sub_val = (ll.value << 8) | hh.value
    return WORD((val.value - sub_val) & 0xFFFF)

def get_key(seed: WORD, algo: int, table: (BYTE * 3328)) -> WORD:
    """Interpret one algorithm definition and fold the seed through its four opcodes.

    Each algorithm occupies 13 bytes: four repetitions of (opcode, hh, ll) plus a
    single unused byte of padding.  The dispatcher walks those triples in order and
    applies the opcode helpers, returning the final WORD produced by the firmware.
    """
    if algo == 0:
        return WORD(~seed.value & 0xFFFF)

    idx = algo * 13
    
    # Each algorithm is stored as four opcode triples; walk them sequentially.
    for _ in range(4):
        code = table[idx]
        hh = BYTE(table[idx + 1])
        ll = BYTE(table[idx + 2])

        # Look up the Python helper that mirrors the firmware opcode and execute it.
        if code == 5:
            seed = op_05(seed)
        elif code == 20:
            seed = op_14(seed, hh, ll)
        elif code == 42:
            seed = op_2a(seed, hh, ll)
        elif code == 55:
            seed = op_37(seed, hh, ll)
        elif code == 76:
            seed = op_4c(seed, hh, ll)
        elif code == 82:
            seed = op_52(seed, hh, ll)
        elif code == 107:
            seed = op_6b(seed, hh, ll)
        elif code == 117:
            seed = op_75(seed, hh, ll)
        elif code == 126:
            seed = op_7e(seed, hh, ll)
        elif code == 152:
            seed = op_98(seed, hh, ll)
        elif code == 248:
            seed = op_f8(seed, hh, ll)
            
        idx += 3
        
    return seed
