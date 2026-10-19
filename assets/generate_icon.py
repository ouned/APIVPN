#!/usr/bin/env python3
"""
APIVPN application icon generator.

The mark: a red fishhook hung off the top node of a triangle of connections --
the hooking mechanism, hanging from the VPN mesh.

Renders assets/apivpn.ico (16-256px), assets/apivpn.svg and a 256px
assets/apivpn.png preview from one shared primitive list, so the raster and
vector sources can never drift apart.

    python assets/generate_icon.py
"""

import io
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))

BASE = 1024
SS = 4                      # supersample factor for anti-aliasing
N = BASE * SS               # master render size (4096)

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

# --- palette ---------------------------------------------------------------
BG_TOP = (10, 10, 12)       # near-black tile, slight lift so the corner
BG_BOT = (0, 0, 0)          # radius still registers on a dark desktop
LINK = (156, 32, 42)        # mesh edges
NODE = (239, 68, 68)        # mesh nodes
NODE_CORE = (255, 224, 224)
HERO = (255, 255, 255)      # the hook

# --- geometry (1024x1024 design space) -------------------------------------
TRI_TOP = (512.0, 200.0)
TRI_LEFT = (124.0, 846.0)
TRI_RIGHT = (900.0, 846.0)
NODE_R = 50.0
NODE_CORE_R = 21.0
LINK_W = 32.0

EYE_R, EYE_W = 78.0, 18.0           # eyelet ring
HOOK_HW, HOOK_SHANK, HOOK_BEND = 20.0, 375.0, 60.0
TIP_DX, TIP_LEN, TIP_W = 22.0, 128.0, 1.0
TILT = 12.0                         # degrees, clockwise


# ============================================================================
# Primitive drawing (Pillow)
# ============================================================================

def make_bg(top, bottom):
    grad = Image.new("RGB", (1, N))
    px = grad.load()
    for y in range(N):
        t = y / (N - 1)
        px[0, y] = tuple(int(a + (b - a) * t) for a, b in zip(top, bottom))
    grad = grad.resize((N, N), Image.Resampling.BILINEAR)

    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, N - 1, N - 1], radius=int(N * 0.225), fill=255
    )
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    return img


def draw_prims(img, prims, scale):
    d = ImageDraw.Draw(img, "RGBA")
    for p in prims:
        t = p["t"]

        if t in ("line", "poly"):
            pts = [(x * scale, y * scale) for x, y in p["pts"]]
            w = max(1, int(p["w"] * scale))
            d.line(pts, fill=p["color"], width=w, joint="curve")
            rr = w / 2.0
            for (x, y) in (pts[0], pts[-1]):
                d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=p["color"])

        elif t == "ellipse":
            cx, cy = p["c"]
            box = [(cx - p["rx"]) * scale, (cy - p["ry"]) * scale,
                   (cx + p["rx"]) * scale, (cy + p["ry"]) * scale]
            d.ellipse(box, fill=p.get("fill"), outline=p.get("stroke"),
                      width=int(p.get("w", 0) * scale) if p.get("stroke") else 0)

        elif t == "polygon":
            d.polygon([(x * scale, y * scale) for x, y in p["pts"]],
                      fill=p["fill"])


def render_master(prims):
    img = make_bg(BG_TOP, BG_BOT)
    glowers = [p for p in prims if p.get("glow")]
    if glowers:
        glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
        draw_prims(glow, glowers, SS)
        glow = glow.filter(ImageFilter.GaussianBlur(18 * SS))
        r, g, b, a = glow.split()
        glow = Image.merge("RGBA", (r, g, b, a.point(lambda v: int(v * 0.7))))
        img = Image.alpha_composite(img, glow)
    draw_prims(img, prims, SS)
    return img


def rasterize(prims, sizes):
    master = render_master(prims)
    return [(s, master.resize((s, s), Image.Resampling.LANCZOS)) for s in sizes]


# ============================================================================
# Primitive drawing (SVG)
# ============================================================================

def _rgb(c):
    return "none" if c is None else f"rgb({c[0]},{c[1]},{c[2]})"


def prims_to_svg(prims):
    body = []

    def emit(p, out):
        t = p["t"]
        if t in ("line", "poly"):
            pts = " ".join(f"{x:.2f},{y:.2f}" for x, y in p["pts"])
            out.append(
                f'<polyline points="{pts}" fill="none" '
                f'stroke="{_rgb(p["color"])}" stroke-width="{p["w"]}" '
                f'stroke-linecap="round" stroke-linejoin="round"/>'
            )
        elif t == "ellipse":
            cx, cy = p["c"]
            out.append(
                f'<ellipse cx="{cx:.2f}" cy="{cy:.2f}" rx="{p["rx"]:.2f}" '
                f'ry="{p["ry"]:.2f}" fill="{_rgb(p.get("fill"))}" '
                f'stroke="{_rgb(p.get("stroke"))}" '
                f'stroke-width="{p.get("w", 0):.2f}"/>'
            )
        elif t == "polygon":
            pts = " ".join(f"{x:.2f},{y:.2f}" for x, y in p["pts"])
            out.append(f'<polygon points="{pts}" fill="{_rgb(p["fill"])}"/>')

    glowers = [p for p in prims if p.get("glow")]
    if glowers:
        body.append('<g filter="url(#softGlow)" opacity="0.7">')
        for p in glowers:
            emit(p, body)
        body.append("</g>")
    for p in prims:
        emit(p, body)

    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{_rgb(BG_TOP)}"/>
      <stop offset="1" stop-color="{_rgb(BG_BOT)}"/>
    </linearGradient>
    <filter id="softGlow" x="-30%" y="-30%" width="160%" height="160%">
      <feGaussianBlur stdDeviation="18"/>
    </filter>
  </defs>
  <rect x="0" y="0" width="1024" height="1024" rx="230" fill="url(#bg)"/>
{chr(10).join("  " + line for line in body)}
</svg>
'''


# ============================================================================
# Geometry helpers
# ============================================================================

def _rot_pts(pts, pivot, deg):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    px, py = pivot
    return [(px + (x - px) * ca - (y - py) * sa,
             py + (x - px) * sa + (y - py) * ca) for x, y in pts]


def _rotate(prims, pivot, deg):
    out = []
    for p in prims:
        q = dict(p)
        if "pts" in q:
            q["pts"] = _rot_pts(q["pts"], pivot, deg)
        elif "c" in q:
            q["c"] = _rot_pts([q["c"]], pivot, deg)[0]
        out.append(q)
    return out


def _translate(prims, dx, dy):
    out = []
    for p in prims:
        q = dict(p)
        if "pts" in q:
            q["pts"] = [(x + dx, y + dy) for x, y in q["pts"]]
        elif "c" in q:
            q["c"] = (q["c"][0] + dx, q["c"][1] + dy)
        out.append(q)
    return out


# ============================================================================
# The mark
# ============================================================================

def fishhook(colour, hang, tilt=TILT):
    """Tapered fishhook drawn as one silhouette: long shank, round bend, sharp
    point.  The local origin is the eyelet centre with the shank on x = 0, so
    rotating about that origin swings the hook from its hanging point."""
    cap = EYE_R + EYE_W / 2.0 - 2.0                  # shank cap inside the eye
    y0 = cap + HOOK_SHANK                            # centreline start of bend
    B1 = (2 * HOOK_BEND, y0)                         # centreline end of bend
    P = (2 * HOOK_BEND - TIP_DX, y0 - TIP_LEN)       # point tip
    L = math.hypot(P[0] - B1[0], P[1] - B1[1])
    ux, uy = (P[0] - B1[0]) / L, (P[1] - B1[1]) / L
    nx, ny = -uy, ux                                 # outward point normal

    pts = [(HOOK_HW, cap)]
    for k in range(0, 13):                           # rounded shank top
        a = math.pi * k / 12.0
        pts.append((HOOK_HW * math.cos(a), cap - HOOK_HW * math.sin(a)))
    pts.append((-HOOK_HW, y0))
    ro = HOOK_BEND + HOOK_HW
    for k in range(0, 25):                           # outer edge of the bend
        a = math.radians(180 - k * 7.5)
        pts.append((HOOK_BEND + ro * math.cos(a), y0 + ro * math.sin(a)))
    pts.append((B1[0] + HOOK_HW * nx, B1[1] + HOOK_HW * ny))
    pts.append((P[0] + TIP_W * nx, P[1] + TIP_W * ny))
    pts.append((P[0] - TIP_W * nx, P[1] - TIP_W * ny))
    pts.append((B1[0] - HOOK_HW * nx, B1[1] - HOOK_HW * ny))
    ri = HOOK_BEND - HOOK_HW
    for k in range(0, 25):                           # inner edge back to shank
        a = math.radians(0 + k * 7.5)
        pts.append((HOOK_BEND + ri * math.cos(a), y0 + ri * math.sin(a)))

    prims = [dict(t="polygon", pts=pts, fill=colour, glow=True),
             dict(t="ellipse", c=(0.0, 0.0), rx=EYE_R, ry=EYE_R,
                  stroke=colour, w=EYE_W)]
    prims = _rotate(prims, (0.0, 0.0), tilt)
    return _translate(prims, hang[0], hang[1])


def build_icon():
    """Hooked mesh: hook hung off the top node of a triangle of connections."""
    p = []
    for a, b in ((TRI_TOP, TRI_LEFT), (TRI_TOP, TRI_RIGHT),
                 (TRI_LEFT, TRI_RIGHT)):
        p.append(dict(t="line", pts=[a, b], w=LINK_W, color=LINK))
    for (x, y) in (TRI_TOP, TRI_LEFT, TRI_RIGHT):
        p.append(dict(t="ellipse", c=(x, y), rx=NODE_R, ry=NODE_R,
                      fill=NODE, glow=True))
        p.append(dict(t="ellipse", c=(x, y), rx=NODE_CORE_R, ry=NODE_CORE_R,
                      fill=NODE_CORE))

    # The eyelet rests on the top node, so its centre hangs one clearance
    # (inner radius minus node radius) below the node's centre.
    clearance = (EYE_R - EYE_W / 2.0) - NODE_R
    p += fishhook(HERO, (TRI_TOP[0], TRI_TOP[1] + clearance))
    return p


# ============================================================================
# ICO output
# ============================================================================

def _bmp_entry(im):
    """32-bit BGRA DIB plus AND mask for one ICO directory entry."""
    w, h = im.size
    px = im.load()
    and_stride = ((w + 31) // 32) * 4
    xor = bytearray()
    and_mask = bytearray()
    for y in range(h - 1, -1, -1):                   # DIB rows are bottom-up
        bits = bytearray(and_stride)
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
            if a < 128:                              # legacy 1-bit transparency
                bits[x // 8] |= 0x80 >> (x % 8)
        and_mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                         len(xor) + len(and_mask), 0, 0, 0, 0)
    return bytes(header) + bytes(xor) + bytes(and_mask)


def write_ico(path, images):
    """images: ordered list of (size_px, PIL.Image).

    Sizes below 256 are written as BMP (maximum shell compatibility); 256 is
    written as PNG, which is what Windows itself does."""
    blobs = []
    for size, im in images:
        if size >= 256:
            buf = io.BytesIO()
            im.save(buf, format="PNG")
            blobs.append(buf.getvalue())
        else:
            blobs.append(_bmp_entry(im))

    offset = 6 + 16 * len(images)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for (size, _), blob in zip(images, blobs):
            dim = 0 if size >= 256 else size          # 0 means 256 in the format
            f.write(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                len(blob), offset))
            offset += len(blob)
        for blob in blobs:
            f.write(blob)


def _straight_rgba(im):
    """Straight (non-premultiplied) RGBA copy of `im`.

    Pillow resizes the RGBA channels independently, so a partially covered edge
    pixel comes back pre-darkened -- effectively premultiplied.  ImGui blends
    with SRC_ALPHA / INV_SRC_ALPHA, so divide the colour back out."""
    w, h = im.size
    src, out = im.load(), Image.new("RGBA", im.size)
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if 0 < a < 255:
                r = min(255, (r * 255 + a // 2) // a)
                g = min(255, (g * 255 + a // 2) // a)
                b = min(255, (b * 255 + a // 2) // a)
            dst[x, y] = (r, g, b, a)
    return out


# ============================================================================

def main():
    prims = build_icon()
    images = rasterize(prims, ICO_SIZES)
    by_size = dict(images)

    ico = os.path.join(HERE, "apivpn.ico")
    svg = os.path.join(HERE, "apivpn.svg")
    png = os.path.join(HERE, "apivpn.png")
    rgba = os.path.join(HERE, "apivpn_rgba.bin")

    write_ico(ico, images)
    with open(svg, "w", encoding="utf-8") as f:
        f.write(prims_to_svg(prims))
    by_size[256].save(png)

    # Raw pixels for createAppIconTexture() in the client GUI.  Byte order is
    # R,G,B,A, matching DXGI_FORMAT_R8G8B8A8_UNORM.
    with open(rgba, "wb") as f:
        f.write(_straight_rgba(by_size[256]).tobytes())

    print(f"wrote {ico}  ({', '.join(str(s) for s in ICO_SIZES)})")
    print(f"wrote {svg}")
    print(f"wrote {png}")
    print(f"wrote {rgba}  (256x256 RGBA)")


if __name__ == "__main__":
    main()
