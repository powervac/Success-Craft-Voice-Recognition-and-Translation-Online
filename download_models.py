"""Download the two models used by the app into <root>/hf (default: ./data/hf).

    python download_models.py            # large-v3 (recognition) + NLLB-200 1.3B (translation)
    python download_models.py --small    # use "medium" for recognition (less VRAM, faster, less accurate)

Models are downloaded automatically on first start as well; this script just lets you do it up front.
"""
import os
import sys

root = os.environ.get("DICTATION_ROOT") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
os.makedirs(os.path.join(root, "hf"), exist_ok=True)
os.environ.setdefault("HF_HOME", os.path.join(root, "hf"))
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

from faster_whisper.utils import download_model
from huggingface_hub import snapshot_download

whisper = "medium" if "--small" in sys.argv else "large-v3"
print("Downloading Whisper %s ..." % whisper)
print(download_model(whisper, cache_dir=os.path.join(root, "hf")))
print("Downloading NLLB-200 distilled 1.3B (CTranslate2, int8) ...")
print(snapshot_download("OpenNMT/nllb-200-distilled-1.3B-ct2-int8", cache_dir=os.path.join(root, "hf")))
print("Done. Models are in", os.path.join(root, "hf"))
