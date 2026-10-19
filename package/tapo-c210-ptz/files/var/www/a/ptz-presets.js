/**
 * PTZ Presets, Angle Presets, Speed Control & Smart Queue for OpenIPC
 */
(function() {
  // Speed definitions: index 1..5 -> { step: half-steps, cruise: us, label: name }
  const SPEED_CONFIG = {
    1: { step: 40, cruise: 4500, label: '1x (Fine)' },
    2: { step: 80, cruise: 3500, label: '2x (Normal)' },
    3: { step: 130, cruise: 2500, label: '3x (Brisk)' },
    4: { step: 200, cruise: 2000, label: '4x (Fast)' },
    5: { step: 300, cruise: 1600, label: '5x (Max)' }
  };

  let currentSpeed = 3;
  try {
    const saved = localStorage.getItem('mj_ptz_speed');
    if (saved && SPEED_CONFIG[saved]) currentSpeed = parseInt(saved, 10);
  } catch(e) {}

  // Orientation safety shim
  if (window.MajesticPtzOrient) {
    const origOf = window.MajesticPtzOrient.of;
    window.MajesticPtzOrient.of = function(t) {
      if (t && typeof t === 'object' && t.rotate === undefined) {
        t.rotate = 0;
      }
      return origOf(t);
    };
  }

  function initPresets() {
    const ptzBox = document.getElementById('mj-ptz');
    if (!ptzBox) {
      setTimeout(initPresets, 200);
      return;
    }

    if (document.getElementById('mj-ptz-preset-dock')) return;

    function showToast(msg, duration) {
      const toasts = document.getElementById('mj-toasts');
      if (!toasts) return;
      const t = document.createElement('p');
      t.className = 'mj-adapt-toast small';
      t.setAttribute('role', 'status');
      t.textContent = msg;
      t.style.cursor = 'pointer';
      t.addEventListener('click', () => t.remove());
      toasts.appendChild(t);
      setTimeout(() => { if (t.parentNode) t.remove(); }, duration || 3000);
    }

    const fetcher = window.apiFetch || window.fetch;

    // Create Presets & Controls Dock Widget
    const dock = document.createElement('div');
    dock.id = 'mj-ptz-preset-dock';
    dock.className = 'mj-ptz-presets';
    dock.style.cssText = [
      'display: flex',
      'flex-direction: column',
      'gap: 4px',
      'padding: 5px 6px',
      'width: calc(3 * 2.625rem + 16px)',
      'min-width: 152px',
      'background: rgba(13, 15, 20, 0.85)',
      'border: 1px solid rgba(255, 255, 255, 0.12)',
      'border-radius: var(--bs-border-radius, 4px)',
      'box-sizing: border-box',
      'font-family: inherit',
      'color: #e5e7eb'
    ].join('; ');

    dock.innerHTML = `
      <!-- Presets Header -->
      <div style="display: flex; align-items: center; justify-content: space-between;">
        <span style="display: flex; align-items: center; gap: 4px; font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: rgba(255,255,255,0.7);">
          <span id="mj-preset-status-dot" style="display: inline-block; width: 6px; height: 6px; border-radius: 50%; background: #9ca3af;"></span>
          Presets
        </span>
        <button id="mj-preset-cal" type="button" title="Recalibrate motor endstops (~10s)" style="background: none; border: none; color: rgba(255,255,255,0.45); font-size: 10px; cursor: pointer; padding: 0 2px;">⟳ Calib</button>
      </div>

      <!-- Presets Dropdown & Go -->
      <div style="display: flex; gap: 3px; align-items: center;">
        <select id="mj-preset-sel" class="form-select form-select-sm" style="flex: 1; background: #181b22; color: #fff; border: 1px solid rgba(255,255,255,0.2); font-size: 11px; padding: 2px 4px; height: 24px; border-radius: 3px;">
          <option value="">Loading...</option>
        </select>
        <button id="mj-preset-go" type="button" class="btn btn-sm btn-primary" title="Go to selected preset" style="font-size: 11px; padding: 1px 7px; height: 24px; line-height: 20px;">Go</button>
      </div>

      <!-- Save & Delete -->
      <div style="display: flex; gap: 3px;">
        <button id="mj-preset-save" type="button" class="btn btn-sm btn-outline-success" title="Save current camera angle as preset" style="flex: 1; font-size: 10px; padding: 1px 4px; height: 21px; line-height: 17px; color: #4ade80; border-color: rgba(74, 222, 128, 0.4);">+ Save View</button>
        <button id="mj-preset-del" type="button" class="btn btn-sm btn-outline-danger" title="Delete selected preset" style="font-size: 10px; padding: 1px 6px; height: 21px; line-height: 17px; color: #f87171; border-color: rgba(248, 113, 113, 0.4);">✕</button>
      </div>

      <!-- Horizontal Angle Presets -->
      <div style="border-top: 1px solid rgba(255,255,255,0.08); padding-top: 3px; margin-top: 1px;">
        <div style="font-size: 9px; font-weight: 600; text-transform: uppercase; color: rgba(255,255,255,0.5); margin-bottom: 2px;">Pan Angle</div>
        <div style="display: flex; justify-content: space-between; gap: 2px;">
          <button type="button" class="mj-pan-angle-btn" data-deg="0" data-pan="0" title="Pan to 0° (Left Stop)" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">0°</button>
          <button type="button" class="mj-pan-angle-btn" data-deg="45" data-pan="488" title="Pan to 45°" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">45°</button>
          <button type="button" class="mj-pan-angle-btn" data-deg="90" data-pan="975" title="Pan to 90°" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">90°</button>
          <button type="button" class="mj-pan-angle-btn" data-deg="180" data-pan="1950" title="Pan to 180° (Center)" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">180°</button>
          <button type="button" class="mj-pan-angle-btn" data-deg="270" data-pan="2925" title="Pan to 270°" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">270°</button>
        </div>
      </div>

      <!-- Vertical Angle Presets -->
      <div style="border-top: 1px solid rgba(255,255,255,0.08); padding-top: 3px; margin-top: 1px;">
        <div style="font-size: 9px; font-weight: 600; text-transform: uppercase; color: rgba(255,255,255,0.5); margin-bottom: 2px;">Tilt Angle</div>
        <div style="display: flex; justify-content: space-between; gap: 2px;">
          <button type="button" class="mj-tilt-angle-btn" data-type="top" data-tilt="2048" title="Tilt Top (Up)" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">▲ Top</button>
          <button type="button" class="mj-tilt-angle-btn" data-type="mid" data-tilt="1024" title="Tilt Middle (Level)" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">▬ Mid</button>
          <button type="button" class="mj-tilt-angle-btn" data-type="bot" data-tilt="0" title="Tilt Bottom (Down)" style="flex: 1; font-size: 10px; padding: 2px 0; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; color: #d1d5db; cursor: pointer;">▼ Bot</button>
        </div>
      </div>

      <!-- Speed Control Bar (1x - 5x) -->
      <div style="border-top: 1px solid rgba(255,255,255,0.08); padding-top: 3px; margin-top: 1px;">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 2px;">
          <span style="font-size: 9px; font-weight: 600; text-transform: uppercase; color: rgba(255,255,255,0.5);">PTZ Speed</span>
          <span id="mj-speed-indicator" style="font-size: 9px; color: #60a5fa;">${SPEED_CONFIG[currentSpeed].label}</span>
        </div>
        <div id="mj-speed-bar" style="display: flex; justify-content: space-between; gap: 2px;">
          <button type="button" class="mj-speed-btn" data-spd="1" style="flex: 1; font-size: 10px; font-weight: 600; padding: 2px 0; border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; cursor: pointer;">1x</button>
          <button type="button" class="mj-speed-btn" data-spd="2" style="flex: 1; font-size: 10px; font-weight: 600; padding: 2px 0; border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; cursor: pointer;">2x</button>
          <button type="button" class="mj-speed-btn" data-spd="3" style="flex: 1; font-size: 10px; font-weight: 600; padding: 2px 0; border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; cursor: pointer;">3x</button>
          <button type="button" class="mj-speed-btn" data-spd="4" style="flex: 1; font-size: 10px; font-weight: 600; padding: 2px 0; border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; cursor: pointer;">4x</button>
          <button type="button" class="mj-speed-btn" data-spd="5" style="flex: 1; font-size: 10px; font-weight: 600; padding: 2px 0; border: 1px solid rgba(255,255,255,0.15); border-radius: 2px; cursor: pointer;">5x</button>
        </div>
      </div>
    `;

    ptzBox.appendChild(dock);

    // Update Speed Button UI Styles
    function updateSpeedUI() {
      const btns = dock.querySelectorAll('.mj-speed-btn');
      btns.forEach(b => {
        const spd = parseInt(b.dataset.spd, 10);
        if (spd === currentSpeed) {
          b.style.background = '#2563eb';
          b.style.color = '#ffffff';
          b.style.borderColor = '#3b82f6';
        } else {
          b.style.background = 'rgba(255,255,255,0.08)';
          b.style.color = '#9ca3af';
          b.style.borderColor = 'rgba(255,255,255,0.12)';
        }
      });
      const ind = document.getElementById('mj-speed-indicator');
      if (ind) ind.textContent = SPEED_CONFIG[currentSpeed].label;
    }

    dock.querySelectorAll('.mj-speed-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        currentSpeed = parseInt(e.currentTarget.dataset.spd, 10);
        try { localStorage.setItem('mj_ptz_speed', currentSpeed); } catch(err) {}
        updateSpeedUI();
        showToast(`PTZ Speed: ${SPEED_CONFIG[currentSpeed].label}`, 2000);
      });
    });

    updateSpeedUI();

    // Angle Presets Handlers
    dock.querySelectorAll('.mj-pan-angle-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        const panSteps = e.currentTarget.dataset.pan;
        const degText = e.currentTarget.textContent;
        const cruise = SPEED_CONFIG[currentSpeed].cruise;
        showToast(`Panning to ${degText}...`, 2000);
        fetcher(`/cgi-bin/j/ptz.cgi?act=abs&pan=${panSteps}&tilt=-1&cruise=${cruise}`, {
          method: 'POST',
          credentials: 'same-origin'
        })
        .then(r => r.json())
        .then(res => {
          if (res.status === 'ok') showToast(`Arrived at ${degText}`, 2000);
        });
      });
    });

    dock.querySelectorAll('.mj-tilt-angle-btn').forEach(btn => {
      btn.addEventListener('click', (e) => {
        const tiltSteps = e.currentTarget.dataset.tilt;
        const tiltText = e.currentTarget.textContent;
        const cruise = SPEED_CONFIG[currentSpeed].cruise;
        showToast(`Tilting to ${tiltText}...`, 2000);
        fetcher(`/cgi-bin/j/ptz.cgi?act=abs&pan=-1&tilt=${tiltSteps}&cruise=${cruise}`, {
          method: 'POST',
          credentials: 'same-origin'
        })
        .then(r => r.json())
        .then(res => {
          if (res.status === 'ok') showToast(`Arrived at ${tiltText}`, 2000);
        });
      });
    });

    // Smart Click Accumulator & Queue for D-Pad Directional Buttons
    const DIR_VECTORS = {
      ul: [-1, 1], uc: [0, 1], ur: [1, 1],
      lc: [-1, 0],              rc: [1, 0],
      dl: [-1, -1], dc: [0, -1], dr: [1, -1]
    };

    let accumPan = 0;
    let accumTilt = 0;
    let isRequestInflight = false;
    let flushTimer = null;
    let holdInterval = null;

    function flushAccumulatedMove() {
      if (isRequestInflight) return;
      if (accumPan === 0 && accumTilt === 0) return;

      const panToSend = accumPan;
      const tiltToSend = accumTilt;
      accumPan = 0;
      accumTilt = 0;
      isRequestInflight = true;

      const cruise = SPEED_CONFIG[currentSpeed].cruise;
      fetcher(`/cgi-bin/j/ptz.cgi?act=step&pan=${panToSend}&tilt=${tiltToSend}&cruise=${cruise}`, {
        method: 'POST',
        credentials: 'same-origin'
      })
      .then(r => r.json())
      .catch(() => {})
      .finally(() => {
        isRequestInflight = false;
        // If more clicks arrived while the camera was moving, flush immediately!
        if (accumPan !== 0 || accumTilt !== 0) {
          flushAccumulatedMove();
        }
      });
    }

    function addStep(dx, dy) {
      const stepUnit = SPEED_CONFIG[currentSpeed].step;
      accumPan += dx * stepUnit;
      accumTilt += dy * stepUnit;

      clearTimeout(flushTimer);
      flushTimer = setTimeout(flushAccumulatedMove, 30);
    }

    // Bind capture listeners to the 8 directional buttons on #mj-ptz-pad
    const pad = document.getElementById('mj-ptz-pad');
    if (pad) {
      pad.querySelectorAll('button[data-dir]').forEach(btn => {
        const dir = btn.dataset.dir;
        if (dir === 'cc') {
          // Center button -> Home
          btn.addEventListener('click', (e) => {
            e.stopPropagation();
            e.preventDefault();
            showToast('Moving to Home...', 2000);
            fetcher('/cgi-bin/j/ptz.cgi?act=home', { method: 'POST', credentials: 'same-origin' })
              .then(() => showToast('Arrived at Home', 2000));
          }, { capture: true });
          return;
        }

        const vec = DIR_VECTORS[dir];
        if (!vec) return;

        // Intercept pointerdown for immediate queuing and hold-repeat
        btn.addEventListener('pointerdown', (e) => {
          e.stopPropagation();
          e.preventDefault();
          btn.classList.add('mj-ptz-on');

          // 1. Immediate step on initial click/tap
          addStep(vec[0], vec[1]);

          // 2. If held down for > 350ms, start repeating at 160ms interval
          clearInterval(holdInterval);
          const holdStart = setTimeout(() => {
            holdInterval = setInterval(() => {
              addStep(vec[0], vec[1]);
            }, 160);
          }, 350);

          const stopHold = () => {
            clearTimeout(holdStart);
            clearInterval(holdInterval);
            holdInterval = null;
            btn.classList.remove('mj-ptz-on');
            window.removeEventListener('pointerup', stopHold);
            window.removeEventListener('pointercancel', stopHold);
          };

          window.addEventListener('pointerup', stopHold);
          window.addEventListener('pointercancel', stopHold);
        }, { capture: true });

        // Stop native preview-ptz click handler from interfering
        btn.addEventListener('click', (e) => {
          e.stopPropagation();
          e.preventDefault();
        }, { capture: true });
      });
    }

    // Presets Management (Load, Go, Save, Delete, Calibrate)
    let cachedPresets = {};
    let isHomed = false;

    function loadPresets(selectId) {
      fetcher('/cgi-bin/j/ptz.cgi?act=presets', { credentials: 'same-origin' })
        .then(r => r.json())
        .then(data => {
          isHomed = !!data.homed;
          cachedPresets = data.presets || {};

          const dot = document.getElementById('mj-preset-status-dot');
          if (dot) {
            dot.style.background = isHomed ? '#22c55e' : '#f59e0b';
            dot.title = isHomed ? `Homed (pan: ${data.pan_pos}, tilt: ${data.tilt_pos})` : 'Uncalibrated (click Calib to home)';
          }

          const pSpan = data.pan_span || 3900;
          const tSpan = data.tilt_span || 2048;
          dock.querySelectorAll('.mj-pan-angle-btn').forEach(btn => {
            const deg = btn.dataset.deg;
            if (deg !== undefined) {
              btn.dataset.pan = Math.round(pSpan * parseInt(deg, 10) / 360);
            }
          });
          const topBtn = dock.querySelector('.mj-tilt-angle-btn[data-type="top"]');
          if (topBtn) topBtn.dataset.tilt = tSpan;
          const midBtn = dock.querySelector('.mj-tilt-angle-btn[data-type="mid"]');
          if (midBtn) midBtn.dataset.tilt = Math.round(tSpan / 2);

          const sel = document.getElementById('mj-preset-sel');
          if (!sel) return;
          sel.innerHTML = '';
          const keys = Object.keys(cachedPresets).sort((a, b) => parseInt(a) - parseInt(b));
          if (keys.length === 0) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = 'No presets';
            sel.appendChild(opt);
            return;
          }
          keys.forEach(id => {
            const opt = document.createElement('option');
            opt.value = id;
            const p = cachedPresets[id];
            opt.textContent = `${id}: ${p.name}`;
            opt.title = `Slot ${id} - Pan: ${p.pan}, Tilt: ${p.tilt}`;
            if (selectId && String(id) === String(selectId)) opt.selected = true;
            sel.appendChild(opt);
          });
        })
        .catch(err => {
          console.error('Error loading presets:', err);
        });
    }

    function gotoPreset(id) {
      if (!id) return;
      const p = cachedPresets[id];
      const name = p ? p.name : id;
      showToast(`Moving to preset "${name}"...`, 2000);
      fetcher('/cgi-bin/j/ptz.cgi?act=goto&preset=' + encodeURIComponent(id), {
        method: 'POST',
        credentials: 'same-origin'
      }).then(r => r.json()).then(res => {
        if (res.status === 'ok') {
          showToast(`Arrived at preset "${name}"`, 2000);
        } else {
          showToast(`Preset move failed: ${res.error || 'error'}`, 3000);
        }
      }).catch(() => {
        showToast(`Failed to move to preset "${name}"`, 3000);
      });
    }

    document.getElementById('mj-preset-go').addEventListener('click', () => {
      const sel = document.getElementById('mj-preset-sel');
      if (sel && sel.value) gotoPreset(sel.value);
    });

    document.getElementById('mj-preset-sel').addEventListener('change', (e) => {
      if (e.target.value) gotoPreset(e.target.value);
    });

    document.getElementById('mj-preset-save').addEventListener('click', () => {
      const sel = document.getElementById('mj-preset-sel');
      const currentId = sel && sel.value ? sel.value : '';
      const currentName = (currentId && cachedPresets[currentId]) ? cachedPresets[currentId].name : '';

      const name = prompt('Enter preset / template name (e.g. Door, Yard):', currentName);
      if (!name || !name.trim()) return;
      const cleanName = name.trim().replace(/[^a-zA-Z0-9_-]/g, '_');

      let targetSlot = null;
      for (const id in cachedPresets) {
        if (cachedPresets[id].name.toLowerCase() === cleanName.toLowerCase()) {
          targetSlot = id;
          break;
        }
      }

      if (targetSlot) {
        if (!confirm(`Preset "${cleanName}" already exists in Slot ${targetSlot}. Overwrite with current position?`)) {
          return;
        }
      } else {
        const keys = Object.keys(cachedPresets).map(Number).filter(n => !isNaN(n));
        let nextId = 1;
        while (keys.includes(nextId)) nextId++;
        targetSlot = String(nextId);
      }

      showToast(`Saving "${cleanName}" (Slot ${targetSlot})...`, 2000);
      fetcher(`/cgi-bin/j/ptz.cgi?act=set&preset=${targetSlot}&name=${encodeURIComponent(cleanName)}`, {
        method: 'POST',
        credentials: 'same-origin'
      })
      .then(r => r.json())
      .then(res => {
        if (res.status === 'ok') {
          showToast(`✓ Saved preset ${targetSlot}: "${cleanName}"`, 3000);
          loadPresets(targetSlot);
        } else {
          showToast(`Error saving preset: ${res.error || 'unknown'}`, 4000);
        }
      })
      .catch(() => {
        showToast('Failed to save preset', 3000);
      });
    });

    document.getElementById('mj-preset-del').addEventListener('click', () => {
      const sel = document.getElementById('mj-preset-sel');
      const id = sel ? sel.value : null;
      if (!id) return;
      if (String(id) === '1') {
        alert('Preset 1 ("home") is the default reference and cannot be deleted.');
        return;
      }
      const p = cachedPresets[id];
      const name = p ? p.name : id;
      if (!confirm(`Delete preset ${id}: "${name}"?`)) return;

      fetcher('/cgi-bin/j/ptz.cgi?act=remove&preset=' + encodeURIComponent(id), {
        method: 'POST',
        credentials: 'same-origin'
      })
      .then(r => r.json())
      .then(res => {
        if (res.status === 'ok') {
          showToast(`✓ Deleted preset "${name}"`, 2500);
          loadPresets();
        } else {
          showToast('Failed to delete preset', 3000);
        }
      })
      .catch(() => {
        showToast('Failed to delete preset', 3000);
      });
    });

    document.getElementById('mj-preset-cal').addEventListener('click', () => {
      if (!confirm('Recalibrate camera motors? This will home pan and tilt against physical endstops (~10s).')) return;
      showToast('Calibrating motors... please wait.', 10000);
      fetcher('/cgi-bin/j/ptz.cgi?act=calibrate', {
        method: 'POST',
        credentials: 'same-origin'
      })
      .then(r => r.json())
      .then(res => {
        if (res.status === 'ok') {
          showToast('✓ Calibration complete! Camera homed.', 4000);
          loadPresets(1);
        } else {
          showToast('Calibration failed', 4000);
        }
      })
      .catch(() => {
        showToast('Calibration request error', 3000);
      });
    });

    loadPresets();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initPresets);
  } else {
    initPresets();
  }
})();
