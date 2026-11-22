#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutable-format view: the colour attachment's conversion words come from the VIEW's format.

The fake backend renders nothing (it has no shader interpreter), so the pixel assertions in t_vk_draw --only view_format
only run on hardware. What the host can see is the words the driver emits, decoded by tests/golden.py: the FBD carries the
clear value already converted to the attachment format, which is exactly the view/image format decision.

Frame 1: R8G8B8A8_UNORM image, R8G8B8A8_SRGB view (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT), clear 0.5 linear grey.
         The sRGB encoding of 0.5 is 0xbc (188); the linear value the bug emits is 0x80 (128).
Frame 2: R8G8B8A8_SRGB image, R8G8B8A8_UNORM view: 0x80, not 0xbc.
Frames 3, 4: the sampled-view cases render into a UNORM image with a UNORM view: 0x80.
PanVK takes the colour format from the image view (panvk_vX_cmd_draw.c:159,171 and the conversion format at :794).
"""
import sys

WANT = [(1, 'bc'), (2, '80'), (3, '80'), (4, '80')]


def fbd_words(text):
    """Clear-value words of every RUN_FRAGMENT frame, in kick order (they live in the RT descriptor)."""
    frames, cur, in_rtd = [], None, False
    for line in text.splitlines():
        if line.startswith('kick '):
            cur = []
            frames.append(cur)
        elif 'RUN_FRAGMENT regs' in line:
            in_rtd = False
        elif line.strip() == 'RTD':
            in_rtd = True
        elif in_rtd and cur is not None:
            toks = [t for t in line.split() if len(t) == 8 and all(c in '0123456789abcdef' for c in t)]
            if not toks:
                in_rtd = False
                continue
            cur += toks
    return frames


def main():
    if len(sys.argv) != 2:
        print('usage: vfmt-words.py <decoded stream>')
        return 2
    with open(sys.argv[1]) as f:
        frames = [w for w in fbd_words(f.read()) if w]
    if len(frames) < len(WANT):
        print('%d frames, want %d (t_vk_draw --only view_format)' % (len(frames), len(WANT)))
        return 1
    bad = []
    for n, enc in WANT:
        words = frames[n - 1]
        hit = any(w[2:] == enc * 3 for w in words)
        if not hit:
            bad.append('frame %d: no 0x%02x%02x%02x%02x clear word in the RT descriptor (%s)'
                       % (n, int(enc, 16), int(enc, 16), int(enc, 16), int(enc, 16), ' '.join(words[:16])))
    if bad:
        print('; '.join(bad))
        return 1
    print('frames 1-4 clear words %s (view format of the attachment)'
          % ', '.join('0x%s' % (e * 4) for _, e in WANT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
