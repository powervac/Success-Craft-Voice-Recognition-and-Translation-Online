"""Whisper + NLLB + speaker-identification worker for VoiceWindow.exe.

stdin frames  : 1 byte type + uint32 LE length + payload
   'A' = channel byte + int16 mono 16 kHz audio   (channel 0 = microphone, 1 = system audio)
   'E' = channel byte                              (end of an utterance on that channel)
   'L' = "src|tgt|reverse|diarize|sensitivity"     (languages, options)
   'R' = reset the known speakers, 'M' = switch the speaker model (model id)
   'Q' = quit
stdout lines  : KIND \\t text \\n   for S (status), N (translation unavailable), V (speaker detection unavailable)
                C = speaker-model catalog entry, M = active speaker model
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
NLLB = {'ru': 'rus_Cyrl', 'en': 'eng_Latn', 'de': 'deu_Latn', 'fr': 'fra_Latn', 'es': 'spa_Latn',
        'uk': 'ukr_Cyrl', 'pl': 'pol_Latn', 'it': 'ita_Latn'}
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

model_ref = MODEL            # becomes a local folder once the model is on disk: loading then never touches the network
try:
    import dl_progress
    from faster_whisper.utils import _MODELS
    if MODEL in _MODELS:
        model_ref = dl_progress.fetch(_MODELS[MODEL], os.path.join(ROOT, 'hf'), 'Recognition model %s' % MODEL, send,
                          allow_patterns=['config.json', 'preprocessor_config.json', 'model.bin', 'tokenizer.json', 'vocabulary.*'])
except Exception as e:
    sys.stderr.write('model pre-download failed: %r\n' % (e,))

try:
    model = WhisperModel(model_ref, device='cuda', compute_type='int8_float16', download_root=os.path.join(ROOT, 'hf'))
    dev = 'GPU'
except Exception as e:
    sys.stderr.write('GPU failed: %r\n' % (e,))
    send('S', 'GPU unavailable, loading on the CPU...')
    model = WhisperModel(model_ref, device='cpu', compute_type='int8', download_root=os.path.join(ROOT, 'hf'))
    dev = 'CPU'

# ---- translation engines (CTranslate2), selectable from the UI ----
TR_MODELS = [   # id, label, repo, size in MB, kind
    ('nllb13', 'NLLB 1.3B - fast (default)', 'OpenNMT/nllb-200-distilled-1.3B-ct2-int8', 1400, 'nllb'),
    ('nllb33', 'NLLB 3.3B - better', 'OpenNMT/nllb-200-3.3B-ct2-int8', 3400, 'nllb'),
    ('madlad3b', 'MADLAD-400 3B - best, fluent text', 'Nextcloud-AI/madlad400-3b-mt-ct2-int8', 3000, 'madlad'),
]
translator = tok = None
tr_id, tr_kind = None, 'nllb'
tr_lock = threading.Lock()
tr_loading, tr_pending = False, None
import dl_progress


def tr_cached(repo):
    try:
        from huggingface_hub import snapshot_download
        snapshot_download(repo, cache_dir=os.path.join(ROOT, 'hf'), local_files_only=True)
        return True
    except Exception:
        return False


def send_tr_catalog():
    for i, (mid, label, repo, mb, kind) in enumerate(TR_MODELS):
        send('K', i, mid, label, mb, 1 if tr_cached(repo) else 0)


def load_translator(mid):
    """Download (with progress) and activate a translation model; falls back to the CPU if the GPU is full."""
    global translator, tok, tr_id, tr_kind, cache
    entry = next((m for m in TR_MODELS if m[0] == mid), None)
    if entry is None:
        return
    import ctranslate2
    from tokenizers import Tokenizer
    path = dl_progress.fetch(entry[2], os.path.join(ROOT, 'hf'), 'Translation model ' + entry[1].split(' -')[0], send)
    new_tok = Tokenizer.from_file(os.path.join(path, 'tokenizer.json'))
    translator = None                                    # free the old model before loading the new one
    import gc
    gc.collect()
    try:
        new_tr = ctranslate2.Translator(path, device='cuda' if dev == 'GPU' else 'cpu',
                                        compute_type='int8_float16' if dev == 'GPU' else 'int8')
    except Exception as e:
        sys.stderr.write('translator on GPU failed: %r' % (e,) + chr(10))
        send('S', 'Not enough GPU memory for this translation model - using the CPU (slower).')
        new_tr = ctranslate2.Translator(path, device='cpu', compute_type='int8')
    translator, tok, tr_id, tr_kind = new_tr, new_tok, mid, entry[4]
    cache = {}
    send('Z', mid)
    send_tr_catalog()


def switch_translator(mid):
    global tr_loading, tr_pending
    with tr_lock:
        if tr_loading:
            tr_pending = mid
            return
        tr_loading = True

    def run(m):
        global tr_loading, tr_pending
        while m:
            try:
                if m != tr_id:
                    load_translator(m)
            except Exception as e:
                sys.stderr.write('translation model error: %r' % (e,) + chr(10))
                send('D', -1, '')
                send('S', 'Could not load the translation model: %s' % (e,))
                send('Z', tr_id or '')
            with tr_lock:
                m, tr_pending = tr_pending, None
                if not m:
                    tr_loading = False

    threading.Thread(target=run, args=(mid,), daemon=True).start()


try:
    initial_tr = os.environ.get('TRANSLATION_MODEL') or 'nllb13'
    if not any(m[0] == initial_tr for m in TR_MODELS):
        initial_tr = 'nllb13'
    send('S', 'Loading the translation model...')
    load_translator(initial_tr)
except Exception as e:
    sys.stderr.write('translation unavailable: %r' % (e,) + chr(10))
    translator = None
    send('N', 'Translation model unavailable: %s' % (e,))
    send_tr_catalog()

# ---- speaker embedding models (sherpa-onnx), selectable from the UI ----
SPK_BASE = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/'
SPEAKER_MODELS = [   # id, label, file, size in MB
    ('resnet34', 'ResNet34 - fast, recommended', 'wespeaker_en_voxceleb_resnet34_LM.onnx', 25),
    ('campp', 'CAM++ - accurate and light', 'wespeaker_en_voxceleb_CAM++_LM.onnx', 27),
    ('resnet152', 'ResNet152 - experimental', 'wespeaker_en_voxceleb_resnet152_LM.onnx', 75),
    ('resnet293', 'ResNet293 - experimental, slow', 'wespeaker_en_voxceleb_resnet293_LM.onnx', 109),
    ('titanet', 'TitaNet Large (NVIDIA)', 'nemo_en_titanet_large.onnx', 96),
    ('campp_zhen', 'CAM++ Chinese + English', '3dspeaker_speech_campplus_sv_zh_en_16k-common_advanced.onnx', 26),
]
# the larger models score every pair of voices higher; their decision threshold is shifted accordingly
# Cosine-similarity scale differs between the models; this is the clustering threshold that separated two voices on
# real speech (movie dialogue) at the default sensitivity. The sensitivity slider moves it +-0.08 around that point.
SPK_BASE_T = {'resnet34': 0.80, 'campp': 0.80, 'resnet152': 0.88, 'resnet293': 0.88, 'titanet': 0.80, 'campp_zhen': 0.80}
extractor = None
spk_id = None
spk_lock = threading.Lock()
spk_loading, spk_pending = False, None
import dl_progress


def spk_file(mid):
    entry = next(m for m in SPEAKER_MODELS if m[0] == mid)
    return os.path.join(ROOT, 'speaker', entry[2])


def send_catalog():
    for i, (mid, label, fn, mb) in enumerate(SPEAKER_MODELS):
        send('C', i, mid, label, mb, 1 if os.path.isfile(spk_file(mid)) else 0)


def load_speaker(mid):
    """Download (with progress) and activate a speaker-embedding model; centroids of the old model are dropped."""
    global extractor, spk_id
    entry = next((m for m in SPEAKER_MODELS if m[0] == mid), None)
    if entry is None:
        return
    import sherpa_onnx
    path = spk_file(mid)
    if not os.path.isfile(path):
        send('S', 'Downloading the speaker model "%s" (%d MB, one time only)...' % (entry[1], entry[3]))
        dl_progress.download_file(SPK_BASE + entry[2], path, 'Speaker model ' + entry[1].split(' -')[0], send)
    new = sherpa_onnx.SpeakerEmbeddingExtractor(
        sherpa_onnx.SpeakerEmbeddingExtractorConfig(model=path, num_threads=2, provider='cpu'))
    with cond:
        extractor, spk_id = new, mid
        for c in chans:
            c.reset_speakers()
            c.last_spk = 0
    send('M', mid)
    send_catalog()


def switch_speaker(mid):
    """Switch the speaker model in the background (the audio keeps flowing meanwhile)."""
    global spk_loading, spk_pending
    with spk_lock:
        if spk_loading:
            spk_pending = mid
            return
        spk_loading = True

    def run(m):
        global spk_loading, spk_pending
        while m:
            try:
                if m != spk_id:
                    load_speaker(m)
            except Exception as e:
                sys.stderr.write('speaker model error: %r' % (e,) + chr(10))
                send('D', -1, '')
                send('S', 'Could not load the speaker model: %s' % (e,))
                send('M', spk_id or '')
            with spk_lock:
                m, spk_pending = spk_pending, None
                if not m:
                    spk_loading = False

    threading.Thread(target=run, args=(mid,), daemon=True).start()


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
        self.embs = []         # voiceprints of the finished pieces of speech (unit vectors)
        self.ids = []          # speaker number given to each stored voiceprint
        self.nxt = 1           # next free speaker number
        self.gcent = {}        # global speaker number -> sum of voiceprints (neural diarization path)
        self.gnext = 1
        self.last_spk = 0
        self.utt_lang = None   # language decided for the utterance in progress
        self.last_lang = None  # language of the previous finished utterance
        self.dbg = None        # (duration, similarities) of the last identification, for the diagnostic log


def _reset_speakers(self):
    self.embs, self.ids, self.nxt = [], [], 1
    self.gcent, self.gnext = {}, 1


Chan.reset_speakers = _reset_speakers
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


def split_sentences(sg):
    """Cut a Whisper segment into sentences (end mark or a pause > 0.8 s) using the word timestamps."""
    out, cur, st, last_end = [], [], None, 0.0
    for w in sg.words:
        if cur and w.start - last_end > 0.8:
            out.append((''.join(cur).strip(), st, last_end))
            cur, st = [], None
        if st is None:
            st = float(w.start)
        cur.append(w.word)
        last_end = float(w.end)
        if w.word.rstrip().endswith(('.', '?', '!', '…')):
            out.append((''.join(cur).strip(), st, last_end))
            cur, st = [], None
    if cur:
        out.append((''.join(cur).strip(), st, last_end))
    return [o for o in out if o[0]]


def transcribe(audio_f32, final, lang):
    """Returns ([(text, start_s, end_s), ...], detected_language). Timestamps let us split a long utterance
    by speaker."""
    if len(audio_f32) < RATE * 0.3:
        return [], [], None
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
        without_timestamps=False,
        word_timestamps=final,          # word times let us cut a segment into sentences (for speaker detection)
    )
    # drop segments Whisper itself considers non-speech (it likes to invent "Thank you." on noise)
    short = len(audio_f32) < RATE * 2.5       # short clips are where Whisper hallucinates most
    nsp, lp = (0.45, -0.45) if short else (0.7, -0.7)
    items, words = [], []
    for sg in segs:
        if sg.no_speech_prob > nsp and sg.avg_logprob < lp:
            continue
        if final and getattr(sg, 'words', None):
            words.extend((w.word, float(w.start), float(w.end)) for w in sg.words)
        if final and getattr(sg, 'words', None):
            items.extend(split_sentences(sg))
        else:
            txt = sg.text.strip()
            if txt:
                items.append((txt, float(sg.start), float(sg.end)))
    return items, words, info.language


def embed(audio_f32):
    st = extractor.create_stream()
    st.accept_waveform(RATE, audio_f32)
    st.input_finished()
    e = np.array(extractor.compute(st), dtype=np.float32)
    return e / (np.linalg.norm(e) + 1e-9)


def cluster(vecs, T):
    """Average-linkage agglomerative clustering of unit vectors: merge while the best average similarity >= T.
    Returns a cluster label per vector."""
    X = np.stack(vecs)
    S = X @ X.T
    np.fill_diagonal(S, -1.0)
    size = np.ones(len(X))
    members = {i: [i] for i in range(len(X))}
    while len(members) > 1:
        idx = list(members)
        sub = S[np.ix_(idx, idx)]
        a, b = np.unravel_index(int(np.argmax(sub)), sub.shape)
        if sub[a, b] < T:
            break
        ia, ib = idx[a], idx[b]
        sa, sb = size[ia], size[ib]
        S[ia, :] = (sa * S[ia, :] + sb * S[ib, :]) / (sa + sb)
        S[:, ia] = S[ia, :]
        S[ia, ia] = -1.0
        size[ia] = sa + sb
        members[ia] += members.pop(ib)
    lab = [0] * len(X)
    for k, mem in enumerate(members.values()):
        for i in mem:
            lab[i] = k
    return lab


def identify(c, audio_f32, final, prev=None):
    """Speaker number (1-based) of one piece of speech inside a channel; 0 when speaker detection is off.

    Every finished piece keeps its voiceprint; a new piece is clustered together with all of them (average
    linkage), so one odd short piece cannot drag a "voice centre" around. The piece takes the number most of its
    cluster mates already have, or a new number when it has no mates. Pieces under 0.8 s are too short to tell and
    keep the previous speaker."""
    if not diarize or extractor is None:
        return 0
    dur = len(audio_f32) / RATE
    fallback = prev or c.last_spk or 1
    if dur < 0.8:
        return fallback
    e = embed(audio_f32)
    T = SPK_BASE_T.get(spk_id, 0.80) + 0.002 * (sens - 60)
    lab = cluster(c.embs + [e], T)
    mine = lab[-1]
    votes = [c.ids[j] for j in range(len(c.ids)) if lab[j] == mine]
    new = False
    if votes:
        sid = max(set(votes), key=votes.count)
    elif c.nxt <= MAX_SPEAKERS and (dur >= 1.3 or not c.embs):   # a very short piece never creates a new voice
        sid, new = c.nxt, True
    else:                                           # no free number: closest existing voice
        sims = {}
        for j, v in enumerate(c.embs):
            sims.setdefault(c.ids[j], []).append(float(e @ v))
        sid = max(sims, key=lambda k: float(np.mean(sims[k]))) if sims else fallback
    c.dbg = (dur, T, sid, len(c.embs))
    if final:
        c.embs.append(e)
        c.ids.append(sid)
        if new:
            c.nxt += 1
        if len(c.embs) > 80:                        # keep the clustering cheap in very long sessions
            c.embs, c.ids = c.embs[-80:], c.ids[-80:]
    return sid


def span(audio, start, end):
    return audio[max(0, int((start - 0.1) * RATE)):int((end + 0.1) * RATE)]


SEG_URL = ('https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/'
           'sherpa-onnx-pyannote-segmentation-3-0.tar.bz2')
seg_path = os.path.join(ROOT, 'speaker', 'pyannote-segmentation-3-0.onnx')
diarizer, diar_for, seg_busy = None, None, False


def ensure_segmentation():
    """Download the speaker-turn model (6 MB) once, with a progress bar."""
    global seg_busy
    if os.path.isfile(seg_path) or seg_busy:
        return
    seg_busy = True
    try:
        send('S', 'Downloading the speaker-turn detector (6 MB, one time only)...')
        tgz = seg_path + '.tar.bz2'
        dl_progress.download_file(SEG_URL, tgz, 'Speaker-turn detector', send)
        import tarfile
        with tarfile.open(tgz) as tf:
            for m in tf.getmembers():
                if m.name.endswith('model.onnx'):
                    with open(seg_path, 'wb') as out_f:
                        out_f.write(tf.extractfile(m).read())
                    break
        os.remove(tgz)
        send('S', 'Speaker-turn detector ready.')
    except Exception as e:
        sys.stderr.write('segmentation model failed: %r' % (e,) + chr(10))
        send('S', 'Could not download the speaker-turn detector: %s' % (e,))
    finally:
        seg_busy = False


def get_diarizer():
    global diarizer, diar_for
    if diarizer is not None and diar_for == spk_id:
        return diarizer
    if extractor is None or not os.path.isfile(seg_path) or spk_id is None:
        return None
    import sherpa_onnx as so
    cfg = so.OfflineSpeakerDiarizationConfig(
        segmentation=so.OfflineSpeakerSegmentationModelConfig(
            pyannote=so.OfflineSpeakerSegmentationPyannoteModelConfig(model=seg_path), num_threads=2),
        embedding=so.SpeakerEmbeddingExtractorConfig(model=spk_file(spk_id), num_threads=2),
        clustering=so.FastClusteringConfig(num_clusters=-1, threshold=0.5),
        min_duration_on=0.2, min_duration_off=0.3)
    diarizer, diar_for = so.OfflineSpeakerDiarization(cfg), spk_id
    return diarizer


def unit(v):
    return v / (np.linalg.norm(v) + 1e-9)


def match_global(c, audio_f32):
    """Live preview: which known voice does this audio sound like (0 if nothing is known yet)."""
    if len(audio_f32) < RATE * 0.8 or not c.gcent:
        return c.last_spk or 1
    e = embed(audio_f32)
    best = max(c.gcent, key=lambda g: float(e @ unit(c.gcent[g])))
    return best if float(e @ unit(c.gcent[best])) >= SPK_BASE_T.get(spk_id, 0.80) - 0.08 else (c.last_spk or best)


def snap_runs(runs):
    """Word times are a little imprecise: move a turn boundary to the nearest sentence end (within 2-3 words)."""
    ends = ('.', '?', '!', '…')
    runs = [[g, t.split()] for g, t in runs]
    for i in range(len(runs) - 1):
        a, b = runs[i][1], runs[i + 1][1]
        tail = a[-3:]
        ks = [k for k, w in enumerate(tail[:-1]) if w.endswith(ends)]      # sentence end followed by 1-2 words
        if ks:
            cut = len(a) - len(tail) + ks[-1] + 1
            runs[i][1], runs[i + 1][1] = a[:cut], a[cut:] + b
            continue
        head = b[:3]
        ks = [k for k, w in enumerate(head[:-1]) if w.endswith(ends)]      # the previous speaker's last words
        if ks and not a[-1].endswith(ends):
            cut = ks[0] + 1
            runs[i][1], runs[i + 1][1] = a + b[:cut], b[cut:]
    return [(g, ' '.join(w)) for g, w in runs if w]


def runs_by_diarization(c, audio, items, words):
    """Split the utterance at speaker turns found by the neural segmentation and number the voices consistently."""
    sd = get_diarizer()
    if sd is None or len(audio) < RATE * 2.0:
        return None
    segs = [(r.start, r.end, r.speaker) for r in sd.process(audio).sort_by_start_time()]
    if not segs:
        return None
    Tm = SPK_BASE_T.get(spk_id, 0.80) - 0.05       # same voice in two different utterances
    # one voiceprint per local speaker of this utterance (all of its speech)
    embs = {}
    for ls in sorted(set(x[2] for x in segs)):
        parts = [audio[int(a * RATE):int(b * RATE)] for a, b, sp in segs if sp == ls]
        a = np.concatenate(parts)[:RATE * 12]
        if len(a) >= RATE * 0.8:
            embs[ls] = embed(a)
    cand = sorted(((float(e @ unit(cent)), ls, g) for ls, e in embs.items() for g, cent in c.gcent.items()), reverse=True)
    mapping, used = {}, set()
    for sim, ls, g in cand:                        # one global number per local speaker, best matches first
        if ls not in mapping and g not in used and sim >= Tm:
            mapping[ls] = g
            used.add(g)
    for ls in sorted(embs, key=lambda k: sum(b - a for a, b, sp in segs if sp == k), reverse=True):
        if ls not in mapping:
            if c.gnext <= MAX_SPEAKERS:
                mapping[ls] = c.gnext
                c.gnext += 1
            else:                                   # no free number: closest known voice
                mapping[ls] = max(c.gcent, key=lambda g: float(embs[ls] @ unit(c.gcent[g])))
            used.add(mapping[ls])
    for ls, g in mapping.items():                   # learn
        c.gcent[g] = c.gcent.get(g, 0) + embs[ls]
    fallback = c.last_spk or 1

    def speaker_at(t0, t1):
        mid = (t0 + t1) / 2
        best, dist = None, 1e9
        for a, b, sp in segs:
            d = 0.0 if a <= mid <= b else min(abs(mid - a), abs(mid - b))
            if d < dist:
                best, dist = sp, d
        return mapping.get(best, fallback)

    units = words if words else [(' ' + t, st, en) for t, st, en in items]
    runs = []
    for text, st, en in units:
        g = speaker_at(st, en)
        if runs and runs[-1][0] == g:
            runs[-1] = (g, runs[-1][1] + text)
        else:
            runs.append((g, text))
    runs = snap_runs([(g, t.strip()) for g, t in runs if t.strip()])
    if runs:
        c.last_spk = runs[-1][0]
        sys.stderr.write('DIAR ch%d %s neural: %d turns, voices %s, known %d' % (
            c.idx, spk_id, len(segs), sorted(set(g for g, _ in runs)), len(c.gcent)) + chr(10))
        sys.stderr.flush()
    return runs or None


def make_runs(c, audio, items, final, words=None):
    """Group the pieces of an utterance into runs of the same speaker -> [(speaker, text)]."""
    if not items:
        return []
    joined = ' '.join(t for t, _, _ in items)
    if not diarize or extractor is None:
        return [(0, joined)]
    if not final:                       # live preview: one line, speaker of the latest piece
        t, st, en = items[-1]
        a = span(audio, st, en)
        return [(match_global(c, a) if c.gcent else identify(c, a, False, c.last_spk), joined)]
    try:
        runs = runs_by_diarization(c, audio, items, words)
        if runs:
            return runs
    except Exception as e:
        sys.stderr.write('neural diarization failed: %r' % (e,) + chr(10))
    runs, prev = [], None               # fallback: voiceprints of the sentences
    for n, (t, st, en) in enumerate(items):
        c.dbg = None
        spk = identify(c, span(audio, st, en), True, prev)
        prev = spk
        if c.dbg:   # diagnostic log (worker.log)
            sys.stderr.write('DIAR ch%d %s T=%.2f piece %d dur=%.1fs known=%d -> speaker %d | %s' % (
                c.idx, spk_id, c.dbg[1], n, c.dbg[0], c.dbg[3], spk, t[:40]) + chr(10))
            sys.stderr.flush()
        if runs and runs[-1][0] == spk:
            runs[-1] = (spk, runs[-1][1] + ' ' + t)
        else:
            runs.append((spk, t))
    c.last_spk = prev
    return runs


def translate(text, s, t, final):
    """Sentence-by-sentence translation (whole paragraphs make the models drop sentences)."""
    tr, tk, kind, mid = translator, tok, tr_kind, tr_id        # a model switch may happen meanwhile
    if not tr or not text or t == 'none' or s == t or s not in NLLB or t not in NLLB:
        return text if (s == t and t != 'none') else ''
    sents = [x for x in re.split(r'(?<=[.!?\u2026])\s+', text) if x.strip()]
    todo = [x for x in sents if (mid, s, t, final, x) not in cache]
    if todo:
        beam = 4 if final else 1
        if kind == 'madlad':
            batch = []
            for x in todo:
                tokens = tk.encode('<2%s> %s' % (t, x)).tokens
                batch.append(tokens if tokens[-1] == '</s>' else tokens + ['</s>'])
            res = tr.translate_batch(batch, beam_size=beam, max_decoding_length=256)
            outs = [r.hypotheses[0] for r in res]
        else:
            batch = [[NLLB[s]] + tk.encode(x, add_special_tokens=False).tokens + ['</s>'] for x in todo]
            res = tr.translate_batch(batch, target_prefix=[[NLLB[t]]] * len(batch), beam_size=beam,
                                     max_decoding_length=256, repetition_penalty=1.1, no_repeat_ngram_size=4)
            outs = [r.hypotheses[0][1:] for r in res]
        for x, toks in zip(todo, outs):
            ids = [tk.token_to_id(p) for p in toks]
            cache[(mid, s, t, final, x)] = tk.decode([i for i in ids if i is not None], skip_special_tokens=True).strip()
        if len(cache) > 2000:
            cache.clear()
    return ' '.join(cache[(mid, s, t, final, x)] for x in sents)


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
        det, runs = None, []
        want, target = s, t
        try:
            want, target = plan(c, audio, s, t, rev, is_final)
            items, words, det = transcribe(audio, is_final, want)
            runs = make_runs(c, audio, items, is_final, words)
        except Exception as e:
            sys.stderr.write('transcribe error: %r\n' % (e,))
        if is_final:
            if want in (s, t):
                c.last_lang = want
            c.utt_lang = None
        if not runs:
            runs = [(0, '')]
        lang_out = want or det or ''
        for spk, text in runs:
            send('F' if is_final else 'P', c.idx, spk, lang_out, text)
            try:
                tr = translate(text, want or det, target, is_final) if text else ''
            except Exception as e:
                sys.stderr.write('translate error: %r\n' % (e,))
                tr = ''
            send('U' if is_final else 'T', c.idx, spk, target if target != 'none' else '', tr)


try:
    initial = os.environ.get('SPEAKER_MODEL') or 'resnet34'
    if not any(m[0] == initial for m in SPEAKER_MODELS):
        initial = 'resnet34'
    load_speaker(initial)
except Exception as e:
    sys.stderr.write('speaker detection unavailable: %r' % (e,) + chr(10))
    extractor = None
    send('V', 'Speaker detection unavailable: %s' % (e,))
    send_catalog()

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
                if diarize and not os.path.isfile(seg_path):
                    threading.Thread(target=ensure_segmentation, daemon=True).start()
                    try:
                        sens = max(0, min(100, int(parts[4])))
                    except ValueError:
                        pass
        elif t == 'M':
            switch_speaker(payload.decode('utf-8', 'ignore'))
        elif t == 'G':
            switch_translator(payload.decode('utf-8', 'ignore'))
        elif t == 'R':
            with cond:
                for c in chans:
                    c.reset_speakers()
                    c.last_spk = 0
        elif t == 'Q':
            break
except EOFError:
    pass
quit_flag = True
os._exit(0)
