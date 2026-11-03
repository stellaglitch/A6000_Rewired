"""Check a bent ARW with LibRaw (through rawpy), independently of this project's own decoder.

    python verify_arw.py ORIGINAL.ARW BENT.ARW [OUT_PREVIEW.JPG]

Reports how many raw pixels differ between the two as LibRaw reads them, that
the embedded preview still decodes, and optionally saves LibRaw's own render of
the bent file - the proof that a stock raw developer opens what the camera wrote.
"""
import io
import sys

import numpy as np
import rawpy
from PIL import Image


def main():
    orig_path, bent_path = sys.argv[1], sys.argv[2]
    with rawpy.imread(orig_path) as a, rawpy.imread(bent_path) as b:
        ra = a.raw_image_visible.astype(np.int32)
        rb = b.raw_image_visible.astype(np.int32)
        diff = np.abs(ra - rb)
        print(f"LibRaw size {rb.shape}, black {b.black_level_per_channel}, white {b.white_level}")
        print(f"pixels changed: {np.count_nonzero(diff)} of {diff.size} "
              f"({100.0 * np.count_nonzero(diff) / diff.size:.2f}%), max change {diff.max()}")
        try:
            thumb = b.extract_thumb()
            img = Image.open(io.BytesIO(thumb.data))
            img.load()
            print(f"embedded preview: {img.size[0]}x{img.size[1]} JPEG, decodes")
        except Exception as e:  # noqa: BLE001 - report whatever LibRaw or PIL objects to
            print(f"embedded preview: FAILED ({e})")
            sys.exit(1)
        if len(sys.argv) > 3:
            rgb = b.postprocess(use_camera_wb=True, half_size=True)
            Image.fromarray(rgb).save(sys.argv[3], quality=90)
            print(f"LibRaw render saved to {sys.argv[3]}")


if __name__ == "__main__":
    main()
