"""Download helpers that report progress as  D <percent> <text>  lines (shown by the GUI as a progress bar).

`send` is the worker's line writer: send(kind, *fields).
"""
import time


def fmt_eta(sec):
    sec = int(max(0, sec))
    if sec < 60:
        return '%d s' % sec
    if sec < 3600:
        return '%d min %02d s' % (sec // 60, sec % 60)
    return '%d h %02d min' % (sec // 3600, (sec % 3600) // 60)


class Progress:
    """Aggregates the byte counters of several files and reports percent, size and remaining time."""

    def __init__(self, label, send):
        self.label, self.send = label, send
        self.t0 = time.time()
        self.parts = {}
        self.last_t, self.last_pct = 0.0, -1

    def set(self, key, done, total):
        self.parts[key] = (done, total)
        self.report()

    def report(self, force=False):
        done = sum(d for d, _ in self.parts.values())
        total = sum(t for _, t in self.parts.values())
        if total <= 0:
            return
        pct = min(100, int(done * 100 / total))
        now = time.time()
        if not force and now - self.last_t < 0.25 and pct < 100:
            return
        self.last_t, self.last_pct = now, pct
        elapsed, eta = now - self.t0, ''
        if elapsed > 3 and done > 0:
            eta = ', about %s left' % fmt_eta(elapsed * (total - done) / done)
        self.send('D', pct, '%s: %d%% (%.0f of %.0f MB%s)' % (self.label, pct, done / 1e6, total / 1e6, eta))


def tqdm_class(label, send):
    """A tqdm replacement for huggingface_hub that feeds `Progress` instead of drawing a console bar."""
    from tqdm import tqdm
    prog = Progress(label, send)

    class P(tqdm):
        def __init__(self, *a, **kw):
            kw.pop('name', None)
            kw['disable'] = False
            super().__init__(*a, **kw)
            self._track()

        def _track(self):
            if getattr(self, 'unit', '') == 'B' and self.total:
                prog.set(id(self), self.n, self.total)

        def update(self, n=1):
            r = super().update(n)
            self._track()
            return r

        def display(self, *a, **kw):   # never print to the console
            pass

        def close(self):
            super().close()
            self._track()
            prog.report(True)          # make sure the final 100 % is reported

    return P


def fetch(repo, cache_dir, label, send, allow_patterns=None):
    """snapshot_download with a visible progress bar; instant when the files are already cached."""
    from huggingface_hub import snapshot_download
    try:
        return snapshot_download(repo, cache_dir=cache_dir, allow_patterns=allow_patterns, local_files_only=True)
    except Exception:
        pass
    send('S', 'Downloading %s (one time only)...' % label)
    path = snapshot_download(repo, cache_dir=cache_dir, allow_patterns=allow_patterns,
                             tqdm_class=tqdm_class(label, send))
    send('D', -1, '')
    return path


def download_file(url, dest, label, send):
    import os
    import urllib.request
    prog = Progress(label, send)

    def hook(blocks, block_size, total):
        if total > 0:
            prog.set('f', min(blocks * block_size, total), total)

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    urllib.request.urlretrieve(url, dest + '.part', hook)
    prog.report(True)
    os.replace(dest + '.part', dest)
    send('D', -1, '')
