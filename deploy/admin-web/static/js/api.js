/* ============================================================
   Thunder Admin — API 封装
   ============================================================ */

var API = (function() {
  var BASE = '/api';

  function request(path, opts) {
    opts = opts || {};
    return fetch(BASE + path, {
      method: opts.method || 'GET',
      headers: Object.assign({ 'Content-Type': 'application/json' }, opts.headers || {}),
      body: opts.body ? JSON.stringify(opts.body) : undefined,
    }).then(function(r) {
      return r.json().then(function(raw) {
        // Unwrap Go backend {ok, data} / {ok, error} envelope
        var data = raw, error = null;
        if (raw && typeof raw === 'object' && 'ok' in raw) {
          if (raw.ok) {
            data = raw.data !== undefined ? raw.data : raw;
          } else {
            error = raw.error || 'unknown error';
            data = null;
          }
        }
        return { ok: r.ok, status: r.status, data: data, error: error };
      }).catch(function() {
        return { ok: r.ok, status: r.status, data: null, error: 'Invalid JSON response' };
      });
    });
  }

  function get(path)         { return request(path); }
  function post(path, body)  { return request(path, { method: 'POST', body: body }); }
  function put(path, body)   { return request(path, { method: 'PUT', body: body }); }

  /* ---------- Endpoints ---------- */
  function overview()          { return get('/overview'); }
  function storageStats()     { return get('/storage/stats'); }
  function getNodes(type)      { return get('/nodes?node_type=' + encodeURIComponent(type || '')); }
  function getCanary(service)  { return get('/canary/' + encodeURIComponent(service) + '/weights'); }
  function setCanary(svc, w)   { return post('/canary/' + encodeURIComponent(svc) + '/weights', { weights: w }); }
  function getConfig(module, cfgType) { return get('/config/' + encodeURIComponent(module) + '?type=' + encodeURIComponent(cfgType)); }
  function setConfig(module, cfgType, c) { return put('/config/' + encodeURIComponent(module), { type: cfgType, content: c }); }
  function getPlugins()        { return get('/plugins'); }
  function soList(type)         { return get('/plugins/' + encodeURIComponent(type)); }
  function soUpload(type, file) {
    return new Promise(function(resolve, reject) {
      var xhr = new XMLHttpRequest();
      xhr.open('PUT', BASE + '/plugins/' + encodeURIComponent(type) + '/' + encodeURIComponent(file.name));
      xhr.onload = function() { try { var r = JSON.parse(xhr.responseText); resolve({ ok: r.ok, data: r.data, error: r.error }); } catch(e) { reject(e); } };
      xhr.onerror = function() { reject(new Error('upload failed')); };
      xhr.send(file);
    });
  }

  return {
    overview: overview,
    storageStats: storageStats,
    getNodes: getNodes,
    getCanary: getCanary,
    setCanary: setCanary,
    getConfig: getConfig,
    setConfig: setConfig,
    getPlugins: getPlugins,
    soList: soList,
    soUpload: soUpload,
    request: request,
  };
})();
