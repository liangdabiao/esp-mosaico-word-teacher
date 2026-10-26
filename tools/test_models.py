# word_teacher 三条模型通路的 PC 端冒烟测试（与固件完全同构的请求形态）。
# 用法：python tools/test_models.py <api_key>
import json, sys, urllib.request

KEY = sys.argv[1] if len(sys.argv) > 1 else ""
BASE = "https://dashscope.aliyuncs.com"

def post(path, body, timeout=90):
    req = urllib.request.Request(BASE + path,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Authorization": "Bearer " + KEY})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())

# 1) AI 老师讲解（与 wt_provider.c 同构：compatible-mode + enable_thinking=false）
s, r = post("/compatible-mode/v1/chat/completions", {
    "model": "qwen-flash", "stream": False, "enable_thinking": False,
    "max_tokens": 1800,
    "messages": [
        {"role": "system", "content":
            "你是学生的英语单词老师。用简体中文讲解，英文例句地道自然，语气尊重。"
            "不使用 Markdown 表格、LaTeX、音标符号或表情。只输出面向学生的解释，回答控制在600汉字以内。"},
        {"role": "user", "content":
            "请围绕我正在学习的单词讲解，先复述这个词的核心含义，再讲词根词缀或记忆方法，"
            "然后给两个新例句（英文加中文翻译），最后点出最常见的搭配或易错点。"
            "单词：cancel。词典释义：v 取消，废除；抵消。教材例句：Our flight was cancelled.（我们的航班取消了。）"}]})
ok1 = s == 200 and r["choices"][0]["finish_reason"] == "stop" and r["choices"][0]["message"]["content"]
print(f"[teacher] HTTP {s} finish={r['choices'][0].get('finish_reason') if s==200 else '-'} "
      f"answer={r['choices'][0]['message']['content'][:120] if s==200 else r}")
if ok1:
    usage = r.get("usage", {})
    print(f"          tokens: prompt={usage.get('prompt_tokens')} completion={usage.get('completion_tokens')}")

# 2) ASR（1 秒静音 wav 的 base64，只验通路与鉴权）
import base64, struct
rate = 16000
pcm = b"\x00\x00" * rate
wav = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
       struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) +
       b"data" + struct.pack("<I", len(pcm)) + pcm)
b64 = base64.b64encode(wav).decode()
s, r = post("/compatible-mode/v1/chat/completions", {
    "model": "qwen3-asr-flash", "stream": False,
    "messages": [{"role": "user", "content": [
        {"type": "input_audio", "input_audio": {"data": b64, "format": "wav"}}]}]})
print(f"[asr]     HTTP {s} resp={json.dumps(r, ensure_ascii=False)[:200]}")

# 3) TTS（与 wt_provider_audio 同构：native 端点 + Cherry 音色）
s, r = post("/api/v1/services/aigc/multimodal-generation/generation", {
    "model": "qwen3-tts-flash",
    "input": {"text": "你好", "voice": "Cherry", "language_type": "Chinese"}})
url = ""
if s == 200:
    url = r["output"]["audio"]["url"]
print(f"[tts]     HTTP {s} audio_url={url[:90] if url else json.dumps(r, ensure_ascii=False)[:200]}")
