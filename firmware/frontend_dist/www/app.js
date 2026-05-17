(() => {
  function formatDuration(us) {
    if (typeof us !== "number" || !Number.isFinite(us)) return "—";
    const ms = Math.floor(us / 1000);
    const s = Math.floor(ms / 1000);
    const cs = Math.floor((ms % 1000) / 10);
    return `${s}.${String(cs).padStart(2, "0")}s`;
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
    } catch (_) {
      /* ignore */
    }
  }

  function applyBatteryPercent(pct) {
    const wrap = document.getElementById("batteryWrap");
    const icon = document.getElementById("batteryIcon");
    const label = document.getElementById("batteryPct");
    if (!wrap || !icon || !label) return;
    if (typeof pct !== "number" || !Number.isFinite(pct)) {
      label.textContent = "—";
      icon.textContent = "🔋";
      wrap.classList.remove("bat-low", "bat-mid", "bat-high");
      wrap.title = "Battery";
      wrap.setAttribute("aria-label", "Battery unknown");
      return;
    }
    const p = Math.max(0, Math.min(100, Math.round(pct)));
    label.textContent = `${p}%`;
    icon.textContent = p < 20 ? "🪫" : "🔋";
    wrap.classList.remove("bat-low", "bat-mid", "bat-high");
    if (p < 20) wrap.classList.add("bat-low");
    else if (p < 60) wrap.classList.add("bat-mid");
    else wrap.classList.add("bat-high");
    wrap.title = `Battery ${p}%`;
    wrap.setAttribute("aria-label", `Battery ${p} percent`);
  }

  function renderStatus(s) {
    const badge = document.getElementById("stateBadge");
    const ver = document.getElementById("fwVer");
    if (badge) badge.textContent = s.state || "—";
    applyBatteryPercent(s.batteryPercent);
    if (ver) ver.textContent = s.firmwareVersion ? `fw ${s.firmwareVersion}` : "";
  }

  function shortId(s) {
    if (!s) return "—";
    const t = String(s);
    return t.length > 10 ? `${t.slice(0, 6)}…${t.slice(-4)}` : t;
  }

  function renderActiveMatch(am) {
    const el = document.getElementById("activeTournamentLine");
    if (!el) return;
    el.classList.remove("err");
    if (!am || typeof am !== "object" || Array.isArray(am)) {
      el.textContent = "Bracket: idle.";
      return;
    }
    const tid = am.tournamentId;
    const name = am.name || "";
    if (!tid) {
      el.textContent = "Bracket: idle.";
      return;
    }

    const parts = [];
    parts.push(`${name ? name + " — " : ""}${shortId(tid)}`);

    const pend = am.pendingNextRun;
    if (pend && typeof pend.slot === "string") {
      parts.push(`next device run binds to slot ${pend.slot} (${shortId(pend.matchId)})`);
    }

    const m = am.match;
    if (m && typeof m === "object") {
      const ra = shortId(m.runIdA);
      const rb = shortId(m.runIdB);
      const pa = shortId(m.participantAId);
      const pb = shortId(m.participantBId);
      parts.push(`focus: ${pa} vs ${pb} • runs ${ra} / ${rb}`);
      if (m.winnerParticipantId) parts.push(`winner ${shortId(m.winnerParticipantId)}`);
    }

    el.textContent = parts.filter(Boolean).join(" · ") || "(active)";
  }

  function renderRuns(runs) {
    const body = document.getElementById("runsBody");
    if (!body || !Array.isArray(runs)) return;
    body.innerHTML = "";
    const frag = document.createDocumentFragment();
    for (const row of runs) {
      const tr = document.createElement("tr");
      const pid = row.participant_id || "";
      const name =
        row.claimed && pid
          ? participantName(pid)
          : "—";
      tr.innerHTML = `
        <td>${row.finished_at || "—"}</td>
        <td>${formatDuration(row.duration_us)}</td>
        <td>${name}</td>
        <td class="${row.claimed ? "tag-yes" : "tag-no"}">${row.claimed ? "yes" : "no"}</td>
      `;
      frag.appendChild(tr);
    }
    body.appendChild(frag);
  }

  async function tickIndex() {
    await loadParticipants();
    try {
      const [rs, rr] = await Promise.all([fetch("/api/status"), fetch("/api/runs")]);
      const st = await rs.json();
      const runs = await rr.json();
      renderStatus(st);
      renderActiveMatch(st.activeMatch);
      renderRuns(runs.slice().reverse());
    } catch (e) {
      const badge = document.getElementById("stateBadge");
      if (badge) badge.textContent = "OFFLINE";
    }
  }

  async function tickClaimTarget() {
    const el = document.getElementById("targetRunId");
    if (!el) return;
    try {
      const rs = await fetch("/api/status");
      const st = await rs.json();
      el.textContent = st.unclaimedRunId || "(none)";
    } catch (_) {
      el.textContent = "(error)";
    }
  }

  function initClaimForm() {
    const form = document.getElementById("claimForm");
    const msg = document.getElementById("claimMsg");
    if (!form) return;
    form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      if (msg) {
        msg.textContent = "";
        msg.classList.remove("err");
      }
      const rs = await fetch("/api/status");
      const status = await rs.json();
      const runId = status.unclaimedRunId;
      if (!runId) {
        if (msg) {
          msg.textContent = "No unclaimed run.";
          msg.classList.add("err");
        }
        return;
      }
      const name = document.getElementById("claimName")?.value?.trim();
      const pid = document.getElementById("claimPid")?.value?.trim();
      const body = pid ? { participantId: pid } : { name: name || "" };
      try {
        const resp = await fetch(`/api/runs/${encodeURIComponent(runId)}/claim`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        const txt = await resp.text();
        if (!resp.ok) {
          if (msg) {
            msg.textContent = txt || resp.statusText;
            msg.classList.add("err");
          }
          return;
        }
        if (msg) msg.textContent = "Claimed.";
        await tickClaimTarget();
      } catch (e) {
        if (msg) {
          msg.textContent = String(e);
          msg.classList.add("err");
        }
      }
    });
  }

  function connectLiveWs() {
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = `${proto}//${window.location.host}/ws`;
    let ws;
    let reconnectTimer;

    function scheduleReconnect() {
      if (reconnectTimer) return;
      reconnectTimer = window.setTimeout(() => {
        reconnectTimer = null;
        connectLiveWs();
      }, 2000);
    }

    try {
      ws = new WebSocket(url);
    } catch (_) {
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      try {
        ws.send(".");
      } catch (_) {
        /* ignore */
      }
    };

    ws.onmessage = (ev) => {
      let msg;
      try {
        msg = JSON.parse(ev.data);
      } catch (_) {
        return;
      }
      if (!msg || typeof msg.type !== "string") return;

      if (msg.type === "timer" && typeof msg.elapsedUs === "number") {
        const el = document.getElementById("live-timer");
        if (el) el.textContent = formatDuration(msg.elapsedUs);
        return;
      }
      if (msg.type === "state" && typeof msg.state === "string") {
        const badge = document.getElementById("stateBadge");
        if (badge) badge.textContent = msg.state;
        const lt = document.getElementById("live-timer");
        if (lt && msg.state !== "RUNNING" && msg.state !== "FINISHED") {
          lt.textContent = "—";
        }
        return;
      }
      if (msg.type === "runFinished") {
        const lt = document.getElementById("live-timer");
        if (lt && typeof msg.durationUs === "number") {
          lt.textContent = formatDuration(msg.durationUs);
        }
        tickIndex();
        return;
      }
      if (msg.type === "battery" && typeof msg.percent === "number") {
        applyBatteryPercent(msg.percent);
        return;
      }
    };

    ws.onclose = () => scheduleReconnect();
    ws.onerror = () => {
      try {
        ws.close();
      } catch (_) {
        /* ignore */
      }
    };
  }

  const page = window.__CYBEER_PAGE__;
  if (page === "index") {
    tickIndex();
    setInterval(tickIndex, 5000);
    connectLiveWs();
  } else if (page === "claim") {
    tickClaimTarget();
    setInterval(tickClaimTarget, 5000);
    initClaimForm();
  }
})();
