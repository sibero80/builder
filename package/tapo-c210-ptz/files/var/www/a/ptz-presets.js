/**
 * PTZ Presets & Template Manager for OpenIPC WebUI (live.cgi)
 */
(function() {
  function initPresets() {
  // Safety shim: ensure MajesticPtzOrient never rejects due to missing image.rotate in majestic.yaml
  if (window.MajesticPtzOrient) {
    const origOf = window.MajesticPtzOrient.of;
    window.MajesticPtzOrient.of = function(t) {
      if (t && typeof t === 'object' && t.rotate === undefined) {
        t.rotate = 0;
      }
      return origOf(t);
    };
  }
    const ptzBox = document.getElementById('mj-ptz');
    if (!ptzBox) {
      setTimeout(initPresets, 200);
      return;
    }

    if (document.getElementById('mj-ptz-preset-dock')) return;

    // Toast helper using native OpenIPC #mj-toasts
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
      setTimeout(() => { if (t.parentNode) t.remove(); }, duration || 4000);
    }

    const fetcher = window.apiFetch || window.fetch;

    // Create Presets Dock Widget
    const dock = document.createElement('div');
    dock.id = 'mj-ptz-preset-dock';
    dock.className = 'mj-ptz-presets';
    dock.style.cssText = [
      'display: flex',
      'flex-direction: column',
      'gap: 4px',
      'padding: 5px 6px',
      'width: calc(3 * 2.625rem + 10px)',
      'background: rgba(13, 15, 20, 0.78)',
      'border: 1px solid rgba(255, 255, 255, 0.12)',
      'border-radius: var(--bs-border-radius, 4px)',
      'box-sizing: border-box',
      'font-family: inherit'
    ].join('; ');

    dock.innerHTML = `
      <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 2px;">
        <span style="display: flex; align-items: center; gap: 4px; font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: rgba(255,255,255,0.7);">
          <span id="mj-preset-status-dot" style="display: inline-block; width: 6px; height: 6px; border-radius: 50%; background: #9ca3af;"></span>
          Presets
        </span>
        <button id="mj-preset-cal" type="button" title="Recalibrate motor endstops (~10s)" style="background: none; border: none; color: rgba(255,255,255,0.5); font-size: 11px; cursor: pointer; padding: 0 2px;">⟳ Calib</button>
      </div>
      <div style="display: flex; gap: 4px; align-items: center;">
        <select id="mj-preset-sel" class="form-select form-select-sm" style="flex: 1; background: #181b22; color: #fff; border: 1px solid rgba(255,255,255,0.2); font-size: 11px; padding: 2px 4px; height: 24px; border-radius: 3px;">
          <option value="">Loading...</option>
        </select>
        <button id="mj-preset-go" type="button" class="btn btn-sm btn-primary" title="Go to selected preset" style="font-size: 11px; padding: 1px 7px; height: 24px; line-height: 20px;">Go</button>
      </div>
      <div style="display: flex; gap: 4px; margin-top: 2px;">
        <button id="mj-preset-save" type="button" class="btn btn-sm btn-outline-success" title="Save current camera angle as preset" style="flex: 1; font-size: 11px; padding: 1px 4px; height: 22px; line-height: 18px; color: #4ade80; border-color: rgba(74, 222, 128, 0.4);">+ Save</button>
        <button id="mj-preset-del" type="button" class="btn btn-sm btn-outline-danger" title="Delete selected preset" style="font-size: 11px; padding: 1px 6px; height: 22px; line-height: 18px; color: #f87171; border-color: rgba(248, 113, 113, 0.4);">✕</button>
      </div>
    `;

    ptzBox.appendChild(dock);

    let cachedPresets = {};
    let isHomed = false;

    function loadPresets(selectId) {
      fetcher('/cgi-bin/j/ptz.cgi?act=presets', { credentials: 'same-origin' })
        .then(r => r.json())
        .then(data => {
          isHomed = !!data.homed;
          cachedPresets = data.presets || {};

          // Update status dot
          const dot = document.getElementById('mj-preset-status-dot');
          if (dot) {
            dot.style.background = isHomed ? '#22c55e' : '#f59e0b';
            dot.title = isHomed ? `Homed (pan: ${data.pan_pos}, tilt: ${data.tilt_pos})` : 'Uncalibrated (click Calib to home)';
          }

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

    // Go to preset
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
          showToast(`Arrived at preset "${name}"`, 2500);
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

    // Save Preset
    document.getElementById('mj-preset-save').addEventListener('click', () => {
      const sel = document.getElementById('mj-preset-sel');
      const currentId = sel && sel.value ? sel.value : '';
      const currentName = (currentId && cachedPresets[currentId]) ? cachedPresets[currentId].name : '';

      const name = prompt('Enter preset / template name (e.g. Door, Yard):', currentName);
      if (!name || !name.trim()) return;
      const cleanName = name.trim().replace(/[^a-zA-Z0-9_-]/g, '_');

      // Check if an existing preset matches this name
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
        // Allocate next available slot ID
        const keys = Object.keys(cachedPresets).map(Number).filter(n => !isNaN(n));
        let nextId = 1;
        while (keys.includes(nextId)) nextId++;
        targetSlot = String(nextId);
      }

      showToast(`Saving current view as "${cleanName}" (Slot ${targetSlot})...`, 2000);
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

    // Delete Preset
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

    // Calibrate
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
