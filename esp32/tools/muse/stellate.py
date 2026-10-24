#!/usr/bin/env python3
"""Icosahedron stellations from Du Val cell notation: tools/muse/stellate.py

The 20 face planes of an icosahedron (inradius 1) cut space into cells. A
cell's power is how many planes lie between it and the centre; cells of one
power form a shell, lettered a (the icosahedron) to h. A shell whose cells
fall into more than one orbit under the icosahedral group has numbered cell
types (e1, e2, ...). A stellation is a union of cells, written as in Coxeter
et al., "The Fifty-Nine Icosahedra": a capital letter is that shell and every
shell inside it (F = a..f), lower case adds one cell type (Fg1 = F plus g1).

  stellate.py cells                  lists the cell types per shell
  stellate.py png <cells> <out.png>  renders a stellation, e.g. Fg1
  stellate.py c <cells> <name>       prints its outer faces as a C triangle table
  stellate.py mesh <cells> <name>    prints it as the avatar's v3_t/face_t tables

Plain Python (no numpy); png needs Pillow.
"""
import math
import sys

PHI = (1 + 5 ** 0.5) / 2
EPS = 1e-9


def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): return mul(a, 1 / math.sqrt(dot(a, a)))


def face_normals():
    """The icosahedron's face normals: a dodecahedron's vertices."""
    v = [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    for s in (-1, 1):
        for t in (-1, 1):
            v += [(0, s / PHI, t * PHI), (s / PHI, t * PHI, 0), (s * PHI, 0, t / PHI)]
    return [norm(p) for p in v]


def split(poly, a, b, c):
    """Splits a convex 2D polygon by the line a*x + b*y = c: (below, above)."""
    lo, hi = [], []
    n = len(poly)
    for k in range(n):
        p, q = poly[k], poly[(k + 1) % n]
        dp, dq = a * p[0] + b * p[1] - c, a * q[0] + b * q[1] - c
        if dp <= EPS:
            lo.append(p)
        if dp >= -EPS:
            hi.append(p)
        if (dp < -EPS and dq > EPS) or (dp > EPS and dq < -EPS):
            t = dp / (dp - dq)
            x = (p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1]))
            lo.append(x)
            hi.append(x)
    ok = lambda s: len(s) >= 3 and abs(area2(s)) > 1e-12
    return (lo if ok(lo) else None), (hi if ok(hi) else None)


def area2(poly):
    return sum(poly[k][0] * poly[(k + 1) % len(poly)][1] - poly[(k + 1) % len(poly)][0] * poly[k][1]
               for k in range(len(poly)))


def centroid2(poly):
    a = area2(poly)
    cx = cy = 0.0
    for k in range(len(poly)):
        p, q = poly[k], poly[(k + 1) % len(poly)]
        w = p[0] * q[1] - q[0] * p[1]
        cx += (p[0] + q[0]) * w
        cy += (p[1] + q[1]) * w
    return (cx / (3 * a), cy / (3 * a))


def arrangement(R=30.0):
    """Every facet of the cell decomposition: (plane, 3D polygon, cell below, cell above).

    A cell is a bit mask: bit j set where the cell is beyond plane j. Facets
    reaching the bounding square belong to unbounded cells and are dropped,
    with those cells remembered in `unbounded`."""
    N = face_normals()
    facets, unbounded = [], set()
    for i, n in enumerate(N):
        u = norm(cross(n, (0.3, 0.5, 0.8)))
        v = cross(n, u)
        polys = [[(-R, -R), (R, -R), (R, R), (-R, R)]]
        for j, m in enumerate(N):
            if j == i or abs(dot(m, n) + 1) < 1e-9:
                continue   # itself, and the parallel opposite face
            a, b, c = dot(m, u), dot(m, v), 1 - dot(m, n)
            out = []
            for p in polys:
                lo, hi = split(p, a, b, c)
                out += [s for s in (lo, hi) if s]
            polys = out
        for p in polys:
            cx, cy = centroid2(p)
            pt = add(n, add(mul(u, cx), mul(v, cy)))
            def mask(x):
                return sum(1 << j for j, m in enumerate(N) if j != i and dot(m, x) > 1 + 1e-12)
            below = mask(sub(pt, mul(n, 1e-6)))
            above = mask(add(pt, mul(n, 1e-6))) | (1 << i)
            if any(abs(x) > R - 1e-6 or abs(y) > R - 1e-6 for x, y in p):
                unbounded.update((below, above))
                continue
            poly3 = [add(n, add(mul(u, x), mul(v, y))) for x, y in p]
            if area2(p) < 0:
                poly3.reverse()
            facets.append((i, poly3, abs(area2(p)) / 2, below, above))
    return N, facets, unbounded


def cells():
    """The bounded cells, grouped into types: {(power, k): set(masks)}, k from 1.

    Cells of one type have the same volume; within a shell, types are numbered
    by increasing distance from the centre."""
    N, facets, unbounded = arrangement()
    vol, far, count = {}, {}, {}
    for i, poly, area, below, above in facets:
        for cell, sign in ((below, 1), (above, -1)):
            vol[cell] = vol.get(cell, 0) + sign * area / 3
            r = max(math.sqrt(dot(p, p)) for p in poly)
            far[cell] = max(far.get(cell, 0), r)
    groups = {}
    for cell in vol:
        if cell in unbounded:
            continue
        power = bin(cell).count("1")
        key = (power, round(vol[cell], 6), round(far[cell], 6))
        groups.setdefault(key, set()).add(cell)
    types = {}
    for power in sorted({k[0] for k in groups}):
        keys = sorted((k for k in groups if k[0] == power), key=lambda k: (k[2], k[1]))
        for n, k in enumerate(keys, 1):
            types[(power, n)] = (groups[k], k[1], k[2])
    return types, facets


def parse(spec, types):
    """Du Val notation to a set of cells: capitals take whole shells inward."""
    letters = "abcdefgh"
    chosen, k = set(), 0
    while k < len(spec):
        ch = spec[k]
        k += 1
        num = ""
        while k < len(spec) and spec[k].isdigit():
            num += spec[k]
            k += 1
        p = letters.index(ch.lower())
        if ch.isupper():
            for (power, n), (c, _, _) in types.items():
                if power <= p:
                    chosen |= c
        else:
            for (power, n), (c, _, _) in types.items():
                if power == p and (not num or n == int(num)):
                    chosen |= c
    return chosen


def surface(spec):
    """The stellation's outer facets as (normal, polygon), wound outward."""
    types, facets = cells()
    inside = parse(spec, types)
    N = face_normals()
    out = []
    for i, poly, area, below, above in facets:
        if (below in inside) == (above in inside):
            continue
        if below in inside:
            out.append((N[i], poly))
        else:
            out.append((mul(N[i], -1), list(reversed(poly))))
    return out


def render(spec, path, size=480):
    from PIL import Image, ImageDraw
    faces = surface(spec)
    img = Image.new("RGB", (size * 3, size), (12, 14, 24))
    light = norm((0.4, 0.7, 1.0))
    views = [(0.42, 0.3), (1.1, -0.5), (2.0, 0.9)]
    rmax = max(math.sqrt(dot(p, p)) for _, poly in faces for p in poly)
    for v, (yaw, pitch) in enumerate(views):
        cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
        def rot(p):
            x, y, z = p[0] * cy + p[2] * sy, p[1], -p[0] * sy + p[2] * cy
            return (x, y * cp - z * sp, y * sp + z * cp)
        drawn = []
        for n, poly in faces:
            rn = rot(n)
            if rn[2] <= 0:
                continue   # facing away
            pts = [rot(p) for p in poly]
            depth = sum(p[2] for p in pts) / len(pts)
            shade = 0.25 + 0.75 * max(0.0, dot(rn, light))
            drawn.append((depth, pts, shade))
        draw = ImageDraw.Draw(img)
        s = size * 0.45 / rmax
        for depth, pts, shade in sorted(drawn, key=lambda d: d[0]):
            xy = [(v * size + size / 2 + p[0] * s, size / 2 - p[1] * s) for p in pts]
            col = (int(70 * shade), int(150 * shade), int(255 * shade))
            draw.polygon(xy, fill=col, outline=(int(110 * shade), int(190 * shade), 255))
    ImageDraw.Draw(img).text((8, 8), spec, fill=(255, 255, 255))
    img.save(path)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == "cells":
        types, _ = cells()
        for (power, n), (c, volume, far) in sorted(types.items()):
            print(f"{'abcdefgh'[power]}{n}: {len(c):3d} cells, volume {volume:.5f}, reaches r={far:.4f}")
    elif argv[1] == "png":
        render(argv[2], argv[3])
        print(f"{argv[2]}: {len(surface(argv[2]))} facets -> {argv[3]}")
    elif argv[1] == "mesh":
        # Shared vertices scaled to radius 1, and convex facets with outward
        # normals, as the avatar's v3_t / face_t initialisers.
        spec, name = argv[2], argv[3]
        faces = surface(spec)
        index, verts = {}, []
        def vid(p):
            key = tuple(round(x, 6) for x in p)
            if key not in index:
                index[key] = len(verts)
                verts.append(p)
            return index[key]
        polys = [(n, [vid(p) for p in poly]) for n, poly in faces]
        rmax = max(math.sqrt(dot(p, p)) for p in verts)
        assert len(verts) < 256 and all(len(v) <= 5 for _, v in polys)
        print(f"/* {spec}: {len(verts)} vertices, {len(polys)} convex facets, radius 1."
              f" Generated: tools/muse/stellate.py mesh {spec} {name} */")
        print(f"#define {name}_NV {len(verts)}\n#define {name}_NF {len(polys)}")
        print(f"static const v3_t {name}_V[{name}_NV] = {{")
        for p in verts:
            print("    { " + ", ".join(f"{x / rmax:.5f}f" for x in p) + " },")
        print("};")
        print(f"static const face_t {name}_F[{name}_NF] = {{")
        for n, v in polys:
            vs = ", ".join(str(i) for i in v + [0] * (5 - len(v)))
            print(f"    {{ {len(v)}, {{ {vs} }}, {{ {n[0]:.5f}f, {n[1]:.5f}f, {n[2]:.5f}f }}, 0 }},")
        print("};")
    elif argv[1] == "c":
        faces = surface(argv[2])
        tris = [(n, poly[0], poly[k], poly[k + 1]) for n, poly in faces for k in range(1, len(poly) - 1)]
        print(f"/* {argv[2]}: {len(tris)} triangles (tools/muse/stellate.py) */")
        print(f"static const float {argv[3]}[{len(tris)}][9] = {{")
        for _, a, b, c in tris:
            print("    { " + ", ".join(f"{x:.5f}f" for p in (a, b, c) for x in p) + " },")
        print("};")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
