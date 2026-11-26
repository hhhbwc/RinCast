from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "headset/app/src/main/cpp/media/video_decoder.cpp"


def compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def accepts(source: str) -> bool:
    code = compact(source)
    return (
        "if(cropStatus!=AMEDIA_OK||"
        "!IsValidDecodedCrop(desc.width,desc.height,crop.left,crop.top,crop.right,crop.bottom))"
    ) in code


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    if not accepts(source):
        print("FAIL: decoded image crop does not enforce the shared bounds validator")
        return 1

    code = compact(source)
    old = "!IsValidDecodedCrop(desc.width,desc.height,crop.left,crop.top,crop.right,crop.bottom)"
    mutations = [
        code.replace(old, "crop.right<=crop.left||crop.bottom<=crop.top", 1),
        code.replace(old, "crop.left>=0&&crop.top>=0", 1),
        code.replace(old, "true", 1),
        code.replace(old, "", 1),
    ]
    for index, mutated in enumerate(mutations, 1):
        if mutated == code or accepts(mutated):
            print(f"FAIL: decoded-crop mutation {index} was not rejected")
            return 1

    print("PASS: decoded image crop uses bounds validation; 4 regressions rejected")
    print("Scope: source contract only; MediaCodec, EGL and visual output were not executed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
