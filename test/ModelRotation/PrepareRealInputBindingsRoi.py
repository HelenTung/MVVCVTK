"""Rebuild or bitwise verify the fixed real CT ROI used by input binding tests."""

import argparse
import hashlib
from pathlib import Path

SOURCE_DIM = 1536
ROI_START = 704
ROI_DIM = 128
SOURCE_SHA256 = "1ea9320a021232c48feea8a26c2afe008262ca331a0889749d961abe20f8f358"
ROI_SHA256 = "8d170587f36e1eb29e4f21f888133437e70641a7c1424ce70199695caf71e677"


def get_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(16 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--roi", required=True, type=Path)
    args = parser.parse_args()

    source = args.source
    roi = args.roi
    source_bytes = SOURCE_DIM**3 * 4
    roi_bytes = ROI_DIM**3 * 4
    if source.stat().st_size != source_bytes or get_sha256(source) != SOURCE_SHA256:
        raise ValueError("source RAW size or SHA-256 differs from archived CT input")
    exists = roi.exists()
    if exists and roi.stat().st_size != roi_bytes:
        raise ValueError("existing ROI has the wrong byte length")
    if not exists:
        roi.parent.mkdir(parents=True, exist_ok=True)

    digest = hashlib.sha256()
    with source.open("rb") as original, roi.open("rb" if exists else "xb") as output:
        for z in range(ROI_START, ROI_START + ROI_DIM):
            for y in range(ROI_START, ROI_START + ROI_DIM):
                offset = ((z * SOURCE_DIM + y) * SOURCE_DIM + ROI_START) * 4
                original.seek(offset)
                row = original.read(ROI_DIM * 4)
                if len(row) != ROI_DIM * 4:
                    raise ValueError("source RAW ended inside ROI")
                digest.update(row)
                if exists:
                    if output.read(len(row)) != row:
                        raise ValueError("existing ROI differs from source voxel bits")
                else:
                    output.write(row)
        if exists and output.read(1):
            raise ValueError("existing ROI contains extra bytes")

    if digest.hexdigest() != ROI_SHA256 or get_sha256(roi) != ROI_SHA256:
        raise ValueError("ROI SHA-256 differs from fixed real-data fixture")
    print(f"source_sha256={SOURCE_SHA256} roi_sha256={ROI_SHA256} "
          f"start_xyz={ROI_START},{ROI_START},{ROI_START} "
          f"end_exclusive_xyz={ROI_START + ROI_DIM},{ROI_START + ROI_DIM},{ROI_START + ROI_DIM} "
          f"verified_voxels={ROI_DIM**3}")


if __name__ == "__main__":
    main()
