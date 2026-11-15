from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "banner-source.png"
OUTPUT = ROOT / "banner.png"
SIZE = (256, 128)


def main() -> None:
    original = Image.open(SOURCE).convert("RGB")

    # The final supplied art is landscape, close to the CIA banner ratio.
    # Crop only its vertical margins to 2:1; keep the shield and wordmark intact.
    banner = ImageOps.fit(
        original,
        SIZE,
        method=Image.Resampling.LANCZOS,
        centering=(0.5, 0.5),
    )
    banner.save(OUTPUT, format="PNG", optimize=True)


if __name__ == "__main__":
    main()
