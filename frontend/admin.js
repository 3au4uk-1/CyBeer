const PIN_SESSION_KEY = "cybeer_admin_pin";

function pinHeaders() {
  const p = sessionStorage.getItem(PIN_SESSION_KEY);
  return p ? { "X-Admin-Pin": p } : {};
}

function combineHeaders(extra) {
  return { ...(extra || {}), ...pinHeaders(), "Content-Type": "application/json" };
}

function showMsg(txt, isErr) {
  const el = document.getElementById("adminMsg");
  if (el) {
    el.textContent = txt;
    el.classList.toggle("err", !!isErr);
  }
}

async function fetchStatus() {
  const res = await fetch("/api/status");
  if (!res.ok) throw new Error("status " + res.status);
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

  prefillLedSettings(cfg);
  loadTournamentParticipants();
  loadTournamentsList();
}

function prefillLedSettings(cfg) {
  const lc = document.getElementById("ledCountInput");
  const lb = document.getElementById("ledBrightnessInput");
  if (lc && typeof cfg.ledCount === "number") lc.value = cfg.ledCount;
  if (lb && typeof cfg.ledBrightness === "number") lb.value = cfg.ledBrightness;
}

async function loadTournamentParticipants() {
  try {
    const r = await fetch("/api/participants");
    const data = await r.json();
    const wrap = document.getElementById("torParticipants");
    if (!wrap || !Array.isArray(data)) return;
    wrap.innerHTML = "";
    for (const p of data) {
      if (!p || !p.id || !p.name) continue;
      const lbl = document.createElement("label");
      lbl.innerHTML =
        '<input type="checkbox" name="tor_pid" value="' +
        p.id +
        '"> <span>' +
        p.name +
        "</span>";
      wrap.appendChild(lbl);
    }
  } catch (_) {}
}

async function loadTournamentsList() {
  try {
    const r = await fetch("/api/admin/tournaments", { headers: pinHeaders() });
    const data = await r.json();
    const sel = document.getElementById("torSelect");
    if (!sel || !Array.isArray(data)) return;
    sel.innerHTML = '<option value="">— выберите —</option>';
    for (const t of data) {
      if (!t || !t.id) continue;
      const opt = document.createElement("option");
      opt.value = t.id;
      opt.textContent = (t.name || t.id.slice(0, 8)) + " [" + (t.status || "?") + "]";
      sel.appendChild(opt);
    }
  } catch (_) {}
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
      body: JSON.stringify({ pin }),
    });
    if (!res.ok) {
      showMsg(await res.text(), true);
      return;
    }
    showMsg("PIN сохранён. Перезагрузка.");
    sessionStorage.removeItem(PIN_SESSION_KEY);
    await fetchStatus().then(applyVisibility);
  } catch (e) {
    showMsg(String(e.message || e), true);
  }
});

document.getElementById("unlockForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const pin = String(document.getElementById("unlockPin").value || "").trim();
  if (pin.length < 4) {
    showMsg("PIN слишком короткий", true);
    return;
  }

  try {
    const res = await fetch("/api/admin/pin/verify", {
      method: "POST",
      headers: { "X-Admin-Pin": pin },
    });
    if (!res.ok) {
      showMsg("Неверный PIN", true);
      return;
    }
    sessionStorage.setItem(PIN_SESSION_KEY, pin);
    showMsg("PIN принят.");
    await fetchStatus().then(applyVisibility);
  } catch (e) {
    showMsg(String(e.message || e), true);
  }
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
  if (id) body.id = id;
  const res = await fetch("/api/admin/runs", {
    method: "POST",
    headers: combineHeaders({}),
    body: JSON.stringify(body),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Заезд добавлен: " + txt : txt, !res.ok);
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
    showMsg("Тело PATCH должно быть валидным JSON", true);
    return;
  }
  const res = await fetch("/api/admin/runs/" + encodeURIComponent(id), {
    method: "PATCH",
    headers: combineHeaders({}),
    body: JSON.stringify(patch),
  });
  const txt = await res.text();
  showMsg(res.ok ? "OK " + txt : txt, !res.ok);
});

document.getElementById("deleteRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const id = String(fd.get("delete_id") || "").trim();
  if (!window.confirm("Удалить заезд " + id + "?")) return;
  const res = await fetch("/api/admin/runs/" + encodeURIComponent(id), {
    method: "DELETE",
    headers: pinHeaders(),
  });
  const txt = await res.text();
  showMsg(res.ok ? "OK " + txt : txt, !res.ok);
});

document.getElementById("resetBtn").addEventListener("click", async () => {
  showMsg("");
  if (!window.confirm("Очистить все заезды, участников и турниры?")) return;
  const res = await fetch("/api/admin/data/reset", {
    method: "DELETE",
    headers: pinHeaders(),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Сброс OK " + txt : txt, !res.ok);
});

document.getElementById("exportForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fmt = document.getElementById("exportFmt").value;
  const res = await fetch("/api/export?format=" + encodeURIComponent(fmt), {
    headers: pinHeaders(),
  });
  if (!res.ok) {
    showMsg((await res.text()) || "ошибка экспорта", true);
    return;
  }
  const blob = await res.blob();
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = fmt === "csv" ? "cybeer-export.csv" : "cybeer-export.json";
  a.click();
  URL.revokeObjectURL(a.href);
  showMsg("Скачивание начато.");
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
      body,
    });
    const txt = await res.text();
    if (!res.ok) {
      showMsg(txt, true);
      return;
    }
    showMsg("Сохранено; устройство перезагружается…");
  } catch (e) {
    showMsg(String(e.message || e), true);
  }
});

document.getElementById("tournamentCreateForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const name = document.getElementById("torName").value.trim();
  const checks = document.querySelectorAll('#torParticipants input[name="tor_pid"]:checked');
  const pids = Array.from(checks).map(function (c) {
    return c.value;
  });
  if (!name || pids.length < 2) {
    showMsg("Нужно имя и минимум 2 участника", true);
    return;
  }
  const res = await fetch("/api/admin/tournaments", {
    method: "POST",
    headers: combineHeaders({}),
    body: JSON.stringify({ name: name, participantIds: pids }),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Турнир создан: " + txt : txt, !res.ok);
  if (res.ok) loadTournamentsList();
});

document.getElementById("tournamentStartForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const tid = document.getElementById("torSelect").value;
  if (!tid) {
    showMsg("Выберите турнир", true);
    return;
  }
  const res = await fetch("/api/admin/tournaments/" + encodeURIComponent(tid) + "/start", {
    method: "POST",
    headers: pinHeaders(),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Турнир запущен" : txt, !res.ok);
  if (res.ok) loadTournamentsList();
});

document.getElementById("tournamentAssignForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const mid = document.getElementById("assignMatchId").value.trim();
  const slot = document.getElementById("assignSlot").value;
  if (!mid) {
    showMsg("Укажите ID матча", true);
    return;
  }
  const res = await fetch(
    "/api/admin/tournaments/matches/" + encodeURIComponent(mid) + "/assign",
    {
      method: "POST",
      headers: combineHeaders({}),
      body: JSON.stringify({ slot: slot }),
    }
  );
  const txt = await res.text();
  showMsg(res.ok ? "Слот назначен" : txt, !res.ok);
});

fetchStatus()
  .then(applyVisibility)
  .catch(function (e) {
    showMsg("Не удалось связаться с устройством: " + (e.message || e), true);
  });
