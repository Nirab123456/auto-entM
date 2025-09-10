// static/app.js
(function(){
  const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  const runningEl = document.getElementById('running');
  const gainValEl = document.getElementById('gainVal');
  const lastSeqEl = document.getElementById('last_seq');
  const highestEl = document.getElementById('highest');
  const samplesEl = document.getElementById('samples');
  const gainSlider = document.getElementById('gain');
  const gainNumber = document.getElementById('gainNumber');
  const applyBtn = document.getElementById('apply');
  const applyWSBtn = document.getElementById('applyWS');
  const logEl = document.getElementById('log');

  function log(s){ const d = document.createElement('div'); d.textContent = s; logEl.appendChild(d); if (logEl.childNodes.length>50) logEl.removeChild(logEl.firstChild);}

  function applyGain(g) {
    // POST /control
    fetch('/control', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({gain: g})
    }).then(r => r.json()).then(j => { log('Applied gain via POST: ' + g); updateStatus(j); }).catch(e => log('POST error: ' + e));
  }

  function applyGainWS(g) {
    // Send via WebSocket as JSON
    const m = {cmd:'set', gain: g};
    ws.send(JSON.stringify(m));
    log('Sent gain via WS: ' + g);
  }

  function updateStatus(j) {
    runningEl.textContent = j.running ? 'yes' : 'no';
    gainValEl.textContent = Number(j.gain).toFixed(2);
    lastSeqEl.textContent = j.last_seq;
    highestEl.textContent = j.highest_sample_index;
    samplesEl.textContent = j.samples_written;
    gainSlider.value = j.gain;
    gainNumber.value = j.gain;
  }

  ws.onopen = () => log('WS open');
  ws.onclose = () => log('WS closed');
  ws.onmessage = (evt) => {
    try {
      const j = JSON.parse(evt.data);
      updateStatus(j);
    } catch(e) { log('ws parse err: ' + e); }
  };

  // On UI actions
  gainSlider.addEventListener('input', (e) => { gainNumber.value = e.target.value; });
  gainNumber.addEventListener('input', (e) => { gainSlider.value = e.target.value; });

  applyBtn.addEventListener('click', () => { applyGain(parseFloat(gainNumber.value)); });
  applyWSBtn.addEventListener('click', () => { applyGainWS(parseFloat(gainNumber.value)); });

  // Initial status fetch (fallback if WS doesn't update)
  fetch('/status').then(r => r.json()).then(j => updateStatus(j)).catch(e => log('status fetch failed: '+e));
})();
