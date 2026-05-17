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

  function renderStatus(s) {
    const badge = document.getElementById("stateBadge");
    const bat = document.getElementById("batteryPct");
    const ver = document.getElementById("fwVer");
    if (badge) badge.textContent = s.state || "—";
    if (bat) bat.textContent = `${s.batteryPercent ?? "—"}%`;
    if (ver) ver.textContent = s.firmwareVersion ? `fw ${s.firmwareVersion}` : "";
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

  const page = window.__CYBEER_PAGE__;
  if (page === "index") {
    tickIndex();
    setInterval(tickIndex, 5000);
  } else if (page === "claim") {
    tickClaimTarget();
    setInterval(tickClaimTarget, 5000);
    initClaimForm();
  }
})();
