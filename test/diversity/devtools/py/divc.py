"""
DEVELOPMENT TOOL. Minimal reader for .divc capture files.

Layout from src/diversity_capture.h: a 280-byte header, then per block a
208-byte struct divcap_block followed by arm 0 and arm 1, each nfft
complex64 samples (interleaved float I/Q), then a trailer. Only the fields
the scorers need are unpacked; the offsets were taken from offsetof() on
the C struct, not guessed.
"""
import struct

HDR, BLK = 280, 208
REC_MAGIC = 0x214B4C42          # "BLK!"

# struct divcap_block field offsets
OFF_OFFSET, OFF_SIDETONE, OFF_MODE = 32, 40, 48
OFF_FILTER_LOW, OFF_FILTER_HIGH, OFF_REF = 52, 56, 60
OFF_WEIGHTING, OFF_AUTO_MODE, OFF_TAU = 68, 100, 112

REFS = {0: 'band', 1: 'carrier', 2: 'rade', 3: 'digital', 4: 'cw'}


def open_divc(path):
    """Open a capture and return (file positioned at the first block, header dict)."""
    f = open(path, 'rb')
    h = f.read(HDR)
    ver, rate, nfft, block_bytes = struct.unpack_from('<IIII', h, 8)
    note = h[88:280].split(b'\0')[0].decode(errors='replace')
    return f, dict(version=ver, rate=rate, nfft=nfft, block_bytes=block_bytes, note=note)


def block_meta(m):
    """The recorded settings of one block record."""
    return dict(offset=struct.unpack_from('<q', m, OFF_OFFSET)[0],
                sidetone=struct.unpack_from('<i', m, OFF_SIDETONE)[0],
                mode=struct.unpack_from('<i', m, OFF_MODE)[0],
                filter_low=struct.unpack_from('<i', m, OFF_FILTER_LOW)[0],
                filter_high=struct.unpack_from('<i', m, OFF_FILTER_HIGH)[0],
                ref=struct.unpack_from('<i', m, OFF_REF)[0],
                weighting=struct.unpack_from('<i', m, OFF_WEIGHTING)[0],
                auto_mode=struct.unpack_from('<i', m, OFF_AUTO_MODE)[0],
                tau=struct.unpack_from('<d', m, OFF_TAU)[0])
