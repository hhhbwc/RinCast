#!/usr/bin/env python3
"""lint_output.py - 执行 AI 交稿门禁：把「编字段 / 把工具名当能力 / 无出处断言」拦在合入前。

    python glm-toolkit/lint_output.py <产出.md> [更多文件...] [--strict] [--json]

退出码: 0=PASS(可含 WARN)  1=有 FAIL  2=参数/环境问题
纯标准库；不修改任何被检查的文件。
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT / "RinCast"

NEG = ("用错", "不能", "不适用", "不可", "错误", "别用", "无法", "读不出", "量不到",
       "没有", "并非", "不是", "未", "跳过", "缺少", "缺失", "冲突", "反了",
       "站不住", "有误")
KNOWN = {
    "cmake", "git", "powershell", "Windows", "UNKNOWN", "CONTEXT.md",
    "GROUND-TRUTH.md", "SKILL.md", "TASK-CARD.md", "HANDOFF.md", "STATUS.json",
    "FACT-RECAP.md", "StreamProfile", "QueueGuard", "Setup.exe", "README.md",
    "rincast-core-tests.exe", "RinCast", "VirtualDesktop", "profile-paired.py",
    "vd_capture.py", "vd_headset_dump.py", "analyze_pcap.py", "preflight.py",
    "lint_output.py", "settings.json", "StreamerSettings.json", "Main10",
    "qprofile", "qprofile-sharp",
}
NEW_MARK = ("拟新增", "新增", "计划引入", "待实现", "当前仓库不存在", "将引入",
            "建议新增", "尚不存在", "准备加")
FUTURE = ("待", "将", "需", "计划", "等包", "等有空", "由你", "由用户",
          "会真机", "真机 A/B", "真机定", "要真机", "需你真机", "若", "如果",
          "验收里", "验收时", "之后", "再做", "再跑")
EVID_MARK = (".log", ".json", ".cpp", ".h", ".py", ":行", "行号", "§", "F0", "F1", "F2",
             "grep ", "python ", "sed ", "cat ", "head ", "powershell", "git ",
             "出处", "来源", "证据", "见 ", "见`", "日志", "cmds")
CLAIM = ("实测", "已验证", "已核实", "已确认", "已生效", "验证过", "证实", "确认无误")


def load_ctx():
    p = HERE / "out" / "context.json"
    if not p.exists():
        return None
    try:
        j = json.loads(p.read_text(encoding="utf-8"))
        return {f["id"]: f for f in j["facts"]}
    except (ValueError, OSError, KeyError):
        return None


def source_blob():
    # os.walk 带剪枝，不用 rglob：构建目录会被并发删除，rglob 会撞 TOCTOU
    # （FileNotFoundError WinError 3），见 DEVIATIONS D-003。
    parts = []
    exts = (".cpp", ".h", ".hpp", ".c", ".py", ".js", ".html", ".sh", ".cmake")
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "dist", "out")]
        for name in filenames:
            if not name.lower().endswith(exts):
                continue
            try:
                parts.append((Path(dirpath) / name).read_text(encoding="utf-8", errors="replace"))
            except OSError:
                pass
    for p in (ROOT / "measure-vd").glob("*.py"):
        try:
            parts.append(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    return "\n".join(parts)


def inline_codes(s):
    return re.findall(r"`([^`\n]+)`", s)


ABSENT_MARK = ("grep 不到", "查无", "不存在", "搜不到", "全仓无", "未实现")
# 工具包自带的规则/模板文档会引用错误句子当反例，不该被当成「产出」检查
RULE_DOC_MARK = "<!-- lint: rule-doc -->"


def resolve_exists(raw, root):
    """支持全称路径、仓库根相对路径，以及评审里常见的缩写路径（gfx/scene.cpp）。"""
    cands = [Path(raw)] if Path(raw).is_absolute() else [
        root / raw, ROOT / "RinCast" / raw, ROOT / "measure-vd" / raw]
    if any(c.exists() for c in cands):
        return True
    parts = [x for x in raw.replace("\\", "/").split("/") if x]
    if not parts:
        return False
    name = parts[-1]
    # pathlib 的 rglob 拒绝绝对/带盘符的 pattern（Windows 上会抛 NotImplementedError）
    if any(ch in name for ch in (":", "*", "?", "[", "]", "<", ">")):
        return False
    for base in (ROOT / "RinCast", ROOT / "measure-vd"):
        if not base.is_dir():
            continue
        try:
            for p in base.rglob(name):
                if any(x in p.parts for x in (".git", "build", "dist")):
                    continue
                pp = p.as_posix().split("/")
                if len(pp) >= len(parts) and pp[-len(parts):] == parts:
                    return True
        except (OSError, ValueError, NotImplementedError):
            continue
    return False


EXT_RE = r"\.(?:cpp|hpp|html|h|c|py|sh|md|json|js|txt|exe|patch|yaml|yml)\b"


def check_paths(text, root, issues):
    """反引号内的整段路径优先（可含空格，如 C:\\ProgramData\\Virtual Desktop\\...）。"""
    cands = []
    for code in inline_codes(text):
        c = code.strip()
        if not re.search(r"[\\/]", c) or not re.search(EXT_RE, c, re.I) or len(c) >= 220:
            continue
        c = re.split(r"\s+/\s+", c)[0]              # "a.cpp:1 / :2" 只取第一段
        c = re.sub(r":\d+(?:-\d+)?$", "", c)        # 去掉 :行号 / :行号-行号，保留盘符
        if "..." in c:                              # 缩写路径（headset/.../x.cpp）不查
            continue
        cands.append(c)
    exts = r"(?:cpp|hpp|html|h|c|py|sh|md|json|js|txt|exe|patch|yaml|yml)\b"
    pat = re.compile(r"(?:[A-Za-z_][\w.\-]*/)+[\w.\-]+\." + exts +
                     r"|C:/[^\s`\"'，。；）)]+\." + exts, re.I)
    for m in pat.finditer(text):
        cands.append(m.group(0).rstrip(".,;:)"))

    seen = set()
    for raw in cands:
        norm = raw.replace("\\", "/")
        if norm in seen:
            continue
        seen.add(norm)
        low = norm.lower()
        if any(x in low for x in ("patches/", "build/bin", "dist/", "out/", "glm-output/")):
            continue  # 交付物路径，不要求此刻存在
        if not resolve_exists(norm, root):
            pos = text.find(raw[:40])
            seg = text[max(0, pos - 140):pos + 180] if pos >= 0 else ""
            if any(k in seg for k in NEW_MARK + ABSENT_MARK):
                continue
            issues.append(("WARN", "PATH", "引用的路径不存在: %s" % raw))


def check_identifiers(text, blob, issues):
    seen = set()
    for code in inline_codes(text):
        c = code.strip()
        if c in seen or c in KNOWN:
            continue
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:.]{5,}", c):
            continue
        if re.fullmatch(r"[\w.\-]+\.(?:cpp|hpp|h|py|md|json|js|html|sh):\d+", c):
            continue  # 文件:行号 引用
        if re.fullmatch(r"[0-9a-f]{7,40}", c, re.I):
            continue  # commit hash
        if "." in c and re.search(r"\.(cpp|h|py|md|json|js|html|sh|exe|log)$", c):
            continue
        seen.add(c)
        if c in blob:
            continue
        pos = text.find("`" + c + "`")
        seg = text[max(0, pos - 220):pos + 220] if pos >= 0 else ""
        if any(k in seg for k in NEW_MARK + ABSENT_MARK):
            continue
        issues.append(("WARN", "IDENT", "反引号标识符在源码中不存在: `%s`（拟新增请显式标注）" % c))


def check_evidence(text, issues):
    body = text
    fm = re.match(r"^---\s*\n[\s\S]*?\n---\s*\n", text)
    if fm:
        body = text[fm.end():]          # 跳过 YAML frontmatter（skill 描述不是断言）
    for line in body.splitlines():
        if not any(k in line for k in CLAIM):
            continue
        if line.lstrip().startswith("#"):
            continue  # 章节标题不是断言
        if any(k in line for k in EVID_MARK):
            continue
        if any(k in line for k in FUTURE) or any(k in line for k in NEG):
            continue  # 未来计划 / 否定句，不是「已验证」断言
        s = line.strip()
        if len(s) > 110:
            s = s[:107] + "..."
        issues.append(("WARN", "EVID", "「实测/已验证」类断言缺出处: %s" % s))


def check_sections(text, name, issues):
    n = (name + text[:120]).lower()
    is_plan = any(k in n for k in ("plan", "计划", "task", "任务", "proposal", "提案"))
    is_hand = any(k in n for k in ("handoff", "交接", "review", "评审"))
    if not (is_plan or is_hand):
        return
    need = [("验收", ("验收", "判据", "acceptance")),
            ("证据出处", ("证据", "出处", "来源", "evidence")),
            ("风险回退", ("风险", "回退", "回滚", "rollback")),
            ("未做/UNKNOWN", ("未做", "unknown", "未知", "未完成", "不做"))]
    for label, keys in need:
        if not any(k in text.lower() for k in keys):
            issues.append(("FAIL", "SECTION", "计划书/交接类文档缺必需章节: %s" % label))
    if not re.search(r"```(?:bash|sh|powershell|cmd)|`(?:python|grep|cmake|git|powershell|sed|head|cat) ",
                     text):
        issues.append(("FAIL", "SECTION", "没有任何可执行验证命令（验收判据不可机械验证）"))
    if is_hand and not re.search(r"(偏离记录|DEVIATIONS|D-\d{3})", text):
        issues.append(("WARN", "SECTION",
                       "HANDOFF 缺「偏离记录」字段（没有偏离也要写「偏离记录: 无」）"))


DEV_SECTIONS = [("类型", ("类型", "type")),
                ("查了什么/证据", ("证据", "命令", "查了", "verify", "输出")),
                ("决定", ("决定", "做了什么", "decision")),
                ("影响/回滚", ("影响", "回滚", "impact", "rollback", "风险"))]


def check_deviations(text, issues):
    """DEVIATIONS.md 每条 D-xxx 必须四要素齐全，否则视为没写清。"""
    blocks = re.split(r"^##\s+D-\d{1,4}.*$", text, flags=re.M)[1:]
    if not blocks:
        issues.append(("WARN", "DEV", "DEVIATIONS.md 里没有任何 `## D-001` 条目（空模板可以，但别谎称已记录）"))
        return
    for i, b in enumerate(blocks, 1):
        for label, keys in DEV_SECTIONS:
            if not any(k in b.lower() for k in keys):
                issues.append(("FAIL", "DEV", "D-%03d 缺要素: %s" % (i, label)))


def check_dev_refs(text, issues):
    """HANDOFF 里写的「偏离记录: D-008」必须在 DEVIATIONS.md 里真有这一条。"""
    dev = HERE / "DEVIATIONS.md"
    if not dev.exists():
        return
    known = set(re.findall(r"^##\s+(D-\d{1,4})",
                           dev.read_text(encoding="utf-8", errors="replace"), re.M))
    if not known:
        return
    for ref in sorted(set(re.findall(r"\bD-\d{3,4}\b", text))):
        if ref not in known:
            issues.append(("FAIL", "DEVREF",
                           "引用了不存在的偏离记录 %s（DEVIATIONS.md 里没有这一条）" % ref))


def check_semantics(text, ctx, issues, is_review=False, blob=""):
    """项目专属语义规则：基于 preflight 实测值，抓已被证实的历史误判。"""
    if ctx is None:
        issues.append(("WARN", "CTX", "未找到 out/context.json，语义规则未运行；先跑 preflight.py"))
        return
    if is_review:
        # 评审文档的职责是引用并批评错误句子，不该因为引用了错误句子被打回
        return

    def val(fid, key, default=None):
        f = ctx.get(fid)
        v = f["value"] if f else None
        return v.get(key, default) if isinstance(v, dict) else default

    def intval(fid):
        v = ctx.get(fid, {}).get("value")
        return v if isinstance(v, int) else 0

    def line_has(*words):
        for line in text.splitlines():
            if all(w in line for w in words) and not any(g in line for g in NEG):
                return line
        return None

    def hit(pattern):
        """按句子匹配，且该句不含否定词——避免把「未经真机验证」判成「声称已验证」。"""
        for s in re.split(r"[。\n；;]", text):
            if re.search(pattern, s) and not any(g in s for g in NEG):
                return s
        return None

    def blocks(t):
        """自然段（空行分隔）；Markdown 表格逐行自成一块——表格行间无空行，不切会把两行并成一段。"""
        for para in re.split(r"\n\s*\n", t):
            lines = [l for l in para.splitlines() if l.strip()]
            if lines and all(l.lstrip().startswith("|") for l in lines):
                yield from lines
            else:
                yield para

    # R1 工具名当能力
    l = line_has("profile-paired.py", "VD")
    if l and any(k in l for k in ("工具", "现成", "用", "判据", "测", "对比")):
        issues.append(("FAIL", "SEM-R1",
                       "引用 profile-paired.py 量 VD：该脚本硬编码 RinCast 日志路径，VD 不产这些日志（F26）"))

    # R2 守卫真机验证
    if (hit(r"(守卫|QueueGuard|自适应|自动码率)[^。\n]{0,40}(真机|实测|已验证|不沉底)") or
            hit(r"(真机|实测|已验证)[^。\n]{0,30}(不沉底|稳住|自动爬回)")):
        if val("F15", "adaptive", 0) == 0:
            issues.append(("FAIL", "SEM-R2",
                           "声称守卫真机已验证，但 streamer.log 里 adaptive bitrate 出现 0 次（F15）"))

    # R3 tenBit —— 话题词与短语必须落在同一自然段（表格则同一行），跨段不算（ROUND14 B-1）
    for _blk in blocks(text):
        if (re.search(r"tenBit|10-bit|Main10", _blk) and
                any(re.search(r"(会真开|才生效|真生效|画质.{0,4}(提升|收益)|修复.{0,10}画质)", _s)
                    and not any(g in _s for g in NEG)
                    for _s in re.split(r"[。\n；;]", _blk))):
            if intval("F07") > 0:
                issues.append(("FAIL", "SEM-R3",
                               "把修 PC tenBit 死开关写成画质收益；真机日志早有 10-bit HEVC（F06/F07）"))
            break

    # R4 解码安全
    if re.search(r"0\.9|0\.8", text) and hit(r"(解码安全|都安全|硬解压力低于上限|扛得住)"):
        pd = val("F12", "prediction_decode") or {}
        sp = pd.get("over_11_1ms", 0) if isinstance(pd, dict) else 0
        if sp:
            issues.append(("WARN", "SEM-R4",
                           "用「低于硬解上限」论证解码安全；真机解码耗时 p90 %s / max %s ms，"
                           "超 11.1ms 已 %d 条（F12）" % (pd.get("p90"), pd.get("max"), sp)))

    # R5 分辨率 no-op
    if hit(r"(分辨率|vrScale)[^。\n]{0,25}(至少|拉到|提升到|保证)\s*1\.0"):
        if intval("F08") > 0:
            issues.append(("WARN", "SEM-R5",
                           "把 vrScale 提到 1.0 算进收益；真机 2176x2176 表明现场已是 1.0（F08）"))

    # R6 比 VD 却没基线
    if hit(r"(超越\s*VD|不输\s*VD|优于\s*VD|比\s*VD\s*(更|好|强))") and not re.search(
            r"(vd_headset_dump|vd_capture|VD 基线|VD基线|基线采集|抓包)", text):
        issues.append(("WARN", "SEM-R6",
                       "声称与 VD 对比，但没有 VD 基线采集步骤（F25/F27/F28）"))

    # R8 偏离/质疑任务卡却没有留痕（对应 SKILL.md 第 11 条）
    # 注意：不能用 hit()——「有误」本身在 NEG 词表里，会把这条规则永久抑制掉
    if re.search(r"(偏离任务卡|超出任务卡|任务卡(有误|错了|不对|不合理)|计划书(有误|错了|不对)|"
                 r"自行(决定|调整)|未经(授权|确认))", text):
        if not re.search(r"(DEVIATIONS\.md|D-\d{3})", text):
            issues.append(("FAIL", "SEM-R8",
                           "偏离/质疑了任务卡或计划，但没有写 DEVIATIONS.md 并引用 D-xxx 编号"))

    # R7 拟新增标识符（源码里已经存在的不再提示，避免规则自己过期）
    for c in ("profileControlsVideo", "ProfileVideoHintsFor", "ApplyVideoHints",
              "QualityProfilePreset", "QualityProfileTwoPass", "QualityProfileQpMap",
              "QualityProfileFoveaFor"):
        if c in text and (not blob or c not in blob):
            seg = text[max(0, text.find(c) - 200):text.find(c) + 200]
            if not any(k in seg for k in NEW_MARK):
                issues.append(("WARN", "SEM-R7",
                               "`%s` 是计划新增项，须标注「当前仓库不存在」" % c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--strict", action="store_true", help="WARN 也算失败")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    ctx = load_ctx()
    blob = source_blob()
    all_issues = []
    skipped = []
    for f in a.files:
        p = Path(f)
        if not p.is_absolute() and not p.exists():
            p = ROOT / f          # 再试工作区根（方便从任意 cwd 调用）
        if not p.exists():
            print("ERROR: 文件不存在 %s" % f)
            return 2
        text = p.read_text(encoding="utf-8", errors="replace")
        issues = []
        if "DEVIATIONS" in p.name.upper():
            check_deviations(text, issues)
            all_issues.extend({"file": p.name, "level": lv, "kind": kd, "msg": m}
                              for lv, kd, m in issues)
            continue
        if RULE_DOC_MARK in text:
            skipped.append(p.name)
            continue
        check_paths(text, ROOT, issues)
        check_identifiers(text, blob, issues)
        check_evidence(text, issues)
        check_sections(text, p.name, issues)
        check_dev_refs(text, issues)
        check_semantics(text, ctx, issues,
                        is_review=any(k in (p.name + text[:150]).lower()
                                      for k in ("review", "评审", "critique")),
                        blob=blob)
        for lv, kind, msg in issues:
            all_issues.append({"file": p.name, "level": lv, "kind": kind, "msg": msg})

    nf = sum(1 for i in all_issues if i["level"] == "FAIL")
    nw = sum(1 for i in all_issues if i["level"] == "WARN")
    verdict = "FAIL" if nf or (a.strict and nw) else "PASS"

    if a.json:
        print(json.dumps({"verdict": verdict, "fail": nf, "warn": nw,
                          "skipped": skipped, "issues": all_issues},
                         ensure_ascii=False, indent=2))
    else:
        print("lint: %s  (FAIL=%d WARN=%d, 检查 %d 个文件%s)" %
              (verdict, nf, nw, len(a.files) - len(skipped),
               ", 跳过 %d 个规则文档" % len(skipped) if skipped else ""))
        for i in all_issues:
            print("  [%-4s] %-10s %s | %s" % (i["level"], i["kind"], i["file"], i["msg"]))
        if verdict == "PASS":
            print("  -> 可交稿；WARN 需在 HANDOFF 的「未做/UNKNOWN」里逐条解释。")
        else:
            print("  -> 打回修改。修到 PASS 再交，不要自行绕过规则。")
    return 1 if verdict == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
