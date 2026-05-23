const PIN_STORAGE_KEY = "cybeer_admin_pin";
const DEFAULT_PIN = "1111";

function getAdminPin() {
  return localStorage.getItem(PIN_STORAGE_KEY) || DEFAULT_PIN;
}

function setAdminPin(pin) {
  localStorage.setItem(PIN_STORAGE_KEY, pin);
}

function pinHeaders() {
  return { "X-Admin-Pin": getAdminPin() };
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

async function probeAdminPin() {
  try {
    const res = await fetch("/api/admin/pin/verify", {
      method: "POST",
      headers: pinHeaders(),
    });
    if (!res.ok) {
      showMsg(
        "PIN в браузере не совпадает с устройством. Нажмите «Сбросить PIN → 1111» или смените PIN.",
        true
      );
      return false;
    }
    return true;
  } catch (e) {
    showMsg(String(e.message || e), true);
    return false;
  }
}

async function loadWifiStatus() {
  try {
    const cfg = await fetchStatus();
    const el = document.getElementById("wifiStatus");
    if (!el || !cfg.wifi) return;

    const w = cfg.wifi;
    let html = "";
    if (w.sta && w.ssid) {
      html += '<p><strong>Сеть:</strong> ' + w.ssid + '</p>';
      html += '<p><strong>IP:</strong> ' + (w.staIp || "—") + '</p>';
      if (w.rssi) html += '<p><strong>Сигнал:</strong> ' + w.rssi + ' dBm</p>';
    } else {
      html += '<p>Не подключена к WiFi</p>';
    }
    if (w.apFallback) {
      html += '<p class="hint" style="color:var(--amber)">Режим: Fallback AP</p>';
    }
    el.innerHTML = html;

    const sec = document.getElementById("wifiSection");
    if (sec && w.sta) sec.removeAttribute("open");
  } catch (_) {}
}

function scanWifi() {
  const btn = document.getElementById("wifiScanBtn");
  const sel = document.getElementById("wifiSsidSelect");
  const connectBtn = document.getElementById("wifiConnectBtn");
  btn.disabled = true;
  btn.textContent = "Сканирование...";
  showMsg("");

  fetch("/api/setup/scan")
    .then(function (r) {
      return r.json().then(function (data) {
        return { ok: r.ok, data: data };
      });
    })
    .then(function (res) {
      var data = res.data;
      sel.innerHTML = "";
      if (!res.ok && data && data.error) {
        showMsg("Сканирование не удалось: " + data.error, true);
        sel.innerHTML = '<option value="">(ошибка сканирования)</option>';
        sel.disabled = true;
        connectBtn.disabled = true;
        return;
      }
      var list = Array.isArray(data) ? data : [];
      if (list.length === 0) {
        showMsg(
          "Сети не найдены. Если устройство в режиме точки доступа — подождите и повторите.",
          true
        );
        sel.innerHTML = '<option value="">(сети не найдены)</option>';
        sel.disabled = true;
        connectBtn.disabled = true;
      } else {
        for (var i = 0; i < list.length; i++) {
          var o = document.createElement("option");
          o.value = list[i].ssid;
          o.textContent = list[i].ssid + " (" + list[i].rssi + " dBm, " + list[i].auth + ")";
          sel.appendChild(o);
        }
        sel.disabled = false;
        connectBtn.disabled = false;
      }
    })
    .catch(function (e) {
      showMsg("Ошибка сканирования: " + e, true);
    })
    .finally(function () {
      btn.disabled = false;
      btn.textContent = "Сканировать сети";
    });
}

function connectWifi() {
  var sel = document.getElementById("wifiSsidSelect");
  var pass = document.getElementById("wifiPassInput");
  var btn = document.getElementById("wifiConnectBtn");
  var ssid = sel.value;
  if (!ssid) { showMsg("Выберите сеть", true); return; }
  btn.disabled = true;
  showMsg("Подключение...");

  fetch("/api/setup/wifi", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ssid: ssid, password: pass.value })
  })
    .then(function (r) { return r.json(); })
    .then(function (j) {
      if (j.ok) {
        showMsg("Подключено! Устройство перезагружается...");
      } else {
        showMsg("Ошибка: " + (j.error || "неизвестная"), true);
        btn.disabled = false;
      }
    })
    .catch(function (e) {
      showMsg("Ошибка: " + e, true);
      btn.disabled = false;
    });
}

function forgetWifi() {
  if (!window.confirm("Забыть текущую WiFi сеть? Устройство перезагрузится в режим точки доступа.")) return;
  showMsg("");
  fetch("/api/admin/wifi/forget", { method: "POST", headers: pinHeaders() })
    .then(function (r) {
      if (r.ok) {
        showMsg("Сеть забыта, устройство перезагружается...");
      } else {
        return r.text().then(function (t) { showMsg(t || "Ошибка", true); });
      }
    })
    .catch(function (e) { showMsg("Ошибка: " + e, true); });
}

document.getElementById("wifiScanBtn").addEventListener("click", scanWifi);
document.getElementById("wifiConnectBtn").addEventListener("click", connectWifi);
document.getElementById("wifiForgetBtn").addEventListener("click", forgetWifi);

function initAdminPanel() {
  loadWifiStatus();
  prefillLedSettingsFromStatus();
  loadTournamentParticipants();
  loadTournamentsList();
}

async function prefillLedSettingsFromStatus() {
  try {
    const cfg = await fetchStatus();
    prefillLedSettings(cfg);
  } catch (_) {}
}

function ledBrightnessToPercent(raw) {
  if (typeof raw !== "number" || !Number.isFinite(raw)) return 25;
  return Math.max(1, Math.min(100, Math.round((raw * 100) / 255)));
}

function ledPercentToBrightness(pct) {
  const p = Number(pct);
  if (!Number.isFinite(p)) return 64;
  return Math.max(1, Math.min(255, Math.round((p * 255) / 100)));
}

function prefillLedSettings(cfg) {
  const lc = document.getElementById("ledCountInput");
  const lb = document.getElementById("ledBrightnessInput");
  if (lc && typeof cfg.ledCount === "number") lc.value = cfg.ledCount;
  if (lb && typeof cfg.ledBrightness === "number") lb.value = ledBrightnessToPercent(cfg.ledBrightness);
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
    if (!r.ok) return;
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

document.getElementById("changePinForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const newPin = String(document.getElementById("newPinInput").value || "").trim();
  if (newPin.length < 4) {
    showMsg("Новый PIN слишком короткий", true);
    return;
  }
  try {
    const res = await fetch("/api/admin/pin/change", {
      method: "POST",
      headers: combineHeaders({}),
      body: JSON.stringify({ newPin: newPin }),
    });
    if (!res.ok) {
      showMsg("Не удалось сменить PIN (проверьте текущий PIN в браузере)", true);
      return;
    }
    setAdminPin(newPin);
    document.getElementById("newPinInput").value = "";
    showMsg("PIN на устройстве изменён.");
  } catch (e) {
    showMsg(String(e.message || e), true);
  }
});

document.getElementById("resetPinBtn").addEventListener("click", async () => {
  if (!window.confirm("Сбросить PIN на устройстве на 1111?")) return;
  showMsg("");
  try {
    const res = await fetch("/api/admin/pin/reset-default", { method: "POST" });
    if (!res.ok) {
      showMsg(await res.text(), true);
      return;
    }
    setAdminPin(DEFAULT_PIN);
    showMsg("PIN на устройстве сброшен на 1111.");
    await probeAdminPin();
    loadTournamentsList();
  } catch (e) {
    showMsg(String(e.message || e), true);
  }
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
    brightness: ledPercentToBrightness(fd.get("brightness")),
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

if (!localStorage.getItem(PIN_STORAGE_KEY)) {
  setAdminPin(DEFAULT_PIN);
}

probeAdminPin().then(function (ok) {
  if (ok) {
    initAdminPanel();
  }
});

fetchStatus().catch(function (e) {
  showMsg("Не удалось связаться с устройством: " + (e.message || e), true);
});

/* --- OTA Update --- */

const OTA_STAGE_NAMES = {
  downloading: "Скачивание...",
  receiving: "Получение...",
  firmware: "Запись прошивки...",
  littlefs: "Запись интерфейса...",
};

let otaPollTimer = null;

function otaApplyProgress(data) {
  if (!data) return;
  if (data.stage) {
    document.getElementById("otaStage").textContent = OTA_STAGE_NAMES[data.stage] || data.stage;
  }
  if (typeof data.percent === "number") {
    document.getElementById("otaBar").value = data.percent;
  }
}

function otaStopPolling() {
  if (otaPollTimer) {
    clearInterval(otaPollTimer);
    otaPollTimer = null;
  }
}

function otaStartPolling() {
  otaStopPolling();
  otaPollTimer = setInterval(async function () {
    try {
      const res = await fetch("/api/admin/ota/status", { headers: pinHeaders() });
      if (!res.ok) return;
      const data = await res.json();
      otaApplyProgress(data);
      if (data.error) {
        otaStopPolling();
        otaShowError(data.error);
      } else if (data.stage === "done") {
        otaStopPolling();
        document.getElementById("otaStage").textContent = "Обновлено! Перезагрузка...";
        document.getElementById("otaBar").value = 100;
        setTimeout(function () {
          location.reload();
        }, 7000);
      }
    } catch (_) {}
  }, 1000);
}

async function otaCheck() {
  try {
    const res = await fetch("/api/admin/ota/check", { headers: pinHeaders() });
    if (!res.ok) throw new Error("status " + res.status);
    const data = await res.json();

    document.getElementById("otaCurrentVer").textContent = "Версия: " + data.currentVersion;

    if (data.available) {
      document.getElementById("otaAvailable").style.display = "";
      document.getElementById("otaUpToDate").style.display = "none";
      document.getElementById("otaNewVer").textContent = "Доступна v" + data.remoteVersion;
      document.getElementById("otaChangelog").textContent = data.changelog || "";
    } else {
      document.getElementById("otaAvailable").style.display = "none";
      document.getElementById("otaUpToDate").style.display = "";
    }
  } catch (e) {
    document.getElementById("otaCurrentVer").textContent = "Не удалось проверить обновления";
  }
}

function otaShowProgress() {
  document.getElementById("otaProgress").style.display = "";
  document.getElementById("otaAvailable").style.display = "none";
  document.getElementById("otaUpToDate").style.display = "none";
  document.getElementById("otaError").style.display = "none";
  document.querySelectorAll("#adminPanel details:not(#otaSection) button, #adminPanel details:not(#otaSection) input, #adminPanel details:not(#otaSection) select").forEach(el => el.disabled = true);
  otaStartPolling();
}

function otaShowError(msg) {
  otaStopPolling();
  document.getElementById("otaProgress").style.display = "none";
  document.getElementById("otaError").style.display = "";
  document.getElementById("otaErrMsg").textContent = msg;
  document.querySelectorAll("#adminPanel button, #adminPanel input, #adminPanel select").forEach(el => el.disabled = false);
}

async function otaStart() {
  otaShowProgress();
  try {
    const res = await fetch("/api/admin/ota/start", {
      method: "POST",
      headers: pinHeaders(),
    });
    if (!res.ok && res.status !== 202) {
      const d = await res.json().catch(() => ({}));
      throw new Error(d.error || "Ошибка запуска");
    }
  } catch (e) {
    otaShowError(e.message);
  }
}

async function otaUpload() {
  const fileInput = document.getElementById("otaFileInput");
  const file = fileInput.files[0];
  if (!file) return;

  otaShowProgress();
  try {
    const res = await fetch("/api/admin/ota/upload", {
      method: "POST",
      headers: { "X-Admin-Pin": getAdminPin(), "Content-Type": "application/octet-stream" },
      body: file,
    });
    if (!res.ok && res.status !== 202) {
      const d = await res.json().catch(() => ({}));
      throw new Error(d.error || "Ошибка загрузки");
    }
  } catch (e) {
    otaShowError(e.message);
  }
}

function otaHandleWsMessage(data) {
  if (data.type === "ota_progress") {
    otaApplyProgress(data);
  } else if (data.type === "ota_done") {
    otaStopPolling();
    document.getElementById("otaStage").textContent = "Обновлено! Перезагрузка...";
    document.getElementById("otaBar").value = 100;
    setTimeout(function () {
      location.reload();
    }, 7000);
  } else if (data.type === "ota_error") {
    otaShowError(data.message);
  }
}

(function initOta() {
  document.getElementById("otaStartBtn")?.addEventListener("click", otaStart);
  document.getElementById("otaUploadBtn")?.addEventListener("click", otaUpload);
  document.getElementById("otaRetryBtn")?.addEventListener("click", otaCheck);
  document.getElementById("otaCheckBtn")?.addEventListener("click", otaCheck);
  document.getElementById("otaFileInput")?.addEventListener("change", function () {
    document.getElementById("otaUploadBtn").disabled = !this.files.length;
  });

  otaCheck();
})();
