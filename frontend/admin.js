const PIN_SESSION_KEY = "cybeer_admin_pin";

function pinHeaders() {
  const p = sessionStorage.getItem(PIN_SESSION_KEY);
  if (!p) {
    return {};
  }
  return { "X-Admin-Pin": p };
}

function combineHeaders(extra) {
  return { ...(extra || {}), ...pinHeaders(), "Content-Type": "application/json" };
}

function showMsg(txt) {
  const el = document.getElementById("adminMsg");
  if (el) {
    el.textContent = txt;
  }
}

async function fetchStatus() {
  const res = await fetch("/api/status", { credentials: "same-origin" });
  if (!res.ok) {
    throw new Error(`status ${res.status}`);
  }
  return res.json();
}

function applyVisibility(cfg) {
  const setup = document.getElementById("setupPanel");
  const unlock = document.getElementById("unlockPanel");
  const admin = document.getElementById("adminPanel");
  const hasPinStored = !!sessionStorage.getItem(PIN_SESSION_KEY);
  const pinConfigured = !!cfg.adminPinConfigured;

  if (!pinConfigured) {
    setup.classList.remove("hidden");
    unlock.classList.add("hidden");
    admin.classList.add("hidden");
    return;
  }

  setup.classList.add("hidden");
  if (!hasPinStored) {
    unlock.classList.remove("hidden");
    admin.classList.add("hidden");
    return;
  }

  unlock.classList.add("hidden");
  admin.classList.remove("hidden");
}

document.getElementById("setupPinForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const fd = new FormData(ev.target);
  const pin = String(fd.get("pin1") || "").trim();
  showMsg("");
  try {
    const res = await fetch("/api/admin/pin/setup", {
      method: "POST",
      headers: combineHeaders({}),
      credentials: "same-origin",
      body: JSON.stringify({ pin }),
    });
    const txt = await res.text();
    if (!res.ok) {
      showMsg(txt);
      return;
    }
    showMsg("PIN saved. Reloading.");
    sessionStorage.removeItem(PIN_SESSION_KEY);
    await fetchStatus().then(applyVisibility);
  } catch (e) {
    showMsg(String(e.message || e));
  }
});

document.getElementById("unlockForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const pin = String(document.getElementById("unlockPin").value || "").trim();
  if (pin.length < 4) {
    showMsg("PIN too short");
    return;
  }
  sessionStorage.setItem(PIN_SESSION_KEY, pin);
  await fetchStatus().then(applyVisibility);
  showMsg("PIN stored for this tab.");
});

document.getElementById("clearPinBtn").addEventListener("click", async () => {
  sessionStorage.removeItem(PIN_SESSION_KEY);
  showMsg("");
  await fetchStatus().then(applyVisibility);
});

document.getElementById("addRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const body = {
    participant_id: String(fd.get("participant_id") || "").trim(),
    duration_us: Number(fd.get("duration_us") || 0),
    finished_at: String(fd.get("finished_at") || "").trim(),
    claimed: !!fd.get("claimed"),
    tournament_match_id: String(fd.get("tournament_match_id") || "").trim(),
  };
  const id = String(fd.get("id") || "").trim();
  if (id) {
    body.id = id;
  }
  const res = await fetch("/api/admin/runs", {
    method: "POST",
    headers: combineHeaders({}),
    credentials: "same-origin",
    body: JSON.stringify(body),
  });
  const txt = await res.text();
  if (!res.ok) {
    showMsg(txt);
    return;
  }
  showMsg("Run added: " + txt);
});

document.getElementById("patchRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const id = String(fd.get("patch_id") || "").trim();
  let patch;
  try {
    patch = JSON.parse(fd.get("patch_json"));
  } catch (_) {
    showMsg("PATCH body must be valid JSON");
    return;
  }
  const url = `/api/admin/runs/${encodeURIComponent(id)}`;
  const res = await fetch(url, {
    method: "PATCH",
    headers: combineHeaders({}),
    credentials: "same-origin",
    body: JSON.stringify(patch),
  });
  const txt = await res.text();
  showMsg(res.ok ? `OK ${txt}` : txt);
});

document.getElementById("deleteRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const id = String(fd.get("delete_id") || "").trim();
  if (!window.confirm(`Delete run ${id}?`)) {
    return;
  }
  const url = `/api/admin/runs/${encodeURIComponent(id)}`;
  const res = await fetch(url, {
    method: "DELETE",
    headers: pinHeaders(),
    credentials: "same-origin",
  });
  const txt = await res.text();
  showMsg(res.ok ? `OK ${txt}` : txt);
});

document.getElementById("resetBtn").addEventListener("click", async () => {
  showMsg("");
  if (!window.confirm("Erase all runs, participants, and tournament JSON?")) {
    return;
  }
  const res = await fetch("/api/admin/data/reset", {
    method: "DELETE",
    headers: pinHeaders(),
    credentials: "same-origin",
  });
  const txt = await res.text();
  showMsg(res.ok ? `Reset OK ${txt}` : txt);
});

document.getElementById("exportForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fmt = document.getElementById("exportFmt").value;
  const res = await fetch(`/api/export?format=${encodeURIComponent(fmt)}`, {
    method: "GET",
    headers: pinHeaders(),
    credentials: "same-origin",
  });
  if (!res.ok) {
    showMsg((await res.text()) || `export failed ${res.status}`);
    return;
  }
  const blob = await res.blob();
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = fmt === "csv" ? "cybeer-export.csv" : "cybeer-export.json";
  a.click();
  URL.revokeObjectURL(a.href);
  showMsg("Download started.");
});

document.getElementById("settingsForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const body = JSON.stringify({
    ledCount: Number(fd.get("ledCount")),
    brightness: Number(fd.get("brightness")),
  });
  try {
    const res = await fetch("/api/settings", {
      method: "PUT",
      headers: combineHeaders({}),
      credentials: "same-origin",
      body,
    });
    const txt = await res.text();
    if (!res.ok) {
      showMsg(txt);
      return;
    }
    showMsg("Saved; device restarting…");
  } catch (e) {
    showMsg(String(e.message || e));
  }
});

fetchStatus()
  .then(applyVisibility)
  .catch((e) => showMsg(`Could not reach device: ${e.message || e}`));
