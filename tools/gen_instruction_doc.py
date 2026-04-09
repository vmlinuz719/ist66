#!/usr/bin/env python3
"""Generate printable HTML pages with instruction format diagrams from a JSON description."""

import json
import sys
import os

def generate_field_diagram(fields, word_bits):
    """Generate an SVG diagram for one instruction format."""
    width = 720
    left_margin = 20
    usable = width - 2 * left_margin
    bit_w = usable / word_bits
    box_y = 30
    box_h = 40

    lines = []
    lines.append(f'<svg width="{width}" height="100" xmlns="http://www.w3.org/2000/svg"'
                 f' style="font-family: monospace; font-size: 12px;">')
    # Crosshatch pattern for unused fields
    lines.append('<defs><pattern id="crosshatch" width="8" height="8"'
                 ' patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
                 '<line x1="0" y1="0" x2="0" y2="8" stroke="#999" stroke-width="1"/>'
                 '</pattern></defs>')

    # Bit number ticks along the top
    bit = 0
    for f in fields:
        x = left_margin + bit * bit_w
        # Left edge label
        lines.append(f'<text x="{x + 2}" y="{box_y - 4}" text-anchor="start"'
                     f' font-size="9" fill="#555">{bit}</text>')
        bit += f["bits"]
    # Final bit number at right edge
    lines.append(f'<text x="{left_margin + bit * bit_w - 2}" y="{box_y - 4}"'
                 f' text-anchor="end" font-size="9" fill="#555">{bit - 1}</text>')

    # Draw field boxes
    bit = 0
    colors = ["#e8f0fe", "#fff3e0"]
    for i, f in enumerate(fields):
        x = left_margin + bit * bit_w
        w = f["bits"] * bit_w
        unused = f.get("unused", False)
        if unused:
            lines.append(f'<rect x="{x}" y="{box_y}" width="{w}" height="{box_h}"'
                         f' fill="#ddd" stroke="#333" stroke-width="1.5"/>')
            lines.append(f'<rect x="{x}" y="{box_y}" width="{w}" height="{box_h}"'
                         f' fill="url(#crosshatch)" stroke="none"/>')
            cx = x + w / 2
            bits_label = f'{f["bits"]}b'
            lines.append(f'<text x="{cx}" y="{box_y + box_h + 14}" text-anchor="middle"'
                         f' font-size="10" fill="#666">{bits_label}</text>')
        else:
            fill = colors[i % 2]
            lines.append(f'<rect x="{x}" y="{box_y}" width="{w}" height="{box_h}"'
                         f' fill="{fill}" stroke="#333" stroke-width="1.5"/>')
            # Field name centered in box
            cx = x + w / 2
            cy = box_y + box_h / 2
            label = f["name"]
            font_size = 14 if w > 40 else 11
            lines.append(f'<text x="{cx}" y="{cy + 5}" text-anchor="middle"'
                         f' font-size="{font_size}" font-weight="bold">{label}</text>')
            # Bit count below the box
            bits_label = f'{f["bits"]}b'
            lines.append(f'<text x="{cx}" y="{box_y + box_h + 14}" text-anchor="middle"'
                         f' font-size="10" fill="#666">{bits_label}</text>')
        bit += f["bits"]

    lines.append('</svg>')
    return '\n'.join(lines)


def generate_html(spec):
    """Generate a complete HTML document from the JSON spec."""
    word_bits = spec["word_bits"]
    title = spec.get("title", "Instruction Formats")

    parts = []
    parts.append(f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
  @page {{
    size: letter;
    margin: 0.75in;
  }}
  body {{
    font-family: "Helvetica Neue", Helvetica, Arial, sans-serif;
    max-width: 7.5in;
    margin: 0 auto;
    padding: 20px;
    color: #222;
  }}
  h1 {{
    text-align: center;
    border-bottom: 2px solid #333;
    padding-bottom: 8px;
    font-size: 22px;
  }}
  .format {{
    margin-bottom: 28px;
    break-inside: avoid;
  }}
  .format h2 {{
    font-size: 16px;
    margin: 0 0 4px 0;
  }}
  .format h2 .type {{
    color: #666;
    font-weight: normal;
  }}
  .format .desc {{
    margin: 0 0 6px 0;
    font-size: 13px;
    color: #555;
  }}
  table.fields {{
    border-collapse: collapse;
    font-size: 12px;
    margin-top: 4px;
  }}
  table.fields th {{
    text-align: left;
    padding: 2px 12px 2px 0;
    border-bottom: 1px solid #ccc;
  }}
  table.fields td {{
    padding: 2px 12px 2px 0;
    vertical-align: top;
  }}
  table.values {{
    border-collapse: collapse;
    font-size: 11px;
    margin-top: 4px;
  }}
  table.values th {{
    text-align: left;
    padding: 1px 8px 1px 0;
    border-bottom: 1px solid #ddd;
  }}
  table.values td {{
    padding: 1px 8px 1px 0;
  }}
  p.notes {{
    font-size: 12px;
    color: #333;
    margin: 4px 0;
    margin-left: 12px;
    line-height: 1.4;
  }}
  p.notes:first-of-type {{
    margin-top: 12px;
  }}
  @media print {{
    body {{ padding: 0; }}
  }}
</style>
</head>
<body>
<h1>{title}</h1>
<p style="text-align:center; font-size:13px; color:#666;">
  Each instruction is {word_bits} bits (one word).
</p>
''')

    for fmt in spec["formats"]:
        name = fmt["name"]
        desc = fmt.get("description", "")
        fields = fmt["fields"]
        total = sum(f["bits"] for f in fields)
        if total != word_bits:
            print(f"Warning: format '{name}' has {total} bits, expected {word_bits}",
                  file=sys.stderr)

        svg = generate_field_diagram(fields, word_bits)

        parts.append(f'<div class="format">')
        parts.append(f'<h2>{desc} <span class="type">({name})</span></h2>')
        parts.append(svg)
        parts.append('<table class="fields"><tr><th>Field</th><th>Bits</th><th>Description</th></tr>')
        for f in fields:
            if f.get("unused", False):
                continue
            desc_html = f.get("description", "")
            if "values" in f:
                desc_html += '<table class="values"><tr><th>Value</th><th>Name</th><th>Meaning</th></tr>'
                for v in f["values"]:
                    desc_html += (f'<tr><td>{v["value"]}</td>'
                                  f'<td>{v["name"]}</td>'
                                  f'<td>{v["description"]}</td></tr>')
                desc_html += '</table>'
            elif "values_alt" in f:
                desc_html += '<table class="values"><tr><th>Value</th><th>Meaning</th></tr>'
                for v in f["values_alt"]:
                    desc_html += (f'<tr><td>{v["value"]}</td>'
                                  f'<td>{v["description"]}</td></tr>')
                desc_html += '</table>'
            parts.append(f'<tr><td><b>{f["name"]}</b></td>'
                         f'<td>{f["bits"]}</td>'
                         f'<td>{desc_html}</td></tr>')
        parts.append('</table>')
        if "notes" in fmt:
            paragraphs = []
            current = []
            for note in fmt["notes"]:
                if note.endswith('\n'):
                    current.append(note.rstrip('\n'))
                    paragraphs.append(' '.join(current))
                    current = []
                else:
                    current.append(note)
            if current:
                paragraphs.append(' '.join(current))
            for p in paragraphs:
                parts.append(f'<p class="notes">{p}</p>')
        parts.append('</div>')

    parts.append('</body>\n</html>')
    return '\n'.join(parts)


def main():
    if len(sys.argv) < 2:
        json_path = os.path.join(os.path.dirname(__file__), "instruction_formats.json")
    else:
        json_path = sys.argv[1]

    output_path = sys.argv[2] if len(sys.argv) >= 3 else None

    with open(json_path) as f:
        spec = json.load(f)

    html = generate_html(spec)

    if output_path:
        with open(output_path, 'w') as f:
            f.write(html)
        print(f"Wrote {output_path}", file=sys.stderr)
    else:
        print(html)


if __name__ == "__main__":
    main()
