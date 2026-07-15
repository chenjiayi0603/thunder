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
      return r.json().then(function(data) {
        return { ok: r.ok, status: r.status, data: data };
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
  function getNodes(type)      { return get('/nodes?node_type=' + encodeURIComponent(type || '')); }
  function getCanary(service)  { return get('/canary/' + encodeURIComponent(service) + '/weights'); }
  function setCanary(svc, w)   { return post('/canary/' + encodeURIComponent(svc) + '/weights', { weights: w }); }
  function getConfig(type)     { return get('/config/' + encodeURIComponent(type)); }
  function setConfig(type, c)  { return put('/config/' + encodeURIComponent(type), { content: c }); }
  function getPlugins()        { return get('/plugins'); }

  return {
    overview: overview,
    getNodes: getNodes,
    getCanary: getCanary,
    setCanary: setCanary,
    getConfig: getConfig,
    setConfig: setConfig,
    getPlugins: getPlugins,
    request: request,
  };
})();
