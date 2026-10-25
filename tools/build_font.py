#!/usr/bin/env python3
"""Build the subsetted dynamic font for word_teacher.

LearningSans.ttf(7.4MB) 全量内嵌会让固件超出 ota_0 分区。词库、AI 回答与 UI
文案需要的字符是可枚举的：词库 blob 的字符串池是词典文本的**全量事实源**，
AI 老师回答用 GB2312 一级字表兜底，UI 静态文案来自场景 JSON。子集后一般
~3MB，DYNAMIC_FONT(FreeType) 在运行期渲染动态文本。

Usage:
    python tools/build_font.py   # 输出 ui/fonts/LearningSans.ttf(覆盖内嵌副本)
"""
from __future__ import annotations

import json
import string
import struct
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

RUNTIME_CHARS = "0123456789/%.·:：()-—+×÷=<>‹›%% " + "已掌握学习个词天连续进度清空确认取消正确错误答案发音例句老师问页共未标记记住换种方式再来一次联网需要热点配置模型密钥网络连接失败请稍后重试成功保存重启设备正在识别语音讲解选择本书开始是否重点两次真正全部清空回到首页词库可用加载"
CJK_START = 0x2E80
BLOB_PATH = Path(__file__).resolve().parent.parent / "assets/content/wt_content.bin"


def blob_pool_text(data: bytes) -> str:
    """词库 blob 的字符串池（布局见 main/wt_manifest.h 头注释）。"""
    if len(data) < 32 or data[:4] != b"WDB1":
        raise SystemExit("build_font: 词库 blob 不是 WDB1")
    total_size, = struct.unpack_from("<I", data, 28)
    pool_off, = struct.unpack_from("<I", data, 24)
    return data[pool_off:total_size].decode("utf-8", errors="replace")


def gb2312_level1_chars() -> set:
    """GB2312 一级汉字（3755 个）：AI 回答是运行期文本，无法枚举。"""
    chars: set = set()
    for row in range(0xB0, 0xD8):
        for cell in range(0xA1, 0xFF):
            try:
                chars.add(bytes([row, cell]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


CHAT_PUNCT = "，。！？、；：""''（）《》〈〉【】…—～·「」『』％＋－＝×÷"


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    data = BLOB_PATH.read_bytes()
    chars = set(RUNTIME_CHARS) | set(string.printable)
    chars |= gb2312_level1_chars() | set(CHAT_PUNCT)
    chars |= set(blob_pool_text(data))
    scene = json.loads((repo / "ui/main.json").read_text(encoding="utf-8"))
    chars |= set(json.dumps(scene, ensure_ascii=False))
    chars = {c for c in chars if c.isprintable() and c != "\ufffd"}
    print(f"build_font: {len(chars)} distinct character(s)")

    source = repo / "ui/fonts/LearningSans-Full.ttf"
    if not source.exists():
        (repo / "ui/fonts/LearningSans.ttf").rename(source)
    font = TTFont(str(source))
    options = subset.Options()
    options.layout_features = ["kern", "liga"]
    options.notdef_outline = True
    options.drop_tables += ["DSIG"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text="".join(sorted(chars)))
    subsetter.subset(font)
    out_path = repo / "ui/fonts/LearningSans.ttf"
    font.save(str(out_path))
    print(f"build_font: {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
