# Third-party notices

The noncommercial license applies to this project's original code and contributions, not to upstream rights.

- `main/board_display.c` and its header derive from the ESP-Mosaico Hello World display adapter; Apache-2.0, retained separately in LICENSES/Apache-2.0.txt. Upstream ESP-Mosaico / Espressif notices are preserved.
- ESP-Mosaico utils, BSP and Raylib Lite are external Git submodules. Each keeps its own upstream license; this project does not relicense them. SDK/registry dependencies likewise retain their licenses.
- Noto Sans SC / Source Han-derived font: SIL OFL 1.1. Full notice: projects/learning_assistant/ui/fonts/OFL.txt.
- DejaVu / Bitstream Vera glyphs: original font notices at projects/learning_assistant/ui/fonts/LICENSE. LearningSans combines the supplied Chinese font with missing DejaVu math glyphs; these font licenses remain applicable. They are not placed under the noncommercial software license.
- Glass icons and project cover were generated with ChatGPT; the cover is labelled AI visual illustration. Some app shapes/waves are authored as SVG. No third-party trademark or brand affiliation is granted.
- Device photographs are cropped demonstration materials supplied by the project owner. The illustrative selection screenshot is explicitly test data. Raw footage, room conversations and personal worksheet headers are excluded.

Prebuilt firmware links upstream SDK/components and includes licensed fonts. Its upstream portions retain their licenses; the application author's noncommercial terms apply to the original learning-assistant portions.

- word_teacher 特有部分：词库 blob 与读取层、发音缓存（NAND + LittleFS）、四选一
  出题器移植自 english-dict（MIT, Copyright (c) 2026 Leo）；字体子集脚本改编自
  words_app 的 build_font.py。词书内容（tools 生成）随 english-dict 的内容许可分发。
- 字体：LearningSans 为 Noto Sans SC（SIL OFL 1.1，见 ui/fonts/OFL.txt）与
  DejaVu 字形（ui/fonts/LICENSE）的合并与子集；内嵌副本由 tools/build_font.py
  从 ui/fonts/LearningSans-Full.ttf 生成。
