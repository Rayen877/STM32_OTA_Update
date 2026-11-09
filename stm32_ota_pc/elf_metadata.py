from dataclasses import dataclass
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from crc32 import crc32

MAGIC = 0x46574D44
METADATA_SIZE = 44
CRC_FIELD_OFFSET = 24

@dataclass
class FirmwareMetadata:
    format_version:int; header_size:int; hardware_id:int
    version_major:int; version_minor:int; version_patch:int
    image_size:int; image_crc32:int; build_id:str
    section_address:int; bin_offset:int

def read_metadata_from_elf(elf_path:Path, app_base:int):
    with elf_path.open("rb") as f:
        elf=ELFFile(f); sec=elf.get_section_by_name(".firmware_info")
        if sec is None: raise ValueError("ELF has no .firmware_info section.")
        raw=sec.data()
        if len(raw)<METADATA_SIZE: raise ValueError(".firmware_info is smaller than 44 bytes.")
        magic,fmt,hs,hw=struct.unpack_from("<IHHI",raw,0)
        if magic!=MAGIC: raise ValueError(f"Bad metadata magic 0x{magic:08X}.")
        major,minor,patch,_=struct.unpack_from("<HHHH",raw,12)
        size,image_crc=struct.unpack_from("<II",raw,20)
        build=raw[28:44].split(b"\0",1)[0].decode("ascii","replace")
        addr=sec["sh_addr"]
        if addr<app_base: raise ValueError("Metadata address is below application base.")
        return FirmwareMetadata(fmt,hs,hw,major,minor,patch,size,image_crc,build,addr,addr-app_base)

def patch_bin_metadata(image:bytes, meta:FirmwareMetadata, version=None, build_id=None):
    data=bytearray(image); start=meta.bin_offset
    if start<0 or start+METADATA_SIZE>len(data): raise ValueError("Metadata offset is outside BIN.")
    if struct.unpack_from("<I",data,start)[0]!=MAGIC: raise ValueError("BIN does not contain FWMD at metadata offset.")
    if version:
        parts=version.split(".")
        if len(parts)!=3 or not all(x.isdigit() for x in parts): raise ValueError("Version must be MAJOR.MINOR.PATCH.")
        vals=list(map(int,parts))
        if any(x>65535 for x in vals): raise ValueError("Version component too large.")
        struct.pack_into("<HHH",data,start+12,*vals)
    if build_id:
        b=build_id.encode("ascii")
        if len(b)>15: raise ValueError("build_id must be <=15 ASCII characters.")
        data[start+28:start+44]=b.ljust(16,b"\0")
    struct.pack_into("<I",data,start+20,len(data))
    struct.pack_into("<I",data,start+CRC_FIELD_OFFSET,0)
    struct.pack_into("<I",data,start+CRC_FIELD_OFFSET,crc32(bytes(data)))
    return bytes(data)
