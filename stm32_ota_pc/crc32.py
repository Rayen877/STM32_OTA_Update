def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xEDB88320) if (crc & 1) else (crc >> 1)
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF

def crc32_hex(data: bytes) -> str:
    return f"{crc32(data):08X}"

def crc32_with_zero_range(data: bytes, offset: int, length: int) -> int:
    tmp = bytearray(data)

    if offset < 0 or offset + length > len(tmp):
        raise ValueError("CRC zero range is outside image.")

    for i in range(offset, offset + length):
        tmp[i] = 0

    return crc32(bytes(tmp))