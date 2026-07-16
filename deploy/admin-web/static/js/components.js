/* ============================================================
   Thunder Admin — 通用组件
   Toast / Modal / Loading / escapeHtml
   ============================================================ */

var Components = (function() {

  /* ---------- Toast ---------- */
  var toastTimer = null;

  function toast(message, type, duration) {
    type = type || 'info';
    duration = duration || 3000;

    var container = document.querySelector('.toast-container');
    if (!container) {
      container = document.createElement('div');
      container.className = 'toast-container';
      document.body.appendChild(container);
    }

    var el = document.createElement('div');
    el.className = 'toast toast-' + type;
    el.textContent = message;
    container.appendChild(el);

    setTimeout(function() {
      if (el.parentNode) el.parentNode.removeChild(el);
    }, duration + 300);
  }


  /* ---------- Modal ---------- */
  function modal(opts) {
    opts = opts || {};
    var title = opts.title || 'Confirm';
    var body = opts.body || '';
    var confirmText = opts.confirmText || 'Confirm';
    var cancelText = opts.cancelText || 'Cancel';
    var danger = opts.danger || false;
    var onConfirm = opts.onConfirm || function() {};
    var onCancel = opts.onCancel || function() {};

    var overlay = document.createElement('div');
    overlay.className = 'modal-overlay';
    overlay.innerHTML =
      '<div class="modal">' +
      '<div class="modal-header">' + escapeHtml(title) + '</div>' +
      '<div class="modal-body">' + (typeof body === 'string' ? body : '') + '</div>' +
      '<div class="modal-footer">' +
      '<button class="btn btn-ghost cancel-btn">' + escapeHtml(cancelText) + '</button>' +
      '<button class="btn ' + (danger ? 'btn-danger' : 'btn-primary') + ' confirm-btn">' + escapeHtml(confirmText) + '</button>' +
      '</div>' +
      '</div>';

    function close() {
      if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
      document.removeEventListener('keydown', onKey);
    }

    function onKey(e) { if (e.key === 'Escape') { close(); onCancel(); } }
    document.addEventListener('keydown', onKey);

    overlay.addEventListener('click', function(e) {
      if (e.target === overlay) { close(); onCancel(); }
    });
    overlay.querySelector('.cancel-btn').addEventListener('click', function() { close(); onCancel(); });
    overlay.querySelector('.confirm-btn').addEventListener('click', function() { close(); onConfirm(); });

    document.body.appendChild(overlay);
    return { close: close };
  }

  /* ---------- Loading ---------- */
  function loading(container, show) {
    if (show === false) {
      var existing = container.querySelector('.loading-block');
      if (existing) existing.remove();
      return;
    }
    var el = document.createElement('div');
    el.className = 'loading-block';
    el.innerHTML = '<span class="spinner"></span>';
    container.appendChild(el);
    return {
      remove: function() { if (el.parentNode) el.parentNode.removeChild(el); }
    };
  }

  /* ---------- escapeHtml ---------- */
  function escapeHtml(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  /* ---------- escapeAttr ---------- */
  function escapeAttr(s) {
    return String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/'/g, '&#39;').replace(/</g, '&lt;');
  }

  /* ---------- formatTime ---------- */
  function formatTime(iso) {
    if (!iso) return '-';
    var d = new Date(iso);
    if (isNaN(d.getTime())) return iso;
    return d.toLocaleString();
  }

  /* ---------- timeAgo ---------- */
  function timeAgo(iso) {
    if (!iso) return '-';
    var diff = Date.now() - new Date(iso).getTime();
    if (diff < 0) return 'just now';
    var sec = Math.floor(diff / 1000);
    if (sec < 60) return sec + 's ago';
    var min = Math.floor(sec / 60);
    if (min < 60) return min + 'min ago';
    var hr = Math.floor(min / 60);
    if (hr < 24) return hr + 'h ago';
    return Math.floor(hr / 24) + 'd ago';
  }

  /* ---------- Public API ---------- */
  return {
    toast: toast,
    modal: modal,
    loading: loading,
    escapeHtml: escapeHtml,
    escapeAttr: escapeAttr,
    formatTime: formatTime,
    timeAgo: timeAgo
  };
})();
