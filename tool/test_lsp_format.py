#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""走真实 LSP stdio 协议，验证 capabilities、格式化与 snippet 补全。

这是**断言式**测试：任一条不满足就以非 0 退出。
与 `sinter-lsp --self-test` 的分工：
  - self-test 直接调 `Server::handle_message`，测的是逻辑；
  - 这个脚本真的 `fork/exec` 一个进程，测的是**帧编码、进程生命周期、stdio 缓冲**
    这些「逻辑对了但接线错」的问题。两者都不能省。

二进制路径：环境变量 `SINTER_LSP`，否则 `/tmp/shim/sinter-lsp`。
（`/sdcard`、`/storage` 是 noexec 挂载点，必须指向 /tmp 下的可执行副本。）
"""
import json
import os
import subprocess
import sys

LSP = os.environ.get("SINTER_LSP") or "/tmp/shim/sinter-lsp"

MESSY = """module demo
import std
fun  add(a:i64,b:i64):i64{
return a+b
}
class Foo:Any{
var x:i64
Foo(v:i64){
self.x=v
}
}
fun main():void{
val n=add(1,2)
if(n>2){
println("big")
}else{
println("small")
}
}
"""


def frame(obj):
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


def read_msgs(data):
    out = []
    i = 0
    while i < len(data):
        j = data.find(b"\r\n\r\n", i)
        if j < 0:
            break
        header = data[i:j].decode("ascii", "replace")
        length = 0
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        start = j + 4
        body = data[start:start + length]
        if not body:
            break
        try:
            out.append(json.loads(body.decode("utf-8")))
        except json.JSONDecodeError:
            pass
        i = start + length
    return out


class Checker:
    def __init__(self):
        self.failed = 0
        self.total = 0

    def check(self, what, ok, detail=""):
        self.total += 1
        if ok:
            print(f"  [ OK ] {what}")
        else:
            self.failed += 1
            print(f"  [FAIL] {what}")
            if detail:
                print(f"         {detail}")


def main():
    if not os.path.isfile(LSP):
        print(f"✗ 找不到 sinter-lsp：{LSP}")
        return 1

    proc = subprocess.Popen([LSP, "--stdio"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    uri = "file:///tmp/demo.sinter"

    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"processId": None, "rootUri": None, "capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri, "languageId": "sinter",
                                     "version": 1, "text": MESSY}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting",
         "params": {"textDocument": {"uri": uri},
                    "options": {"tabSize": 4, "insertSpaces": True}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion",
         "params": {"textDocument": {"uri": uri}, "position": {"line": 15, "character": 0}}},
        {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    payload = b"".join(frame(m) for m in msgs)
    out, err = proc.communicate(payload, timeout=30)
    replies = read_msgs(out)
    by_id = {r.get("id"): r for r in replies if "id" in r}

    c = Checker()

    # ---------------- capabilities ----------------
    print("=== capabilities ===")
    caps = (by_id.get(1, {}).get("result") or {}).get("capabilities", {})
    c.check("documentFormattingProvider", caps.get("documentFormattingProvider") is True,
            str(caps.get("documentFormattingProvider")))
    c.check("documentRangeFormattingProvider",
            caps.get("documentRangeFormattingProvider") is True,
            str(caps.get("documentRangeFormattingProvider")))

    # snippetSupport 在 completionProvider.completionItem 里，但 sinter-lsp 现在的
    # 嵌套写法多包了一层 `completionItem`，两种位置都认，别被字段位置卡住。
    comp_provider = caps.get("completionProvider") or {}
    item = comp_provider.get("completionItem") or {}
    inner = item.get("completionItem") or {}
    snippet_support = item.get("snippetSupport", inner.get("snippetSupport"))
    c.check("completionProvider.snippetSupport", snippet_support is True, json.dumps(comp_provider))

    for k in ("hoverProvider", "definitionProvider", "documentSymbolProvider"):
        c.check(k, caps.get(k) is not None, str(caps.get(k)))

    # ---------------- formatting ----------------
    fmt = by_id.get(2, {}).get("result") or []
    print(f"\n=== formatting ===  edits={len(fmt)}")
    c.check("返回 1 个 edit", len(fmt) == 1, f"got {len(fmt)}")

    text = fmt[0].get("newText", "") if fmt else ""
    if text:
        print(text)
        print("--- 检查 ---")
        c.check("缩进 4 空格", "\n    return a + b\n" in text)
        c.check("运算符两侧有空格", "a + b" in text)
        c.check("`:` 后一个空格、`,` 后一个空格", "a: i64, b: i64): i64 {" in text)
        c.check("`{` 留行尾", "fun add(a: i64, b: i64): i64 {" in text)
        c.check("`}` 单独成行", "\n}" in text)
        c.check("else 与 } 同行", "} else {" in text)
        c.check("字符串未改动", '"big"' in text)
        # 反向断言：过去踩过的坑
        c.check("词之间不粘连", "moduledemo" not in text)
        c.check("`(` 前不多空格", "add( a" not in text and "if ( " not in text)
        c.check("`.` 后不多空格", "self. x" not in text)
        # 整文件替换
        rng = fmt[0].get("range", {})
        c.check("range 从 0,0 开始",
                rng.get("start", {}).get("line") == 0 and rng.get("start", {}).get("character") == 0,
                json.dumps(rng))

    # ---------------- completion / snippet ----------------
    comp = by_id.get(3, {}).get("result") or []
    snips = [x for x in comp if x.get("insertTextFormat") == 2]
    print(f"\n=== completion ===  total={len(comp)} snippets={len(snips)}")
    for s in snips[:6]:
        print(f"  {s['label']:10s} -> {s.get('insertText', '')!r}")

    c.check("模板 25 条", len(snips) == 25, f"got {len(snips)}")
    labels = {s.get("label") for s in snips}
    for want in ("module", "fun", "class", "struct", "interface", "enum", "main",
                 "ifelse", "forin", "when", "embed C", "val", "todo", "doc"):
        c.check(f"模板 {want}", want in labels)
    c.check("模板带 tabstop", any("${1:" in s.get("insertText", "") for s in snips))
    c.check("模板带 $0", any("$0" in s.get("insertText", "") for s in snips))
    c.check("模板有 documentation",
            any(s.get("documentation") for s in snips))
    c.check("模板排在关键字前（sortText 前缀 0_）",
            all((s.get("sortText") or "").startswith("0_") for s in snips))

    # ---------------- 进程礼仪 ----------------
    c.check("exit 后进程正常结束", proc.returncode == 0, f"code={proc.returncode}")
    if proc.returncode != 0 and err:
        print(err.decode("utf-8", "replace")[:400])

    print()
    if c.failed == 0:
        print(f"LSP 协议测试通过：{c.total} 项")
        return 0
    print(f"LSP 协议测试失败：{c.failed} / {c.total}")
    return 1


if __name__ == "__main__":
    sys.exit(main())