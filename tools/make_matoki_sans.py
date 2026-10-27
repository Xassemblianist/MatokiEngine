#!/usr/bin/env python3
# tools/make_matoki_sans.py — Yalnız Python standart kütüphanesi ile "Matoki Sans" TrueType (.ttf)
# yazı tipini ve gömülü C dizisini (src/font/matoki_sans_data.c/.h) üretir.
# Dış yazı tipi veya kütüphane kullanılmaz; tüm glifler (ASCII 32..126, Türkçe ğĞıİşŞçÇöÖüÜ
# ve semboller) doğru parçaları ve ikinci derece Bezier konturları olarak kodla tanımlanmıştır.
# Dış konturlar CW (saat yönü, +y yukarı), iç delikler CCW (saat yönü tersi) olarak doğrulanır.

import math
import os
import struct

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF_OUT_PATH = os.path.join(ROOT_DIR, "assets", "fonts", "MatokiSans.ttf")
C_OUT_PATH = os.path.join(ROOT_DIR, "src", "font", "matoki_sans_data.c")
H_OUT_PATH = os.path.join(ROOT_DIR, "src", "font", "matoki_sans_data.h")

# Genel tipografik metrikler (FUnits, units_per_em = 1000)
UPM = 1000
ASCENT = 800
DESCENT = -200
LINE_GAP = 100
CAP_H = 660
X_H = 500
ASC_H = 680
DESC_Y = -180

# Gövde kalınlıkları
STEM_UC = 88   # büyük harf dikey gövde
BAR_UC = 80    # büyük harf yatay bar
STEM_LC = 84   # küçük harf dikey gövde
BAR_LC = 74    # küçük harf yatay bar

K_CIRCLE = math.sqrt(2.0) - 1.0  # ~0.41421356 (8-parçalı ikinci derece Bezier çember sabiti)


def pt(x, y, on=1):
    return (int(round(x)), int(round(y)), 1 if on else 0)


def flatten_contour_for_area(pts):
    """TrueType konturunu (on/off curve + örtük orta nokta) çokgene açar."""
    n = len(pts)
    if n < 2:
        return []
    if pts[0][2]:
        start = (float(pts[0][0]), float(pts[0][1]))
        first_i, last_i = 1, n - 1
    elif pts[-1][2]:
        start = (float(pts[-1][0]), float(pts[-1][1]))
        first_i, last_i = 0, n - 2
    else:
        start = (0.5 * (pts[-1][0] + pts[0][0]), 0.5 * (pts[-1][1] + pts[0][1]))
        first_i, last_i = 0, n - 1

    poly = [start]
    cur = start
    i = first_i

    def add_qbez(p0, p1, p2):
        for step in range(1, 9):
            t = step / 8.0
            u = 1.0 - t
            x = u * u * p0[0] + 2.0 * u * t * p1[0] + t * t * p2[0]
            y = u * u * p0[1] + 2.0 * u * t * p1[1] + t * t * p2[1]
            poly.append((x, y))

    while i <= last_i:
        p = (float(pts[i][0]), float(pts[i][1]))
        if pts[i][2]:
            poly.append(p)
            cur = p
            i += 1
        else:
            if i < last_i:
                np = (float(pts[i + 1][0]), float(pts[i + 1][1]))
                if pts[i + 1][2]:
                    add_qbez(cur, p, np)
                    cur = np
                    i += 2
                else:
                    mid = (0.5 * (p[0] + np[0]), 0.5 * (p[1] + np[1]))
                    add_qbez(cur, p, mid)
                    cur = mid
                    i += 1
            else:
                add_qbez(cur, p, start)
                cur = start
                i += 1
    return poly


def contour_signed_area(pts):
    """Kartezyen (+x sağ, +y yukarı) işaretli alan * 2: CW < 0, CCW > 0."""
    poly = flatten_contour_for_area(pts)
    if len(poly) < 3:
        return 0.0
    a2 = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % len(poly)]
        a2 += x0 * y1 - x1 * y0
    return 0.5 * a2


def orient_contour(pts, clockwise=True):
    """Konturun sargı yönünü CW (clockwise=True) veya CCW (clockwise=False) yapar."""
    cleaned = []
    for p in pts:
        rp = pt(p[0], p[1], p[2])
        if not cleaned or rp != cleaned[-1]:
            cleaned.append(rp)
    if len(cleaned) > 1 and cleaned[0] == cleaned[-1]:
        cleaned.pop()
    area = contour_signed_area(cleaned)
    assert abs(area) > 1.0, f"Dejenere kontur: alan={area}, pts={cleaned}"
    is_cw = area < 0.0
    if is_cw != clockwise:
        cleaned = list(reversed(cleaned))
    return cleaned


def outer(pts):
    return orient_contour(pts, clockwise=True)


def hole(pts):
    return orient_contour(pts, clockwise=False)


def rect_cw(x0, y0, x1, y1):
    return outer([(x0, y0, 1), (x0, y1, 1), (x1, y1, 1), (x1, y0, 1)])


def rect_ccw(x0, y0, x1, y1):
    return hole([(x0, y0, 1), (x0, y1, 1), (x1, y1, 1), (x1, y0, 1)])


def oval_points(cx, cy, rx, ry):
    """12 noktalı ikinci derece Bezier elips (4 kardinal on-curve + 8 off-curve)."""
    kx = K_CIRCLE * rx
    ky = K_CIRCLE * ry
    return [
        pt(cx, cy + ry, 1),
        pt(cx + kx, cy + ry, 0),
        pt(cx + rx, cy + ky, 0),
        pt(cx + rx, cy, 1),
        pt(cx + rx, cy - ky, 0),
        pt(cx + kx, cy - ry, 0),
        pt(cx, cy - ry, 1),
        pt(cx - kx, cy - ry, 0),
        pt(cx - rx, cy - ky, 0),
        pt(cx - rx, cy, 1),
        pt(cx - rx, cy + ky, 0),
        pt(cx - kx, cy + ry, 0),
    ]


def oval_cw(x0, y0, x1, y1):
    cx = 0.5 * (x0 + x1)
    cy = 0.5 * (y0 + y1)
    rx = 0.5 * (x1 - x0)
    ry = 0.5 * (y1 - y0)
    return outer(oval_points(cx, cy, rx, ry))


def oval_ccw(x0, y0, x1, y1):
    cx = 0.5 * (x0 + x1)
    cy = 0.5 * (y0 + y1)
    rx = 0.5 * (x1 - x0)
    ry = 0.5 * (y1 - y0)
    return hole(oval_points(cx, cy, rx, ry))


def d_bowl_points(x_left, y_bot, x_right, y_top):
    """Sol kenarı düz, sağ tarafı ikinci derece Bezier yaylı D-biçimli kontur."""
    cy = 0.5 * (y_bot + y_top)
    ry = 0.5 * (y_top - y_bot)
    curve_x0 = x_left + 0.35 * (x_right - x_left)
    rx = x_right - curve_x0
    kx = K_CIRCLE * rx
    ky = K_CIRCLE * ry
    return [
        pt(x_left, y_bot, 1),
        pt(x_left, y_top, 1),
        pt(curve_x0, y_top, 1),
        pt(curve_x0 + kx, y_top, 0),
        pt(x_right, cy + ky, 0),
        pt(x_right, cy, 1),
        pt(x_right, cy - ky, 0),
        pt(curve_x0 + kx, y_bot, 0),
        pt(curve_x0, y_bot, 1),
    ]


def q_bowl_points(x_left, y_bot, x_right, y_top):
    """Sağ kenarı düz, sol tarafı ikinci derece Bezier yaylı ters-D konturu."""
    cy = 0.5 * (y_bot + y_top)
    ry = 0.5 * (y_top - y_bot)
    curve_x1 = x_right - 0.35 * (x_right - x_left)
    rx = curve_x1 - x_left
    kx = K_CIRCLE * rx
    ky = K_CIRCLE * ry
    return [
        pt(x_right, y_bot, 1),
        pt(curve_x1, y_bot, 1),
        pt(curve_x1 - kx, y_bot, 0),
        pt(x_left, cy - ky, 0),
        pt(x_left, cy, 1),
        pt(x_left, cy + ky, 0),
        pt(curve_x1 - kx, y_top, 0),
        pt(curve_x1, y_top, 1),
        pt(x_right, y_top, 1),
    ]


def stroke_line_cw(x0, y0, x1, y1, thickness):
    """(x0,y0)->(x1,y1) doğru parçası etrafında kalın dörtgen kontur."""
    dx = x1 - x0
    dy = y1 - y0
    length = math.hypot(dx, dy)
    nx = -dy / length * (thickness * 0.5)
    ny = dx / length * (thickness * 0.5)
    return outer([
        pt(x0 + nx, y0 + ny, 1),
        pt(x1 + nx, y1 + ny, 1),
        pt(x1 - nx, y1 - ny, 1),
        pt(x0 - nx, y0 - ny, 1),
    ])


def breve_cw(cx, y_bot, w=220, h=80, thick=52):
    """Türkçe ğ/Ğ için ikinci derece Bezier çanak (breve) aksanı."""
    hw = w * 0.5
    ir = max(hw - thick, 30)
    return outer([
        pt(cx - hw, y_bot + h, 1),
        pt(cx - hw + thick, y_bot + h, 1),
        pt(cx - ir * 0.6, y_bot + thick, 0),
        pt(cx, y_bot + thick, 1),
        pt(cx + ir * 0.6, y_bot + thick, 0),
        pt(cx + hw - thick, y_bot + h, 1),
        pt(cx + hw, y_bot + h, 1),
        pt(cx + hw * 0.65, y_bot, 0),
        pt(cx, y_bot, 1),
        pt(cx - hw * 0.65, y_bot, 0),
    ])


def cedilla_cw(cx, y_top=10, y_bot=-165):
    """Türkçe ç/Ç/ş/Ş için alt çengel (cedilla) konturu."""
    return outer([
        pt(cx - 28, y_top, 1),
        pt(cx + 28, y_top, 1),
        pt(cx + 22, -55, 1),
        pt(cx + 75, -75, 0),
        pt(cx + 75, -120, 1),
        pt(cx + 75, y_bot, 0),
        pt(cx - 10, y_bot, 1),
        pt(cx - 65, y_bot, 1),
        pt(cx - 65, y_bot + 48, 1),
        pt(cx - 12, y_bot + 48, 1),
        pt(cx + 18, y_bot + 48, 0),
        pt(cx + 18, -112, 1),
        pt(cx + 18, -90, 0),
        pt(cx - 28, -82, 1),
    ])


def umlaut_dots_cw(cx, y_bot, r=42, sep=150):
    """Türkçe ö/Ö/ü/Ü için iki yuvarlak nokta."""
    cy = y_bot + r
    return [
        oval_cw(cx - sep * 0.5 - r, cy - r, cx - sep * 0.5 + r, cy + r),
        oval_cw(cx + sep * 0.5 - r, cy - r, cx + sep * 0.5 + r, cy + r),
    ]


def c_arc_cw(x0, y0, x1, y1, thick_x, thick_y, gap_top=140, gap_bot=140):
    """C / c / ç / Ç / G için açık ikinci derece Bezier yay konturu."""
    cx = 0.5 * (x0 + x1)
    cy = 0.5 * (y0 + y1)
    ix0 = x0 + thick_x
    ix1 = x1 - thick_x
    iy0 = y0 + thick_y
    iy1 = y1 - thick_y
    return outer([
        pt(x1, cy + gap_top, 1),
        pt(x1 - thick_x, cy + gap_top - 15, 1),
        pt(ix1 - 15, iy1, 0),
        pt(cx, iy1, 1),
        pt(ix0, iy1, 0),
        pt(ix0, cy, 1),
        pt(ix0, iy0, 0),
        pt(cx, iy0, 1),
        pt(ix1 - 15, iy0, 0),
        pt(x1 - thick_x, cy - gap_bot + 15, 1),
        pt(x1, cy - gap_bot, 1),
        pt(x1 - 20, y0, 0),
        pt(cx, y0, 1),
        pt(x0, y0, 0),
        pt(x0, cy, 1),
        pt(x0, y1, 0),
        pt(cx, y1, 1),
        pt(x1 - 20, y1, 0),
    ])


def s_curve_cw(x0, y0, x1, y1, bar):
    """S / s / ş / Ş için yumuşak ikinci derece Bezier S-omurgası."""
    cx = 0.5 * (x0 + x1)
    cy = 0.5 * (y0 + y1)
    h = y1 - y0
    t = bar
    half_t = int(0.48 * t)
    return outer([
        # Üst sağ uçtan başla -> iç üst yay -> orta omurganın ÜST kenarı -> dış sağ alt yay
        pt(x1 - 12, y1 - int(0.20 * h), 1),
        pt(x1 - 12 - t, y1 - int(0.24 * h), 1),
        pt(x1 - t - 18, y1 - t, 0),
        pt(cx, y1 - t, 1),
        pt(x0 + t, y1 - t, 0),
        pt(x0 + t, cy + int(0.23 * h), 1),
        pt(x0 + t, cy + half_t + int(0.05 * h), 0),
        pt(cx, cy + half_t, 1),
        pt(x1, cy + half_t - int(0.06 * h), 0),
        pt(x1, y0 + int(0.26 * h), 1),
        pt(x1, y0, 0),
        pt(cx, y0, 1),
        pt(x0 + 10, y0, 0),
        pt(x0, y0 + int(0.21 * h), 1),
        # Alt sol uçtan iç alt yay -> orta omurganın ALT kenarı -> dış sol üst yay
        pt(x0 + t, y0 + int(0.25 * h), 1),
        pt(x0 + t + 18, y0 + t, 0),
        pt(cx, y0 + t, 1),
        pt(x1 - t, y0 + t, 0),
        pt(x1 - t, y0 + int(0.26 * h), 1),
        pt(x1 - t, cy - half_t - int(0.05 * h), 0),
        pt(cx, cy - half_t, 1),
        pt(x0, cy - half_t + int(0.06 * h), 0),
        pt(x0, y1 - int(0.25 * h), 1),
        pt(x0, y1, 0),
        pt(cx, y1, 1),
        pt(x1 - 10, y1, 0),
    ])


def circumflex_cw(cx, y_bot, w=210, h=82, thick=48):
    """Şapka aksanı (â, î, û, Â, Î, Û) konturu."""
    hw = int(w * 0.5)
    return outer([
        pt(cx - hw, y_bot, 1),
        pt(cx - 26, y_bot + h, 1),
        pt(cx + 26, y_bot + h, 1),
        pt(cx + hw, y_bot, 1),
        pt(cx + hw - thick, y_bot, 1),
        pt(cx, y_bot + h - thick, 1),
        pt(cx - hw + thick, y_bot, 1),
    ])


def define_all_glyphs():
    """Tüm kod noktaları için (advance, contours) sözlüğü döner."""
    g = {}

    # --- ASCII 32..47 ---
    g[32] = (260, [])  # ' '
    g[33] = (280, [    # '!'
        outer([(96, 190, 1), (90, 680, 1), (190, 680, 1), (184, 190, 1)]),
        oval_cw(88, 0, 192, 104),
    ])
    g[34] = (360, [    # '"'
        outer([(70, 440, 1), (62, 680, 1), (150, 680, 1), (142, 440, 1)]),
        outer([(218, 440, 1), (210, 680, 1), (298, 680, 1), (290, 440, 1)]),
    ])
    g[35] = (580, [    # '#' — tek dış kontur + orta delik
        outer([
            (130, 0, 1), (155, 190, 1), (50, 190, 1), (50, 266, 1),
            (165, 266, 1), (185, 414, 1), (50, 414, 1), (50, 490, 1),
            (195, 490, 1), (220, 680, 1), (296, 680, 1), (271, 490, 1),
            (385, 490, 1), (410, 680, 1), (486, 680, 1), (461, 490, 1),
            (530, 490, 1), (530, 414, 1), (451, 414, 1), (431, 266, 1),
            (530, 266, 1), (530, 190, 1), (421, 190, 1), (396, 0, 1),
            (320, 0, 1), (345, 190, 1), (231, 190, 1), (206, 0, 1),
        ]),
        hole([(241, 266, 1), (355, 266, 1), (375, 414, 1), (261, 414, 1)]),
    ])
    g[36] = (560, [    # '$'
        s_curve_cw(55, 0, 505, 680, 78),
        rect_cw(248, -70, 312, 750),
    ])
    g[37] = (680, [    # '%'
        oval_cw(45, 390, 295, 680),
        oval_ccw(110, 455, 230, 615),
        oval_cw(385, 0, 635, 290),
        oval_ccw(450, 65, 570, 225),
        outer([(140, 0, 1), (475, 680, 1), (540, 680, 1), (205, 0, 1)]),
    ])
    g[38] = (620, [    # '&' — iki delikli ampersand
        outer([
            (460, 0, 1), (385, 75, 1), (315, 0, 0), (220, 0, 1),
            (55, 0, 0), (55, 185, 1), (55, 300, 0), (165, 375, 1),
            (95, 445, 0), (95, 540, 1), (95, 680, 0), (265, 680, 1),
            (435, 680, 0), (435, 540, 1), (435, 435, 0), (325, 365, 1),
            (435, 245, 1), (475, 340, 1), (555, 340, 1), (495, 180, 1),
            (580, 0, 1),
        ]),
        oval_ccw(175, 435, 355, 605),
        oval_ccw(140, 75, 335, 295),
    ])
    g[39] = (230, [    # '\''
        outer([(75, 440, 1), (67, 680, 1), (163, 680, 1), (155, 440, 1)]),
    ])
    g[40] = (340, [    # '('
        outer([
            (250, -130, 1), (170, -130, 1),
            (65, 60, 0), (65, 290, 1),
            (65, 520, 0), (170, 720, 1),
            (250, 720, 1),
            (150, 520, 0), (150, 290, 1),
            (150, 60, 0),
        ]),
    ])
    g[41] = (340, [    # ')'
        outer([
            (90, -130, 1),
            (190, 60, 0), (190, 290, 1),
            (190, 520, 0), (90, 720, 1),
            (170, 720, 1),
            (275, 520, 0), (275, 290, 1),
            (275, 60, 0), (170, -130, 1),
        ]),
    ])
    g[42] = (420, [    # '*'
        rect_cw(178, 330, 242, 680),
        stroke_line_cw(65, 415, 355, 595, 62),
        stroke_line_cw(65, 595, 355, 415, 62),
    ])
    g[43] = (540, [    # '+' — tek birleşik artı konturu
        outer([
            (230, 90, 1), (230, 260, 1), (60, 260, 1), (60, 340, 1),
            (230, 340, 1), (230, 510, 1), (310, 510, 1), (310, 340, 1),
            (480, 340, 1), (480, 260, 1), (310, 260, 1), (310, 90, 1),
        ]),
    ])
    g[44] = (260, [    # ','
        outer([
            (85, 0, 1), (85, 110, 1), (185, 110, 1), (185, -10, 1),
            (185, -95, 0), (95, -140, 1), (70, -90, 1),
            (120, -65, 0), (120, 0, 1),
        ]),
    ])
    g[45] = (380, [    # '-'
        rect_cw(60, 255, 320, 335),
    ])
    g[46] = (260, [    # '.'
        oval_cw(78, 0, 182, 104),
    ])
    g[47] = (380, [    # '/'
        outer([(40, -60, 1), (265, 700, 1), (340, 700, 1), (115, -60, 1)]),
    ])

    # --- Rakamlar 48..57 ('0'..'9') ---
    g[48] = (580, [    # '0'
        oval_cw(50, 0, 530, CAP_H),
        oval_ccw(140, 82, 440, CAP_H - 82),
    ])
    g[49] = (500, [    # '1'
        outer([
            (80, 0, 1), (80, 78, 1), (215, 78, 1), (215, 565, 1),
            (95, 495, 1), (95, 580, 1), (225, 680, 1), (305, 680, 1),
            (305, 78, 1), (425, 78, 1), (425, 0, 1),
        ]),
    ])
    g[50] = (560, [    # '2'
        outer([
            (55, 0, 1), (55, 78, 1), (335, 360, 1),
            (412, 438, 0), (412, 515, 1),
            (412, 602, 0), (280, 602, 1),
            (158, 602, 0), (145, 485, 1),
            (58, 495, 1),
            (72, 680, 0), (282, 680, 1),
            (502, 680, 0), (502, 515, 1),
            (502, 415, 0), (395, 305, 1),
            (175, 80, 1), (505, 80, 1), (505, 0, 1),
        ]),
    ])
    g[51] = (560, [    # '3'
        outer([
            (60, 535, 1), (80, 680, 0), (280, 680, 1),
            (490, 680, 0), (490, 510, 1),
            (490, 395, 0), (375, 355, 1),
            (505, 315, 0), (505, 185, 1),
            (505, 0, 0), (280, 0, 1),
            (70, 0, 0), (50, 155, 1),
            (138, 170, 1),
            (152, 78, 0), (280, 78, 1),
            (415, 78, 0), (415, 188, 1),
            (415, 305, 0), (275, 305, 1),
            (210, 305, 1), (210, 382, 1), (275, 382, 1),
            (402, 382, 0), (402, 500, 1),
            (402, 602, 0), (278, 602, 1),
            (162, 602, 0), (148, 520, 1),
        ]),
    ])
    g[52] = (580, [    # '4' — tek dış kontur + üçgen delik
        outer([
            (340, 0, 1), (340, 165, 1), (40, 165, 1), (40, 245, 1),
            (335, 680, 1), (428, 680, 1), (428, 245, 1), (530, 245, 1),
            (530, 165, 1), (428, 165, 1), (428, 0, 1),
        ]),
        hole([(135, 245, 1), (340, 245, 1), (340, 550, 1)]),
    ])
    g[53] = (560, [    # '5'
        outer([
            (75, 325, 1), (105, 680, 1), (485, 680, 1), (485, 600, 1),
            (182, 600, 1), (164, 405, 1),
            (225, 445, 0), (305, 445, 1),
            (505, 445, 0), (505, 225, 1),
            (505, 0, 0), (278, 0, 1),
            (85, 0, 0), (55, 155, 1),
            (145, 170, 1),
            (165, 78, 0), (278, 78, 1),
            (415, 78, 0), (415, 225, 1),
            (415, 368, 0), (282, 368, 1),
            (195, 368, 0), (155, 315, 1),
        ]),
    ])
    g[54] = (580, [    # '6' — üst yay + kapalı alt kase (delikli)
        outer([
            (490, 535, 1), (405, 515, 1),
            (385, 604, 0), (300, 604, 1),
            (150, 604, 0), (142, 375, 1),
            (205, 435, 0), (310, 435, 1),
            (520, 435, 0), (520, 218, 1),
            (520, 0, 0), (290, 0, 1),
            (55, 0, 0), (55, 310, 1),
            (55, 680, 0), (302, 680, 1),
            (455, 680, 0),
        ]),
        oval_ccw(145, 76, 432, 358),
    ])
    g[55] = (540, [    # '7'
        outer([
            (55, 600, 1), (55, 680, 1), (485, 680, 1), (485, 605, 1),
            (235, 0, 1), (140, 0, 1), (388, 600, 1),
        ]),
    ])
    g[56] = (580, [    # '8' — 2 delikli
        oval_cw(52, 0, 528, 375),
        oval_cw(72, 315, 508, CAP_H),
        oval_ccw(144, 74, 436, 305),
        oval_ccw(160, 385, 420, CAP_H - 72),
    ])
    g[57] = (580, [    # '9' — kapalı üst kase (delikli) + alt kanca
        outer([
            (85, 145, 1), (170, 165, 1),
            (190, 76, 0), (275, 76, 1),
            (425, 76, 0), (433, 305, 1),
            (370, 245, 0), (265, 245, 1),
            (55, 245, 0), (55, 462, 1),
            (55, 680, 0), (285, 680, 1),
            (520, 680, 0), (520, 370, 1),
            (520, 0, 0), (273, 0, 1),
            (120, 0, 0),
        ]),
        oval_ccw(143, 322, 430, 604),
    ])

    # --- ASCII 58..64 ---
    g[58] = (260, [    # ':'
        oval_cw(78, 0, 182, 104),
        oval_cw(78, 370, 182, 474),
    ])
    g[59] = (260, [    # ';'
        oval_cw(78, 370, 182, 474),
        g[44][1][0],
    ])
    g[60] = (540, [    # '<'
        outer([
            (60, 265, 1), (465, 495, 1), (465, 412, 1),
            (160, 295, 1), (465, 178, 1), (465, 95, 1), (60, 325, 1),
        ]),
    ])
    g[61] = (540, [    # '='
        rect_cw(60, 175, 480, 255),
        rect_cw(60, 345, 480, 425),
    ])
    g[62] = (540, [    # '>'
        outer([
            (480, 325, 1), (75, 95, 1), (75, 178, 1),
            (380, 295, 1), (75, 412, 1), (75, 495, 1), (480, 265, 1),
        ]),
    ])
    g[63] = (500, [    # '?'
        outer([
            (60, 505, 1), (75, 680, 0), (255, 680, 1),
            (445, 680, 0), (445, 505, 1),
            (445, 410, 0), (335, 330, 1),
            (285, 290, 0), (285, 195, 1),
            (198, 195, 1),
            (198, 315, 0), (268, 370, 1),
            (352, 435, 0), (352, 505, 1),
            (352, 602, 0), (252, 602, 1),
            (158, 602, 0), (148, 495, 1),
        ]),
        oval_cw(190, 0, 294, 104),
    ])
    g[64] = (740, [    # '@' — dış halka + iç delikli kase
        c_arc_cw(45, -60, 695, 680, 68, 68, gap_top=80, gap_bot=160),
        oval_cw(195, 95, 535, 525),
        oval_ccw(268, 168, 462, 452),
        rect_cw(465, 95, 595, 175),
    ])

    # --- Büyük Harfler 65..90 ('A'..'Z') ---
    g[65] = (640, [    # 'A' — tek dış kontur + üçgen iç delik
        outer([
            (20, 0, 1), (268, CAP_H, 1), (372, CAP_H, 1), (620, 0, 1),
            (522, 0, 1), (462, 175, 1), (178, 175, 1), (118, 0, 1),
        ]),
        hole([(205, 252, 1), (435, 252, 1), (320, 582, 1)]),
    ])
    g[66] = (600, [    # 'B' — sol gövde + üst/alt D kase + 2 D-delik
        outer(d_bowl_points(60, 310, 530, CAP_H)),
        outer(d_bowl_points(60, 0, 550, 375)),
        hole(d_bowl_points(148, 384, 440, CAP_H - 76)),
        hole(d_bowl_points(148, 76, 458, 306)),
    ])
    g[67] = (620, [    # 'C'
        c_arc_cw(50, 0, 575, CAP_H, STEM_UC, BAR_UC, gap_top=145, gap_bot=145),
    ])
    g[68] = (640, [    # 'D' — tek dış D-kontur + tek iç D-delik
        outer(d_bowl_points(60, 0, 585, CAP_H)),
        hole(d_bowl_points(148, 80, 495, CAP_H - 80)),
    ])
    g[69] = (560, [    # 'E' — tek birleşik kontur
        outer([
            (60, 0, 1), (60, CAP_H, 1), (505, CAP_H, 1), (505, CAP_H - BAR_UC, 1),
            (148, CAP_H - BAR_UC, 1), (148, 382, 1), (465, 382, 1), (465, 302, 1),
            (148, 302, 1), (148, BAR_UC, 1), (515, BAR_UC, 1), (515, 0, 1),
        ]),
    ])
    g[70] = (540, [    # 'F' — tek birleşik kontur
        outer([
            (60, 0, 1), (60, CAP_H, 1), (500, CAP_H, 1), (500, CAP_H - BAR_UC, 1),
            (148, CAP_H - BAR_UC, 1), (148, 365, 1), (455, 365, 1), (455, 285, 1),
            (148, 285, 1), (148, 0, 1),
        ]),
    ])
    g[71] = (650, [    # 'G'
        c_arc_cw(50, 0, 590, CAP_H, STEM_UC, BAR_UC, gap_top=150, gap_bot=200),
        outer([
            (325, 265, 1), (325, 345, 1), (590, 345, 1),
            (590, 0, 1), (502, 0, 1), (502, 265, 1),
        ]),
    ])
    g[72] = (640, [    # 'H' — tek birleşik kontur
        outer([
            (60, 0, 1), (60, CAP_H, 1), (148, CAP_H, 1), (148, 384, 1),
            (492, 384, 1), (492, CAP_H, 1), (580, CAP_H, 1), (580, 0, 1),
            (492, 0, 1), (492, 304, 1), (148, 304, 1), (148, 0, 1),
        ]),
    ])
    g[73] = (260, [    # 'I'
        rect_cw(86, 0, 174, CAP_H),
    ])
    g[74] = (480, [    # 'J'
        outer([
            (330, CAP_H, 1), (418, CAP_H, 1), (418, 210, 1),
            (418, 0, 0), (225, 0, 1),
            (45, 0, 0), (45, 195, 1),
            (133, 195, 1),
            (133, 80, 0), (228, 80, 1),
            (330, 80, 0), (330, 210, 1),
        ]),
    ])
    g[75] = (600, [    # 'K'
        rect_cw(60, 0, 148, CAP_H),
        outer([(148, 330, 1), (455, CAP_H, 1), (565, CAP_H, 1), (240, 315, 1)]),
        outer([(205, 380, 1), (575, 0, 1), (460, 0, 1), (148, 320, 1)]),
    ])
    g[76] = (520, [    # 'L'
        outer([
            (60, 0, 1), (60, CAP_H, 1), (148, CAP_H, 1),
            (148, BAR_UC, 1), (485, BAR_UC, 1), (485, 0, 1),
        ]),
    ])
    g[77] = (740, [    # 'M' — tek birleşik kontur
        outer([
            (55, 0, 1), (55, CAP_H, 1), (170, CAP_H, 1), (370, 155, 1),
            (570, CAP_H, 1), (685, CAP_H, 1), (685, 0, 1), (597, 0, 1),
            (597, 515, 1), (405, 0, 1), (335, 0, 1), (143, 515, 1), (143, 0, 1),
        ]),
    ])
    g[78] = (640, [    # 'N' — tek birleşik kontur
        outer([
            (60, 0, 1), (60, CAP_H, 1), (158, CAP_H, 1), (492, 145, 1),
            (492, CAP_H, 1), (580, CAP_H, 1), (580, 0, 1), (482, 0, 1),
            (148, 535, 1), (148, 0, 1),
        ]),
    ])
    g[79] = (660, [    # 'O' — dış CW elips + iç CCW elips delik
        oval_cw(50, 0, 610, CAP_H),
        oval_ccw(142, 82, 518, CAP_H - 82),
    ])
    g[80] = (580, [    # 'P' — sol gövde + üst D-kase + D-delik
        rect_cw(60, 0, 148, CAP_H),
        outer(d_bowl_points(60, 275, 535, CAP_H)),
        hole(d_bowl_points(148, 355, 445, CAP_H - 78)),
    ])
    g[81] = (660, [    # 'Q' — O + alt sağ kuyruk (delikle kesişmez)
        oval_cw(50, 0, 610, CAP_H),
        oval_ccw(142, 82, 518, CAP_H - 82),
        outer([(430, 80, 1), (520, 110, 1), (630, -50, 1), (540, -80, 1)]),
    ])
    g[82] = (610, [    # 'R' — P + sağ alt bacak (delikle kesişmez)
        rect_cw(60, 0, 148, CAP_H),
        outer(d_bowl_points(60, 295, 535, CAP_H)),
        hole(d_bowl_points(148, 372, 445, CAP_H - 78)),
        outer([(315, 315, 1), (410, 315, 1), (575, 0, 1), (472, 0, 1)]),
    ])
    g[83] = (580, [    # 'S'
        s_curve_cw(50, 0, 530, CAP_H, BAR_UC),
    ])
    g[84] = (560, [    # 'T' — tek birleşik kontur
        outer([
            (236, 0, 1), (236, CAP_H - BAR_UC, 1), (30, CAP_H - BAR_UC, 1),
            (30, CAP_H, 1), (530, CAP_H, 1), (530, CAP_H - BAR_UC, 1),
            (324, CAP_H - BAR_UC, 1), (324, 0, 1),
        ]),
    ])
    g[85] = (640, [    # 'U'
        outer([
            (60, CAP_H, 1), (148, CAP_H, 1), (148, 250, 1),
            (148, 82, 0), (320, 82, 1),
            (492, 82, 0), (492, 250, 1),
            (492, CAP_H, 1), (580, CAP_H, 1), (580, 245, 1),
            (580, 0, 0), (320, 0, 1),
            (60, 0, 0), (60, 245, 1),
        ]),
    ])
    g[86] = (600, [    # 'V'
        outer([
            (25, CAP_H, 1), (122, CAP_H, 1), (300, 115, 1),
            (478, CAP_H, 1), (575, CAP_H, 1), (348, 0, 1), (252, 0, 1),
        ]),
    ])
    g[87] = (780, [    # 'W'
        outer([
            (20, CAP_H, 1), (112, CAP_H, 1), (218, 125, 1),
            (346, CAP_H, 1), (434, CAP_H, 1), (562, 125, 1),
            (668, CAP_H, 1), (760, CAP_H, 1), (608, 0, 1),
            (516, 0, 1), (390, 520, 1), (264, 0, 1), (172, 0, 1),
        ]),
    ])
    g[88] = (600, [    # 'X'
        outer([(35, 0, 1), (475, CAP_H, 1), (565, CAP_H, 1), (125, 0, 1)]),
        outer([(35, CAP_H, 1), (125, CAP_H, 1), (565, 0, 1), (475, 0, 1)]),
    ])
    g[89] = (580, [    # 'Y' — tek birleşik kontur
        outer([
            (246, 0, 1), (246, 285, 1), (25, CAP_H, 1), (125, CAP_H, 1),
            (290, 375, 1), (455, CAP_H, 1), (555, CAP_H, 1),
            (334, 285, 1), (334, 0, 1),
        ]),
    ])
    g[90] = (560, [    # 'Z' — tek birleşik kontur
        outer([
            (45, 0, 1), (45, 76, 1), (398, CAP_H - BAR_UC, 1),
            (55, CAP_H - BAR_UC, 1), (55, CAP_H, 1), (515, CAP_H, 1),
            (515, CAP_H - 76, 1), (162, BAR_UC, 1), (515, BAR_UC, 1), (515, 0, 1),
        ]),
    ])

    # --- ASCII 91..96 ---
    g[91] = (320, [    # '['
        outer([
            (70, -120, 1), (70, 720, 1), (265, 720, 1), (265, 648, 1),
            (152, 648, 1), (152, -48, 1), (265, -48, 1), (265, -120, 1),
        ]),
    ])
    g[92] = (380, [    # '\\'
        outer([(40, 700, 1), (115, 700, 1), (340, -60, 1), (265, -60, 1)]),
    ])
    g[93] = (320, [    # ']'
        outer([
            (55, -120, 1), (55, -48, 1), (168, -48, 1), (168, 648, 1),
            (55, 648, 1), (55, 720, 1), (250, 720, 1), (250, -120, 1),
        ]),
    ])
    g[94] = (500, [    # '^'
        outer([
            (55, 420, 1), (212, 680, 1), (288, 680, 1),
            (445, 420, 1), (362, 420, 1), (250, 605, 1), (138, 420, 1),
        ]),
    ])
    g[95] = (500, [    # '_'
        rect_cw(20, -120, 480, -45),
    ])
    g[96] = (300, [    # '`'
        outer([(140, 560, 1), (75, 700, 1), (165, 700, 1), (215, 560, 1)]),
    ])

    # --- Küçük Harfler 97..122 ('a'..'z') ---
    g[97] = (540, [    # 'a' — üst kemer + sağ gövde + alt karın (CCW delikli)
        outer([
            (70, 385, 1), (95, X_H, 0), (275, X_H, 1),
            (470, X_H, 0), (470, 325, 1),
            (470, 0, 1), (386, 0, 1), (386, 295, 1),
            (386, 426, 0), (270, 426, 1),
            (168, 426, 0), (152, 365, 1),
        ]),
        outer(q_bowl_points(50, 0, 470, 295)),
        hole(q_bowl_points(134, 66, 386, 228)),
    ])
    g[98] = (550, [    # 'b' — sol yükselen gövde + sağ kase + CCW delik
        rect_cw(55, 0, 55 + STEM_LC, ASC_H),
        outer(d_bowl_points(55, 0, 500, X_H)),
        hole(d_bowl_points(55 + STEM_LC, 74, 416, X_H - 74)),
    ])
    g[99] = (500, [    # 'c'
        c_arc_cw(45, 0, 465, X_H, STEM_LC, BAR_LC, gap_top=115, gap_bot=115),
    ])
    g[100] = (550, [   # 'd' — sağ yükselen gövde + sol kase + CCW delik
        rect_cw(495 - STEM_LC, 0, 495, ASC_H),
        outer(q_bowl_points(50, 0, 495, X_H)),
        hole(q_bowl_points(134, 74, 495 - STEM_LC, X_H - 74)),
    ])
    g[101] = (540, [   # 'e' — kapalı üst göz (CCW delikli) + açık alt sağ ağız
        outer([
            (485, 225, 1), (132, 225, 1),
            (136, 74, 0), (285, 74, 1),
            (378, 74, 0), (408, 145, 1),
            (485, 125, 1),
            (448, 0, 0), (280, 0, 1),
            (48, 0, 0), (48, 250, 1),
            (48, X_H, 0), (272, X_H, 1),
            (485, X_H, 0), (485, 260, 1),
        ]),
        hole([
            pt(136, 296, 1),
            pt(400, 296, 1),
            pt(396, 428, 0),
            pt(272, 428, 1),
            pt(144, 428, 0),
        ]),
    ])
    g[102] = (360, [   # 'f'
        outer([
            (115, 0, 1), (115, 424, 1), (35, 424, 1), (35, X_H, 1),
            (115, X_H, 1), (115, 565, 1),
            (115, ASC_H, 0), (260, ASC_H, 1),
            (315, ASC_H, 0), (345, ASC_H - 12, 1),
            (345, ASC_H - 84, 1),
            (310, ASC_H - 74, 0), (268, ASC_H - 74, 1),
            (199, ASC_H - 74, 0), (199, 560, 1),
            (199, X_H, 1), (320, X_H, 1), (320, 424, 1),
            (199, 424, 1), (199, 0, 1),
        ]),
    ])
    g[103] = (550, [   # 'g' — sol kase (CCW delikli) + sağ alçalan kanca
        outer(q_bowl_points(50, 0, 495, X_H)),
        hole(q_bowl_points(134, 74, 495 - STEM_LC, X_H - 74)),
        outer([
            (495 - STEM_LC, X_H, 1), (495, X_H, 1), (495, -35, 1),
            (495, DESC_Y, 0), (268, DESC_Y, 1),
            (115, DESC_Y, 0), (75, -95, 1),
            (155, -75, 1),
            (182, DESC_Y + 72, 0), (268, DESC_Y + 72, 1),
            (495 - STEM_LC, DESC_Y + 72, 0), (495 - STEM_LC, -30, 1),
        ]),
    ])
    g[104] = (540, [   # 'h'
        rect_cw(55, 0, 55 + STEM_LC, ASC_H),
        outer([
            (55 + STEM_LC, 345, 1),
            (185, X_H, 0), (315, X_H, 1),
            (485, X_H, 0), (485, 315, 1),
            (485, 0, 1), (485 - STEM_LC, 0, 1), (485 - STEM_LC, 305, 1),
            (485 - STEM_LC, 424, 0), (288, 424, 1),
            (139, 424, 0), (139, 285, 1),
        ]),
    ])
    g[105] = (250, [   # 'i'
        rect_cw(83, 0, 83 + STEM_LC, X_H),
        oval_cw(75, 565, 175, 665),
    ])
    g[106] = (260, [   # 'j'
        outer([
            (92, X_H, 1), (92 + STEM_LC, X_H, 1), (92 + STEM_LC, -45, 1),
            (92 + STEM_LC, DESC_Y, 0), (40, DESC_Y, 1),
            (-10, DESC_Y, 1), (-10, DESC_Y + 72, 1), (32, DESC_Y + 72, 1),
            (92, DESC_Y + 72, 0), (92, -40, 1),
        ]),
        oval_cw(84, 565, 184, 665),
    ])
    g[107] = (510, [   # 'k'
        rect_cw(55, 0, 55 + STEM_LC, ASC_H),
        outer([(139, 235, 1), (378, X_H, 1), (482, X_H, 1), (222, 225, 1)]),
        outer([(195, 280, 1), (492, 0, 1), (385, 0, 1), (139, 230, 1)]),
    ])
    g[108] = (250, [   # 'l'
        rect_cw(83, 0, 83 + STEM_LC, ASC_H),
    ])
    g[109] = (800, [   # 'm'
        rect_cw(55, 0, 55 + STEM_LC, X_H),
        outer([
            (139, 350, 1), (180, X_H, 0), (288, X_H, 1),
            (435, X_H, 0), (435, 320, 1),
            (435, 0, 1), (435 - STEM_LC, 0, 1), (435 - STEM_LC, 310, 1),
            (435 - STEM_LC, 424, 0), (265, 424, 1),
            (139, 424, 0), (139, 290, 1),
        ]),
        outer([
            (425, 350, 1), (470, X_H, 0), (582, X_H, 1),
            (740, X_H, 0), (740, 320, 1),
            (740, 0, 1), (740 - STEM_LC, 0, 1), (740 - STEM_LC, 310, 1),
            (740 - STEM_LC, 424, 0), (562, 424, 1),
            (435, 424, 0), (435, 290, 1),
        ]),
    ])
    g[110] = (540, [   # 'n'
        rect_cw(55, 0, 55 + STEM_LC, X_H),
        outer([
            (55 + STEM_LC, 345, 1),
            (185, X_H, 0), (315, X_H, 1),
            (485, X_H, 0), (485, 315, 1),
            (485, 0, 1), (485 - STEM_LC, 0, 1), (485 - STEM_LC, 305, 1),
            (485 - STEM_LC, 424, 0), (288, 424, 1),
            (139, 424, 0), (139, 285, 1),
        ]),
    ])
    g[111] = (540, [   # 'o' — dış CW elips + iç CCW elips delik
        oval_cw(46, 0, 494, X_H),
        oval_ccw(132, 76, 408, X_H - 76),
    ])
    g[112] = (550, [   # 'p' — sol alçalan gövde + sağ kase + CCW delik
        rect_cw(55, DESC_Y, 55 + STEM_LC, X_H),
        outer(d_bowl_points(55, 0, 500, X_H)),
        hole(d_bowl_points(55 + STEM_LC, 74, 416, X_H - 74)),
    ])
    g[113] = (550, [   # 'q' — sağ alçalan gövde + sol kase + CCW delik
        rect_cw(495 - STEM_LC, DESC_Y, 495, X_H),
        outer(q_bowl_points(50, 0, 495, X_H)),
        hole(q_bowl_points(134, 74, 495 - STEM_LC, X_H - 74)),
    ])
    g[114] = (380, [   # 'r'
        rect_cw(55, 0, 55 + STEM_LC, X_H),
        outer([
            (139, 340, 1), (175, X_H, 0), (295, X_H, 1),
            (340, X_H, 0), (365, X_H - 12, 1),
            (365, X_H - 88, 1),
            (330, 420, 0), (280, 420, 1),
            (139, 420, 0), (139, 275, 1),
        ]),
    ])
    g[115] = (490, [   # 's'
        s_curve_cw(42, 0, 448, X_H, BAR_LC),
    ])
    g[116] = (370, [   # 't'
        outer([
            (115, 130, 1), (115, 424, 1), (32, 424, 1), (32, X_H, 1),
            (115, X_H, 1), (115, 635, 1), (199, 635, 1), (199, X_H, 1),
            (330, X_H, 1), (330, 424, 1), (199, 424, 1), (199, 135, 1),
            (199, 72, 0), (268, 72, 1),
            (308, 72, 0), (338, 82, 1),
            (338, 10, 1),
            (298, 0, 0), (252, 0, 1),
            (115, 0, 0),
        ]),
    ])
    g[117] = (540, [   # 'u'
        outer([
            (55, X_H, 1), (55 + STEM_LC, X_H, 1), (55 + STEM_LC, 195, 1),
            (55 + STEM_LC, 76, 0), (252, 76, 1),
            (485 - STEM_LC, 76, 0), (485 - STEM_LC, 215, 1),
            (485 - STEM_LC, X_H, 1), (485, X_H, 1), (485, 0, 1),
            (485 - STEM_LC, 0, 1), (485 - STEM_LC, 95, 1),
            (345, 0, 0), (225, 0, 1),
            (55, 0, 0), (55, 185, 1),
        ]),
    ])
    g[118] = (500, [   # 'v'
        outer([
            (20, X_H, 1), (110, X_H, 1), (250, 95, 1),
            (390, X_H, 1), (480, X_H, 1), (294, 0, 1), (206, 0, 1),
        ]),
    ])
    g[119] = (700, [   # 'w'
        outer([
            (20, X_H, 1), (104, X_H, 1), (195, 98, 1),
            (308, X_H, 1), (392, X_H, 1), (505, 98, 1),
            (596, X_H, 1), (680, X_H, 1), (546, 0, 1),
            (462, 0, 1), (350, 385, 1), (238, 0, 1), (154, 0, 1),
        ]),
    ])
    g[120] = (500, [   # 'x'
        outer([(30, 0, 1), (382, X_H, 1), (470, X_H, 1), (118, 0, 1)]),
        outer([(30, X_H, 1), (118, X_H, 1), (470, 0, 1), (382, 0, 1)]),
    ])
    g[121] = (500, [   # 'y'
        outer([
            (20, X_H, 1), (110, X_H, 1), (252, 105, 1),
            (390, X_H, 1), (480, X_H, 1), (235, -180, 1), (145, -180, 1),
            (208, -10, 1),
        ]),
    ])
    g[122] = (480, [   # 'z' — tek birleşik kontur
        outer([
            (40, 0, 1), (40, 68, 1), (332, X_H - BAR_LC, 1),
            (50, X_H - BAR_LC, 1), (50, X_H, 1), (440, X_H, 1),
            (440, X_H - 68, 1), (148, BAR_LC, 1), (445, BAR_LC, 1), (445, 0, 1),
        ]),
    ])

    # --- ASCII 123..126 ---
    g[123] = (340, [   # '{'
        outer([
            (260, -120, 1), (195, -120, 1),
            (120, -120, 0), (120, -30, 1),
            (120, 215, 1),
            (120, 260, 0), (50, 260, 1),
            (50, 340, 1),
            (120, 340, 0), (120, 385, 1),
            (120, 630, 1),
            (120, 720, 0), (195, 720, 1),
            (260, 720, 1), (260, 648, 1), (218, 648, 1),
            (196, 648, 0), (196, 615, 1),
            (196, 375, 1),
            (196, 315, 0), (138, 300, 1),
            (196, 285, 0), (196, 225, 1),
            (196, -15, 1),
            (196, -48, 0), (218, -48, 1),
            (260, -48, 1),
        ]),
    ])
    g[124] = (240, [   # '|'
        rect_cw(82, -140, 158, 740),
    ])
    g[125] = (340, [   # '}'
        outer([
            (80, -120, 1), (80, -48, 1), (122, -48, 1),
            (144, -48, 0), (144, -15, 1),
            (144, 225, 1),
            (144, 285, 0), (202, 300, 1),
            (144, 315, 0), (144, 375, 1),
            (144, 615, 1),
            (144, 648, 0), (122, 648, 1),
            (80, 648, 1), (80, 720, 1), (145, 720, 1),
            (220, 720, 0), (220, 630, 1),
            (220, 385, 1),
            (220, 340, 0), (290, 340, 1),
            (290, 260, 1),
            (220, 260, 0), (220, 215, 1),
            (220, -30, 1),
            (220, -120, 0), (145, -120, 1),
        ]),
    ])
    g[126] = (540, [   # '~'
        outer([
            (45, 250, 1),
            (85, 385, 0), (182, 385, 1),
            (242, 385, 0), (305, 330, 1),
            (358, 285, 0), (402, 285, 1),
            (448, 285, 0), (468, 365, 1),
            (505, 345, 1),
            (472, 210, 0), (375, 210, 1),
            (315, 210, 0), (252, 265, 1),
            (198, 310, 0), (154, 310, 1),
            (108, 310, 0), (85, 230, 1),
        ]),
    ])

    # --- Türkçe Harfler (ğ Ğ ı İ ş Ş ç Ç ö Ö ü Ü) ---
    # Ç (U+00C7)
    g[0x00C7] = (620, list(g[67][1]) + [cedilla_cw(320)])
    # Ö (U+00D6)
    g[0x00D6] = (660, list(g[79][1]) + umlaut_dots_cw(330, 720, r=40, sep=160))
    # Ü (U+00DC)
    g[0x00DC] = (640, list(g[85][1]) + umlaut_dots_cw(320, 720, r=40, sep=160))
    # ç (U+00E7)
    g[0x00E7] = (500, list(g[99][1]) + [cedilla_cw(260)])
    # ö (U+00F6)
    g[0x00F6] = (540, list(g[111][1]) + umlaut_dots_cw(270, 565, r=40, sep=145))
    # ü (U+00FC)
    g[0x00FC] = (540, list(g[117][1]) + umlaut_dots_cw(270, 565, r=40, sep=145))
    # Ğ (U+011E)
    g[0x011E] = (650, list(g[71][1]) + [breve_cw(330, 720, w=230, h=80, thick=46)])
    # ğ (U+011F)
    g[0x011F] = (550, list(g[103][1]) + [breve_cw(272, 562, w=210, h=80, thick=46)])
    # İ (U+0130)
    g[0x0130] = (260, list(g[73][1]) + [oval_cw(82, 720, 178, 800)])
    # ı (U+0131)
    g[0x0131] = (250, [rect_cw(83, 0, 83 + STEM_LC, X_H)])
    # Ş (U+015E)
    g[0x015E] = (580, list(g[83][1]) + [cedilla_cw(290)])
    # ş (U+015F)
    g[0x015F] = (490, list(g[115][1]) + [cedilla_cw(245)])

    # Şapkalı Türkçe harfler (Â, Î, Û, â, î, û — Latin-1 aralığında, FontAtlas'a doğrudan girer)
    g[0x00C2] = (640, list(g[65][1]) + [circumflex_cw(320, 720, w=220, h=80, thick=46)])  # Â
    g[0x00CE] = (260, list(g[73][1]) + [circumflex_cw(130, 720, w=190, h=80, thick=44)])  # Î
    g[0x00DB] = (640, list(g[85][1]) + [circumflex_cw(320, 720, w=220, h=80, thick=46)])  # Û
    g[0x00E2] = (540, list(g[97][1]) + [circumflex_cw(270, 562, w=210, h=80, thick=46)])  # â
    g[0x00EE] = (250, [rect_cw(83, 0, 83 + STEM_LC, X_H), circumflex_cw(125, 562, w=185, h=80, thick=44)])  # î
    g[0x00FB] = (540, list(g[117][1]) + [circumflex_cw(270, 562, w=210, h=80, thick=46)])  # û

    # --- Ek Latin-1 ve Unicode Sembolleri ---
    g[0x00A0] = (260, [])  # NBSP
    g[0x00A9] = (740, [    # ©
        oval_cw(40, -20, 700, 660),
        oval_ccw(105, 45, 635, 595),
        c_arc_cw(215, 155, 525, 485, 56, 56, gap_top=80, gap_bot=80),
    ])
    g[0x00AB] = (500, [    # «
        outer([(40, 240, 1), (220, 420, 1), (285, 420, 1), (125, 240, 1), (285, 60, 1), (220, 60, 1)]),
        outer([(215, 240, 1), (395, 420, 1), (460, 420, 1), (300, 240, 1), (460, 60, 1), (395, 60, 1)]),
    ])
    g[0x00AE] = (740, [    # ®
        oval_cw(40, -20, 700, 660),
        oval_ccw(105, 45, 635, 595),
        rect_cw(240, 145, 305, 495),
        outer(d_bowl_points(240, 305, 495, 495)),
        hole(d_bowl_points(305, 358, 435, 442)),
        outer([(355, 315, 1), (420, 315, 1), (505, 145, 1), (440, 145, 1)]),
    ])
    g[0x00B0] = (400, [    # ° (derece)
        oval_cw(65, 410, 335, 680),
        oval_ccw(130, 475, 270, 615),
    ])
    g[0x00B1] = (540, [    # ±
        outer([
            (230, 165, 1), (230, 310, 1), (60, 310, 1), (60, 386, 1),
            (230, 386, 1), (230, 530, 1), (310, 530, 1), (310, 386, 1),
            (480, 386, 1), (480, 310, 1), (310, 310, 1), (310, 165, 1),
        ]),
        rect_cw(60, 20, 480, 96),
    ])
    g[0x00B7] = (260, [    # · (orta nokta)
        oval_cw(78, 238, 182, 342),
    ])
    g[0x00BB] = (500, [    # »
        outer([(460, 240, 1), (280, 60, 1), (215, 60, 1), (375, 240, 1), (215, 420, 1), (280, 420, 1)]),
        outer([(285, 240, 1), (105, 60, 1), (40, 60, 1), (200, 240, 1), (40, 420, 1), (105, 420, 1)]),
    ])
    g[0x00D7] = (540, [    # ×
        stroke_line_cw(80, 90, 460, 470, 76),
        stroke_line_cw(80, 470, 460, 90, 76),
    ])
    g[0x00F7] = (540, [    # ÷
        rect_cw(60, 255, 480, 335),
        oval_cw(218, 415, 322, 519),
        oval_cw(218, 70, 322, 174),
    ])
    g[0x2013] = (500, [    # – (en-dash)
        rect_cw(40, 255, 460, 335),
    ])
    g[0x2014] = (760, [    # — (em-dash)
        rect_cw(30, 255, 730, 335),
    ])
    g[0x2022] = (360, [    # • (madde imi)
        oval_cw(85, 195, 275, 385),
    ])
    g[0x2026] = (720, [    # … (üç nokta)
        oval_cw(60, 0, 164, 104),
        oval_cw(308, 0, 412, 104),
        oval_cw(556, 0, 660, 104),
    ])
    g[0x20BA] = (580, [    # ₺ (Türk Lirası simgesi)
        outer([
            (130, 0, 1), (130, CAP_H, 1), (218, CAP_H, 1), (218, 84, 1),
            (315, 84, 0), (435, 195, 1), (435, 285, 1), (520, 285, 1),
            (520, 165, 1), (370, 0, 0), (218, 0, 1),
        ]),
        outer([(45, 415, 1), (445, 515, 1), (445, 445, 1), (45, 345, 1)]),
        outer([(45, 285, 1), (445, 385, 1), (445, 315, 1), (45, 215, 1)]),
    ])

    return g


def point_in_poly_winding(px, py, poly):
    """(px, py) noktasının yönlü çokgen içindeki sargı sayısını (winding number) döner."""
    wn = 0
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        if y0 <= py:
            if y1 > py:
                if (x1 - x0) * (py - y0) - (px - x0) * (y1 - y0) > 0:
                    wn += 1
        else:
            if y1 <= py:
                if (x1 - x0) * (py - y0) - (px - x0) * (y1 - y0) < 0:
                    wn -= 1
    return wn


def verify_glyph_holes_and_winding(glyph_dict):
    """Tüm gliflerin kontur sargı yönünü ve delikli gliflerin delik merkezlerinde
    net sargı sayısının 0 (boş) olduğunu doğrular."""
    must_have_holes = [
        ord('O'), ord('D'), ord('P'), ord('Q'), ord('R'), ord('B'), ord('A'),
        ord('a'), ord('b'), ord('d'), ord('e'), ord('g'), ord('o'), ord('p'), ord('q'),
        ord('0'), ord('4'), ord('6'), ord('8'), ord('9'),
        0x011F, 0x00D6, 0x00F6,  # ğ, Ö, ö
    ]
    for cp in must_have_holes:
        adv, contours = glyph_dict[cp]
        polys = [flatten_contour_for_area(c) for c in contours]
        areas = [contour_signed_area(c) for c in contours]
        holes = [idx for idx, a in enumerate(areas) if a > 0]
        assert len(holes) >= 1, f"U+{cp:04X} ({chr(cp)}) en az 1 CCW delik icermeli!"
        for h_idx in holes:
            hpoly = polys[h_idx]
            cx = sum(p[0] for p in hpoly) / len(hpoly)
            cy = sum(p[1] for p in hpoly) / len(hpoly)
            total_wn = sum(point_in_poly_winding(cx, cy, p) for p in polys)
            assert total_wn == 0, (
                f"U+{cp:04X} ({chr(cp)}) delik #{h_idx} merkezinde ({cx:.1f},{cy:.1f}) "
                f"net sargi 0 olmali, bulunan={total_wn}"
            )


def pad4(b):
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def encode_simple_glyph(contours):
    if not contours:
        return b"", (0, 0, 0, 0)
    end_pts = []
    pts = []
    idx = 0
    for c in contours:
        idx += len(c)
        end_pts.append(idx - 1)
        pts.extend(c)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    xmin, ymin, xmax, ymax = min(xs), min(ys), max(xs), max(ys)

    flags = bytearray()
    x_bytes = bytearray()
    y_bytes = bytearray()
    px, py = 0, 0
    for (x, y, on_curve) in pts:
        dx = x - px
        dy = y - py
        px, py = x, y
        fl = 0x01 if on_curve else 0x00
        if dx == 0:
            fl |= 0x10
        elif -255 <= dx <= 255:
            fl |= 0x02 | (0x10 if dx > 0 else 0x00)
            x_bytes += struct.pack(">B", abs(dx))
        else:
            x_bytes += struct.pack(">h", dx)

        if dy == 0:
            fl |= 0x20
        elif -255 <= dy <= 255:
            fl |= 0x04 | (0x20 if dy > 0 else 0x00)
            y_bytes += struct.pack(">B", abs(dy))
        else:
            y_bytes += struct.pack(">h", dy)
        flags.append(fl)

    hdr = struct.pack(">hhhhh", len(contours), xmin, ymin, xmax, ymax)
    ends = struct.pack(">%dH" % len(end_pts), *end_pts)
    ins = struct.pack(">H", 0)
    return pad4(hdr + ends + ins + bytes(flags) + bytes(x_bytes) + bytes(y_bytes)), (xmin, ymin, xmax, ymax)


def build_cmap_tables(cp_to_gid):
    """Sıralı (cp -> gid) eşlemesinden cmap biçim 4 ve biçim 12 tablolarını üretir."""
    sorted_items = sorted(cp_to_gid.items())
    groups = []
    for cp, gid in sorted_items:
        if groups and cp == groups[-1][1] + 1 and gid == groups[-1][2] + (cp - groups[-1][0]):
            groups[-1] = (groups[-1][0], cp, groups[-1][2])
        else:
            groups.append((cp, cp, gid))

    # Biçim 4 (BMP <= 0xFFFF)
    segs = []
    for sc, ec, sgid in groups:
        if ec <= 0xFFFF:
            delta = (sgid - sc) & 0xFFFF
            segs.append((sc, ec, delta))
    segs.append((0xFFFF, 0xFFFF, 1))

    seg_count = len(segs)
    search_range = 2 * (1 << int(math.floor(math.log2(seg_count))))
    entry_selector = int(math.floor(math.log2(seg_count)))
    range_shift = seg_count * 2 - search_range

    end_counts = [s[1] for s in segs]
    start_counts = [s[0] for s in segs]
    id_deltas = [s[2] for s in segs]
    id_range_offs = [0] * seg_count

    sub4_body = (
        struct.pack(">%dH" % seg_count, *end_counts)
        + struct.pack(">H", 0)
        + struct.pack(">%dH" % seg_count, *start_counts)
        + struct.pack(">%dH" % seg_count, *id_deltas)
        + struct.pack(">%dH" % seg_count, *id_range_offs)
    )
    sub4_len = 14 + len(sub4_body)
    sub4 = struct.pack(
        ">HHHHHHH", 4, sub4_len, 0, seg_count * 2, search_range, entry_selector, range_shift
    ) + sub4_body
    sub4 = pad4(sub4)

    # Biçim 12
    sub12_len = 16 + len(groups) * 12
    sub12 = struct.pack(">HHIII", 12, 0, sub12_len, 0, len(groups))
    for sc, ec, sgid in groups:
        sub12 += struct.pack(">III", sc, ec, sgid)

    off4 = 4 + 2 * 8
    off12 = off4 + len(sub4)
    hdr = struct.pack(">HH", 0, 2)
    hdr += struct.pack(">HHI", 3, 1, off4)
    hdr += struct.pack(">HHI", 3, 10, off12)
    return hdr + sub4 + sub12


def build_kern_table(cp_to_gid):
    """Tipografik kerning çiftlerini (kern biçim 0, sıralı) üretir."""
    char_pairs = [
        # Büyük harf + büyük/küçük harf çiftleri
        ("A", "V", -55), ("A", "W", -45), ("A", "Y", -60), ("A", "T", -55),
        ("A", "O", -25), ("A", "C", -25), ("A", "G", -25), ("A", "Ğ", -25), ("A", "Ö", -25),
        ("A", "v", -30), ("A", "w", -25), ("A", "y", -30), ("A", "g", -20), ("A", "ğ", -25),
        ("T", "a", -55), ("T", "e", -55), ("T", "o", -55), ("T", "ö", -45),
        ("T", "u", -45), ("T", "ü", -35), ("T", "c", -55), ("T", "ç", -55),
        ("T", "s", -45), ("T", "ş", -45), ("T", "r", -40), ("T", "ı", -30),
        ("T", "A", -55), ("T", ".", -60), ("T", ",", -60),
        ("V", "a", -50), ("V", "e", -50), ("V", "o", -50), ("V", "ö", -40),
        ("V", "u", -40), ("V", "ı", -30), ("V", "A", -55), ("V", ".", -65), ("V", ",", -65),
        ("W", "a", -40), ("W", "e", -40), ("W", "o", -40), ("W", "A", -45),
        ("Y", "a", -60), ("Y", "e", -60), ("Y", "o", -60), ("Y", "ö", -45),
        ("Y", "u", -45), ("Y", "ı", -35), ("Y", "A", -60), ("Y", ".", -70), ("Y", ",", -70),
        ("P", "a", -35), ("P", "e", -35), ("P", "o", -35), ("P", "A", -50),
        ("P", ".", -75), ("P", ",", -75),
        ("F", "a", -40), ("F", "e", -40), ("F", "o", -40), ("F", "A", -45),
        ("F", ".", -65), ("F", ",", -65),
        ("L", "T", -55), ("L", "V", -55), ("L", "W", -45), ("L", "Y", -60), ("L", "'", -55),
        # Türkçe metin akışı çiftleri ("Ağaç kökü İğdır'da şöyle" dahil)
        ("k", "o", -20), ("k", "ö", -20), ("k", "e", -20), ("k", "a", -15),
        ("r", "o", -20), ("r", "e", -20), ("r", "a", -20), ("r", ".", -45), ("r", ",", -45),
        ("r", "'", -30), ("d", "a", -15), ("ş", "ö", -18), ("y", "l", -12),
        ("v", "a", -25), ("v", "e", -25), ("v", "o", -25), ("v", ".", -50), ("v", ",", -50),
        ("y", "a", -25), ("y", "e", -25), ("y", "o", -25), ("y", ".", -50), ("y", ",", -50),
    ]
    gid_pairs = {}
    for lc, rc, val in char_pairs:
        l_cp = ord(lc)
        r_cp = ord(rc)
        if l_cp in cp_to_gid and r_cp in cp_to_gid:
            key = (cp_to_gid[l_cp], cp_to_gid[r_cp])
            gid_pairs[key] = val

    sorted_pairs = sorted(gid_pairs.items(), key=lambda kv: (kv[0][0] << 16) | kv[0][1])
    n = len(sorted_pairs)
    p2 = 1 << int(math.floor(math.log2(max(n, 1))))
    search_range = p2 * 6
    entry_selector = int(math.floor(math.log2(max(n, 1))))
    range_shift = (n - p2) * 6

    sub_len = 14 + n * 6
    sub = struct.pack(">HHHHHHH", 0, sub_len, 0x0001, n, search_range, entry_selector, range_shift)
    for (lgid, rgid), val in sorted_pairs:
        sub += struct.pack(">HHh", lgid, rgid, val)
    return struct.pack(">HH", 0, 1) + sub, n


def build_matoki_sans_ttf():
    glyph_dict = define_all_glyphs()
    verify_glyph_holes_and_winding(glyph_dict)

    # Glif 0: .notdef (dış kutu + iç delik)
    notdef_contours = [
        rect_cw(60, 0, 440, CAP_H),
        rect_ccw(130, 74, 370, CAP_H - 74),
    ]
    glyph_records = [(0, 500, notdef_contours)]

    sorted_cps = sorted(glyph_dict.keys())
    cp_to_gid = {}
    for cp in sorted_cps:
        gid = len(glyph_records)
        cp_to_gid[cp] = gid
        adv, contours = glyph_dict[cp]
        glyph_records.append((cp, adv, contours))

    num_glyphs = len(glyph_records)
    glyf_chunks = []
    offs = [0]
    hmetrics = []
    all_xmin, all_ymin, all_xmax, all_ymax = 0, 0, 0, 0

    for gid, (cp, adv, contours) in enumerate(glyph_records):
        raw, bbox = encode_simple_glyph(contours)
        glyf_chunks.append(raw)
        offs.append(offs[-1] + len(raw))
        lsb = bbox[0] if contours else 0
        hmetrics.append((adv, lsb))
        if contours:
            all_xmin = min(all_xmin, bbox[0])
            all_ymin = min(all_ymin, bbox[1])
            all_xmax = max(all_xmax, bbox[2])
            all_ymax = max(all_ymax, bbox[3])

    glyf = b"".join(glyf_chunks)
    long_loca = offs[-1] >= 0x20000
    if long_loca:
        loca = struct.pack(">%dI" % len(offs), *offs)
    else:
        loca = struct.pack(">%dH" % len(offs), *(o // 2 for o in offs))

    head = struct.pack(
        ">IIIIHHqqhhhhHHhhh",
        0x00010000,
        0x00010000,
        0,
        0x5F0F3CF5,
        0x000B,
        UPM,
        0,
        0,
        all_xmin,
        all_ymin,
        all_xmax,
        all_ymax,
        0,
        8,
        2,
        1 if long_loca else 0,
        0,
    )

    max_adv = max(adv for adv, _ in hmetrics)
    min_lsb = min(lsb for _, lsb in hmetrics)
    hhea = struct.pack(
        ">IhhhHhhhhhhhhhhhH",
        0x00010000,
        ASCENT,
        DESCENT,
        LINE_GAP,
        max_adv,
        min_lsb,
        0,
        all_xmax,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        num_glyphs,
    )

    hmtx = b"".join(struct.pack(">Hh", adv, lsb) for adv, lsb in hmetrics)
    maxp = struct.pack(">IH", 0x00010000, num_glyphs) + b"\x00" * 26
    cmap = build_cmap_tables(cp_to_gid)
    kern, num_kern = build_kern_table(cp_to_gid)

    tables = {
        b"cmap": cmap,
        b"glyf": glyf,
        b"head": head,
        b"hhea": hhea,
        b"hmtx": hmtx,
        b"kern": kern,
        b"loca": loca,
        b"maxp": maxp,
    }

    nt = len(tables)
    p2 = 1 << int(math.floor(math.log2(nt)))
    search_range = p2 * 16
    entry_selector = int(math.floor(math.log2(nt)))
    range_shift = nt * 16 - search_range

    out = struct.pack(">IHHHH", 0x00010000, nt, search_range, entry_selector, range_shift)
    data_off = 12 + nt * 16
    body = b""
    for tag in sorted(tables.keys()):
        raw = tables[tag]
        out += struct.pack(">4sIII", tag, 0, data_off + len(body), len(raw))
        body += pad4(raw)

    return out + body, num_glyphs, num_kern


def write_c_embed(ttf_bytes):
    os.makedirs(os.path.dirname(H_OUT_PATH), exist_ok=True)
    h_content = f"""/* font/matoki_sans_data.h — Gömülü "Matoki Sans" TrueType (.ttf) yazı tipi ikili verisi.
 * tools/make_matoki_sans.py tarafından otomatik üretilmiştir; dış yazı tipi bağımlılığı yoktur.
 * Doğrudan ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font) ile kullanılır.
 */
#ifndef FONT_MATOKI_SANS_DATA_H
#define FONT_MATOKI_SANS_DATA_H

#include "core/types.h"

#define MATOKI_SANS_TTF_LEN {len(ttf_bytes)}u

extern const u8  g_matoki_sans_ttf[MATOKI_SANS_TTF_LEN];
extern const u32 g_matoki_sans_ttf_len;

#endif
"""
    with open(H_OUT_PATH, "w", encoding="utf-8") as f:
        f.write(h_content)

    rows = []
    for i in range(0, len(ttf_bytes), 16):
        chunk = ttf_bytes[i : i + 16]
        rows.append("    " + ", ".join(f"0x{b:02X}u" for b in chunk))
    array_body = ",\n".join(rows)

    c_content = f"""/* font/matoki_sans_data.c — Gömülü "Matoki Sans" TrueType (.ttf) bayt dizisi.
 * tools/make_matoki_sans.py tarafından otomatik üretilmiştir; elle düzenlemeyiniz.
 * Toplam boyut: {len(ttf_bytes)} bayt.
 */
#include "font/matoki_sans_data.h"

const u32 g_matoki_sans_ttf_len = MATOKI_SANS_TTF_LEN;

const u8 g_matoki_sans_ttf[MATOKI_SANS_TTF_LEN] = {{
{array_body}
}};
"""
    with open(C_OUT_PATH, "w", encoding="utf-8") as f:
        f.write(c_content)


def main():
    ttf_bytes, num_glyphs, num_kern = build_matoki_sans_ttf()
    os.makedirs(os.path.dirname(TTF_OUT_PATH), exist_ok=True)
    with open(TTF_OUT_PATH, "wb") as f:
        f.write(ttf_bytes)
    write_c_embed(ttf_bytes)
    print(
        f"uretildi: MatokiSans.ttf ({len(ttf_bytes)} bayt, {num_glyphs} glif, {num_kern} kern cifti) "
        f"+ src/font/matoki_sans_data.c/.h"
    )


if __name__ == "__main__":
    main()
