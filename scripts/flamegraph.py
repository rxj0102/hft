#!/usr/bin/env python3
"""Turn the sampler's raw addresses (bench_t2t --profile) into a flame graph.

  scripts/flamegraph.py samples.txt out.svg [--folded out.folded] [--top N]

Addresses are resolved with addr2line, which expands inlined calls from the
debug info, so a function the compiler folded into its caller still shows up as
its own box. Build with the `perf` preset (-g, frame pointers). The SVG is
self-contained (no scripts); hover a box to see its name and share.
"""
import argparse
import collections
import html
import re
import subprocess
import sys


def load(path):
    exe, bias, stacks = None, 0, []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# exe "):
                exe = line[6:]
            elif line.startswith("# bias "):
                bias = int(line[7:], 16)
            elif line.startswith("#") or not line:
                continue
            else:
                stacks.append([int(x, 16) for x in line.split()])
    return exe, bias, stacks


def resolve(exe, bias, stacks):
    """Map every distinct return address to a list of frames, innermost first."""
    need = set()
    for st in stacks:
        for i, a in enumerate(st):
            need.add(a if i == 0 else a - 1)  # a return address points after the call
    in_exe = sorted(a for a in need if bias <= a < bias + (1 << 30))
    out = {}
    for k in range(0, len(in_exe), 2000):
        chunk = in_exe[k:k + 2000]
        args = ["addr2line", "-a", "-f", "-C", "-i", "-e", exe] + [hex(a - bias) for a in chunk]
        res = subprocess.run(args, capture_output=True, text=True, check=True).stdout.splitlines()
        cur, frames, j = None, [], 0
        for ln in res:
            if ln.startswith("0x"):
                if cur is not None:
                    out[cur] = frames
                cur = chunk[j] if False else None
                cur = int(ln, 16) + bias
                frames = []
                j += 1
            else:
                frames.append(ln)
        if cur is not None:
            out[cur] = frames
        # frames list alternates function, file:line
    clean = {}
    for a, lines in out.items():
        fr = []
        for i in range(0, len(lines) - 1, 2):
            fr.append(short(lines[i]))
        clean[a] = fr
    return clean


def short(name):
    name = name.replace("hft::", "").replace("(anonymous namespace)::", "")
    # collapse template argument lists: foo<a, b>::bar -> foo<>::bar
    depth, out = 0, []
    for ch in name:
        if ch == "<":
            if depth == 0:
                out.append("<")
            depth += 1
        elif ch == ">":
            depth -= 1
            if depth == 0:
                out.append(">")
        elif depth == 0:
            out.append(ch)
    s = "".join(out)
    s = re.sub(r"\(.*$", "", s)  # drop parameter lists
    return s or name


def fold(exe, bias, stacks):
    sym = resolve(exe, bias, stacks)
    folded = collections.Counter()
    unknown = 0
    for st in stacks:
        frames = []  # leaf first
        for i, a in enumerate(st):
            key = a if i == 0 else a - 1
            fr = sym.get(key)
            if fr is None:
                if i == 0:
                    frames.append("[libc or kernel]")
                    unknown += 1
                break  # left the executable
            frames.extend(f for f in fr if f != "??")
        if not frames:
            continue
        folded[";".join(reversed(frames))] += 1
    return folded, unknown


class Node:
    __slots__ = ("name", "total", "self_", "kids")

    def __init__(self, name):
        self.name, self.total, self.self_, self.kids = name, 0, 0, {}


def build(folded):
    root = Node("all")
    for stack, n in folded.items():
        root.total += n
        node = root
        for f in stack.split(";"):
            node = node.kids.setdefault(f, Node(f))
            node.total += n
        node.self_ += n
    return root


def color(name):
    h = 0
    for ch in name:
        h = (h * 31 + ord(ch)) & 0xFFFFFF
    r = 205 + (h % 50)
    g = 60 + ((h >> 8) % 130)
    b = (h >> 16) % 60
    return f"rgb({r},{g},{b})"


def svg(root, title, width=1400, row=17):
    def depth(n):
        return 1 + max((depth(k) for k in n.kids.values()), default=0)

    height = depth(root) * row + 60
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" font-family="Verdana, sans-serif" font-size="11">',
           '<rect width="100%" height="100%" fill="#f8f8f8"/>',
           f'<text x="{width/2}" y="22" text-anchor="middle" font-size="15">{html.escape(title)}</text>']
    total = root.total

    def draw(n, x, level):
        w = n.total / total * (width - 20)
        if w < 0.5:
            return
        y = height - 30 - (level + 1) * row
        pct = 100.0 * n.total / total
        tip = f"{n.name}: {n.total} samples ({pct:.2f}%), self {100.0 * n.self_ / total:.2f}%"
        out.append(f'<g><title>{html.escape(tip)}</title><rect x="{x + 10:.2f}" y="{y}" width="{w:.2f}" height="{row - 1}" fill="{color(n.name)}" rx="2"/>')
        if w > 45:
            maxch = int(w / 6.2)
            label = n.name if len(n.name) <= maxch else n.name[: max(maxch - 2, 1)] + ".."
            out.append(f'<text x="{x + 13:.2f}" y="{y + 12}" fill="#000">{html.escape(label)}</text>')
        out.append("</g>")
        cx = x
        for k in sorted(n.kids.values(), key=lambda k: k.name):
            draw(k, cx, level + 1)
            cx += k.total / total * (width - 20)

    draw(root, 0, 0)
    out.append("</svg>")
    return "\n".join(out)


def self_table(folded, top):
    selfc = collections.Counter()
    total = sum(folded.values())
    for stack, n in folded.items():
        selfc[stack.split(";")[-1]] += n
    rows = [(n, f) for f, n in selfc.most_common(top)]
    return total, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("svg")
    ap.add_argument("--folded")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    exe, bias, stacks = load(a.samples)
    if not stacks:
        sys.exit("no samples")
    folded, unknown = fold(exe, bias, stacks)
    root = build(folded)
    title = f"tick-to-trade replay, {root.total} samples"
    open(a.svg, "w").write(svg(root, title))
    if a.folded:
        with open(a.folded, "w") as f:
            for s, n in sorted(folded.items()):
                f.write(f"{s} {n}\n")
    total, rows = self_table(folded, a.top)
    print(f"{total} samples; {unknown} landed outside the executable (libc, kernel)")
    print("self time by function:")
    for n, f in rows:
        print(f"  {100.0 * n / total:5.1f}%  {f}")


if __name__ == "__main__":
    main()
