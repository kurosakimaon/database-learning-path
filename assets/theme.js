/* 三态主题控件：跟随系统 / 浅色 / 深色。
   页面 <head> 里还有一段内联引导，在首屏渲染前就把 data-theme 定好，
   所以刷新/切换都不会闪一下深色。这里只负责把控件挂上、记住选择，
   并让「跟随系统」在系统设置变化时实时生效。
   选择存在 localStorage 的 lh-theme：auto 时删键，light/dark 时写值。 */
(function (global) {
  "use strict";

  var KEY = "lh-theme";

  function mode() {
    var p = document.documentElement.getAttribute("data-theme-pref");
    return p === "light" || p === "dark" ? p : "auto";
  }

  function system() {
    try {
      return global.matchMedia && matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
    } catch (e) { return "dark"; }
  }

  function apply(pref, persist) {
    var m = pref === "light" || pref === "dark" ? pref : "auto";
    var theme = m === "auto" ? system() : m;
    document.documentElement.setAttribute("data-theme", theme);
    document.documentElement.setAttribute("data-theme-pref", m);
    if (persist) {
      try {
        if (m === "auto") localStorage.removeItem(KEY);
        else localStorage.setItem(KEY, m);
      } catch (e) {}
    }
    var sel = document.getElementById("lhThemeSel");
    if (sel) sel.value = m;
    try {
      global.dispatchEvent(new CustomEvent("lh-theme-change", { detail: { theme: theme, mode: m } }));
    } catch (e) {}
    return theme;
  }

  function el(tag, attrs, kids) {
    var n = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      // text 不是 option 的内容属性：写成 attribute 浏览器不认，三个选项全是空的，
      // 控件就只剩一个下拉箭头。安防根仓那套 LH.el 早把 text 当特殊键走 textContent，
      // 这里对齐——否则右下角看不出自己选的是哪一档。
      if (k === "text") n.textContent = attrs[k];
      else n.setAttribute(k, attrs[k]);
    }
    if (kids) kids.forEach(function (c) { n.appendChild(c); });
    return n;
  }

  function init() {
    if (document.getElementById("lhThemeSel")) return;

    var sel = el("select", { id: "lhThemeSel", "aria-label": "主题模式", title: "主题：跟随系统 / 浅色 / 深色" }, [
      el("option", { value: "auto", text: "跟随系统" }),
      el("option", { value: "light", text: "浅色" }),
      el("option", { value: "dark", text: "深色" })
    ]);
    sel.value = mode();
    sel.addEventListener("change", function () { apply(sel.value, true); });

    var box = el("div", { class: "lh-theme-sw", role: "group", "aria-label": "主题切换" }, [
      el("span", { class: "tico", text: "◐", "aria-hidden": "true" }),
      sel
    ]);
    document.body.appendChild(box);

    try {
      var mq = global.matchMedia && matchMedia("(prefers-color-scheme: light)");
      if (mq && mq.addEventListener) mq.addEventListener("change", function () {
        if (mode() === "auto") apply("auto", false);
      });
    } catch (e) {}
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();

  global.LhTheme = { apply: apply, mode: mode, system: system };
})(window);
