// Dark-mode switcher — a tiny, dependency-free, classic (non-module) script so
// it can be shared verbatim by the dashboard and the marketing pages.
//
// The initial theme is applied by a small inline script in <head> (before first
// paint, to avoid a flash); this file only keeps the visible toggle button(s)
// in sync and reacts to clicks. On change it dispatches a `themechange` event so
// canvas charts (which can't inherit CSS colours automatically) can redraw.
(function () {
  "use strict";
  var KEY = "hivehub-theme";
  var root = document.documentElement;

  function current() {
    return root.getAttribute("data-theme") === "dark" ? "dark" : "light";
  }

  function syncButtons(theme) {
    var dark = theme === "dark";
    var btns = document.querySelectorAll("[data-theme-toggle]");
    for (var i = 0; i < btns.length; i++) {
      var b = btns[i];
      b.textContent = dark ? "☀️" : "🌙";
      var label = dark ? "Switch to light mode" : "Switch to dark mode";
      b.setAttribute("aria-label", label);
      b.setAttribute("title", label);
      b.setAttribute("aria-pressed", String(dark));
    }
  }

  // Rendering only. Saving is a separate step that just the toggle takes: an
  // OS theme change must not turn into a stored preference, or the page would
  // follow the first OS switch and then ignore every later one.
  function apply(theme) {
    root.setAttribute("data-theme", theme);
    syncButtons(theme);
    window.dispatchEvent(new CustomEvent("themechange", { detail: { theme: theme } }));
  }

  function save(theme) {
    try { localStorage.setItem(KEY, theme); } catch (e) { /* private mode — ignore */ }
  }

  // Only the two values this script writes count as a choice.
  function savedChoice() {
    var saved;
    try { saved = localStorage.getItem(KEY); } catch (e) { saved = null; }
    return saved === "dark" || saved === "light" ? saved : null;
  }

  function init() {
    syncButtons(current());
    document.addEventListener("click", function (e) {
      var btn = e.target.closest && e.target.closest("[data-theme-toggle]");
      if (!btn) return;
      var theme = current() === "dark" ? "light" : "dark";
      save(theme);
      apply(theme);
    });
    // Follow the OS theme live, but only while the user hasn't made an explicit
    // choice this browser (nothing saved).
    if (window.matchMedia) {
      var mq = window.matchMedia("(prefers-color-scheme: dark)");
      var onChange = function (e) {
        if (savedChoice()) return;
        apply(e.matches ? "dark" : "light");
      };
      if (mq.addEventListener) mq.addEventListener("change", onChange);
      else if (mq.addListener) mq.addListener(onChange);
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
