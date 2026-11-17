# Success Craft Voice Recognition and Translation Online

A small native Windows app that listens to a microphone, **recognizes speech on the fly** and **translates it into another language as text** – fully offline, on your own machine.

Pick a microphone, choose the spoken language and the target language (for example *English → Russian* or *Russian → English*), press **Start**, and watch two live panels fill in: the recognized speech on top and its translation below.

```
+--------------------------------------------------------------+
| Microphone:  [ System default                              v]|
| Speech language: [English v] [<->] Translate to: [Russian  v]|
| [Start] [Clear] [Copy text] [Copy translation] [Save...] |||||| level
| Gain:  ----o--------  +0 dB          [x] Auto gain (AGC)     |
| [x] Noise suppression  ---o------  50%   [ ] Boost +12 dB    |
| Status line                                                  |
| Recognized speech                                            |
| +----------------------------------------------------------+ |
| | live recognized text ...                                 | |
| +----------------------------------------------------------+ |
| Translation                                                  |
| +----------------------------------------------------------+ |
| | live translated text ...                                 | |
| +----------------------------------------------------------+ |
+--------------------------------------------------------------+
```

## Features

- **Live speech recognition** with [Whisper large-v3](https://github.com/openai/whisper) running through [faster-whisper](https://github.com/SYSTRAN/faster-whisper) (CTranslate2). Partial text is refreshed about once per second while you speak; each phrase is finalized with a more accurate pass after a pause.
- **Live text translation** with [NLLB-200 (distilled 1.3B)](https://huggingface.co/facebook/nllb-200-distilled-1.3B), also on CTranslate2 (int8). Translation is text only – there is no speech synthesis.
- **Languages:** Russian, English, German, French, Spanish, Ukrainian, Polish, Italian. The speech language can also be set to *Auto*; the translation can be switched off. The **swap** button reverses the direction in one click.
- **Conversation mode:** tick *Capture the other side (system audio)* and the app listens to two sources at once - your microphone (**You**) and whatever your PC plays, e.g. a call in Zoom, Teams or Discord (**Them**, captured with WASAPI loopback). Every line goes on its own row with a coloured speaker label, in both panels. Each utterance is recognized in whichever of the two chosen languages is actually spoken and translated into the other one.
- **One or several interlocutors:** *One interlocutor* labels everything from the other side as "Them". *Several (detect voices)* tells voices apart with a speaker-embedding model (WeSpeaker via sherpa-onnx) and labels them "Them", "Them 2", "Them 3"...; the *sensitivity* slider controls how readily a new voice is created.
- **Echo filter:** with loudspeakers your microphone also hears the other side. *Speakers mode (echo filter)* mutes the microphone while the other side is audible, so nothing is duplicated; turn it off when you use headphones.
- **Microphone selection** from the list of Windows input devices.
- **Sound controls**, applied before recognition:
  - gain slider, from -10 dB to +30 dB;
  - automatic gain control (AGC) that levels quiet and loud speakers;
  - a one-click +12 dB boost for very quiet sources;
  - noise suppression (high-pass filter + adaptive noise gate) with an adjustable strength.
- **Robust speech detection:** an adaptive energy detector with hysteresis; its noise floor cannot be dragged upwards by quiet speech, so the app does not "go deaf" during long sessions. The status line shows whether speech is currently detected.
- **Always on top:** the window stays above all other windows and hides only when you minimize it.
- **Save to disk:** writes both versions of the document, for example `Transcript_2026-11-17_17-11_en.txt` and `Transcript_2026-11-17_17-11_ru.txt` (UTF-8). **Copy** buttons place either panel on the clipboard.
- All settings (microphone, languages, sliders, checkboxes) are remembered between runs.
- 100% local: no cloud services, no API keys, no audio leaves your computer.

## How it works

```
 microphone --(WinMM, 16 kHz mono)--> VoiceWindow.exe (C++ / Win32)
        gain / AGC / noise suppression, speech detection (VAD)
                         |   audio frames over a pipe
                         v
                    worker.py (Python)
        faster-whisper  -->  recognized text  --> NLLB-200 --> translation
                         |   text lines over a pipe
                         v
                  two live text panels
```

- `main.cpp` – the whole GUI and audio front end (Win32 API + WinMM, no third-party C++ libraries).
- `worker.py` – hidden helper process started by the app. It loads both models once, receives audio, and streams back partial/final text and translations. Translation is done sentence by sentence (NLLB tends to drop sentences when fed a whole paragraph) with a small cache, so earlier sentences stay stable while the current one grows.

## Requirements

| Item | Requirement |
|------|-------------|
| OS | Windows 10 / 11 (64-bit) |
| GPU | NVIDIA GPU with CUDA 12 support is strongly recommended (about 5 GB of free VRAM for large-v3 + NLLB). Without it the app falls back to the CPU – use the `medium` model for acceptable speed |
| Python | 3.10 or newer, available as `python` in PATH (used only for setup and by the worker) |
| C++ compiler | `g++` with C++17 from [MinGW-w64](https://www.mingw-w64.org/) or [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) (for example `winget install MartinStorsjo.LLVM-MinGW.UCRT`) |
| Disk | about 5 GB for the Python environment and models |
| Internet | only for the one-time setup (packages and models) |

## Quick start

```powershell
git clone https://github.com/powervac/Success-Craft-Voice-Recognition-and-Translation-Online.git
cd Success-Craft-Voice-Recognition-and-Translation-Online

.\setup.ps1          # creates .\venv, installs dependencies, downloads the models (~4.5 GB)
.\build.bat          # compiles VoiceWindow.exe
.\VoiceWindow.exe    # first start takes 15-30 s while the models load
```

Use `.\setup.ps1 -Small` to download the lighter `medium` Whisper model instead of `large-v3` (and set `DICTATE_MODEL=medium`, see below).

When the status line says the model is loaded, choose a microphone and the languages, then press **Start**.

## Dependencies

All Python dependencies are listed in [`requirements.txt`](requirements.txt); a fully pinned, known-good set is in [`requirements-lock.txt`](requirements-lock.txt):

- `faster-whisper`, `ctranslate2` – inference engines
- `tokenizers`, `huggingface_hub` – NLLB tokenizer and model download
- `numpy`
- `sherpa-onnx` - speaker embeddings for the "several interlocutors" mode
- `nvidia-cublas-cu12`, `nvidia-cudnn-cu12` – CUDA 12 runtime libraries for GPU inference (installed from PyPI, no separate CUDA toolkit needed)

Models (downloaded by `download_models.py` or on first start into `data/hf`):

| Purpose | Model | Size |
|---------|-------|------|
| Speech recognition | [`Systran/faster-whisper-large-v3`](https://huggingface.co/Systran/faster-whisper-large-v3) | ~3 GB |
| Translation | [`OpenNMT/nllb-200-distilled-1.3B-ct2-int8`](https://huggingface.co/OpenNMT/nllb-200-distilled-1.3B-ct2-int8) | ~1.4 GB |
| Speaker detection | WeSpeaker ResNet34 (`wespeaker_en_voxceleb_resnet34_LM.onnx`, from the sherpa-onnx releases) | 26 MB |

## Configuration

Settings are stored next to the executable in `settings.ini` (created automatically; UTF-16). Besides the values the app writes itself, you can add a `[paths]` section:

```ini
[paths]
python=D:\tools\venv\Scripts\python.exe   ; interpreter that has the dependencies installed
root=D:\voice-data                         ; where models (hf\) and temp files live
```

Defaults: `venv\Scripts\python.exe` next to the executable and the `data\` folder next to the executable.

Environment variables read by the worker:

| Variable | Meaning | Default |
|----------|---------|---------|
| `DICTATE_MODEL` | Whisper model name (`large-v3`, `medium`, `small`, ...) | `large-v3` |
| `DICTATION_ROOT` | same as `[paths] root` | `data` |

## Usage tips

- **Interlocutor is quiet** (call on speakers, loopback device): enable *Auto gain* and, if needed, *Boost +12 dB*.
- **Background noise** (fan, keyboard): enable *Noise suppression* and raise its strength until the noise stops producing text; very high values can clip the beginnings of words.
- **Direction swap:** the button works when both languages are concrete (not *Auto* and not *No translation*).
- **Long monologues** are split automatically every 20 seconds so that latency stays low.

- **Loopback hears everything your PC plays.** Music or a video playing next to the call ends up in the "Them" side too - pause it, or use the call app's own output device.

## Troubleshooting

| Symptom | What to check |
|---------|---------------|
| Status says Python cannot be started | Run `.\setup.ps1`, or set `[paths] python=` in `settings.ini` |
| Window opens but the model never loads | Read `worker.log` next to the executable; check that the models were downloaded and that there is enough free VRAM |
| "GPU unavailable" in the status line | The worker fell back to the CPU; install a recent NVIDIA driver, or use `DICTATE_MODEL=medium` |
| No text while speaking | Watch the level meter and the status line ("Listening" vs "Hearing speech"); raise gain or enable AGC; check the selected microphone |
| Translation panel stays empty | Make sure the target language is not *No translation* and differs from the speech language |

## Project layout

| File | Purpose |
|------|---------|
| `main.cpp` | Win32 GUI, audio capture, DSP, speech detection |
| `worker.py` | Whisper + NLLB worker process |
| `download_models.py` | optional up-front model download |
| `setup.ps1` | one-time environment setup |
| `build.bat` | compiles `VoiceWindow.exe` |
| `requirements.txt`, `requirements-lock.txt` | Python dependencies |

## Licenses and credits

The code in this repository is released under the [MIT License](LICENSE).

Third-party components keep their own licenses: Whisper (MIT), faster-whisper (MIT), CTranslate2 (MIT). **The NLLB-200 translation model is distributed under the CC-BY-NC 4.0 license, i.e. for non-commercial use only** – check the model card before using the app commercially, or swap in another CTranslate2-compatible translation model.
