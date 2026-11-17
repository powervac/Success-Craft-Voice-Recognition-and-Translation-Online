"""Whisper + NLLB worker for VoiceWindow.exe.
stdin frames : 1 byte type + uint32 LE length + payload
   'A' = int16 mono 16 kHz audio, 'E' = end of utterance,
   'L' = "src|tgt" (language codes, src may be "auto", tgt may be "none"), 'Q' = quit
stdout lines : KIND \\t text \\n
   S = status, P/F = partial/final recognized text, T/U = partial/final translation,
   N = translation engine unavailable (message)
"""
import os, sys, glob, re, threading, struct

ROOT = os.environ.get('DICTATION_ROOT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
os.makedirs(os.path.join(ROOT, 'tmp'), exist_ok=True)
os.environ.setdefault('HF_HOME', os.path.join(ROOT, 'hf'))
os.environ.setdefault('TMP', os.path.join(ROOT, 'tmp'))
os.environ.setdefault('TEMP', os.path.join(ROOT, 'tmp'))
os.environ.setdefault('HF_HUB_DISABLE_SYMLINKS_WARNING', '1')

site = os.path.join(sys.prefix, 'Lib', 'site-packages')
for d in glob.glob(os.path.join(site, 'nvidia', '*', 'bin')):
    os.add_dll_directory(d)
    os.environ['PATH'] = d + os.pathsep + os.environ['PATH']

RATE = 16000
MODEL = os.environ.get('DICTATE_MODEL', 'large-v3')
NLLB_REPO = 'OpenNMT/nllb-200-distilled-1.3B-ct2-int8'
NLLB = {'ru': 'rus_Cyrl', 'en': 'eng_Latn', 'de': 'deu_Latn', 'fr': 'fra_Latn', 'es': 'spa_Latn',
        'uk': 'ukr_Cyrl', 'pl': 'pol_Latn', 'it': 'ita_Latn'}
out = sys.stdout.buffer
inp = sys.stdin.buffer
out_lock = threading.Lock()


def send(kind, text=''):
    text = text.replace('\r', ' ').replace('\n', ' ').replace('\t', ' ')
    with out_lock:
        out.write((kind + '\t' + text + '\n').encode('utf-8'))
        out.flush()


send('S', 'Loading the recognition model %s...' % MODEL)
import numpy as np
from faster_whisper import WhisperModel

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
    from huggingface_hub import snapshot_download
    path = snapshot_download(NLLB_REPO, cache_dir=os.path.join(ROOT, 'hf'))
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

src_lang, tgt_lang = 'ru', 'none'
buf = bytearray()
last_len = 0
final_pending = False
quit_flag = False
cond = threading.Condition()
PARTIAL_STEP = int(RATE * 2 * 0.9)   # re-transcribe every ~0.9 s of new audio (bytes)
cache = {}


def transcribe(audio_bytes, final):
    audio = np.frombuffer(bytes(audio_bytes), dtype=np.int16).astype(np.float32) / 32768.0
    if len(audio) < RATE * 0.3:
        return '', None
    peak = float(np.abs(audio).max())
    if 0.003 < peak < 0.3:          # quiet source (far speaker, low-volume call): lift it, capped gain
        audio = audio * min(0.5 / peak, 12.0)
    segs, info = model.transcribe(
        audio,
        language=None if src_lang == 'auto' else src_lang,
        beam_size=5 if final else 1,
        best_of=1,
        temperature=0.0,
        vad_filter=True,
        condition_on_previous_text=False,
        without_timestamps=True,
    )
    return ' '.join(s.text.strip() for s in segs).strip(), info.language


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


def worker():
    global last_len, final_pending
    while True:
        with cond:
            while not quit_flag and not final_pending and not (len(buf) > 0 and len(buf) - last_len >= PARTIAL_STEP):
                cond.wait(0.2)
            if quit_flag:
                return
            audio = bytes(buf)
            s, t = src_lang, tgt_lang
            if final_pending:
                is_final = True
                buf.clear()
                last_len = 0
                final_pending = False
            else:
                is_final = False
                last_len = len(buf)
        text, lang = '', None
        try:
            text, lang = transcribe(audio, is_final)
        except Exception as e:
            sys.stderr.write('transcribe error: %r\n' % (e,))
        send('F' if is_final else 'P', text)
        try:
            tr = translate(text, lang if s == 'auto' else s, t, is_final) if text else ''
        except Exception as e:
            sys.stderr.write('translate error: %r\n' % (e,))
            tr = ''
        send('U' if is_final else 'T', tr)


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
        if t == 'A':
            with cond:
                buf.extend(payload)
                cond.notify()
        elif t == 'E':
            with cond:
                final_pending = True
                cond.notify()
        elif t == 'L':
            parts = payload.decode('utf-8', 'ignore').split('|')
            if len(parts) == 2:
                src_lang, tgt_lang = parts
        elif t == 'Q':
            break
except EOFError:
    pass
quit_flag = True
os._exit(0)
