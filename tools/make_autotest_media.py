#!/usr/bin/env python3
"""Makes the sample music files Windows cannot write itself, for s2mautotest. Run by app/build.sh on every build (a
new set each time: the melody's notes are shuffled), not kept in git. They are built into the autotest as resources
(autotest_media.rc, manifest.txt) and written out to the test's temporary folder at its start, then removed with it.

The autotest's melody (as atmusic.cpp makes it: notes over a quiet pad, never silent) as OGG Vorbis (stereo, mono
22 kHz) and as MP3 / WAV / FLAC variants the player must read: mono 22 kHz MP3, VBR MP3 with ID3 tags, MP3 with cover
art, 8-bit / 24-bit / 32-bit float WAV, 24-bit 96 kHz FLAC.

  python3 tools/make_autotest_media.py      (needs ffmpeg with libvorbis and libmp3lame, and PIL)
Manifest lines: music|file|-
"""
import math, os, random, struct, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'app', 'autotest-media')


def run(args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors='replace')[-2000:])
        raise SystemExit('failed: ' + ' '.join(args))


def melody_wav(path, notes, rate=44100, seconds=8.0):
    """Stereo 16-bit: notes of 0.25 s with soft edges over a quiet pad, a short fade at the ends."""
    n = int(seconds * rate)
    note_len = rate // 4
    tau = 2 * math.pi
    data = bytearray()
    for i in range(n):
        t = i / rate
        k = i % note_len
        env = k / 400 if k < 400 else ((note_len - k) / 400 if k > note_len - 400 else 1.0)
        v = 0.30 * env * math.sin(tau * notes[(i // note_len) % len(notes)] * t) + 0.10 * math.sin(tau * 130.81 * t) + \
            0.06 * math.sin(tau * 196.0 * t)
        edge = t / 0.05 if t < 0.05 else ((seconds - t) / 0.05 if seconds - t < 0.05 else 1.0)
        s = int(v * edge * 32767)
        data += struct.pack('<hh', s, int(s * 0.8))
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def make_broken(out, src_mp3, src_flac, manifest):
    """Broken music files (a new random choice and cut on every build) the player must skip or end early, next to a
    good one: empty, text named as music, random bytes, a header followed by junk, a WAV whose header claims far more
    data than there is, files cut short (MP3 / FLAC: the start plays, then the next file)."""
    rnd = random.Random()
    mp3 = open(src_mp3, 'rb').read()
    flac = open(src_flac, 'rb').read()

    def junk(n):
        return bytes(rnd.getrandbits(8) for _ in range(n))

    def wav_lying():
        frames = 22050
        data = b''.join(struct.pack('<hh', int(8000 * math.sin(i / 7.0)), 0) for i in range(frames))
        claimed = len(data) * rnd.randint(5, 50)
        return (b'RIFF' + struct.pack('<I', 36 + claimed) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16) +
                b'data' + struct.pack('<I', claimed) + data)

    files = [
        ('broken-empty.mp3', lambda: b''),
        ('broken-text.wav', lambda: b'not music, just text\r\n' * 40),
        ('broken-random.ogg', lambda: junk(rnd.randint(2000, 20000))),
        ('broken-ogg-header-junk.ogg', lambda: b'OggS' + junk(rnd.randint(2000, 20000))),
        ('broken-flac-header-junk.flac', lambda: b'fLaC' + junk(rnd.randint(2000, 20000))),
        ('broken-id3-only.mp3', lambda: b'ID3\x03\x00\x00\x00\x00\x00\x0a' + b'\x00' * 10),
        ('broken-wav-lying-size.wav', wav_lying),
        ('broken-cut-%d.mp3' % rnd.randint(10, 60), lambda: mp3[:len(mp3) * int(rnd.randint(10, 60)) // 100]),
        ('broken-cut-%d.flac' % rnd.randint(10, 60), lambda: flac[:len(flac) * int(rnd.randint(10, 60)) // 100]),
    ]
    for name, make in rnd.sample(files, rnd.randint(5, len(files))):
        with open(os.path.join(out, name), 'wb') as f:
            f.write(make())
        manifest.append('broken-music|%s|-' % name)


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    # pentatonic notes in a new order on every build
    notes = [523.25, 587.33, 659.25, 783.99, 880.00, 1046.50, 1174.66, 1318.51]
    random.shuffle(notes)
    tmp = tempfile.gettempdir()
    src, short = os.path.join(tmp, 's2m_melody.wav'), os.path.join(tmp, 's2m_melody3.wav')
    melody_wav(src, notes)
    melody_wav(short, notes, seconds=3.0)          # uncompressed / lossless ones: 3 s (size)
    cover = os.path.join(tmp, 's2m_cover.jpg')
    Image.new('RGB', (200, 200), tuple(random.randint(40, 220) for _ in range(3))).save(cover, 'JPEG')
    files = [
        ('melody-vorbis.ogg', ['-c:a', 'libvorbis', '-q:a', '3']),
        ('melody-vorbis-mono.ogg', ['-c:a', 'libvorbis', '-q:a', '1', '-ac', '1', '-ar', '22050']),
        ('melody-mono-22k.mp3', ['-c:a', 'libmp3lame', '-b:a', '48k', '-ac', '1', '-ar', '22050']),
        ('melody-vbr-id3.mp3', ['-c:a', 'libmp3lame', '-q:a', '4', '-metadata', 'title=Autotest melody', '-metadata',
                                'artist=Speak2Mic', '-id3v2_version', '3']),
        ('melody-8bit.wav', ['-c:a', 'pcm_u8']),
        ('melody-24bit.wav', ['-c:a', 'pcm_s24le']),
        ('melody-float.wav', ['-c:a', 'pcm_f32le']),
        ('melody-24bit-96k.flac', ['-c:a', 'flac', '-sample_fmt', 's32', '-ar', '96000']),
    ]
    manifest = []
    for name, args in files:
        lossless = name.endswith(('.wav', '.flac'))
        run(['ffmpeg', '-y', '-loglevel', 'error', '-i', short if lossless else src] + args + [os.path.join(OUT, name)])
        manifest.append('music|%s|-' % name)
    name = 'melody-cover.mp3'                      # an attached picture stream before the sound
    run(['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-i', cover, '-map', '0:a', '-map', '1:v', '-c:a', 'libmp3lame',
         '-b:a', '96k', '-c:v', 'mjpeg', '-id3v2_version', '3', '-metadata:s:v', 'title=Cover', '-disposition:v', 'attached_pic',
         os.path.join(OUT, name)])
    manifest.append('music|%s|-' % name)
    make_broken(OUT, os.path.join(OUT, 'melody-vbr-id3.mp3'), os.path.join(OUT, 'melody-24bit-96k.flac'), manifest)
    with open(os.path.join(OUT, 'manifest.txt'), 'w') as f:
        f.write('\n'.join(manifest) + '\n')
    lines = ['// Generated by tools/make_autotest_media.py on every build: the autotest sample files (RCDATA).',
             '500 RCDATA "autotest-media/manifest.txt"']
    for i, m in enumerate(manifest):
        lines.append('%d RCDATA "autotest-media/%s"' % (501 + i, m.split('|')[1]))
    with open(os.path.join(OUT, '..', 'autotest_media.rc'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print('autotest samples: %d files, %d KB in %s' % (len(os.listdir(OUT)), total // 1024, os.path.normpath(OUT)))


if __name__ == '__main__':
    main()
