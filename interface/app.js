"use strict";

/*
 * Web Serial tabanli izleme arayuzu.
 *
 * Hat bicimi (firmware/Core/Inc/protocol.h, docs/code-notes.md#paket-protokolu):
 * her mesaj ASCII metindir, bosluklarla 63 bayta tamamlanir, 64. bayt LF'dir.
 *   TEL,<seq>,S<n>,<temp_centi_c>,<temp_raw>,<vdda_mv>,<extra_us>,<period_us>,<txq>
 *   BTN,<event_id>,S<n>,PRESSED,<seq>,<t0>,<t1>,<t2>
 *   ACK,<seq>,S<n>
 *   REC,S<n>,<event_id>,<t0>,<t1-t0>,<t2-t1>,<t3-t2>,<t4-t3>,<status>
 *   CNT,S<n>,<ad>,<deger>
 *   END,S<n>,<REC_sayisi>
 *
 * Karta yalnizca kullanici bir senaryo sectiginde ya da "Olcumu bitir"e
 * bastiginda 5 baytlik komut gider; olcum sirasinda hatta komut trafigi yoktur.
 */

const LINE_SIZE = 64;
const LF = 0x0a;
const DEADLINE_US = 20000;
const REC_STATUSES = ["ok", "tx_drop", "btn_drop", "tx_error", "timeout"];

// Kart REC satirinda dusmenin nerede oldugunu ayri bildirir. CSV'ye sartnamedeki
// "drop" yazilir; neden bos zamanlardan da okunur (buton kuyrugu: yalnizca t0,
// TX kuyrugu: t0..t2).
const DROP_REASON = { tx_drop: "TX kuyruğu", btn_drop: "buton kuyruğu" };
const csvStatus = (s) => (s in DROP_REASON ? "drop" : s);

const CMD_SYNC0 = 0xaa;
const CMD_SYNC1 = 0x55;
const CMD_TYPE_SCENARIO = 0x43; // 'C'
const CMD_ARG_DUMP = 0xfe;
const CMD_ARG_QUERY = 0xff;
const ACK_TIMEOUT_MS = 1500;
const DUMP_TIMEOUT_MS = 3000; // ~100 satir x 5,56 ms ~ 0,6 s

let port = null;
let reader = null;
let keepReading = false;
let rxBuffer = new Uint8Array(0);
let skipPartialLine = true; // baglanti anindaki yarim satir hata sayilmaz
const decoder = new TextDecoder("ascii");

let lastSeq = null;
let seqGapCount = 0;
let lineErrorCount = 0;
let btnCount = 0;
let selectedEventId = null; // null = otomatik: son "ok" olay

/** event_id -> { scenarioId, t: [t0..t4] (null = bilinmiyor), status, hostRecvIso } */
const events = new Map();
let windowCounters = {}; // CNT satirlari: ad -> deger
let endRecords = null; // END ile kartin bildirdigi REC sayisi

let activeScenario = null; // kartin onayladigi senaryo
let requestedScenario = null; // ACK bekleyen istek
let ackTimer = null;
let scenarioError = "";
let unsavedEvents = false;
let dumpState = null; // null | "requested" (ACK bekleniyor) | "receiving" (END bekleniyor)
let dumpReceived = 0;

const el = (id) => document.getElementById(id);
const nowIso = () => new Date().toISOString();
const u32diff = (b, a) => (b - a) >>> 0; // TIM2 sayaci basa sarsa da dogru

function setConnected(isConnected) {
  const status = el("conn-status");
  status.textContent = isConnected ? "Bağlı" : "Bağlı değil";
  status.className = "status " + (isConnected ? "status--connected" : "status--disconnected");
  el("btn-connect").disabled = isConnected;
  el("btn-disconnect").disabled = !isConnected;
}

function showPressToast(eventId, scenarioId) {
  const toast = el("press-toast");
  toast.textContent = `Butona basıldı · Olay ${eventId} (S${scenarioId})`;
  toast.hidden = false;
  toast.style.animation = "none";
  void toast.offsetHeight; // animasyonu yeniden tetikle
  toast.style.animation = "";
  clearTimeout(showPressToast._t);
  showPressToast._t = setTimeout(() => { toast.hidden = true; }, 1600);
}

/* ---------------------------------------------------------------------
 * Web Serial baglanti yonetimi
 * ------------------------------------------------------------------- */

async function connect() {
  if (!("serial" in navigator)) {
    el("serial-support-warning").hidden = false;
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    // Varsayilan tampon 255 bayt (4 satir): sayfa kisa bir an takilirsa veri kaybolur.
    // Hiz, kartin UART_BAUDRATE degeriyle (main.h) ayni olmali. Sartname tabani
    // 115200; jitter deneyi icin 230400'e cikarildi.
    // await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none", bufferSize: 65536 });
    await port.open({ baudRate: 230400, dataBits: 8, stopBits: 1, parity: "none", bufferSize: 65536 });
    keepReading = true;
    rxBuffer = new Uint8Array(0);
    skipPartialLine = true;
    lastSeq = null; // baglanti yokken cikan satirlar kayip sayilmasin
    setConnected(true);
    readLoop();
    renderScenario();
    queryScenario();
  } catch (err) {
    console.error("Bağlantı hatası:", err);
  }
}

async function disconnect() {
  keepReading = false;
  try {
    if (reader) await reader.cancel();
  } catch (err) {
    /* zaten kapanmis olabilir */
  }
  try {
    if (port) await port.close();
  } catch (err) {
    /* yoksay */
  }
  port = null;
  reader = null;
  setConnected(false);
  clearTimeout(ackTimer);
  activeScenario = null;
  requestedScenario = null;
  dumpState = null;
  scenarioError = "";
  renderScenario();
}

async function readLoop() {
  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) appendBytes(value);
      }
    } catch (err) {
      console.error("Okuma hatası:", err);
    } finally {
      reader.releaseLock();
    }
  }
}

/* ---------------------------------------------------------------------
 * Senaryo secimi ve olcum penceresi (yer istasyonu -> kart komutu)
 * Aktif senaryo YALNIZCA kartin ACK'i ile degisir.
 * ------------------------------------------------------------------- */

async function sendCommand(arg) {
  if (!port || !port.writable) return false;
  const b = new Uint8Array([CMD_SYNC0, CMD_SYNC1, CMD_TYPE_SCENARIO, arg, 0]);
  b[4] = (b[0] + b[1] + b[2] + b[3]) & 0xff;
  const writer = port.writable.getWriter();
  try {
    await writer.write(b);
    return true;
  } catch (err) {
    console.error("Komut gönderilemedi:", err);
    return false;
  } finally {
    writer.releaseLock();
  }
}

function armAckTimeout(message, ms = ACK_TIMEOUT_MS) {
  clearTimeout(ackTimer);
  ackTimer = setTimeout(() => {
    requestedScenario = null;
    dumpState = null;
    scenarioError = message;
    renderScenario();
  }, ms);
}

async function queryScenario() {
  scenarioError = "";
  if (await sendCommand(CMD_ARG_QUERY)) {
    armAckTimeout("Kart senaryo sorgusuna yanıt vermedi: senaryo değiştirilemez. USB-TTL TX → PA3 bağlı mı, firmware güncel mi?");
  }
}

async function requestScenario(id) {
  if (!port || requestedScenario !== null || dumpState !== null || id === activeScenario) return;
  if (
    unsavedEvents &&
    events.size > 0 &&
    !window.confirm(`Listede CSV'ye kaydedilmemiş ${events.size} olay var. Senaryo değişince liste temizlenecek. Devam edilsin mi?`)
  ) {
    return;
  }
  scenarioError = "";
  requestedScenario = id;
  renderScenario();
  if (!(await sendCommand(id))) {
    requestedScenario = null;
    scenarioError = "Komut gönderilemedi.";
    renderScenario();
    return;
  }
  armAckTimeout(`Kart S${id} isteğine yanıt vermedi. USB-TTL TX → PA3 bağlantısını kontrol edin.`);
}

// Olcum penceresini kapatir: kart susar (S0), kayitlari REC + CNT + END olarak doker.
async function requestDump() {
  if (!port || requestedScenario !== null || dumpState !== null) return;
  dumpState = "requested";
  dumpReceived = 0;
  endRecords = null;
  windowCounters = {};
  scenarioError = "";
  el("dump-summary").textContent = "";
  renderWindowCounters();
  renderScenario();
  if (!(await sendCommand(CMD_ARG_DUMP))) {
    dumpState = null;
    scenarioError = "Komut gönderilemedi.";
    renderScenario();
    return;
  }
  armAckTimeout("Kart ölçümü bitir komutuna yanıt vermedi. USB-TTL TX → PA3 bağlantısını kontrol edin.");
}

function onAck(scenarioId) {
  clearTimeout(ackTimer);
  if (dumpState === "requested") {
    // Olcum bitti: kart S0'a gecti, kayitlar geliyor. Liste TEMIZLENMEZ.
    dumpState = "receiving";
    activeScenario = scenarioId;
    scenarioError = "";
    armAckTimeout("Döküm tamamlanmadı: END gelmedi. Tekrar 'Ölçümü bitir'e basabilirsiniz.", DUMP_TIMEOUT_MS);
    renderScenario();
    return;
  }
  const requested = requestedScenario === scenarioId;
  const unexpected = !requested && activeScenario !== null && activeScenario !== scenarioId;
  activeScenario = scenarioId;
  requestedScenario = null;
  scenarioError = unexpected ? `Kart senaryoyu kendisi S${scenarioId} yaptı (yeniden başlatıldı mı?).` : "";
  if (requested) {
    // Yeni olcum penceresi: liste, sayaclar ve eski telemetri degerleri temizlenir.
    clearEvents();
    resetTelemetryPanel();
  }
  renderScenario();
}

function onEnd(records) {
  clearTimeout(ackTimer);
  dumpState = null;
  endRecords = records;

  // Kayitsiz kalan canli olaylar: kart bu olay icin REC gondermedi.
  for (const ev of events.values()) {
    if (ev.status === "pending") ev.status = "no_record";
  }

  const byStatus = {};
  for (const ev of events.values()) {
    const s = ev.status in DROP_REASON ? `drop (${DROP_REASON[ev.status]})` : ev.status;
    byStatus[s] = (byStatus[s] || 0) + 1;
  }
  const parts = Object.entries(byStatus).map(([s, n]) => `${s} ${n}`).join(", ");
  const accepted = windowCounters.accepted;

  const problems = [];
  if (dumpReceived !== records) problems.push(`kart ${records} kayıt gönderdi, ${records - dumpReceived} tanesi yolda kayboldu`);
  if (accepted !== undefined && accepted !== records) problems.push(`kabul edilen ${accepted} basıştan ${accepted - records} tanesinin kaydı yok`);
  for (const k of ["pool_overflow", "droplog_overflow", "tx_start_fail", "spurious_tc", "encode_error"]) {
    if (windowCounters[k] > 0) problems.push(`${k} = ${windowCounters[k]}`);
  }

  const summary = el("dump-summary");
  summary.textContent =
    `Ölçüm penceresi kapandı: ${dumpReceived} kayıt alındı (${parts || "olay yok"}).` +
    (problems.length ? ` Uyarı: ${problems.join("; ")}.` : " Kayıt bütünlüğü tam.");
  summary.classList.toggle("bad", problems.length > 0);

  renderScenario();
  renderEvents();
  renderBreakdown();
  drawChart();
}

function renderScenario() {
  const connected = !!port;
  const busy = requestedScenario !== null || dumpState !== null;
  for (const btn of document.querySelectorAll(".sc-btn")) {
    const id = Number(btn.dataset.sc);
    btn.disabled = !connected || busy;
    btn.classList.toggle("active", id === activeScenario);
    btn.classList.toggle("pending", id === requestedScenario);
  }
  el("btn-dump").disabled = !connected || busy;
  el("stat-scenario").textContent = activeScenario === null ? "—" : "S" + activeScenario;

  const status = el("sc-status");
  status.classList.toggle("bad", !!scenarioError);
  status.textContent = !connected
    ? "Bağlı değil"
    : scenarioError
    ? scenarioError
    : dumpState === "requested"
    ? "Ölçüm bitiriliyor, kartın onayı bekleniyor…"
    : dumpState === "receiving"
    ? `Kart sustu (S0), kayıtlar alınıyor… (${dumpReceived})`
    : requestedScenario !== null
    ? `S${requestedScenario} isteniyor, kartın onayı bekleniyor…`
    : activeScenario !== null
    ? `Aktif: S${activeScenario} (kart onayladı). Değiştirmek için bir senaryo seçin.`
    : "Kartın senaryosu bekleniyor…";
}

/* ---------------------------------------------------------------------
 * Satir cozme: LF ile cercevele, her satirin tam 64 bayt oldugunu dogrula
 * ------------------------------------------------------------------- */

function appendBytes(chunk) {
  const merged = new Uint8Array(rxBuffer.length + chunk.length);
  merged.set(rxBuffer, 0);
  merged.set(chunk, rxBuffer.length);
  rxBuffer = merged;
  processBuffer();
}

function lineError() {
  lineErrorCount++;
  el("stat-frame-errors").textContent = String(lineErrorCount);
}

function processBuffer() {
  for (;;) {
    const lf = rxBuffer.indexOf(LF);
    if (lf === -1) {
      if (rxBuffer.length > LINE_SIZE) {
        lineError(); // 64 bayt icinde LF yok: bozuk veri
        rxBuffer = new Uint8Array(0);
      }
      return;
    }
    const line = rxBuffer.subarray(0, lf + 1);
    rxBuffer = rxBuffer.slice(lf + 1);
    if (line.length !== LINE_SIZE) {
      if (!skipPartialLine) lineError();
      skipPartialLine = false;
      continue;
    }
    skipPartialLine = false;
    if (!handleLine(decoder.decode(line.subarray(0, LINE_SIZE - 1)).trimEnd())) lineError();
  }
}

const num = (s) => (/^\d+$/.test(s) ? Number(s) : null);
const int = (s) => (/^-?\d+$/.test(s) ? Number(s) : null);
const scen = (s) => (/^S[0-5]$/.test(s) ? Number(s[1]) : null);
const allValid = (...xs) => xs.every((x) => x !== null);

function handleLine(text) {
  const f = text.split(",");
  switch (f[0]) {
    case "TEL": return f.length === 9 && onTel(f);
    case "BTN": return f.length === 8 && f[3] === "PRESSED" && onBtn(f);
    case "ACK": return f.length === 3 && onAckLine(f);
    case "REC": return f.length === 9 && onRec(f);
    case "CNT": return f.length === 4 && onCnt(f);
    case "END": return f.length === 3 && onEndLine(f);
    default: return false;
  }
}

function trackSeq(seq) {
  if (lastSeq !== null) {
    const expected = (lastSeq + 1) & 0xffff;
    if (seq !== expected) {
      seqGapCount += (seq - expected) & 0xffff;
      el("stat-seq-gaps").textContent = String(seqGapCount);
    }
  }
  lastSeq = seq;
}

// Canli satirlar (TEL/BTN) yalnizca aktif senaryoya aitse islenir; senaryo
// degisiminden once kuyruga girmis gec satirlar yok sayilir.
function acceptLive(sc) {
  if (activeScenario === null) {
    activeScenario = sc; // sorgu yaniti henuz gelmedi: ilk satirdan ogren
    renderScenario();
    return true;
  }
  return sc === activeScenario;
}

function onTel(f) {
  const seq = num(f[1]), sc = scen(f[2]), temp = int(f[3]), raw = num(f[4]), vdda = num(f[5]);
  const extra = num(f[6]), period = num(f[7]), txq = num(f[8]);
  if (!allValid(seq, sc, temp, raw, vdda, extra, period, txq)) return false;
  trackSeq(seq);
  if (!acceptLive(sc)) return true;

  el("tel-temp").textContent = (temp / 100).toFixed(2) + " °C";
  el("tel-temp-raw").textContent = `${raw} / ${vdda} mV`;
  el("tel-period").textContent =
    period === 0 ? "—" : `${(period / 1000).toFixed(2)} ms (${(1e6 / period).toFixed(1)} Hz)`;
  el("tel-txq").textContent = `${txq} / 16`;
  el("tel-extra-load").textContent = extra + " µs";
  el("tel-last-time").textContent = `${new Date().toLocaleTimeString()} · seq ${seq}`;
  return true;
}

function onBtn(f) {
  const id = num(f[1]), sc = scen(f[2]), seq = num(f[4]);
  const t0 = num(f[5]), t1 = num(f[6]), t2 = num(f[7]);
  if (!allValid(id, sc, seq, t0, t1, t2)) return false;
  trackSeq(seq);
  if (!acceptLive(sc)) return true;

  unsavedEvents = true;
  btnCount++;
  el("stat-btn-count").textContent = String(btnCount);
  el("stat-last-event").textContent = String(id);
  // Canli BTN yalnizca t0..t2 tasir; t3/t4 ve durum kartin kayit havuzunda
  // kapanir ve "Olcumu bitir" ile REC olarak gelir.
  events.set(id, { scenarioId: sc, t: [t0, t1, t2, null, null], status: "pending", hostRecvIso: nowIso() });
  showPressToast(id, sc);
  renderEvents();
  renderBreakdown();
  return true;
}

function onAckLine(f) {
  const seq = num(f[1]), sc = scen(f[2]);
  if (!allValid(seq, sc)) return false;
  trackSeq(seq);
  onAck(sc);
  return true;
}

function onRec(f) {
  const sc = scen(f[1]), id = num(f[2]), t0 = num(f[3]), status = f[8];
  if (!allValid(sc, id, t0) || !REC_STATUSES.includes(status)) return false;
  const t = [t0, null, null, null, null];
  for (let i = 1; i <= 4; i++) {
    const s = f[3 + i];
    if (s === "") continue; // bilinmeyen ya da >= 1 s: bos birakilmis
    const d = num(s);
    if (d === null) return false;
    if (t[i - 1] !== null) t[i] = (t[i - 1] + d) >>> 0;
  }
  // Dokum kaydi senaryo filtresinden muaftir: kart bu sirada zaten S0'dadir.
  const ev = events.get(id) || { hostRecvIso: nowIso() };
  Object.assign(ev, { scenarioId: sc, t, status });
  events.set(id, ev);
  unsavedEvents = true;
  if (dumpState !== null) dumpReceived++;
  renderScenario();
  renderEvents();
  renderBreakdown();
  drawChart();
  return true;
}

function onCnt(f) {
  const sc = scen(f[1]), name = f[2], value = num(f[3]);
  if (!allValid(sc, value) || !/^[a-z_]+$/.test(name)) return false;
  windowCounters[name] = value;
  renderWindowCounters();
  return true;
}

function onEndLine(f) {
  const sc = scen(f[1]), records = num(f[2]);
  if (!allValid(sc, records)) return false;
  onEnd(records);
  return true;
}

/* ---------------------------------------------------------------------
 * Paneller
 * ------------------------------------------------------------------- */

const COUNTER_LABELS = {
  accepted: "Kabul edilen basış",
  repeat: "Elenen kenar (sıçrama)",
  btn_queue_drop: "Buton kuyruğu dolu",
  tx_queue_drop: "TX kuyruğu dolu (TEL+BTN)",
  tx_queue_max: "TX kuyruğu tepe (16)",
  pool_overflow: "Kayıt havuzu taşması (64)",
  droplog_overflow: "Düşen olay kaydı taşması",
  tx_timeout: "TX zaman aşımı (TC)",
  tx_start_fail: "TX başlatma hatası",
  spurious_tc: "Sahte TC",
  encode_error: "63 bayta sığmayan satır",
  tel_sent: "Gönderilen TEL",
  tel_period_min_us: "TEL periyodu min",
  tel_period_avg_us: "TEL periyodu ort.",
  tel_period_max_us: "TEL periyodu maks",
  btn_direct: "BTN hızlı yol: hemen başladı",
  btn_latched: "BTN hızlı yol: mandaldan başladı",
  btn_fallback: "BTN yedek yol (ButtonTask)",
  btn_result_lost: "BTN sonucu kuyruğa giremedi",
};

function renderWindowCounters() {
  const names = Object.keys(windowCounters);
  el("stat-window-hint").hidden = names.length > 0;
  el("stat-window").innerHTML = names
    .map((n) => {
      const v = windowCounters[n];
      const shown = n.startsWith("tel_period") ? `${(v / 1000).toFixed(2)} ms` : String(v);
      return `<dt>${COUNTER_LABELS[n] || n}</dt><dd>${shown}</dd>`;
    })
    .join("");
}

function resetTelemetryPanel() {
  for (const id of ["tel-temp", "tel-temp-raw", "tel-period", "tel-txq", "tel-extra-load", "tel-last-time"]) {
    el(id).textContent = "—";
  }
}

const STATUS_BADGE = {
  pending: '<span class="badge badge--pending">ölçüm sürüyor</span>',
  no_record: '<span class="badge badge--drop">kayıt yok</span>',
  tx_drop: '<span class="badge badge--miss">drop · TX kuyruğu</span>',
  btn_drop: '<span class="badge badge--miss">drop · buton kuyruğu</span>',
  tx_error: '<span class="badge badge--miss">tx_error</span>',
  timeout: '<span class="badge badge--miss">timeout</span>',
};

const rOf = (ev) => (ev.t[4] !== null ? u32diff(ev.t[4], ev.t[0]) : null);

function statusBadge(ev) {
  if (ev.status === "ok") {
    return rOf(ev) > DEADLINE_US
      ? '<span class="badge badge--miss">ok · aşıldı</span>'
      : '<span class="badge badge--ok">ok</span>';
  }
  return STATUS_BADGE[ev.status] || ev.status;
}

function renderEvents() {
  const rows = [...events.entries()].sort((a, b) => b[0] - a[0]);
  el("events-tbody").innerHTML = rows
    .map(([id, ev]) => {
      const r = rOf(ev);
      const budget =
        ev.status !== "ok"
          ? "—"
          : r > DEADLINE_US
          ? '<span class="badge badge--miss">R &gt; 20ms</span>'
          : '<span class="badge badge--ok">R ≤ 20ms</span>';
      const cls = id === selectedEventId ? ' class="selected"' : "";
      const cell = (v) => (v === null ? "—" : v);
      return `<tr data-id="${id}"${cls}>
        <td>${id}</td>
        <td>S${ev.scenarioId}</td>
        <td>${cell(ev.t[0])}</td>
        <td>${cell(ev.t[1])}</td>
        <td>${cell(ev.t[2])}</td>
        <td>${cell(ev.t[3])}</td>
        <td>${cell(ev.t[4])}</td>
        <td>${r === null ? "—" : (r / 1000).toFixed(2)}</td>
        <td>${budget}</td>
        <td>${statusBadge(ev)}</td>
      </tr>`;
    })
    .join("");
}

/* ---------------------------------------------------------------------
 * Gecikme dagilimi: R = (t1-t0) + (t2-t1) + (t3-t2) + (t4-t3)
 * ------------------------------------------------------------------- */

const SEGMENTS = [
  { key: "wait", short: "Görev bekleme", label: "t₁−t₀: görev bekleme", cls: "seg-wait" },
  { key: "prep", short: "Hazırlama", label: "t₂−t₁: hazırlama", cls: "seg-prep" },
  { key: "txq", short: "TX öncesi", label: "t₃−t₂: TX öncesi", cls: "seg-txq" },
  { key: "uart", short: "UART + TC", label: "t₄−t₃: UART + TC", cls: "seg-uart" },
];

const fmtMs = (us) =>
  (us / 1000).toLocaleString("tr-TR", { minimumFractionDigits: 2, maximumFractionDigits: 2 });

// 1 ms altindaki bilesenler (orn. 2 us hazirlama) "0,00 ms" gorunmesin.
const fmtDur = (us) => (us < 1000 ? `${Math.round(us)} µs` : `${fmtMs(us)} ms`);

function partsOf(ev) {
  const d = (i) => (ev.t[i - 1] !== null && ev.t[i] !== null ? u32diff(ev.t[i], ev.t[i - 1]) : null);
  return { wait: d(1), prep: d(2), txq: d(3), uart: d(4) };
}

const sumParts = (p) => SEGMENTS.reduce((s, seg) => s + (p[seg.key] ?? 0), 0);

function pickEvent() {
  if (selectedEventId !== null && events.has(selectedEventId)) {
    return [selectedEventId, events.get(selectedEventId)];
  }
  const ok = [...events.entries()].filter(([, ev]) => ev.status === "ok");
  const pool = ok.length ? ok : [...events.entries()];
  if (!pool.length) return null;
  return pool.reduce((a, b) => (b[0] > a[0] ? b : a));
}

function axisFor(maxUs) {
  const maxMs = maxUs / 1000;
  const top = [25, 50, 100, 150, 200, 300, 500, 1000].find((v) => maxMs <= v * 0.98) ??
    Math.ceil(maxMs / 500) * 500;
  return { maxUs: top * 1000, stepMs: top / 5 };
}

function barHtml(parts, axisMaxUs) {
  const segs = SEGMENTS.filter((s) => parts[s.key] !== null)
    .map((s) => {
      const v = parts[s.key];
      const pct = (v / axisMaxUs) * 100;
      const text = pct >= 9 ? s.short : "";
      return `<div class="seg ${s.cls}" style="width:${pct}%" title="${s.label}: ${fmtDur(v)}">${text}</div>`;
    })
    .join("");
  return segs + `<div class="bd-deadline" style="left:${(DEADLINE_US / axisMaxUs) * 100}%"></div>`;
}

function setBig(id, text, bad) {
  el(id).textContent = text;
  el(id).className = "bd-big" + (text === "—" ? " muted" : bad ? " bad" : "");
}

const STATUS_VERDICT = {
  pending: "Ölçüm sürüyor: t₃, t₄ ve durum kartın kayıt havuzunda. Sonuç, 'Ölçümü bitir ve kayıtları al' ile gelir.",
  no_record: "Kart bu olay için kayıt göndermedi (havuz ya da düşen olay kaydı taşmış olabilir).",
  tx_drop: "drop (TX kuyruğu): yanıt t₂'de TX kuyruğuna giremedi (kuyruk dolu); t₃/t₄ yok. Kayıp yanıt deadline'ı karşılamış sayılmaz.",
  btn_drop: "drop (buton kuyruğu): olay ISR'de buton kuyruğuna giremedi (kuyruk dolu); yalnızca t₀ var.",
  tx_error: "tx_error: UART gönderimi başlatılamadı ya da TC başka bir aktarıma aitti.",
  timeout: "timeout: TC 50 ms içinde gelmedi ya da R ≥ 1 s (deney zaman aşımı).",
};

function renderBreakdown() {
  const picked = pickEvent();
  const verdict = el("bd-verdict");

  if (!picked) {
    for (const id of ["bd-bar-event", "bd-bar-avg", "bd-axis", "bd-legend"]) el(id).innerHTML = "";
    el("bd-row-avg").hidden = true;
    setBig("bd-r", "—");
    setBig("bd-margin", "—");
    el("bd-selected").textContent = "";
    verdict.className = "bd-verdict pending";
    verdict.textContent = "Henüz buton olayı yok.";
    return;
  }

  const [eventId, ev] = picked;
  const parts = partsOf(ev);
  const total = sumParts(parts);

  const okEvents = [...events.values()].filter((e) => e.status === "ok");
  let avg = null;
  if (okEvents.length) {
    avg = {};
    for (const s of SEGMENTS) {
      avg[s.key] = okEvents.reduce((acc, e) => acc + partsOf(e)[s.key], 0) / okEvents.length;
    }
  }

  const axis = axisFor(Math.max(total, avg ? sumParts(avg) : 0, DEADLINE_US));
  el("bd-label-event").textContent = `Olay #${eventId}`;
  el("bd-bar-event").innerHTML = barHtml(parts, axis.maxUs);
  el("bd-row-avg").hidden = !avg;
  if (avg) {
    el("bd-label-avg").textContent = `Ortalama (${okEvents.length} ok olay)`;
    el("bd-bar-avg").innerHTML = barHtml(avg, axis.maxUs);
  }

  let ticks = "";
  for (let ms = 0; ms <= axis.maxUs / 1000; ms += axis.stepMs) {
    const pct = ((ms * 1000) / axis.maxUs) * 100;
    ticks += `<span${pct >= 99.9 ? ' class="last"' : ""} style="left:${pct}%">${ms}</span>`;
  }
  el("bd-axis").innerHTML = ticks;

  el("bd-legend").innerHTML =
    SEGMENTS.map((s) => {
      const v = parts[s.key];
      return `<span><i class="${s.cls}"></i>${s.label}: <b>${v === null ? "—" : fmtDur(v)}</b></span>`;
    }).join("") + '<span><i class="bd-legend-dl"></i>D = 20 ms</span>';

  el("bd-selected").textContent =
    selectedEventId === null
      ? "Otomatik: son 'ok' olay. Başka bir olay için tablodan satıra tıklayın."
      : "Seçili olay. Otomatiğe dönmek için aynı satıra tekrar tıklayın.";

  if (ev.status !== "ok") {
    setBig("bd-r", "—");
    setBig("bd-margin", "—");
    verdict.className = "bd-verdict " + (ev.status === "pending" ? "pending" : "bad");
    verdict.textContent = STATUS_VERDICT[ev.status] || ev.status;
    return;
  }

  const margin = DEADLINE_US - total;
  const ok = margin >= 0;
  setBig("bd-r", fmtMs(total) + " ms", !ok);
  setBig("bd-margin", (ok ? "+" : "−") + fmtMs(Math.abs(margin)) + " ms", !ok);

  const biggest = SEGMENTS.reduce((a, b) => (parts[b.key] > parts[a.key] ? b : a));
  verdict.className = "bd-verdict" + (ok ? "" : " bad");
  verdict.textContent =
    (ok ? `Deadline karşılanıyor (${fmtMs(margin)} ms pay).` : `Deadline ${fmtMs(-margin)} ms aşıldı.`) +
    ` En büyük bileşen: ${biggest.label} (%${Math.round((parts[biggest.key] / total) * 100)}).`;
}

/* ---------------------------------------------------------------------
 * Grafik: olay numarasi -> R. Cizim tamponu, canvas'in gercek CSS boyutu
 * x devicePixelRatio'ya ayarlanir; aksi halde tarayici sabit genislikli
 * tamponu yatayda gerer (egik yazi, elips noktalar).
 * ------------------------------------------------------------------- */

function drawChart() {
  const canvas = el("chart");
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (w === 0 || h === 0) return;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const done = [...events.entries()].filter(([, ev]) => ev.status === "ok").sort((a, b) => a[0] - b[0]);

  const padL = 56, padR = 16, padT = 14, padB = 30;
  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  const maxR = Math.max(DEADLINE_US * 1.5, ...done.map(([, ev]) => rOf(ev)));
  const yOf = (us) => padT + plotH - (us / maxR) * plotH;

  ctx.strokeStyle = "#2a3348";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(padL, padT);
  ctx.lineTo(padL, padT + plotH);
  ctx.lineTo(padL + plotW, padT + plotH);
  ctx.stroke();

  ctx.fillStyle = "#9aa4b8";
  ctx.font = "11px sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const us of [0, DEADLINE_US, maxR]) {
    ctx.fillText(`${Math.round(us / 1000)} ms`, padL - 6, yOf(us));
  }

  ctx.strokeStyle = "#ff5c5c";
  ctx.setLineDash([5, 4]);
  ctx.beginPath();
  ctx.moveTo(padL, yOf(DEADLINE_US));
  ctx.lineTo(padL + plotW, yOf(DEADLINE_US));
  ctx.stroke();
  ctx.setLineDash([]);

  if (done.length === 0) return;

  const n = done.length;
  const xOf = (i) => padL + (n === 1 ? plotW / 2 : (i / (n - 1)) * plotW);
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillStyle = "#9aa4b8";
  ctx.fillText(`olay ${done[0][0]}`, xOf(0) + (n === 1 ? 0 : 16), padT + plotH + 8);
  if (n > 1) ctx.fillText(`olay ${done[n - 1][0]}`, xOf(n - 1) - 16, padT + plotH + 8);

  done.forEach(([, ev], i) => {
    const r = rOf(ev);
    ctx.fillStyle = r > DEADLINE_US ? "#ffb020" : "#4c9aff";
    ctx.beginPath();
    ctx.arc(xOf(i), yOf(r), 4, 0, Math.PI * 2);
    ctx.fill();
  });
}

/* ---------------------------------------------------------------------
 * CSV disa aktarma — sartname bicimi:
 *   scenario,event_id,t0_us,t1_us,t2_us,t3_us,t4_us,status
 * Eksik zaman 0 yazilmaz, bos birakilir. Pencere sayaclari ayri dosyadadir.
 * status sartname degerleridir (ok, drop, tx_error, timeout); bkz. csvStatus.
 * ------------------------------------------------------------------- */

function download(name, text) {
  const blob = new Blob([text], { type: "text/csv" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

function exportCsv() {
  if (events.size === 0) {
    window.alert("Kaydedilecek olay yok.");
    return;
  }
  const pending = [...events.values()].filter((e) => e.status === "pending").length;
  if (pending > 0 && !window.confirm(`Ölçüm bitirilmedi: ${pending} olay "pending" olarak kaydedilecek. Önce "Ölçümü bitir" önerilir. Yine de kaydedilsin mi?`)) {
    return;
  }

  const sorted = [...events.entries()].sort((a, b) => a[0] - b[0]);
  const scenario = sorted[0][1].scenarioId;
  const rows = ["scenario,event_id,t0_us,t1_us,t2_us,t3_us,t4_us,status"];
  for (const [id, ev] of sorted) {
    rows.push([`S${ev.scenarioId}`, id, ...ev.t.map((v) => (v === null ? "" : v)), csvStatus(ev.status)].join(","));
  }
  download(`S${scenario}.csv`, rows.join("\n") + "\n");

  const counters = ["name,value"];
  for (const [k, v] of Object.entries(windowCounters)) counters.push(`${k},${v}`);
  counters.push(`host_rec_received,${dumpReceived}`);
  counters.push(`host_rec_expected,${endRecords ?? ""}`);
  counters.push(`host_seq_gaps,${seqGapCount}`);
  counters.push(`host_line_errors,${lineErrorCount}`);
  download(`S${scenario}_counters.csv`, counters.join("\n") + "\n");

  unsavedEvents = false;
}

function clearEvents() {
  events.clear();
  btnCount = 0;
  seqGapCount = 0;
  lineErrorCount = 0;
  lastSeq = null;
  dumpReceived = 0;
  endRecords = null;
  windowCounters = {};
  el("stat-btn-count").textContent = "0";
  el("stat-seq-gaps").textContent = "0";
  el("stat-frame-errors").textContent = "0";
  el("stat-last-event").textContent = "—";
  el("dump-summary").textContent = "";
  selectedEventId = null;
  unsavedEvents = false;
  renderWindowCounters();
  renderEvents();
  renderBreakdown();
  drawChart();
}

/* ---------------------------------------------------------------------
 * Kurulum
 * ------------------------------------------------------------------- */

if (!("serial" in navigator)) {
  el("serial-support-warning").hidden = false;
}

el("btn-connect").addEventListener("click", connect);
el("btn-disconnect").addEventListener("click", disconnect);
el("btn-export-csv").addEventListener("click", exportCsv);
el("btn-clear").addEventListener("click", clearEvents);
el("btn-dump").addEventListener("click", requestDump);
el("events-tbody").addEventListener("click", (e) => {
  const row = e.target.closest("tr[data-id]");
  if (!row) return;
  const id = Number(row.dataset.id);
  selectedEventId = selectedEventId === id ? null : id;
  renderEvents();
  renderBreakdown();
});
el("sc-buttons").addEventListener("click", (e) => {
  const btn = e.target.closest(".sc-btn");
  if (btn && !btn.disabled) requestScenario(Number(btn.dataset.sc));
});
window.addEventListener("resize", drawChart);

renderScenario();
renderWindowCounters();
renderBreakdown();
drawChart();
