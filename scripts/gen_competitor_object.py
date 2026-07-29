#!/usr/bin/env python3
"""Generate a minimal valid Competitor .DAT object for headless multiplayer
testing (see TASKS.md backlog item / KNOWLEDGEBASE.md § Headless fixture).

The OpenGraphics release zip fetched by CMake (see the FetchContent_Declare
for `openloco_objects` in CMakeLists.txt) ships an *empty* Competitor/
directory (verified: the checked-out openloco_objects-src/Competitor folder
has zero files) -- there is no upstream source to build from, so this
hand-crafts the on-disk .DAT bytes directly using the format implemented in:
  - src/OpenLoco/include/OpenLoco/Objects/Object.h (ObjectHeader, 0x10 bytes:
    uint32 flags, char name[8], uint32 checksum)
  - src/OpenLoco/include/OpenLoco/Objects/CompetitorObject.h (fixed 0x38-byte
    struct)
  - src/OpenLoco/src/Objects/ObjectStringTable.cpp (string table: repeated
    [lang:u8][cstring\0] entries terminated by 0xFF)
  - src/OpenLoco/src/Objects/ObjectImageTable.cpp (image table: G1Header
    {numEntries:u32, totalSize:u32} + G1Element32[numEntries] + pixel data;
    numEntries=0 is a valid, fully-parsed empty table)
  - src/OpenLoco/src/S5/SawyerStream.cpp (chunk = [encoding:u8][length:u32]
    [payload]; encoding 0 = uncompressed)
  - src/OpenLoco/src/Objects/ObjectManager.cpp `computeObjectChecksum` /
    `computeChecksum` (rotl-based checksum over: header flags byte 0, header
    name[8], then the *decoded* chunk payload -- verified byte-for-byte
    against a real shipped object, OG_COAL.dat, before trusting this script)

CompetitorObject::load() only requires bit 0 of `emotions` to be set
(CompetitorObject::validate()) and copies the fixed struct + string table
entries in verbatim; the image table is parsed but never dereferenced
unless the object is actually drawn (headless runs never open a window --
see KNOWLEDGEBASE.md "Ui::render/Gfx::renderAndUpdate already null-check
_window"). So a zero-entry image table is legitimate and sufficient for
`ObjectManager::load()` / `CompetitorObject::load()` to succeed, which is
all `CompanyManager::selectNewCompetitor()` needs.

Regenerate with:
    python scripts/gen_competitor_object.py [output_dir] [count]

Default output dir assumes the standard Windows build layout:
    build/windows/Release/data/objects/Competitor/ (FIXTCP00.DAT ...)
"""
import struct
import sys
from pathlib import Path

OBJECT_TYPE_COMPETITOR = 32  # OpenLoco::ObjectType::competitor (see Object.h)
SOURCE_GAME_CUSTOM = 0  # OpenLoco::SourceGame::custom
OBJECT_CHECKSUM_MAGIC = 0xF369A75B  # ObjectManager.cpp: objectChecksumMagic
LANG_ENGLISH_UK = 0  # Localisation::LocoLanguageId::english_uk

# A company consumes its competitor object exclusively
# (CompanyManager::selectNewCompetitor skips competitors already in use), so
# multiplayer tests need one object per company that can exist: the host's,
# each joining player's, and any AI companies. Default output is a small set.
DEFAULT_COUNT = 8

# CompetitorObject fields (see CompetitorObject.h); intelligence/aggressiveness/
# competitiveness must each be in [1, 9] per CompetitorObject::validate().
INTELLIGENCE = 5
AGGRESSIVENESS = 5
COMPETITIVENESS = 5
EMOTIONS = 1 << 0  # validate() requires bit 0 set; no other emotion needed
# Bit 0 set on both so CompanyManager::createCompany()'s AI-naming branch
# (index 0 into a non-empty vector) can never divide-by-zero / index an empty
# vector if this object is ever selected for an AI company, even though the
# only path this fixture is meant to exercise (joining-player company
# creation) takes the isPlayer branch and never reads these fields.
AVAILABLE_NAME_PREFIXES = 1 << 0
AVAILABLE_PLAY_STYLES = 1 << 0


def rotl32(x: int, n: int) -> int:
    x &= 0xFFFFFFFF
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def compute_checksum(data: bytes, seed: int) -> int:
    checksum = seed
    for b in data:
        checksum = rotl32(checksum ^ b, 11)
    return checksum


def build_string_table_entry(lang: int, text: str) -> bytes:
    return bytes([lang]) + text.encode("ascii") + b"\x00"


def build_object_data(first_name: str, last_name: str) -> bytes:
    # CompetitorObject fixed struct, 0x38 bytes. firstName/lastName/images are
    # overwritten unconditionally by CompetitorObject::load(), so their
    # on-disk values are irrelevant -- zero them.
    struct_bytes = struct.pack(
        "<HHIII9IBBBB",
        0,  # firstName (StringId, overwritten on load)
        0,  # lastName (StringId, overwritten on load)
        AVAILABLE_NAME_PREFIXES,
        AVAILABLE_PLAY_STYLES,
        EMOTIONS,
        *([0] * 9),  # images[9] (overwritten on load)
        INTELLIGENCE,
        AGGRESSIVENESS,
        COMPETITIVENESS,
        0,  # var_37, unused elsewhere in the codebase
    )
    assert len(struct_bytes) == 0x38, len(struct_bytes)

    string_table = (
        build_string_table_entry(LANG_ENGLISH_UK, first_name)
        + b"\xff"
        + build_string_table_entry(LANG_ENGLISH_UK, last_name)
        + b"\xff"
    )

    # G1Header{numEntries=0, totalSize=0}: a fully valid, empty image table.
    image_table = struct.pack("<II", 0, 0)

    return struct_bytes + string_table + image_table


def build_dat_file(object_name: bytes, first_name: str, last_name: str) -> bytes:
    data = build_object_data(first_name, last_name)

    flags = (OBJECT_TYPE_COMPETITOR & 0x3F) | (SOURCE_GAME_CUSTOM << 6)
    assert len(object_name) == 8

    checksum = compute_checksum(bytes([flags & 0xFF]), OBJECT_CHECKSUM_MAGIC)
    checksum = compute_checksum(object_name, checksum)
    checksum = compute_checksum(data, checksum)

    header = struct.pack("<I8sI", flags, object_name, checksum)
    chunk = struct.pack("<BI", 0, len(data)) + data  # encoding=uncompressed
    return header + chunk


def main() -> None:
    default_dir = (
        Path(__file__).resolve().parent.parent
        / "build"
        / "windows"
        / "Release"
        / "data"
        / "objects"
        / "Competitor"
    )
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else default_dir
    count = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_COUNT
    out_dir.mkdir(parents=True, exist_ok=True)

    # Remove the old single-object layout so the index doesn't hold both
    legacy = out_dir / "FIXTCOMP.DAT"
    if legacy.exists():
        legacy.unlink()

    for i in range(count):
        name8 = f"FIXTCP{i:02d}".encode("ascii")
        out_path = out_dir / f"FIXTCP{i:02d}.DAT"
        out_path.write_bytes(build_dat_file(name8, "Fixture", f"Competitor {i}"))
        print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
