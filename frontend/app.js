(() => {
  /* ========== Утилиты ========== */

  function formatDuration(us) {
    if (typeof us !== "number" || !Number.isFinite(us)) return "—";
    const ms = Math.floor(us / 1000);
    const s = Math.floor(ms / 1000);
    const cs = Math.floor((ms % 1000) / 10);
    return s + "." + String(cs).padStart(2, "0") + "с";
  }

  function formatDate(iso) {
    if (!iso) return "—";
    try {
      const d = new Date(iso);
      if (isNaN(d.getTime())) return iso;
      return d.toLocaleDateString("ru-RU", {
        day: "numeric",
        month: "short",
        hour: "2-digit",
        minute: "2-digit",
      });
    } catch (_) {
      return iso;
    }
  }

  const nameByPid = new Map();

  function participantName(pid) {
    if (!pid) return "—";
    return nameByPid.get(pid) || pid.slice(0, 8) + "…";
  }

  async function loadParticipants() {
    try {
      const r = await fetch("/api/participants");
      const data = await r.json();
      nameByPid.clear();
      if (Array.isArray(data)) {
        for (const p of data) {
          if (p && p.id && p.name) nameByPid.set(p.id, p.name);
        }
      }
    } catch (_) {}
  }

  /* ========== Shared topbar ========== */

  function injectTopbar() {
    const page = window.__CYBEER_PAGE__;
    if (page === "setup" || page === "spectator") return;

    const header = document.querySelector("header.topbar");
    if (!header) return;

    const links = [
      { href: "/", label: "Главная", page: "index" },
      { href: "/claim.html", label: "Заявить заезд", page: "claim" },
      { href: "/tournament.html", label: "Турнир", page: "tournament" },
      { href: "/admin.html", label: "Админка", page: "admin" },
    ];

    const nav = header.querySelector("nav");
    if (nav) {
      nav.innerHTML = "";
      for (const l of links) {
        const a = document.createElement("a");
        a.href = l.href;
        a.textContent = l.label;
        if (l.page === page) a.classList.add("active");
        nav.appendChild(a);
      }
    }
  }

  /* ========== Battery ========== */

  function applyBatteryPercent(pct) {
    const wrap = document.getElementById("batteryWrap");
    const icon = document.getElementById("batteryIcon");
    const label = document.getElementById("batteryPct");
    if (!wrap || !icon || !label) return;
    if (typeof pct !== "number" || !Number.isFinite(pct)) {
      label.textContent = "—";
      icon.textContent = "🔋";
      wrap.classList.remove("bat-low", "bat-mid", "bat-high");
      return;
    }
    const p = Math.max(0, Math.min(100, Math.round(pct)));
    label.textContent = p + "%";
    icon.textContent = p < 20 ? "🪫" : "🔋";
    wrap.classList.remove("bat-low", "bat-mid", "bat-high");
    if (p < 20) wrap.classList.add("bat-low");
    else if (p < 60) wrap.classList.add("bat-mid");
    else wrap.classList.add("bat-high");
    wrap.title = "Батарея " + p + "%";
  }

  /* ========== FSM state ========== */

  function renderStatus(s) {
    const badge = document.getElementById("stateBadge");
    const ver = document.getElementById("fwVer");
    if (badge) {
      const labels = {
        PREP: "ГОТОВ",
        RUNNING: "ИДЁТ",
        FINISHED: "ФИНИШ",
        READY: "ОЖИДАНИЕ",
      };
      badge.textContent = labels[s.state] || s.state || "—";
    }
    applyBatteryPercent(s.batteryPercent);
    if (ver) ver.textContent = s.firmwareVersion ? "fw " + s.firmwareVersion : "";
  }

  /* ========== Timer hero ========== */

  let currentFsmState = "PREP";
  let lastFinishedDurationUs = null;
  let lastUnclaimedRunId = null;
  let lastUnclaimedDurationUs = null;

  function syncUnclaimedFromStatus(st) {
    if (!st) return;
    if (st.unclaimedRunId) {
      lastUnclaimedRunId = st.unclaimedRunId;
      if (typeof st.unclaimedRunDurationUs === "number") {
        lastUnclaimedDurationUs = st.unclaimedRunDurationUs;
        lastFinishedDurationUs = st.unclaimedRunDurationUs;
      }
    }
  }

  function updateClaimableUi() {
    const hero = document.getElementById("timerHero");
    const hint = document.getElementById("timerHint");
    if (!hero) return;

    const claimable =
      !!lastUnclaimedRunId &&
      (currentFsmState === "FINISHED" || currentFsmState === "READY");

    hero.classList.toggle("claimable", claimable);
    if (hint) {
      if (claimable) {
        hint.hidden = false;
        hint.textContent = "Нажмите, чтобы привязать участника";
      } else {
        hint.hidden = true;
        hint.textContent = "";
      }
    }
  }

  function updateTimerHero(state, durationUs) {
    const hero = document.getElementById("timerHero");
    const val = document.getElementById("timerValue");
    const lbl = document.getElementById("timerLabel");
    if (!hero || !val) return;

    hero.className = "timer-hero panel state-" + state.toLowerCase();
    currentFsmState = state;

    const labels = {
      PREP: "Готов к старту",
      RUNNING: "",
      FINISHED: lastUnclaimedRunId ? "Сохранён, не привязан" : "Ожидает сохранения",
      READY: lastUnclaimedRunId ? "Сохранён, не привязан" : "Последний результат",
    };
    if (lbl) lbl.textContent = labels[state] || "";

    if (state === "PREP") {
      val.textContent = "0.00с";
    } else if (state === "RUNNING") {
      if (typeof durationUs === "number") val.textContent = formatDuration(durationUs);
    } else if (state === "FINISHED" || state === "READY") {
      const dur =
        typeof durationUs === "number"
          ? durationUs
          : lastUnclaimedDurationUs != null
            ? lastUnclaimedDurationUs
            : lastFinishedDurationUs;
      if (typeof dur === "number") val.textContent = formatDuration(dur);
    }

    updateClaimableUi();
  }

  /* ========== Leaderboard ========== */

  async function loadLeaderboard(limit) {
    const body = document.getElementById("leaderboardBody");
    if (!body) return;
    const lim = typeof limit === "number" && limit > 0 ? limit : 20;
    try {
      await loadParticipants();
      const r = await fetch("/api/leaderboard?limit=" + lim);
      const data = await r.json();
      body.innerHTML = "";
      if (!Array.isArray(data) || data.length === 0) {
        body.innerHTML =
          '<tr><td colspan="4" style="text-align:center;color:var(--muted)">Пока нет заездов</td></tr>';
        return;
      }
      const frag = document.createDocumentFragment();
      for (const row of data) {
        const tr = document.createElement("tr");
        const name = row.participantName || participantName(row.participantId);
        const pid = row.participantId || "";
        const nameLink = pid
          ? '<a href="/player.html?id=' + encodeURIComponent(pid) + '">' + name + "</a>"
          : name;
        tr.innerHTML =
          "<td>" +
          row.rank +
          "</td>" +
          "<td>" +
          nameLink +
          "</td>" +
          "<td>" +
          formatDuration(row.durationUs) +
          "</td>" +
          "<td>" +
          formatDate(row.finishedAt) +
          "</td>";
        frag.appendChild(tr);
      }
      body.appendChild(frag);
    } catch (_) {}
  }

  /* ========== Sound ========== */

  let audioCtx = null;
  let soundEnabled = false;

  function ensureAudioContext() {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioCtx.state === "suspended") {
      audioCtx.resume();
    }
  }

  function playFanfare() {
    if (!soundEnabled || !audioCtx) return;
    const notes = [523.25, 659.25, 783.99];
    const now = audioCtx.currentTime;
    for (let i = 0; i < notes.length; i++) {
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.type = "sine";
      osc.frequency.value = notes[i];
      gain.gain.setValueAtTime(0.3, now + i * 0.25);
      gain.gain.exponentialRampToValueAtTime(0.001, now + i * 0.25 + 0.8);
      osc.connect(gain);
      gain.connect(audioCtx.destination);
      osc.start(now + i * 0.25);
      osc.stop(now + i * 0.25 + 0.8);
    }
  }

  function enableSound() {
    ensureAudioContext();
    soundEnabled = true;
  }

  document.addEventListener(
    "click",
    function onFirstClick() {
      enableSound();
      document.removeEventListener("click", onFirstClick);
    },
    { once: true }
  );

  /* ========== WebSocket ========== */

  function connectLiveWs() {
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = proto + "//" + window.location.host + "/ws";
    let ws;
    let reconnectTimer;

    let reconnectDelayMs = 2000;

    function scheduleReconnect() {
      if (reconnectTimer) return;
      reconnectTimer = window.setTimeout(function () {
        reconnectTimer = null;
        connectLiveWs();
        reconnectDelayMs = Math.min(reconnectDelayMs * 2, 15000);
      }, reconnectDelayMs);
    }

    try {
      ws = new WebSocket(url);
    } catch (_) {
      scheduleReconnect();
      return;
    }

    ws.onopen = function () {
      reconnectDelayMs = 2000;
      try {
        ws.send(".");
      } catch (_) {}
    };

    ws.onmessage = function (ev) {
      let msg;
      try {
        msg = JSON.parse(ev.data);
      } catch (_) {
        return;
      }
      if (typeof otaHandleWsMessage === "function" && (msg.type === "ota_progress" || msg.type === "ota_done" || msg.type === "ota_error")) {
        otaHandleWsMessage(msg);
        return;
      }
      if (!msg || typeof msg.type !== "string") return;

      if (msg.type === "timer" && typeof msg.elapsedUs === "number") {
        updateTimerHero("RUNNING", msg.elapsedUs);
        return;
      }
      if (msg.type === "state" && typeof msg.state === "string") {
        currentFsmState = msg.state;
        const badge = document.getElementById("stateBadge");
        const labels = {
          PREP: "ГОТОВ",
          RUNNING: "ИДЁТ",
          FINISHED: "ФИНИШ",
          READY: "ОЖИДАНИЕ",
        };
        if (badge) badge.textContent = labels[msg.state] || msg.state;
        if (msg.state === "PREP") {
          updateTimerHero("PREP", 0);
        } else if (msg.state !== "RUNNING") {
          updateTimerHero(msg.state, lastUnclaimedDurationUs != null ? lastUnclaimedDurationUs : lastFinishedDurationUs);
          if ((msg.state === "FINISHED" || msg.state === "READY") && !lastUnclaimedRunId) {
            pollUnclaimedFromStatus();
          }
        }
        return;
      }
      if (msg.type === "runFinished") {
        lastFinishedDurationUs = msg.durationUs;
        if (typeof msg.durationUs === "number") {
          lastUnclaimedDurationUs = msg.durationUs;
        }
        if (msg.runId) {
          lastUnclaimedRunId = msg.runId;
        }
        updateTimerHero("FINISHED", msg.durationUs);
        playFanfare();
        loadLeaderboard();
        if (!msg.runId) {
          pollUnclaimedFromStatus();
        }

        const targetEl = document.getElementById("targetRunTime");
        if (targetEl && typeof msg.durationUs === "number") {
          targetEl.textContent = formatDuration(msg.durationUs);
        }
        return;
      }
      if (msg.type === "leaderboardUpdate") {
        loadLeaderboard();
        return;
      }
      if (msg.type === "battery" && typeof msg.percent === "number") {
        applyBatteryPercent(msg.percent);
        return;
      }
    };

    ws.onclose = function () {
      scheduleReconnect();
    };
    ws.onerror = function () {
      try {
        ws.close();
      } catch (_) {}
    };
  }

  /* ========== Claim (shared) ========== */

  async function pollUnclaimedFromStatus(attemptsLeft) {
    const left = typeof attemptsLeft === "number" ? attemptsLeft : 8;
    try {
      const rs = await fetch("/api/status");
      const st = await rs.json();
      syncUnclaimedFromStatus(st);
      if (lastUnclaimedRunId) {
        updateTimerHero(currentFsmState, lastUnclaimedDurationUs);
        return;
      }
      if (left > 0 && (currentFsmState === "FINISHED" || currentFsmState === "READY")) {
        window.setTimeout(function () {
          pollUnclaimedFromStatus(left - 1);
        }, 400);
      }
    } catch (_) {}
  }

  async function submitClaimRun(runId, runDuration, body) {
    const payload = Object.assign({ runId: runId }, body);
    const resp = await fetch("/api/claim", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const txt = await resp.text();
    if (!resp.ok) {
      let msg = txt || resp.statusText;
      try {
        const j = JSON.parse(txt);
        if (j.error) msg = j.error;
      } catch (_) {}
      throw new Error(msg);
    }
    lastUnclaimedRunId = null;
    lastUnclaimedDurationUs = null;
    updateTimerHero(currentFsmState, runDuration);
    await loadLeaderboard();
    return runDuration;
  }

  function populateClaimSelect(selectEl) {
    if (!selectEl) return Promise.resolve();
    while (selectEl.options.length > 1) {
      selectEl.remove(1);
    }
    return fetch("/api/participants")
      .then(function (r) {
        return r.json();
      })
      .then(function (data) {
        if (!Array.isArray(data)) return;
        for (const p of data) {
          if (!p || !p.id || !p.name) continue;
          const opt = document.createElement("option");
          opt.value = p.id;
          opt.textContent = p.name;
          selectEl.appendChild(opt);
        }
      })
      .catch(function () {});
  }

  function initClaimModal() {
    const modal = document.getElementById("claimModal");
    const hero = document.getElementById("timerHero");
    const form = document.getElementById("claimModalForm");
    const sel = document.getElementById("claimModalSelect");
    const nameInput = document.getElementById("claimModalName");
    const msg = document.getElementById("claimModalMsg");
    const timeEl = document.getElementById("claimModalTime");
    const cancelBtn = document.getElementById("claimModalCancel");
    if (!modal || !hero || !form) return;

    function closeModal() {
      modal.hidden = true;
      if (msg) {
        msg.textContent = "";
        msg.classList.remove("err");
      }
    }

    function openModal() {
      if (!lastUnclaimedRunId) return;
      const dur = lastUnclaimedDurationUs != null ? lastUnclaimedDurationUs : lastFinishedDurationUs;
      if (timeEl && typeof dur === "number") {
        timeEl.textContent = formatDuration(dur);
      }
      if (sel) sel.value = "";
      if (nameInput) {
        nameInput.value = "";
        nameInput.disabled = false;
      }
      populateClaimSelect(sel);
      modal.hidden = false;
      if (nameInput) nameInput.focus();
    }

    hero.addEventListener("click", function () {
      if (lastUnclaimedRunId && (currentFsmState === "FINISHED" || currentFsmState === "READY")) {
        openModal();
      }
    });
    hero.addEventListener("keydown", function (ev) {
      if (ev.key !== "Enter" && ev.key !== " ") return;
      ev.preventDefault();
      if (lastUnclaimedRunId && (currentFsmState === "FINISHED" || currentFsmState === "READY")) {
        openModal();
      }
    });

    cancelBtn?.addEventListener("click", closeModal);
    modal.addEventListener("click", function (ev) {
      if (ev.target === modal) closeModal();
    });

    sel?.addEventListener("change", function () {
      if (!nameInput) return;
      if (sel.value) {
        nameInput.value = "";
        nameInput.disabled = true;
      } else {
        nameInput.disabled = false;
      }
    });

    form.addEventListener("submit", async function (ev) {
      ev.preventDefault();
      if (msg) {
        msg.textContent = "";
        msg.classList.remove("err");
      }

      const runId = lastUnclaimedRunId;
      const runDuration = lastUnclaimedDurationUs != null ? lastUnclaimedDurationUs : lastFinishedDurationUs;
      if (!runId) {
        if (msg) {
          msg.textContent = "Нет сохранённого заезда.";
          msg.classList.add("err");
        }
        return;
      }

      let body;
      if (sel?.value) {
        body = { participantId: sel.value };
      } else {
        const name = nameInput?.value?.trim();
        if (!name) {
          if (msg) {
            msg.textContent = "Выберите участника или введите имя.";
            msg.classList.add("err");
          }
          return;
        }
        body = { name: name };
      }

      try {
        await submitClaimRun(runId, runDuration, body);
        closeModal();
      } catch (e) {
        if (msg) {
          msg.textContent = String(e.message || e);
          msg.classList.add("err");
        }
      }
    });
  }

  /* ========== Claim page ========== */

  async function tickClaimTarget() {
    const timeEl = document.getElementById("targetRunTime");
    const idEl = document.getElementById("targetRunId");
    try {
      const rs = await fetch("/api/status");
      const st = await rs.json();
      renderStatus(st);
      if (timeEl) {
        timeEl.textContent = st.unclaimedRunDurationUs
          ? formatDuration(st.unclaimedRunDurationUs)
          : "(нет)";
      }
      if (idEl) idEl.textContent = st.unclaimedRunId || "";
    } catch (_) {
      if (timeEl) timeEl.textContent = "(ошибка)";
    }
  }

  function initClaimForm() {
    const form = document.getElementById("claimForm");
    const msg = document.getElementById("claimMsg");
    const sel = document.getElementById("claimSelect");
    const nameInput = document.getElementById("claimName");
    if (!form) return;

    fetch("/api/participants")
      .then(function (r) {
        return r.json();
      })
      .then(function (data) {
        if (!Array.isArray(data)) return;
        for (const p of data) {
          if (!p || !p.id || !p.name) continue;
          const opt = document.createElement("option");
          opt.value = p.id;
          opt.textContent = p.name;
          sel.appendChild(opt);
        }
      })
      .catch(function () {});

    sel.addEventListener("change", function () {
      if (sel.value) {
        nameInput.value = "";
        nameInput.disabled = true;
      } else {
        nameInput.disabled = false;
      }
    });

    form.addEventListener("submit", async function (ev) {
      ev.preventDefault();
      if (msg) {
        msg.textContent = "";
        msg.classList.remove("err");
      }
      const rs = await fetch("/api/status");
      const status = await rs.json();
      syncUnclaimedFromStatus(status);
      const runId = status.unclaimedRunId;
      const runDuration = status.unclaimedRunDurationUs;
      if (!runId) {
        if (msg) {
          msg.textContent = "Нет незаявленного заезда.";
          msg.classList.add("err");
        }
        return;
      }

      let body;
      if (sel.value) {
        body = { participantId: sel.value };
      } else {
        const name = nameInput?.value?.trim();
        if (!name) {
          if (msg) {
            msg.textContent = "Выберите участника или введите имя.";
            msg.classList.add("err");
          }
          return;
        }
        body = { name: name };
      }

      try {
        await submitClaimRun(runId, runDuration, body);
        if (msg) {
          msg.textContent = "Заявлено! Ваш результат: " + formatDuration(runDuration);
          msg.classList.remove("err");
        }
        await tickClaimTarget();
      } catch (e) {
        if (msg) {
          msg.textContent = String(e);
          msg.classList.add("err");
        }
      }
    });
  }

  /* ========== Boot ========== */

  function bootPage() {
    injectTopbar();
    const page = window.__CYBEER_PAGE__;

    if (document.getElementById("stateBadge")) {
      fetch("/api/status")
        .then(function (r) {
          return r.json();
        })
        .then(renderStatus)
        .catch(function () {});
    }

    if (page === "index") {
      loadLeaderboard();
      connectLiveWs();
      initClaimModal();
      fetch("/api/status")
        .then(function (r) {
          return r.json();
        })
        .then(function (st) {
          renderStatus(st);
          syncUnclaimedFromStatus(st);
          var dur = null;
          if (
            (st.state === "FINISHED" || st.state === "READY") &&
            typeof st.unclaimedRunDurationUs === "number"
          ) {
            dur = st.unclaimedRunDurationUs;
          } else if (st.state === "FINISHED" || st.state === "READY") {
            dur = lastFinishedDurationUs;
          }
          updateTimerHero(st.state || "PREP", dur);
        })
        .catch(function () {});
    } else if (page === "claim") {
      tickClaimTarget();
      setInterval(tickClaimTarget, 5000);
      initClaimForm();
      connectLiveWs();
    } else if (page === "admin") {
      connectLiveWs();
    }
  }

  window.__cybeer = {
    formatDuration: formatDuration,
    formatDate: formatDate,
    loadParticipants: loadParticipants,
    participantName: participantName,
    nameByPid: nameByPid,
    enableSound: enableSound,
    playFanfare: playFanfare,
    connectLiveWs: connectLiveWs,
    updateTimerHero: updateTimerHero,
    loadLeaderboard: loadLeaderboard,
  };

  bootPage();
})();
