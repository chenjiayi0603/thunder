/* ============================================================
   Thunder Admin — 全局状态
   ============================================================ */

var STORE = {
  tab: "overview",
  autoRefresh: null,

  overview: { data: null, loading: false, error: null },
  nodes:    { data: [], loading: false, error: null },
  canary:   { data: null, loading: false, error: null },
  config:   { data: null, loading: false, error: null },
  plugins:  { data: [], loading: false, error: null },
  audit:    { data: [], loading: false, error: null },
};
