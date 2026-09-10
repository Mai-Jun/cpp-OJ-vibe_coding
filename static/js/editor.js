// editor.js — 简单 C++ 代码编辑器（自带高亮 + Tab 缩进支持，Phase 4）。
// 基于 textarea 覆盖层实现：底层 textarea 承接输入，上层 <pre> 做语法高亮，
// 通过滚动同步保持一致。仅做最基础的 C++ 关键字/字符串/注释/数字/预处理着色。

(function () {
  "use strict";

  const KEYWORDS = new Set(
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t "
    + "char32_t class compl concept const consteval constexpr constinit const_cast continue co_await "
    + "co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern "
    + "false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr "
    + "operator or or_eq private protected public register reinterpret_cast requires return short signed "
    + "sizeof static static_assert static_cast struct switch template this thread_local throw true try "
    + "typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq"
    .split(/\s+/)
  );

  const TYPES = new Set(
    "string vector map unordered_map set unordered_set pair deque list stack queue priority_queue "
    + "array tuple optional shared_ptr unique_ptr size_t int64_t uint64_t auto long long"
    .split(/\s+/)
  );

  // 单行词法高亮（朴素正则，适用于无引号/无注释冲突的情况）。
  function highlightLine(line) {
    let html = "";
    let i = 0;
    const n = line.length;
    const push = (t) => { html += t; };
    // 按 token 扫描：字符串 / 字符 / 注释 / 预处理 / 数字 / 标识符 / 其余
    while (i < n) {
      const ch = line[i];

      // 块注释或行注释
      if (ch === "/" && line[i + 1] === "/") {
        push('<span class="tok-comment">' + esc(line.slice(i)) + "</span>");
        break;
      }
      if (ch === "/" && line[i + 1] === "*") {
        push('<span class="tok-comment">' + esc(line.slice(i)) + "</span>");
        break;
      }

      // 字符串
      if (ch === '"') {
        let j = i + 1;
        while (j < n && line[j] !== '"') {
          if (line[j] === "\\") j++;
          j++;
        }
        if (j >= n) j = n - 1;
        push('<span class="tok-str">' + esc(line.slice(i, j + 1)) + "</span>");
        i = j + 1;
        continue;
      }

      // 字符字面量
      if (ch === "'") {
        let j = i + 1;
        while (j < n && line[j] !== "'") {
          if (line[j] === "\\") j++;
          j++;
        }
        if (j >= n) j = n - 1;
        push('<span class="tok-char">' + esc(line.slice(i, j + 1)) + "</span>");
        i = j + 1;
        continue;
      }

      // 预处理指令（行首 #）
      if (ch === "#") {
        push('<span class="tok-pre">' + esc(line.slice(i)) + "</span>");
        break;
      }

      // 标识符 / 关键字 / 数字
      if (/[A-Za-z_]/.test(ch)) {
        let j = i;
        while (j < n && /[A-Za-z0-9_]/.test(line[j])) j++;
        const word = line.slice(i, j);
        if (KEYWORDS.has(word)) {
          push('<span class="tok-kw">' + esc(word) + "</span>");
        } else if (TYPES.has(word)) {
          push('<span class="tok-type">' + esc(word) + "</span>");
        } else {
          push(esc(word));
        }
        i = j;
        continue;
      }

      if (/[0-9]/.test(ch) && (i === 0 || !/[A-Za-z0-9_]/.test(line[i - 1]))) {
        let j = i;
        while (j < n && /[0-9A-Fa-fxXoObBeE.+-]/.test(line[j])) j++;
        push('<span class="tok-num">' + esc(line.slice(i, j)) + "</span>");
        i = j;
        continue;
      }

      // 其余字符（运算符、标点）
      push(esc(ch));
      i++;
    }
    return html;
  }

  function highlightCode(code) {
    const lines = code.replace(/\r\n/g, "\n").split("\n");
    const out = [];
    for (const line of lines) {
      out.push(highlightLine(line));
    }
    return out.join("\n");
  }

  // 在容器内创建编辑器。返回 {getValue, setValue}。
  function createEditor(container, opts) {
    opts = opts || {};
    const value = opts.value || "";
    const placeholder = opts.placeholder || "";

    container.classList.add("oj-editor");
    container.innerHTML =
      '<pre class="oj-editor-hl" aria-hidden="true"></pre>' +
      '<textarea class="oj-editor-input" spellcheck="false" autocomplete="off" ' +
      'autocapitalize="off" autocorrect="off" placeholder="' + esc(placeholder) + '"></textarea>' +
      '<div class="oj-editor-ln" aria-hidden="true"></div>';

    const pre = container.querySelector(".oj-editor-hl");
    const ta = container.querySelector(".oj-editor-input");
    const ln = container.querySelector(".oj-editor-ln");

    ta.value = value;
    render();

    function render() {
      pre.innerHTML = highlightCode(ta.value) + "\n";
      renderLineNumbers();
    }

    function renderLineNumbers() {
      const count = ta.value.split("\n").length;
      let s = "";
      for (let i = 1; i <= count; i++) s += i + "\n";
      ln.textContent = s;
    }

    function syncScroll() {
      pre.scrollTop = ta.scrollTop;
      pre.scrollLeft = ta.scrollLeft;
      ln.scrollTop = ta.scrollTop;
    }

    ta.addEventListener("input", render);
    ta.addEventListener("scroll", syncScroll);
    ta.addEventListener("keydown", (e) => {
      if (e.key === "Tab") {
        e.preventDefault();
        const start = ta.selectionStart, end = ta.selectionEnd;
        const indent = opts.tabSize || 2;
        const ins = " ".repeat(indent);
        if (start === end) {
          ta.setRangeText(ins, start, end, "end");
        } else {
          // 多行选择：整行缩进
          const lines = ta.value.slice(0, start).split("\n");
          const lineStart = start - lines[lines.length - 1].length;
          const leading = ta.value.slice(0, lineStart);
          const selected = ta.value.slice(lineStart, end);
          const indented = selected.split("\n").map(l => ins + l).join("\n");
          ta.value = leading + indented + ta.value.slice(end);
          ta.selectionStart = lineStart;
          ta.selectionEnd = lineStart + indented.length;
        }
        render();
        syncScroll();
      }
    });

    return {
      getValue: () => ta.value,
      setValue: (v) => { ta.value = v; render(); syncScroll(); },
      focus: () => ta.focus(),
      container,
    };
  }

  window.CodeEditor = { createEditor };
})();
