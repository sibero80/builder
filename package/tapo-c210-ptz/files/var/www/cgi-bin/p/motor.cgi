<%
has_cap() {
	[ -z "$ptz_caps" ] && return 0
	case " $ptz_caps " in *" $1 "*) return 0 ;; esac
	return 1
}

focus_label="Focus"
%>
<% if [ "$ptz_backend" = "pelco" ]; then %>
<% if [ -n "$ptz_reason" ]; then %>
<p id="mj-ptz-why" class="mj-adapt-toast mj-ptz-why small" role="status" hidden><%= $ptz_reason %></p>
<% fi %>
<% if has_cap focus; then %>
<p id="mj-af-say" class="mj-adapt-toast mj-ptz-why small" role="status" aria-live="polite" hidden></p>
<% fi %>
<% if has_cap zoom || has_cap focus; then %>
<div id="mj-ptz-fn" class="mj-ptz-fn" role="group" aria-label="Zoom and focus" hidden>
	<% if has_cap zoom; then %>
	<span class="mj-ptz-group">Zoom</span>
	<button type="button" class="mj-ptz-fnbtn" data-act="wide" aria-label="Zoom out" title="Zoom out (wide)">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true">
			<circle cx="8.8" cy="8.8" r="5.6"></circle><path d="M12.9 12.9 17 17M6.4 8.8h4.8"></path>
		</svg>
		<span>Wide</span>
	</button>
	<button type="button" class="mj-ptz-fnbtn" data-act="tele" aria-label="Zoom in" title="Zoom in (tele)">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true">
			<circle cx="8.8" cy="8.8" r="5.6"></circle><path d="M12.9 12.9 17 17M6.4 8.8h4.8M8.8 6.4v4.8"></path>
		</svg>
		<span>Tele</span>
	</button>
	<% fi %>
	<% if has_cap focus; then %>
	<span class="mj-ptz-group" id="mj-focus-cap"><%= $focus_label %></span>
	<button type="button" class="mj-ptz-fnbtn" data-act="near" aria-label="Focus near" title="Pull focus nearer">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
			<path d="M3.2 7V3.6h3.4M16.8 7V3.6h-3.4M3.2 13v3.4h3.4M16.8 13v3.4h-3.4"></path>
			<circle cx="10" cy="10" r="3.1"></circle>
		</svg>
		<span>Near</span>
	</button>
	<button type="button" class="mj-ptz-fnbtn" data-act="far" aria-label="Focus far" title="Push focus further away">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
			<path d="M3.2 7V3.6h3.4M16.8 7V3.6h-3.4M3.2 13v3.4h3.4M16.8 13v3.4h-3.4"></path>
			<circle cx="10" cy="10" r="1.4"></circle>
		</svg>
		<span>Far</span>
	</button>
	<% fi %>
	<% if has_cap focus; then %>
	<span class="mj-ptz-group" id="mj-af-cap">Autofocus</span>
	<button type="button" class="mj-ptz-fnbtn mj-ptz-af" data-act="af" aria-label="Autofocus" title="Focus the camera once, now. Trim with Near and Far AFTER it finishes: a manual move first makes the next autofocus search the whole range.">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
			<path d="M3.2 7V3.6h3.4M16.8 7V3.6h-3.4M3.2 13v3.4h3.4M16.8 13v3.4h-3.4"></path>
			<circle cx="10" cy="10" r="2.2" fill="currentColor" stroke="none"></circle>
		</svg>
		<span>Autofocus</span>
	</button>
	<% fi %>
</div>
<% fi %>
<% if has_cap pan || has_cap tilt; then %>
<div id="mj-ptz-pad" class="mj-ptz-pad" role="group" aria-label="Pan and tilt" hidden>
	<span class="mj-ptz-void"></span>
	<% if has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-act="up" aria-label="Tilt up">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5.4 12.4 10 7.6l4.6 4.8"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<span class="mj-ptz-void"></span>
	<% if has_cap pan; then %>
	<button type="button" class="mj-ptz-btn" data-act="left" aria-label="Pan left">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12.4 5.4 7.6 10l4.8 4.6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<button type="button" class="mj-ptz-btn mj-ptz-stop" data-act="stop" aria-label="Stop" title="Stop">
		<svg viewBox="0 0 20 20" width="18" height="18" fill="none" aria-hidden="true"><rect x="6" y="6" width="8" height="8" rx="1.4" fill="currentColor"></rect></svg>
	</button>
	<% if has_cap pan; then %>
	<button type="button" class="mj-ptz-btn" data-act="right" aria-label="Pan right">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7.6 5.4 12.4 10l-4.8 4.6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<span class="mj-ptz-void"></span>
	<% if has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-act="down" aria-label="Tilt down">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5.4 7.6 10 12.4l4.6-4.8"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<span class="mj-ptz-void"></span>
</div>
<div id="mj-ptz-follow" class="mj-ptz-speed mj-ptz-follow" hidden>
	<div class="form-check form-switch m-0">
		<input class="form-check-input" type="checkbox" role="switch" id="mj-ptz-follow-in">
		<label class="form-check-label" for="mj-ptz-follow-in">Follow</label>
	</div>
</div>
<% fi %>
<% else %>
<% if has_cap pan || has_cap tilt; then %>
<div id="mj-ptz-pad" class="mj-ptz-pad" role="group" aria-label="Pan and tilt" hidden>
	<% if has_cap pan && has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="ul" aria-label="Pan up-left">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6.6 12.6V6.6h6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="uc" aria-label="Tilt up">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5.4 12.4 10 7.6l4.6 4.8"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap pan && has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="ur" aria-label="Pan up-right">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7.4 6.6h6v6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap pan; then %>
	<button type="button" class="mj-ptz-btn" data-dir="lc" aria-label="Pan left">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12.4 5.4 7.6 10l4.8 4.6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<button type="button" class="mj-ptz-btn mj-ptz-stop" data-dir="cc" aria-label="Center" title="Centre">
		<svg viewBox="0 0 20 20" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><circle cx="10" cy="10" r="3.4"></circle></svg>
	</button>
	<% if has_cap pan; then %>
	<button type="button" class="mj-ptz-btn" data-dir="rc" aria-label="Pan right">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7.6 5.4 12.4 10l-4.8 4.6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap pan && has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="dl" aria-label="Pan down-left">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6.6 7.4v6h6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="dc" aria-label="Tilt down">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5.4 7.6 10 12.4l4.6-4.8"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
	<% if has_cap pan && has_cap tilt; then %>
	<button type="button" class="mj-ptz-btn" data-dir="dr" aria-label="Pan down-right">
		<svg viewBox="0 0 20 20" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M13.4 7.4v6h-6"></path></svg>
	</button>
	<% else %>
	<span class="mj-ptz-void"></span>
	<% fi %>
</div>
<% fi %>
<% fi %>
<script src="/a/af-state.js"></script>
<script src="/a/ptz-speed.js"></script>
<script src="/a/ptz-stick.js"></script>
<script src="/a/ptz-orient.js"></script>
<script src="/a/ptz-centre.js"></script>
<script src="/a/ptz-follow.js"></script>
<script src="/a/preview-ptz.js"></script>
<script src="/a/ptz-presets.js"></script>
