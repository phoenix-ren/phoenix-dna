// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The toggle button (in the page corner) cycles light/dark; the choice is
// remembered in localStorage and applied before first paint by the inline
// bootstrap that build.sh injects into <head>.
(function () {
  var button = document.getElementById("theme-toggle");
  if (!button) return;
  var current = document.documentElement.dataset.theme ||
    (matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  var render = function (theme) {
    document.documentElement.dataset.theme = theme;
    button.textContent = theme === "dark" ? "☀" : "☾";
    button.title = theme === "dark" ? "Switch to light theme" : "Switch to dark theme";
  };
  render(current);
  button.addEventListener("click", function () {
    var next = (document.documentElement.dataset.theme === "dark") ? "light" : "dark";
    localStorage.setItem("pd-theme", next);
    render(next);
  });
})();

// Copy-to-clipboard buttons for every code block: the distro cards on the
// front page, the command figures on the quickstart page, any fenced code.
(function () {
  var label = function (done) {
    return done ? "Copied ✓" : "Copy";
  };
  document.querySelectorAll("pre").forEach(function (pre) {
    var button = document.createElement("button");
    button.type = "button";
    button.className = "copy-btn";
    button.textContent = label(false);
    button.title = "Copy to clipboard";
    button.addEventListener("click", function () {
      var code = pre.querySelector("code") || pre;
      var text = code.textContent.replace(/\n+$/, "");
      var done = function () {
        button.textContent = label(true);
        button.classList.add("copied");
        setTimeout(function () {
          button.textContent = label(false);
          button.classList.remove("copied");
        }, 1600);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, done);
      } else {
        var ta = document.createElement("textarea");
        ta.value = text;
        ta.style.position = "fixed";
        ta.style.opacity = "0";
        document.body.appendChild(ta);
        ta.select();
        try { document.execCommand("copy"); } catch (e) {}
        document.body.removeChild(ta);
        done();
      }
    });
    pre.appendChild(button);
  });
})();

