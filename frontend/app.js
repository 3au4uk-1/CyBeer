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
      { href: "/participants.html", label: "Участники", page: "participants" },
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
    try {
      const claimResult = JSON.parse(txt);
      if (claimResult.participantId && claimResult.participantName) {
        nameByPid.set(claimResult.participantId, claimResult.participantName);
      }
    } catch (_) {}
    lastUnclaimedRunId = null;
    lastUnclaimedDurationUs = null;
    updateTimerHero(currentFsmState, runDuration);
    await loadLeaderboard();
    return runDuration;
  }

  function buildClaimBody(sel, nameInput) {
    const typed = nameInput?.value?.trim() || "";
    if (typed) {
      return { name: typed };
    }
    if (sel?.value) {
      return { participantId: sel.value };
    }
    return null;
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
        selectEl.selectedIndex = 0;
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

      const body = buildClaimBody(sel, nameInput);
      if (!body) {
        if (msg) {
          msg.textContent = "Выберите участника или введите имя.";
          msg.classList.add("err");
        }
        return;
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

  function setParticipantsMsg(text, isErr) {
    const msg = document.getElementById("participantsMsg");
    if (!msg) return;
    msg.textContent = text || "";
    msg.classList.toggle("err", !!isErr);
  }

  async function fetchParticipantsList() {
    const r = await fetch("/api/participants");
    if (!r.ok) throw new Error("Не удалось загрузить участников");
    const data = await r.json();
    if (!Array.isArray(data)) return [];
    return data.filter(function (p) {
      return p && p.id && p.name;
    });
  }

  function renderParticipantsTable(list) {
    const tbody = document.getElementById("participantsBody");
    if (!tbody) return;

    nameByPid.clear();
    for (const p of list) {
      nameByPid.set(p.id, p.name);
    }

    const sorted = list.slice().sort(function (a, b) {
      return a.name.localeCompare(b.name, "ru");
    });

    tbody.innerHTML = "";
    if (sorted.length === 0) {
      const tr = document.createElement("tr");
      tr.innerHTML = '<td colspan="2">Пока нет участников</td>';
      tbody.appendChild(tr);
      return;
    }

    for (const p of sorted) {
      const tr = document.createElement("tr");
      tr.dataset.participantId = p.id;

      const nameTd = document.createElement("td");
      const nameSpan = document.createElement("span");
      nameSpan.className = "participant-name";
      nameSpan.textContent = p.name;
      nameTd.appendChild(nameSpan);

      const editWrap = document.createElement("div");
      editWrap.className = "participant-edit";
      editWrap.hidden = true;
      const editInput = document.createElement("input");
      editInput.type = "text";
      editInput.maxLength = 32;
      editInput.value = p.name;
      editInput.autocomplete = "off";
      const saveBtn = document.createElement("button");
      saveBtn.type = "button";
      saveBtn.textContent = "Сохранить";
      const cancelBtn = document.createElement("button");
      cancelBtn.type = "button";
      cancelBtn.className = "btn-secondary";
      cancelBtn.textContent = "Отмена";
      editWrap.append(editInput, saveBtn, cancelBtn);
      nameTd.appendChild(editWrap);

      const actionsTd = document.createElement("td");
      actionsTd.className = "participants-actions";

      const profileLink = document.createElement("a");
      profileLink.href = "/player.html?id=" + encodeURIComponent(p.id);
      profileLink.textContent = "Профиль";
      profileLink.className = "btn-link";

      const editBtn = document.createElement("button");
      editBtn.type = "button";
      editBtn.className = "btn-edit";
      editBtn.textContent = "Изменить";

      const delBtn = document.createElement("button");
      delBtn.type = "button";
      delBtn.className = "btn-danger";
      delBtn.textContent = "Удалить";

      function showEdit(show) {
        nameSpan.hidden = show;
        editWrap.hidden = !show;
        editBtn.hidden = show;
        delBtn.disabled = show;
        profileLink.style.pointerEvents = show ? "none" : "";
        if (show) editInput.focus();
      }

      editBtn.addEventListener("click", function () {
        setParticipantsMsg("");
        editInput.value = nameSpan.textContent;
        showEdit(true);
      });

      cancelBtn.addEventListener("click", function () {
        showEdit(false);
      });

      saveBtn.addEventListener("click", async function () {
        const newName = editInput.value.trim();
        if (!newName) {
          setParticipantsMsg("Введите имя.", true);
          return;
        }
        if (newName === p.name) {
          showEdit(false);
          return;
        }
        saveBtn.disabled = true;
        try {
          const resp = await fetch("/api/participants/" + encodeURIComponent(p.id), {
            method: "PATCH",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ name: newName }),
          });
          if (!resp.ok) {
            const err = await resp.json().catch(function () {
              return {};
            });
            if (err.error === "name_taken") {
              throw new Error("Такое имя уже занято.");
            }
            throw new Error("Не удалось сохранить.");
          }
          p.name = newName;
          nameSpan.textContent = newName;
          nameByPid.set(p.id, newName);
          showEdit(false);
          setParticipantsMsg("Имя обновлено.");
        } catch (e) {
          setParticipantsMsg(String(e), true);
        } finally {
          saveBtn.disabled = false;
        }
      });

      delBtn.addEventListener("click", async function () {
        if (
          !confirm(
            "Удалить участника «" + p.name + "»? Нельзя удалить, если есть заявленные заезды."
          )
        ) {
          return;
        }
        delBtn.disabled = true;
        setParticipantsMsg("");
        try {
          const resp = await fetch("/api/participants/" + encodeURIComponent(p.id), {
            method: "DELETE",
          });
          if (!resp.ok) {
            const err = await resp.json().catch(function () {
              return {};
            });
            if (err.error === "has_runs") {
              throw new Error("У участника есть заезды — удаление невозможно.");
            }
            throw new Error("Не удалось удалить.");
          }
          tr.remove();
          nameByPid.delete(p.id);
          if (!tbody.querySelector("tr")) {
            const empty = document.createElement("tr");
            empty.innerHTML = '<td colspan="2">Пока нет участников</td>';
            tbody.appendChild(empty);
          }
          setParticipantsMsg("Участник удалён.");
        } catch (e) {
          setParticipantsMsg(String(e), true);
        } finally {
          delBtn.disabled = false;
        }
      });

      actionsTd.append(profileLink, editBtn, delBtn);
      tr.append(nameTd, actionsTd);
      tbody.appendChild(tr);
    }
  }

  async function reloadParticipantsPage() {
    try {
      const list = await fetchParticipantsList();
      renderParticipantsTable(list);
    } catch (e) {
      setParticipantsMsg(String(e), true);
      const tbody = document.getElementById("participantsBody");
      if (tbody) {
        tbody.innerHTML = '<tr><td colspan="2">Ошибка загрузки</td></tr>';
      }
    }
  }

  function initParticipantsPage() {
    const form = document.getElementById("addParticipantForm");
    const nameInput = document.getElementById("addParticipantName");
    if (!form || !nameInput) return;

    reloadParticipantsPage();

    form.addEventListener("submit", async function (ev) {
      ev.preventDefault();
      const name = nameInput.value.trim();
      if (!name) {
        setParticipantsMsg("Введите имя.", true);
        return;
      }
      setParticipantsMsg("");
      const btn = form.querySelector('button[type="submit"]');
      if (btn) btn.disabled = true;
      try {
        const resp = await fetch("/api/participants", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name: name }),
        });
        if (!resp.ok) {
          const err = await resp.json().catch(function () {
            return {};
          });
          if (err.error === "name_taken") {
            throw new Error("Участник с таким именем уже есть.");
          }
          throw new Error("Не удалось добавить.");
        }
        nameInput.value = "";
        await reloadParticipantsPage();
        setParticipantsMsg("Участник добавлен.");
      } catch (e) {
        setParticipantsMsg(String(e), true);
      } finally {
        if (btn) btn.disabled = false;
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
    } else if (page === "participants") {
      initParticipantsPage();
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
