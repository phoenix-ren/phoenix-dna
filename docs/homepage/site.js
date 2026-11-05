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
