"""Repack the optional Field Console authoring HTML into the self-contained sketch."""
import gzip
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
sketch = root / "src/WaveshareUncannyEyes.ino"
html = (root / "ui/console.html").read_text(encoding="utf-8-sig").strip()
payload = gzip.compress(html.encode("utf-8"), mtime=0)
data = "\n".join(
    "  " + ", ".join(f"0x{byte:02x}" for byte in payload[i:i + 20]) + ","
    for i in range(0, len(payload), 20)
)
source, count = re.subn(
    r"(constexpr uint8_t kPage\[\] PROGMEM = \{\n).*?(\n\};)",
    lambda match: match[1] + data + match[2],
    sketch.read_text(encoding="utf-8"),
    count=1,
    flags=re.S,
)
if count != 1:
    raise SystemExit("Could not find the embedded console asset")
sketch.write_text(source, encoding="utf-8", newline="\n")
print(f"Embedded {len(payload)} compressed bytes")
