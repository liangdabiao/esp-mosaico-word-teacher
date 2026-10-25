# word_teacher 的一次性 UI 场景改造脚本：从 learning-assistant 的 main.json
# 删除 camera/questions 页与旧首页卡片，加入词书卡片、词条页、测验页、进度页。
# 运行一次即可；保留在 tools/ 供复查与再生成。
import json, copy, io, os

here = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(here, '..', 'ui', 'main.json')
d = json.load(open(path, encoding='utf-8'))
objs = d['objects']

def subtree_indices(root, acc):
    acc.append(root)
    for i, o in enumerate(objs):
        if o.get('parent') == root:
            subtree_indices(i, acc)
    return acc

def find(pred):
    return [i for i, o in enumerate(objs) if pred(o)]

deleted = set()
for bind in ('camera', 'questions'):
    for i in find(lambda o: o.get('bind') == bind):
        for j in subtree_indices(i, []):
            deleted.add(j)
for i in range(9, 23):  # home 旧两张玻璃卡组
    deleted.add(i)

new_objs = []
remap = {}
for i, o in enumerate(objs):
    if i in deleted:
        continue
    remap[i] = len(new_objs)
    new_objs.append(o)
for o in new_objs:
    p = o.get('parent')
    if p is not None and p != -1:
        o['parent'] = remap[p]

# ---- home：6 张词书卡片 + 测验/进度入口 ----
home = next(o for o in new_objs if o.get('bind') == 'home')
home_idx = new_objs.index(home)
for i in range(6):
    col, row = i % 2, i // 2
    new_objs.append({"type": "button", "parent": home_idx, "x": 24 + col * 224,
        "y": 104 + row * 98, "w": 208, "h": 90, "radius": 18, "bg_color": "#FFFFFF68",
        "border_color": "#FFFFFF", "border_width": 1, "name": f"book_{i}",
        "callback": f"book_{i}"})
    b = len(new_objs) - 1
    new_objs.append({"type": "label", "parent": b, "x": 0, "y": 14, "w": 208, "h": 34,
        "text": "", "font_size": 22, "fg_color": "#102632", "text_align": "center",
        "name": f"book_name_{i}", "bind": f"book_name_{i}"})
    new_objs.append({"type": "label", "parent": b, "x": 0, "y": 52, "w": 208, "h": 26,
        "text": "", "font_size": 15, "fg_color": "#70818B", "text_align": "center",
        "name": f"book_prog_{i}", "bind": f"book_prog_{i}"})
for name, cb, text, x in (("quiz_open", "quiz_open", "四选一测验", 24),
                          ("progress_open", "progress_open", "学习进度", 248)):
    new_objs.append({"type": "button", "parent": home_idx, "x": x, "y": 400, "w": 208,
        "h": 44, "radius": 14, "bg_color": "#009C92", "name": name, "callback": cb})
    b = len(new_objs) - 1
    new_objs.append({"type": "label", "parent": b, "x": 0, "y": 0, "w": 208, "h": 44,
        "text": text, "font_size": 20, "fg_color": "#FFFFFF", "text_align": "center"})

back_tpl = next(o for o in new_objs if o.get('callback') == 'back_91')
back_label_tpl = next(o for o in new_objs
                      if o.get('parent') == new_objs.index(back_tpl) and o.get('type') == 'label')

_back_seq = 0
def add_back(parent_idx):
    global _back_seq
    b = copy.deepcopy(back_tpl); b['parent'] = parent_idx
    _back_seq += 1
    b['name'] = f'back_new{_back_seq}'
    b['callback'] = f'back_new{_back_seq}'
    new_objs.append(b); bi = len(new_objs) - 1
    l = copy.deepcopy(back_label_tpl); l['parent'] = bi
    new_objs.append(l)

def add_label(parent_idx, **kw):
    o = {"type": "label", "parent": parent_idx}
    o.update(kw)
    new_objs.append(o)

def add_button(parent_idx, **kw):
    o = {"type": "button", "parent": parent_idx, "radius": 14, "bg_color": "#FFFFFF68",
         "border_color": "#FFFFFF", "border_width": 1}
    o.update(kw)
    new_objs.append(o)
    return len(new_objs) - 1

GRAY = "#70818B"; INK = "#102632"; TEAL = "#009C92"

# ---- 词条页 ----
new_objs.append({"type": "container", "parent": -1, "x": 0, "y": 0, "w": 480, "h": 480,
    "bg_color": "#F3F6F8", "radius": 20, "name": "word", "bind": "word",
    "bind_target": "visible", "hidden": True})
W = len(new_objs) - 1
add_back(W)
add_label(W, x=120, y=30, w=240, h=24, text="", font_size=14, fg_color=GRAY,
          text_align="center", name="word_book", bind="word_book")
add_label(W, x=24, y=60, w=432, h=64, text="", font_size=50, fg_color=INK,
          text_align="center", name="word_text", bind="word_text")
add_label(W, x=24, y=126, w=432, h=28, text="", font_size=20, fg_color=GRAY,
          text_align="center", name="word_ipa", bind="word_ipa")
add_label(W, x=36, y=158, w=408, h=84, text="", font_size=23, fg_color=INK,
          name="word_trans", bind="word_trans")
for x, w_, name, cb, text in ((24, 132, "sound_word", "sound_word", "发音"),
                              (172, 132, "sound_sent", "sound_sent", "例句"),
                              (320, 136, "mark_known", "mark_known", "记住了")):
    b = add_button(W, x=x, y=248, w=w_, h=48, name=name, callback=cb)
    add_label(b, x=0, y=0, w=w_, h=48, text=text, font_size=20, fg_color=INK, text_align="center")
add_label(W, x=36, y=304, w=408, h=42, text="", font_size=19, fg_color=INK,
          name="word_sent_en", bind="word_sent_en")
add_label(W, x=36, y=346, w=408, h=38, text="", font_size=16, fg_color=GRAY,
          name="word_sent_cn", bind="word_sent_cn")
add_label(W, x=24, y=386, w=432, h=22, text="", font_size=14, fg_color=GRAY,
          text_align="center", name="word_pos", bind="word_pos")
b = add_button(W, x=24, y=414, w=80, h=52, name="word_prev", callback="word_prev")
add_label(b, x=0, y=0, w=80, h=52, text="‹", font_size=30, fg_color=TEAL, text_align="center")
b = add_button(W, x=112, y=414, w=256, h=52, name="teach", callback="teach", bg_color=TEAL)
add_label(b, x=0, y=0, w=256, h=52, text="问老师", font_size=22, fg_color="#FFFFFF", text_align="center")
b = add_button(W, x=376, y=414, w=80, h=52, name="word_next", callback="word_next")
add_label(b, x=0, y=0, w=80, h=52, text="›", font_size=30, fg_color=TEAL, text_align="center")

# ---- 测验页 ----
new_objs.append({"type": "container", "parent": -1, "x": 0, "y": 0, "w": 480, "h": 480,
    "bg_color": "#F3F6F8", "radius": 20, "name": "quiz", "bind": "quiz",
    "bind_target": "visible", "hidden": True})
Q = len(new_objs) - 1
add_back(Q)
add_label(Q, x=110, y=28, w=210, h=26, text="四选一 · 选出对应的单词", font_size=18,
          fg_color=INK, text_align="center")
add_label(Q, x=330, y=30, w=126, h=24, text="", font_size=15, fg_color=GRAY,
          text_align="center", name="quiz_progress", bind="quiz_progress")
add_label(Q, x=36, y=68, w=408, h=96, text="", font_size=25, fg_color=INK,
          name="quiz_prompt", bind="quiz_prompt")
for i in range(4):
    b = add_button(Q, x=24, y=172 + i * 58, w=432, h=52, name=f"quiz_pick_{i}",
                   callback=f"quiz_pick_{i}")
    add_label(b, x=0, y=0, w=432, h=52, text="", font_size=23, fg_color=INK,
              text_align="center", name=f"quiz_opt_{i}", bind=f"quiz_opt_{i}")
add_label(Q, x=24, y=412, w=200, h=48, text="", font_size=19, fg_color=INK,
          name="quiz_result", bind="quiz_result")
b = add_button(Q, x=248, y=412, w=208, h=48, name="quiz_next", callback="quiz_next", bg_color=TEAL)
add_label(b, x=0, y=0, w=208, h=48, text="下一题", font_size=20, fg_color="#FFFFFF", text_align="center")

# ---- 进度页 ----
new_objs.append({"type": "container", "parent": -1, "x": 0, "y": 0, "w": 480, "h": 480,
    "bg_color": "#F3F6F8", "radius": 20, "name": "progress", "bind": "progress",
    "bind_target": "visible", "hidden": True})
P = len(new_objs) - 1
add_back(P)
add_label(P, x=110, y=28, w=180, h=26, text="学习进度", font_size=20, fg_color=INK, text_align="center")
add_label(P, x=24, y=58, w=432, h=24, text="", font_size=15, fg_color=TEAL,
          text_align="center", name="prog_streak", bind="prog_streak")
for i in range(6):
    add_label(P, x=36, y=92 + i * 54, w=280, h=30, text="", font_size=19, fg_color=INK,
              name=f"prog_name_{i}", bind=f"prog_name_{i}")
    add_label(P, x=330, y=92 + i * 54, w=110, h=30, text="", font_size=17, fg_color=TEAL,
              text_align="center", name=f"prog_count_{i}", bind=f"prog_count_{i}")
b = add_button(P, x=136, y=424, w=208, h=44, name="reset", callback="reset", bg_color="#D79A32")
add_label(b, x=0, y=0, w=208, h=44, text="", font_size=18, fg_color="#FFFFFF",
          text_align="center", name="reset_label", bind="reset_label")

d['objects'] = new_objs
io.open(path, 'w', encoding='utf-8').write(json.dumps(d, ensure_ascii=False, indent=1))
print('objects:', len(new_objs))
tops = [(i, o.get('name') or o.get('bind')) for i, o in enumerate(new_objs) if o.get('parent') == -1]
print('tops:', tops)
