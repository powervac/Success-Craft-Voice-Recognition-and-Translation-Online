"""Downloads the models used by VoiceWindow into <this folder>/data (called by the installer).

    python install_models.py          # CPU: Whisper "small" (the model worker.py uses without an NVIDIA GPU)
    python install_models.py --gpu    # GPU: Whisper "large-v3"
"""
import os
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
hf = os.path.join(root, "hf")
os.makedirs(hf, exist_ok=True)
os.environ.setdefault("HF_HOME", hf)
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

from huggingface_hub import snapshot_download
from faster_whisper.utils import _MODELS

whisper = "large-v3" if "--gpu" in sys.argv else "small"
print("Downloading Whisper %s ..." % whisper, flush=True)
snapshot_download(_MODELS[whisper], cache_dir=hf,
                  allow_patterns=["config.json", "preprocessor_config.json", "model.bin", "tokenizer.json", "vocabulary.*"])
print("Downloading NLLB-200 distilled 1.3B (translation) ...", flush=True)
snapshot_download("OpenNMT/nllb-200-distilled-1.3B-ct2-int8", cache_dir=hf)

# small optional model for the "several interlocutors" mode; the app downloads it itself if this fails
print("Downloading the speaker model (26 MB) ...", flush=True)
try:
    import urllib.request
    spk_dir = os.path.join(root, "speaker")
    os.makedirs(spk_dir, exist_ok=True)
    spk = os.path.join(spk_dir, "wespeaker_en_voxceleb_resnet34_LM.onnx")
    if not os.path.isfile(spk):
        urllib.request.urlretrieve("https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
                                   "wespeaker_en_voxceleb_resnet34_LM.onnx", spk + ".part")
        os.replace(spk + ".part", spk)
except Exception as e:
    print("  skipped (%s) - the app will download it when that mode is switched on" % e, flush=True)

print("Models are ready.", flush=True)
