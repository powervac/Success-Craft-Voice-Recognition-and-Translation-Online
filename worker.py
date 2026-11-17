"""Whisper + NLLB + speaker-identification worker for VoiceWindow.exe.

stdin frames  : 1 byte type + uint32 LE length + payload
   'A' = channel byte + int16 mono 16 kHz audio   (channel 0 = microphone, 1 = system audio)
   'E' = channel byte                              (end of an utterance on that channel)
   'L' = "src|tgt|reverse|diarize|sensitivity"     (languages, options)
   'R' = reset the known speakers
   'Q' = quit
stdout lines  : KIND \\t text \\n   for S (status), N (translation unavailable), V (speaker detection unavailable)
                KIND \\t channel \\t speaker \\t text \\n   for
                P/F = partial/final recognized text, T/U = partial/final translation
"""
import os, sys, glob, re, threading, struct, time

ROOT = os.environ.get('DICTATION_ROOT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
os.makedirs(os.path.join(ROOT, 'tmp'), exist_ok=True)
os.environ.setdefault('HF_HOME', os.path.join(ROOT, 'hf'))
os.environ.setdefault('TMP', os.path.join(ROOT, 'tmp'))
os.environ.setdefault('TEMP', os.path.join(ROOT, 'tmp'))
os.environ.setdefault('HF_HUB_DISABLE_SYMLINKS_WARNING', '1')

RATE = 16000
MODEL = os.environ.get('DICTATE_MODEL', 'large-v3')
NLLB_REPO = 'OpenNMT/nllb-200-distilled-1.3B-ct2-int8'
NLLB = {'ru': 'rus_Cyrl', 'en': 'eng_Latn', 'de': 'deu_Latn', 'fr': 'fra_Latn', 'es': 'spa_Latn',
        'uk': 'ukr_Cyrl', 'pl': 'pol_Latn', 'it': 'ita_Latn'}
SPK_FILE = 'wespeaker_en_voxceleb_resnet34_LM.onnx'
SPK_URL = ('https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/' + SPK_FILE)
MAX_SPEAKERS = 6
out = sys.stdout.buffer
inp = sys.stdin.buffer
out_lock = threading.Lock()


def send(*fields):
    line = '\t'.join(str(f).replace('\r', ' ').replace('\n', ' ').replace('\t', ' ') for f in fields)
    with out_lock:
        out.write((line + '\n').encode('utf-8'))
        out.flush()


HERE = os.path.dirname(os.path.abspath(__file__))


def has_nvidia():
    import shutil
    return bool(shutil.which('nvidia-smi')) or os.path.isfile(
        os.path.join(os.environ.get('SystemRoot', 'C:/Windows'), 'System32', 'nvidia-smi.exe'))


if not os.environ.get('DICTATE_MODEL'):
    MODEL = 'large-v3' if has_nvidia() else 'small'   # large-v3 is too slow for live use on a CPU


def ensure_deps():
    """Compare requirements.txt with what is installed and pip-install only what is missing."""
    import importlib.metadata as md
    import subprocess
    req = os.path.join(HERE, 'requirements.txt')
    if not os.path.isfile(req):
        return
    nvidia = has_nvidia()
    missing = []
    for line in open(req, encoding='utf-8'):
        spec = line.split('#')[0].strip()
        if not spec:
            continue
        name = re.split(r'[<>=!~\[ ;]', spec, maxsplit=1)[0]
        if name.lower().startswith('nvidia-') and not nvidia:
            continue                      # CUDA libraries are pointless without an NVIDIA GPU
        try:
            md.version(name)
        except md.PackageNotFoundError:
            missing.append(spec)
    if not missing:
        return
    send('S', 'Installing missing Python packages (%s)...' % ', '.join(re.split(r'[<>=!~\[ ;]', m, maxsplit=1)[0] for m in missing))
    cmd = [sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check', '--progress-bar', 'raw'] + missing
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                         errors='replace', creationflags=0x08000000)   # CREATE_NO_WINDOW
    cur, t0, last = '', time.time(), 0.0
    for l in p.stdout:
        l = l.strip()
        if not l:
            continue
        m = re.match(r'Progress (\d+) of (\d+)', l)
        if m:
            done, total = int(m.group(1)), int(m.group(2))
            if total and time.time() - last > 0.3:
                last = time.time()
                pct = min(100, done * 100 // total)
                send('D', pct, 'Installing packages - %s: %d%% (%.0f of %.0f MB)' % (cur or 'download', pct, done / 1e6, total / 1e6))
            continue
        sys.stderr.write(l + chr(10))
        sys.stderr.flush()
        m = re.match(r'Downloading (\S+)', l)
        if m:
            cur = m.group(1)[:48]
        elif l.startswith(('Installing collected', 'Collecting', 'Successfully')):
            send('S', 'pip: ' + l[:110])
    send('D', -1, '')
    if p.wait() != 0:
        send('S', 'Installing packages failed (see worker.log); trying to continue...')


try:
    ensure_deps()
except Exception as e:
    sys.stderr.write('dependency check failed: %r\n' % (e,))

site = os.path.join(sys.prefix, 'Lib', 'site-packages')
for d in glob.glob(os.path.join(site, 'nvidia', '*', 'bin')):
    os.add_dll_directory(d)
    os.environ['PATH'] = d + os.pathsep + os.environ['PATH']

send('S', 'Loading the recognition model %s...' % MODEL)
import numpy as np
from faster_whisper import WhisperModel

try:
    import dl_progress
    from faster_whisper.utils import _MODELS
    if MODEL in _MODELS:
        dl_progress.fetch(_MODELS[MODEL], os.path.join(ROOT, 'hf'), 'Recognition model %s' % MODEL, send,
                          allow_patterns=['config.json', 'preprocessor_config.json', 'model.bin', 'tokenizer.json', 'vocabulary.*'])
except Exception as e:
    sys.stderr.write('model pre-download failed: %r\n' % (e,))

try:
    model = WhisperModel(MODEL, device='cuda', compute_type='float16', download_root=os.path.join(ROOT, 'hf'))
    dev = 'GPU'
except Exception as e:
    sys.stderr.write('GPU failed: %r\n' % (e,))
    send('S', 'GPU unavailable, loading on the CPU...')
    model = WhisperModel(MODEL, device='cpu', compute_type='int8', download_root=os.path.join(ROOT, 'hf'))
    dev = 'CPU'

# ---- translation engine (NLLB-200 via CTranslate2), optional ----
translator = tok = None
try:
    send('S', 'Loading the NLLB translation model...')
    import ctranslate2
    from tokenizers import Tokenizer
    import dl_progress
    path = dl_progress.fetch(NLLB_REPO, os.path.join(ROOT, 'hf'), 'Translation model (NLLB)', send)
    tok = Tokenizer.from_file(os.path.join(path, 'tokenizer.json'))
    try:
        translator = ctranslate2.Translator(path, device='cuda' if dev == 'GPU' else 'cpu',
                                            compute_type='int8_float16' if dev == 'GPU' else 'int8')
    except Exception as e:
        sys.stderr.write('translator on GPU failed: %r\n' % (e,))
        translator = ctranslate2.Translator(path, device='cpu', compute_type='int8')
except Exception as e:
    sys.stderr.write('translation unavailable: %r\n' % (e,))
    translator = None
    send('N', 'Translation model unavailable: %s' % (e,))

# ---- speaker embedding model (sherpa-onnx + WeSpeaker), optional ----
extractor = None
try:
    import sherpa_onnx
    spk_path = os.path.join(ROOT, 'speaker', SPK_FILE)
    if not os.path.isfile(spk_path):
        import dl_progress
        send('S', 'Downloading the speaker model (one time only)...')
        dl_progress.download_file(SPK_URL, spk_path, 'Speaker model', send)
    extractor = sherpa_onnx.SpeakerEmbeddingExtractor(
        sherpa_onnx.SpeakerEmbeddingExtractorConfig(model=spk_path, num_threads=2, provider='cpu'))
except Exception as e:
    sys.stderr.write('speaker detection unavailable: %r\n' % (e,))
    extractor = None
    send('V', 'Speaker detection unavailable: %s' % (e,))

# ---- shared state ----
src_lang, tgt_lang, reverse, diarize, sens = 'ru', 'none', True, False, 60
quit_flag = False
cond = threading.Condition()
PARTIAL_STEP = int(RATE * 2 * (0.9 if dev == 'GPU' else 2.0))   # re-transcribe every ~0.9 s (GPU) / 2 s (CPU) of new audio (bytes)
cache = {}


class Chan:
    def __init__(self, idx):
        self.idx = idx
        self.buf = bytearray()
        self.last_len = 0
        self.final = False
        self.last_time = 0.0
        self.cents = []        # speaker centroids (sum of unit embeddings)
        self.last_spk = 0
        self.utt_lang = None   # language decided for the utterance in progress
        self.last_lang = None  # language of the previous finished utterance


chans = [Chan(0), Chan(1)]


def choose_lang(c, probs, s, t, prior, dur):
    """Pick one of the two languages from Whisper's language probabilities, without flapping.

    Forcing the wrong language makes Whisper *translate* the speech instead of transcribing it, so the choice is
    conservative: if neither language is clearly heard (noise, music) keep the previous one, and switch to the
    other language only when the evidence is strong."""
    ps, pt = probs.get(s, 0.0), probs.get(t, 0.0)
    tot = ps + pt
    cur = c.last_lang
    if tot < 0.25:                                   # not clearly either of the two languages
        return cur or prior
    q = {s: ps / tot, t: pt / tot}
    if cur is None:
        return max(q, key=q.get)
    other = t if cur == s else s
    if q[other] >= 0.85 and (dur >= 1.5 or q[other] >= 0.97):
        return other
    return cur


def plan(c, audio_f32, s, t, rev, final=False):
    """Decide (language to recognize, translation target) for an utterance.

    With a concrete language pair (s, t) every utterance is recognized in whichever of the two languages
    is actually spoken, and translated into the other one. Speech language "auto" or no translation falls
    back to the simple rules."""
    if s == 'auto' or t == 'none' or s == t:
        return (None if s == 'auto' else s), t
    prior = t if (c.idx == 1 and rev) else s
    dur = len(audio_f32) / RATE
    lang = None
    if dur >= (0.5 if final else 0.8):
        try:
            a = audio_f32
            peak = float(np.abs(a).max())
            if 0.003 < peak < 0.3:
                a = a * min(0.5 / peak, 12.0)
            probs = dict(model.detect_language(a)[2])
            lang = choose_lang(c, probs, s, t, prior, dur)
        except Exception as e:
            sys.stderr.write('language detection error: %r\n' % (e,))
    if lang is None:
        lang = c.last_lang or prior                  # too short to detect: keep the previous language
    return lang, (t if lang == s else s)


def transcribe(audio_f32, final, lang):
    if len(audio_f32) < RATE * 0.3:
        return '', None
    audio = audio_f32
    peak = float(np.abs(audio).max())
    if 0.003 < peak < 0.3:          # quiet source (far speaker, low-volume call): lift it, capped gain
        audio = audio * min(0.5 / peak, 12.0)
    segs, info = model.transcribe(
        audio,
        language=None if lang == 'auto' else lang,
        beam_size=5 if final else 1,
        best_of=1,
        temperature=0.0,
        vad_filter=True,
        condition_on_previous_text=False,
        without_timestamps=True,
    )
    # drop segments Whisper itself considers non-speech (it likes to invent "Thank you." on noise)
    short = len(audio_f32) < RATE * 2.5       # short clips are where Whisper hallucinates most
    nsp, lp = (0.45, -0.45) if short else (0.7, -0.7)
    text = ' '.join(sg.text.strip() for sg in segs if not (sg.no_speech_prob > nsp and sg.avg_logprob < lp)).strip()
    return text, info.language


def embed(audio_f32):
    st = extractor.create_stream()
    st.accept_waveform(RATE, audio_f32)
    st.input_finished()
    e = np.array(extractor.compute(st), dtype=np.float32)
    return e / (np.linalg.norm(e) + 1e-9)


def identify(c, audio_f32, final):
    """Speaker number (1-based) inside a channel; 0 when speaker detection is off.

    A new speaker is only created from a clearly long utterance (>= 2 s): short phrases such as "yes" or
    "perfect" carry too little voice information and are attached to the closest / previous speaker."""
    if not diarize or extractor is None:
        return 0
    dur = len(audio_f32) / RATE
    if dur < 1.0:                                   # too short for a reliable voiceprint
        return c.last_spk or 1
    e = embed(audio_f32)
    if not c.cents:
        if final:
            c.cents.append(e.copy())
            c.last_spk = 1
        return 1
    sims = [float(e @ (v / (np.linalg.norm(v) + 1e-9))) for v in c.cents]
    k = int(np.argmax(sims))
    thr = 0.55 + 0.30 * sens / 100.0
    if dur < 2.5:
        thr -= 0.06                                    # short phrases are noisier: lenient about "same voice"
    if sims[k] >= thr:
        spk = k + 1
        if final and dur >= 1.5:
            c.cents[k] = c.cents[k] + e                # running centroid
    elif dur >= 2.5 and len(c.cents) < MAX_SPEAKERS:
        spk = len(c.cents) + 1
        if final:
            c.cents.append(e.copy())
    else:
        spk = c.last_spk or (k + 1)
    if final:
        c.last_spk = spk
    return spk


def translate(text, s, t, final):
    """Sentence-by-sentence NLLB translation (whole-paragraph input makes NLLB drop sentences)."""
    if not translator or not text or t == 'none' or s == t or s not in NLLB or t not in NLLB:
        return text if (s == t and t != 'none') else ''
    sents = [x for x in re.split(r'(?<=[.!?…])\s+', text) if x.strip()]
    todo = [x for x in sents if (s, t, final, x) not in cache]
    if todo:
        batch = [[NLLB[s]] + tok.encode(x, add_special_tokens=False).tokens + ['</s>'] for x in todo]
        res = translator.translate_batch(batch, target_prefix=[[NLLB[t]]] * len(batch),
                                         beam_size=4 if final else 1, max_decoding_length=256,
                                         repetition_penalty=1.1, no_repeat_ngram_size=4)
        for x, r in zip(todo, res):
            ids = [tok.token_to_id(p) for p in r.hypotheses[0][1:]]
            cache[(s, t, final, x)] = tok.decode([i for i in ids if i is not None], skip_special_tokens=True).strip()
        if len(cache) > 2000:
            cache.clear()
    return ' '.join(cache[(s, t, final, x)] for x in sents)


def pick():
    """Choose the next job (caller holds cond): finals first, then the channel waiting longest for a partial."""
    for c in chans:
        if c.final:
            audio = bytes(c.buf)
            c.buf.clear()
            c.last_len = 0
            c.final = False
            return c, audio, True
    best = None
    for c in chans:
        if len(c.buf) > 0 and len(c.buf) - c.last_len >= PARTIAL_STEP:
            if best is None or c.last_time < best.last_time:
                best = c
    if best:
        best.last_len = len(best.buf)
        best.last_time = time.time()
        return best, bytes(best.buf), False
    return None


def worker():
    while True:
        with cond:
            job = pick()
            while job is None and not quit_flag:
                cond.wait(0.2)
                job = pick()
            if quit_flag:
                return
            c, raw, is_final = job
            s, t, rev = src_lang, tgt_lang, reverse
        audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
        text, det, spk = '', None, 0
        want, target = s, t
        try:
            want, target = plan(c, audio, s, t, rev, is_final)
            text, det = transcribe(audio, is_final, want)
            if text:
                spk = identify(c, audio, is_final)
        except Exception as e:
            sys.stderr.write('transcribe error: %r\n' % (e,))
        if is_final:
            if want in (s, t):
                c.last_lang = want
            c.utt_lang = None
        send('F' if is_final else 'P', c.idx, spk, want or det or '', text)
        try:
            tr = translate(text, want or det, target, is_final) if text else ''
        except Exception as e:
            sys.stderr.write('translate error: %r\n' % (e,))
            tr = ''
        send('U' if is_final else 'T', c.idx, spk, target if target != 'none' else '', tr)


threading.Thread(target=worker, daemon=True).start()
send('S', 'ready (%s)' % dev)


def read_exact(n):
    data = b''
    while len(data) < n:
        chunk = inp.read(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


try:
    while True:
        hdr = read_exact(5)
        t = chr(hdr[0])
        n = struct.unpack('<I', hdr[1:])[0]
        payload = read_exact(n) if n else b''
        if t == 'A' and len(payload) > 1 and payload[0] < 2:
            with cond:
                chans[payload[0]].buf.extend(payload[1:])
                cond.notify()
        elif t == 'E' and payload and payload[0] < 2:
            with cond:
                chans[payload[0]].final = True
                cond.notify()
        elif t == 'L':
            parts = payload.decode('utf-8', 'ignore').split('|')
            if len(parts) >= 5:
                with cond:
                    src_lang, tgt_lang = parts[0], parts[1]
                    reverse, diarize = parts[2] == '1', parts[3] == '1'
                    try:
                        sens = max(0, min(100, int(parts[4])))
                    except ValueError:
                        pass
        elif t == 'R':
            with cond:
                for c in chans:
                    c.cents.clear()
                    c.last_spk = 0
        elif t == 'Q':
            break
except EOFError:
    pass
quit_flag = True
os._exit(0)
