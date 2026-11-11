// Vitool Box web UI. Finds the apps on GitHub (repositories of OWNER with the
// TOPIC topic, a vitool.json manifest and at least one release) and asks the
// host (VitoolBox.exe) to install, remove and start them.
'use strict';

const OWNER = 'VitomDev';
const TOPIC = 'vitool-box';
const HUB_REPO = 'Vitool-Box';
const CACHE_KEY = 'vitool.catalog.v1';
const SETTINGS_KEY = 'vitool.settings.v1';
const NOTIFIED_KEY = 'vitool.notified.v1';
const API = 'https://api.github.com';
const RAW = 'https://raw.githubusercontent.com';
// Written by scripts/update-catalog.ps1. Raw files have no API rate limit, so
// the API is only asked when this file is missing or older than STALE_DAYS.
const CATALOG_URL = `${RAW}/${OWNER}/${HUB_REPO}/main/catalog.json`;
const STALE_DAYS = 3;
const ETAG_KEY = 'vitool.etags.v1';
// Google Form for bugs and suggestions; its app list follows catalog.json (scripts/feedback-form.gs)
const FEEDBACK_URL = 'https://docs.google.com/forms/d/e/1FAIpQLSd1r9NeMrnkdrf2E5-5THib7mwy1yq975HT-CuIhGInnpYSYw/viewform';
const FEEDBACK_APP_FIELD = 'entry.2001919890';
const NEW_DAYS = 7;
const CHECK_HOURS = 4;

const state = {
  apps: [],
  installed: {},   // appId -> { version }
  busy: {},        // appId -> { action, percent }
  errors: {},      // appId -> error key from the host
  lastAction: {},  // appId -> what to repeat on Retry
  filter: 'all',
  category: '',
  query: '',
  view: 'home',    // home, detail or settings
  current: null,   // repo shown in the detail view
  queue: [],       // installs waiting to run, one at a time
  selecting: false,
  selected: new Set(),
  catalogReady: false,
  autoUpdateDone: false,
  tab: 'about',
  readmes: {},     // repo -> html
  releases: {},    // repo -> list of releases, or 'error'
  hubVersion: null,
  hubLatest: null,
  startup: false,  // starts with Windows and stays in the notification area
  hidden: false,   // started hidden by Windows
  backupMessage: null,
};

const $ = (id) => document.getElementById(id);

// --- Host bridge ---------------------------------------------------------------

const host = window.chrome && window.chrome.webview;

function send(message) {
  if (host) host.postMessage(message);
}

if (host) {
  host.addEventListener('message', async (event) => {
    const message = event.data;
    switch (message.type) {
      case 'hello':
        state.hubVersion = message.version;
        state.startup = message.startup === '1';
        state.hidden = message.hidden === '1';
        $('brand-version').textContent = `v${message.version}`;
        renderBanner();
        if (state.view === 'settings') renderSettings();
        scheduleChecks();
        maybeAutoUpdate();
        break;
      case 'state':
        state.installed = message.apps;
        state.stateReady = true;
        render();
        reportUpdates();
        maybeAutoUpdate();
        break;
      case 'progress':
        if (state.busy[message.appId]) state.busy[message.appId].percent = Number(message.percent);
        if (message.appId === 'hub') renderBanner();
        else renderBusy(message.appId);
        break;
      case 'running': {
        delete state.busy[message.appId];
        const app = appById(message.appId);
        if (app) askToCloseApp(app, message.action);
        render();
        break;
      }
      case 'done':
        delete state.busy[message.appId];
        if (message.error === 'checksum' && message.appId !== 'hub' && await retriedWithNewCatalog(message.appId)) break;
        if (message.error && message.error !== 'cancelled') state.errors[message.appId] = message.error;
        if (message.appId === 'hub') renderBanner();
        refreshInstalled();
        runQueue();
        break;
      case 'startup':
        state.startup = message.enabled === '1';
        scheduleChecks();
        renderSettings();
        break;
      case 'backupDone':
      case 'restoreDone':
        if (message.cancelled === '1') state.backupMessage = null;
        else if (message.error) state.backupMessage = { error: true, text: errorText(message.error) };
        else {
          const names = message.apps.split(',').map((repo) => (appByRepo(repo) || { name: repo }).name).join(', ');
          state.backupMessage = { error: false, text: t(message.type === 'backupDone' ? 'backupDone' : 'restoreDone', { apps: names }) };
        }
        renderSettings();
        break;
      case 'checkUpdates':
        loadCatalog();
        break;
      case 'showUpdates':
        state.filter = 'updates';
        document.querySelectorAll('.chip').forEach((chip) => chip.classList.toggle('active', chip.dataset.filter === 'updates'));
        showHome();
        break;
    }
  });
}

function refreshInstalled() {
  state.stateReady = false;
  send({ type: 'state', ids: state.apps.map((app) => app.appId).join(',') });
}

// Texts the host shows itself: tray menu and the close confirmation.
function sendHostStrings() {
  send({
    type: 'strings', closeBusy: t('closeBusy'), trayOpen: t('trayOpen'), trayCheck: t('trayCheck'), trayQuit: t('trayQuit'),
    trayTip: trayTip(),
  });
}

// --- Catalog -------------------------------------------------------------------

// GitHub answers 304 to a request with a matching ETag, and those do not count
// against the hourly limit.
const etags = readJson(ETAG_KEY, {});

async function getJson(url, accept) {
  const headers = { Accept: accept || 'application/vnd.github+json' };
  const saved = etags[url];
  if (saved) headers['If-None-Match'] = saved.etag;
  const response = await fetch(url, { headers, cache: 'no-store' });
  if (response.status === 304 && saved) return saved.body;
  if (!response.ok) {
    const error = new Error(`HTTP ${response.status}`);
    error.rateLimited = response.status === 403 || response.status === 429;
    error.status = response.status;
    const reset = Number(response.headers.get('x-ratelimit-reset'));
    if (reset) error.resetAt = reset * 1000;
    throw error;
  }
  const body = await response.json();
  const etag = response.headers.get('etag');
  if (etag) {
    etags[url] = { etag, body };
    writeJson(ETAG_KEY, etags);
  }
  return body;
}

function prettyName(repo) {
  return repo.replace(/[-_]+/g, ' ');
}

function rawUrl(repo, branch, path) {
  if (!path) return null;
  if (/^https:\/\//.test(path)) return path;
  return `${RAW}/${OWNER}/${repo.name}/${branch}/${path.replace(/^\.?\//, '')}`;
}

function pickInstaller(assets, pattern) {
  const exes = assets.filter((asset) => /\.exe$/i.test(asset.name));
  if (pattern) {
    const regex = new RegExp('^' + pattern.split('*').map((part) => part.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*') + '$', 'i');
    return exes.find((asset) => regex.test(asset.name));
  }
  return exes.find((asset) => /setup/i.test(asset.name));
}

function releaseAsset(release, pattern) {
  const asset = pickInstaller(release.assets || [], pattern);
  const sha256 = asset && /^sha256:[0-9a-f]{64}$/.test(asset.digest || '') ? asset.digest.slice(7) : null;
  return asset && sha256 ? { name: asset.name, url: asset.browser_download_url, sha256, size: asset.size } : null;
}

function toRelease(release, pattern) {
  return {
    version: (release.tag_name || '').replace(/^v/i, ''),
    date: release.published_at,
    notesHtml: release.body_html || '',
    url: release.html_url,
    asset: releaseAsset(release, pattern),
  };
}

// repo: the repository as the GitHub API describes it; release: its latest release.
function toApp(repo, manifest, release, extra = {}) {
  if (!manifest || !/^\{[0-9A-Fa-f-]{36}\}$/.test(manifest.appId || '') || !release) return null;
  const branch = repo.default_branch;
  const latest = toRelease(release, manifest.installer);
  return {
    repo: repo.name,
    branch,
    name: manifest.name || prettyName(repo.name),
    tagline: manifest.tagline || repo.description || '',
    description: repo.description || '',
    category: manifest.category || 'Tools',
    icon: rawUrl(repo, branch, manifest.icon),
    screenshots: (manifest.screenshots || []).map((path) => rawUrl(repo, branch, path)).filter(Boolean),
    appId: manifest.appId.toUpperCase(),
    exe: manifest.exe || null,
    installer: manifest.installer || null,
    admin: manifest.admin === true,
    settings: Array.isArray(manifest.settings) ? manifest.settings.filter((entry) => typeof entry === 'string') : [],
    homepage: repo.html_url,
    createdAt: repo.created_at,
    updatedAt: latest.date,
    version: latest.version,
    notesHtml: latest.notesHtml,
    releaseUrl: latest.url,
    asset: latest.asset,
    ...extra,
  };
}

function toHubLatest(release) {
  if (!release) return null;
  const latest = toRelease(release, 'VitoolBoxSetup.exe');
  return { version: latest.version, url: latest.url, asset: latest.asset };
}

async function loadApp(repo) {
  let manifest;
  try {
    const response = await fetch(`${RAW}/${OWNER}/${repo.name}/${repo.default_branch}/vitool.json`, { cache: 'no-cache' });
    if (!response.ok) return null;
    manifest = await response.json();
  } catch {
    return null;
  }
  let release;
  try {
    release = await getJson(`${API}/repos/${OWNER}/${repo.name}/releases/latest`, 'application/vnd.github.html+json');
  } catch (error) {
    if (error.status === 404) return null;
    throw error;
  }
  return toApp(repo, manifest, release);
}

async function fetchHubRelease() {
  try {
    return toHubLatest(await getJson(`${API}/repos/${OWNER}/${HUB_REPO}/releases/latest`));
  } catch {
    return null;  // No release of the hub yet, or GitHub unreachable.
  }
}

async function fetchFromApi() {
  const search = await getJson(`${API}/search/repositories?q=topic:${TOPIC}+user:${OWNER}&per_page=100`);
  const results = await Promise.all(search.items.map((repo) => loadApp(repo)));
  return { apps: results.filter(Boolean), hubLatest: await fetchHubRelease() };
}

// The published catalog.json, with every release and README included.
async function fetchPublished() {
  // Raw files are cached for minutes by GitHub's CDN: a unique URL skips that.
  const response = await fetch(`${CATALOG_URL}?t=${Date.now()}`, { cache: 'no-store' });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const catalog = await response.json();
  if (catalog.version !== 1 || !Array.isArray(catalog.apps)) throw new Error('catalog');
  const apps = catalog.apps.map((entry) => {
    const releases = (entry.releases || []).filter((release) => !release.draft);
    return toApp(entry.repo, entry.manifest, releases[0], {
      readmeHtml: entry.readmeHtml || '',
      releaseList: releases.map((release) => toRelease(release, entry.manifest && entry.manifest.installer)),
    });
  }).filter(Boolean);
  return { apps, hubLatest: toHubLatest(catalog.hub), generated: Date.parse(catalog.generated) || 0 };
}

async function fetchCatalog() {
  let published = null;
  try {
    published = await fetchPublished();
    if (Date.now() - published.generated < STALE_DAYS * 86400000) return published;
  } catch { /* Not published or unreachable: ask the API. */ }
  try {
    return await fetchFromApi();
  } catch (error) {
    if (published) return published;
    throw error;
  }
}

function readJson(key, fallback) {
  try {
    return JSON.parse(localStorage.getItem(key)) || fallback;
  } catch {
    return fallback;
  }
}

function writeJson(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch { /* Storage unavailable: it lasts until Vitool Box closes. */ }
}

function readSettings() {
  return { autoUpdate: false, notify: true, language: 'auto', sort: 'name', ...readJson(SETTINGS_KEY, {}) };
}

function writeSettings() {
  writeJson(SETTINGS_KEY, state.settings);
}

function applyCatalog(catalog) {
  state.apps = catalog.apps;
  state.hubLatest = catalog.hubLatest;
  state.releases = {};
  refreshInstalled();
  renderCategories();
  render();
  renderBanner();
}

// Fetched on every start so new releases show up right away; the cached list
// is shown meanwhile and kept when GitHub can't be reached.
async function loadCatalog() {
  const cached = readJson(CACHE_KEY, null);
  if (cached && !state.apps.length) applyCatalog(cached);

  $('refresh').classList.add('spinning');
  try {
    const catalog = await fetchCatalog();
    writeJson(CACHE_KEY, { ...catalog, time: Date.now() });
    state.notice = null;
    state.noticeUntil = null;
    applyCatalog(catalog);
    state.catalogReady = true;
    maybeAutoUpdate();
  } catch (error) {
    state.notice = error.rateLimited ? 'rateLimited' : 'offline';
    state.noticeUntil = error.resetAt || null;
    renderBanner();
    if (!cached) render();
  } finally {
    $('refresh').classList.remove('spinning');
  }
}

async function loadReleases(app) {
  if (state.releases[app.repo]) return;
  if (app.releaseList) {
    state.releases[app.repo] = app.releaseList;
    return;
  }
  state.releases[app.repo] = 'loading';
  try {
    const list = await getJson(`${API}/repos/${OWNER}/${app.repo}/releases?per_page=20`, 'application/vnd.github.html+json');
    state.releases[app.repo] = list.filter((release) => !release.draft).map((release) => toRelease(release, app.installer));
  } catch {
    state.releases[app.repo] = 'error';
  }
  if (state.current === app.repo) renderDetail();
}

// --- Helpers -------------------------------------------------------------------

function compareVersions(a, b) {
  const pa = String(a).split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  const pb = String(b).split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const diff = (pa[i] || 0) - (pb[i] || 0);
    if (diff) return Math.sign(diff);
  }
  return 0;
}

function statusOf(app) {
  const installed = state.installed[app.appId];
  if (!installed) return 'available';
  return compareVersions(app.version, installed.version) > 0 ? 'update' : 'installed';
}

function isNew(app) {
  return Boolean(app.createdAt) && Date.now() - new Date(app.createdAt).getTime() < NEW_DAYS * 86400000;
}

function appById(appId) {
  return state.apps.find((app) => app.appId === appId);
}

function appByRepo(repo) {
  return state.apps.find((app) => app.repo === repo);
}

function el(tag, props, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (key === 'class') node.className = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (value !== undefined && value !== null && value !== false) node.setAttribute(key, value === true ? '' : value);
  }
  for (const child of children.flat()) {
    if (child !== null && child !== undefined && child !== false) node.append(child);
  }
  return node;
}

function appIcon(app, large) {
  const placeholder = el('div', { class: `app-icon placeholder${large ? ' large' : ''}` }, app.name.charAt(0).toUpperCase());
  if (!app.icon) return placeholder;
  const image = el('img', { class: `app-icon${large ? ' large' : ''}`, src: app.icon, alt: '' });
  image.addEventListener('error', () => image.replaceWith(placeholder));
  return image;
}

function formatSize(bytes) {
  return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

function formatDate(iso) {
  return iso ? new Date(iso).toLocaleDateString(currentLanguage, { year: 'numeric', month: 'short', day: 'numeric' }) : '';
}

function stop(handler) {
  return (event) => { event.stopPropagation(); handler(event); };
}

// --- Actions -------------------------------------------------------------------

// options: release (another version than the latest), closeApp (the app was
// open and the user agreed to close it), confirmed (admin notice accepted).
function install(app, options = {}) {
  const asset = options.release ? options.release.asset : app.asset;
  if (!asset || state.busy[app.appId]) return;
  if (app.admin && !options.confirmed && !state.installed[app.appId]) {
    confirmDialog(t('adminTitle'), t('adminText', { name: app.name }), t('continue'), () => install(app, { ...options, confirmed: true }), false);
    return;
  }
  delete state.errors[app.appId];
  state.lastAction[app.appId] = { action: 'install', release: options.release };
  state.busy[app.appId] = { action: 'install', percent: 0 };
  send({
    type: 'install', appId: app.appId, url: asset.url, sha256: asset.sha256, fileName: asset.name,
    exe: app.exe || '', closeApp: options.closeApp ? '1' : '0',
  });
  render();
}

function uninstall(app, options = {}) {
  if (state.busy[app.appId]) return;
  const run = () => {
    delete state.errors[app.appId];
    state.lastAction[app.appId] = { action: 'uninstall' };
    state.busy[app.appId] = { action: 'uninstall', percent: -1 };
    send({ type: 'uninstall', appId: app.appId, exe: app.exe || '', closeApp: options.closeApp ? '1' : '0' });
    render();
  };
  if (options.closeApp) run();
  else confirmDialog(t('uninstallTitle', { name: app.name }), t('uninstallText'), t('uninstall'), run, true);
}

// A release asset replaced after the catalog was read fails its checksum:
// read the catalog again and, if the checksum changed, install once more.
async function retriedWithNewCatalog(appId) {
  const last = state.lastAction[appId];
  if (!last || last.action !== 'install' || last.release || last.retried) return false;
  const before = appById(appId);
  await loadCatalog();
  const app = appById(appId);
  if (!app || !app.asset || !before || !before.asset || app.asset.sha256 === before.asset.sha256) return false;
  install(app, { confirmed: true });
  state.lastAction[appId].retried = true;
  return true;
}

function retry(app) {
  const last = state.lastAction[app.appId];
  if (last && last.action === 'uninstall') uninstall(app, { closeApp: false });
  else install(app, { release: last && last.release, confirmed: true });
}

function cancel(app) {
  send({ type: 'cancel', appId: app.appId });
}

function askToCloseApp(app, action) {
  const last = state.lastAction[app.appId] || {};
  confirmDialog(t('runningTitle', { name: app.name }), t('runningText'), t('runningOk'), () => {
    if (action === 'uninstall') uninstall(app, { closeApp: true });
    else install(app, { release: last.release, closeApp: true, confirmed: true });
  }, false, () => {
    state.queue = state.queue.filter((id) => id !== app.appId);
    runQueue();
  });
}

function installVersion(app, release) {
  const installed = state.installed[app.appId];
  if (installed && compareVersions(release.version, installed.version) < 0) {
    confirmDialog(t('downgradeTitle', { name: app.name, v: release.version }), t('downgradeText'), t('install'),
      () => install(app, { release, confirmed: true }), false);
  } else {
    install(app, { release });
  }
}

function hubHasUpdate() {
  return Boolean(state.hubVersion && state.hubLatest && compareVersions(state.hubLatest.version, state.hubVersion) > 0);
}

function selfUpdate() {
  if (!state.hubLatest || !state.hubLatest.asset || Object.keys(state.busy).length || state.queue.length) return;
  delete state.errors.hub;
  state.busy.hub = { action: 'install', percent: 0 };
  const asset = state.hubLatest.asset;
  send({
    type: 'selfUpdate', url: asset.url, sha256: asset.sha256, fileName: asset.name,
    updTitle: t('updatingTitle'), updSubtitle: t('updatingSubtitle', { version: state.hubLatest.version }),
    updDownload: t('updatingDownload'), updInstall: t('updatingInstall'), updOpen: t('updatingOpen'),
  });
  renderBanner();
}

function updatableApps() {
  return state.apps.filter((app) => statusOf(app) === 'update' && app.asset && !state.busy[app.appId]);
}

// Installers run one at a time. Vitool Box itself is only updated from its
// own banner, never as part of this.
function enqueue(apps) {
  for (const app of apps) {
    if (!state.queue.includes(app.appId)) state.queue.push(app.appId);
  }
  runQueue();
  render();
}

function updateAll() {
  enqueue(updatableApps());
}

function runQueue() {
  if (Object.keys(state.busy).length || !$('dialog').hidden) return;
  const next = state.queue.shift();
  if (!next) return;
  const app = appById(next);
  if (app && statusOf(app) !== 'installed') install(app, { confirmed: true });
  else runQueue();
}

// "Update everything when Vitool Box opens": once per run, when both the
// catalog and the installed versions are known. Not while hidden at startup:
// nobody would see the questions an update can ask.
function maybeAutoUpdate() {
  if (!state.settings.autoUpdate || state.hidden || state.autoUpdateDone || !state.catalogReady || !state.stateReady) return;
  state.autoUpdateDone = true;
  updateAll();
}

function toggle(app) {
  if (state.installed[app.appId]) uninstall(app);
  else install(app);
}

function toggleSelection(app) {
  if (state.installed[app.appId] && statusOf(app) !== 'update') return;
  if (state.selected.has(app.appId)) state.selected.delete(app.appId);
  else state.selected.add(app.appId);
  renderGrid();
}

function setSelecting(on) {
  state.selecting = on;
  state.selected.clear();
  renderGrid();
}

function installSelected() {
  const apps = state.apps.filter((app) => state.selected.has(app.appId) && app.asset);
  setSelecting(false);
  enqueue(apps);
}

function confirmDialog(title, text, okLabel, onOk, danger, onCancel) {
  $('dialog-title').textContent = title;
  $('dialog-text').textContent = text;
  $('dialog-ok').textContent = okLabel;
  $('dialog-ok').className = `button ${danger ? 'danger' : 'primary'}`;
  $('dialog').hidden = false;
  $('dialog-ok').focus();
  // A queue waits while a question is open.
  const close = () => { $('dialog').hidden = true; setTimeout(runQueue); };
  $('dialog-ok').onclick = () => { close(); onOk(); };
  $('dialog-cancel').onclick = () => { close(); if (onCancel) onCancel(); };
}

// --- Updates in the background ---------------------------------------------------

function trayTip() {
  const count = state.stateReady ? updatableApps().length : 0;
  return count ? t('trayUpdates', { n: count }) : 'Vitool Box';
}

// Tells the host how many updates wait (tray icon dot) and, when Vitool Box
// runs in the background, shows a notification for the ones not seen yet.
function reportUpdates() {
  if (!state.catalogReady) return;
  const apps = updatableApps();
  send({ type: 'updates', count: String(apps.length), tip: trayTip() });
  if (!state.startup || !state.settings.notify || !apps.length) return;
  if (!document.hidden && !state.hidden) return;

  const notified = readJson(NOTIFIED_KEY, []);
  const fresh = apps.filter((app) => !notified.includes(`${app.appId}@${app.version}`));
  if (!fresh.length) return;
  writeJson(NOTIFIED_KEY, [...notified, ...fresh.map((app) => `${app.appId}@${app.version}`)].slice(-200));
  if (fresh.length === 1) {
    send({ type: 'notify', title: t('notifyOneTitle'), text: t('notifyOneText', { name: fresh[0].name, v: fresh[0].version }) });
  } else {
    send({ type: 'notify', title: t('notifyManyTitle', { n: fresh.length }), text: t('notifyManyText', { names: fresh.map((app) => app.name).join(', ') }) });
  }
}

let checkTimer = null;

function scheduleChecks() {
  clearInterval(checkTimer);
  checkTimer = state.startup ? setInterval(loadCatalog, CHECK_HOURS * 3600 * 1000) : null;
}

// --- Rendering -----------------------------------------------------------------

function busyView(app) {
  const busy = state.busy[app.appId];
  if (!busy) return null;
  const downloading = busy.action === 'install' && busy.percent < 100;
  const indeterminate = busy.action === 'uninstall' || busy.percent < 0 || busy.percent >= 100;
  const label = busy.action === 'uninstall' ? t('uninstalling') : busy.percent >= 100 ? t('installing')
    : busy.percent >= 0 ? t('downloading', { p: busy.percent }) : t('downloadingPlain');
  return el('div', { class: 'busy', 'data-busy': app.appId },
    el('div', { class: 'busy-row' },
      el('span', { class: 'busy-label' }, label),
      downloading ? el('button', { class: 'link-button', onclick: stop(() => cancel(app)) }, t('cancel')) : null),
    el('div', { class: `progress${indeterminate ? ' indeterminate' : ''}` }, el('div', { style: `width:${Math.max(busy.percent, 0)}%` })));
}

function statusPill(app) {
  const status = statusOf(app);
  const text = { available: app.asset ? t('available') : t('noInstaller'), installed: t('installed'), update: t('updateAvailable') }[status];
  return el('span', { class: `status ${status}` }, text);
}

// The main action of a card, without opening the app: update, open or retry.
function cardAction(app) {
  if (state.errors[app.appId]) {
    return el('button', { class: 'button small', title: errorText(state.errors[app.appId]), onclick: stop(() => retry(app)) }, `⚠ ${t('retry')}`);
  }
  if (statusOf(app) === 'update' && app.asset) {
    const queued = state.queue.includes(app.appId);
    return el('button', { class: 'button primary small', disabled: queued, onclick: stop(() => install(app)) },
      queued ? t('queued') : t('updateTo', { v: app.version }));
  }
  if (state.queue.includes(app.appId)) return el('span', { class: 'status' }, t('queued'));
  if (statusOf(app) === 'installed' && app.exe) {
    return el('button', { class: 'button small', onclick: stop(() => send({ type: 'launch', appId: app.appId, exe: app.exe })) }, t('open'));
  }
  return statusPill(app);
}

function switchButton(app) {
  const on = Boolean(state.installed[app.appId]);
  return el('button', {
    class: 'switch',
    role: 'switch',
    'aria-checked': String(on),
    title: on ? t('turnOff') : t('turnOn'),
    disabled: Boolean(state.busy[app.appId]) || (!on && !app.asset),
    onclick: stop(() => toggle(app)),
  });
}

function selectBox(app) {
  const selectable = !state.installed[app.appId] || statusOf(app) === 'update';
  return el('span', { class: `checkbox${state.selected.has(app.appId) ? ' checked' : ''}${selectable ? '' : ' disabled'}` });
}

function sortApps(apps) {
  const byName = (a, b) => a.name.localeCompare(b.name, currentLanguage);
  const sorters = {
    name: byName,
    newest: (a, b) => (b.createdAt || '').localeCompare(a.createdAt || '') || byName(a, b),
    updated: (a, b) => (b.updatedAt || '').localeCompare(a.updatedAt || '') || byName(a, b),
    installed: (a, b) => Number(Boolean(state.installed[b.appId])) - Number(Boolean(state.installed[a.appId])) || byName(a, b),
  };
  return [...apps].sort(sorters[state.settings.sort] || byName);
}

function visibleApps() {
  const query = state.query.trim().toLowerCase();
  return sortApps(state.apps.filter((app) => {
    if (state.filter === 'installed' && !state.installed[app.appId]) return false;
    if (state.filter === 'updates' && statusOf(app) !== 'update') return false;
    if (state.category && app.category !== state.category) return false;
    if (!query) return true;
    return [app.name, app.tagline, app.description, app.category, categoryName(app.category)].some((text) => text.toLowerCase().includes(query));
  }));
}

function renderCategories() {
  const categories = [...new Set(state.apps.map((app) => app.category))]
    .sort((a, b) => categoryName(a).localeCompare(categoryName(b), currentLanguage));
  if (state.category && !categories.includes(state.category)) state.category = '';
  const select = $('category');
  select.replaceChildren(el('option', { value: '' }, t('allCategories')),
    ...categories.map((category) => el('option', { value: category }, categoryName(category))));
  select.value = state.category;
  select.hidden = categories.length < 2;
}

function renderGrid() {
  const grid = $('grid');
  grid.classList.toggle('selecting', state.selecting);
  grid.replaceChildren(...visibleApps().map((app) => el('article', {
    class: `card${state.selected.has(app.appId) ? ' selected' : ''}`,
    onclick: () => (state.selecting ? toggleSelection(app) : showDetail(app.repo)),
  },
  el('div', { class: 'card-head' }, appIcon(app),
    el('div', {}, el('h3', {}, app.name, isNew(app) ? el('span', { class: 'badge' }, t('new')) : null),
      el('div', { class: 'category' }, categoryName(app.category)))),
  el('p', {}, app.tagline),
  el('div', { class: 'card-foot' }, busyView(app) || cardAction(app), state.selecting ? selectBox(app) : switchButton(app)))));

  $('update-all').hidden = state.selecting || updatableApps().length < 2;
  $('select-mode').hidden = state.selecting || !state.apps.some((app) => !state.installed[app.appId] && app.asset);
  $('selection-bar').hidden = !state.selecting;
  $('selection-text').textContent = state.selected.size ? t('selected', { n: state.selected.size }) : t('selectHint');
  $('selection-install').disabled = state.selected.size === 0;

  const empty = $('empty');
  empty.hidden = grid.children.length > 0;
  if (!state.apps.length) empty.textContent = state.notice ? noticeText() : t('lookingForApps');
  else if (state.filter === 'installed' && !state.query) empty.textContent = t('noneInstalled');
  else if (state.filter === 'updates' && !state.query) empty.textContent = t('allUpToDate');
  else empty.textContent = t('noMatch');
}

function detailActions(app) {
  const installed = state.installed[app.appId];
  const busy = busyView(app);
  const actions = el('div', { class: 'actions' });
  if (busy) {
    actions.append(busy);
  } else if (!installed) {
    actions.append(el('button', { class: 'button primary', disabled: !app.asset, onclick: () => install(app) }, t('install')));
  } else {
    if (statusOf(app) === 'update') actions.append(el('button', { class: 'button primary', onclick: () => install(app) }, t('updateTo', { v: app.version })));
    if (app.exe) {
      actions.append(el('button', { class: statusOf(app) === 'update' ? 'button' : 'button primary', onclick: () => send({ type: 'launch', appId: app.appId, exe: app.exe }) }, t('open')));
    }
    actions.append(el('button', { class: 'button danger', onclick: () => uninstall(app) }, t('uninstall')));
    actions.append(el('div', { class: 'secondary-actions' },
      el('button', { class: 'link-button', onclick: () => send({ type: 'openFolder', appId: app.appId }) }, t('openFolder')),
      statusOf(app) === 'installed' && app.asset ? el('button', { class: 'link-button', onclick: () => install(app, { confirmed: true }) }, t('repair')) : null));
  }
  if (state.errors[app.appId] && !busy) {
    actions.append(el('div', { class: 'error' }, errorText(state.errors[app.appId]), ' ',
      el('button', { class: 'link-button', onclick: () => retry(app) }, t('retry'))));
  }
  if (!app.asset) actions.append(el('div', { class: 'error' }, t('noVerified')));
  if (app.admin && !installed) actions.append(el('div', { class: 'note' }, t('adminNote')));
  return actions;
}

function sanitize(html, app) {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  doc.querySelectorAll('script, style, iframe, object, embed, form, link, meta').forEach((node) => node.remove());
  doc.querySelectorAll('*').forEach((node) => {
    for (const attribute of [...node.attributes]) {
      if (/^on/i.test(attribute.name) || /^\s*javascript:/i.test(attribute.value)) node.removeAttribute(attribute.name);
    }
  });
  // The page header already shows the logo and the name.
  const first = (doc.querySelector('article') || doc.body).firstElementChild;
  if (first && first.matches('p[align="center"]') && !first.textContent.trim()) first.remove();
  const title = doc.querySelector('h1');
  if (title) (title.closest('.markdown-heading') || title).remove();
  // Relative paths in the README point into the repository.
  doc.querySelectorAll('img[src]').forEach((image) => {
    const src = image.getAttribute('src');
    if (!/^(https?:|data:)/i.test(src)) image.src = `${RAW}/${OWNER}/${app.repo}/${app.branch}/${src.replace(/^\.?\//, '')}`;
  });
  doc.querySelectorAll('a[href]').forEach((link) => {
    const href = link.getAttribute('href');
    if (href.startsWith('#')) link.removeAttribute('href');
    else if (!/^https?:/i.test(href)) link.href = `${app.homepage}/blob/${app.branch}/${href.replace(/^\.?\//, '')}`;
  });
  return doc.body.innerHTML;
}

async function loadReadme(app) {
  if (state.readmes[app.repo]) return;
  if (app.readmeHtml) {
    state.readmes[app.repo] = sanitize(app.readmeHtml, app);
    if (state.current === app.repo) renderDetail();
    return;
  }
  try {
    const response = await fetch(`${API}/repos/${OWNER}/${app.repo}/readme`, { headers: { Accept: 'application/vnd.github.html' } });
    if (!response.ok) throw new Error();
    state.readmes[app.repo] = sanitize(await response.text(), app);
  } catch {
    state.readmes[app.repo] = `<p class="loading">${app.description ? '' : t('noDescription')}</p>`;
  }
  if (state.current === app.repo) renderDetail();
}

function versionsView(app) {
  const releases = state.releases[app.repo];
  if (!releases || releases === 'loading') {
    loadReleases(app);
    return el('p', { class: 'loading' }, t('loading'));
  }
  if (releases === 'error') return el('p', { class: 'loading' }, t('versionsError'));
  const installed = state.installed[app.appId];
  return el('div', { class: 'versions' }, releases.map((release, index) => {
    const notes = el('div', { class: 'markdown' });
    notes.innerHTML = release.notesHtml ? sanitize(release.notesHtml, app) : `<p class="loading">${t('noNotes')}</p>`;
    const isInstalled = installed && compareVersions(installed.version, release.version) === 0;
    let action = null;
    if (isInstalled) action = el('span', { class: 'status installed' }, t('installed'));
    else if (release.asset && !state.busy[app.appId]) action = el('button', { class: 'button small', onclick: () => installVersion(app, release) }, t('installThis'));
    return el('details', { class: 'version', open: index === 0 },
      el('summary', {},
        el('strong', {}, release.version),
        index === 0 ? el('span', { class: 'badge' }, t('latest')) : null,
        el('span', { class: 'version-date' }, formatDate(release.date)),
        el('span', { class: 'spacer' }),
        action),
      notes);
  }));
}

function renderDetail() {
  const app = appByRepo(state.current);
  const detail = $('detail');
  if (!app) {
    showHome();
    return;
  }
  const installed = state.installed[app.appId];
  const meta = el('div', { class: 'meta' },
    el('span', {}, t('version', { v: app.version })),
    installed && installed.version !== app.version ? el('span', {}, t('installedVersion', { v: installed.version })) : null,
    el('span', {}, t('updated', { d: formatDate(app.updatedAt) })),
    app.asset ? el('span', {}, formatSize(app.asset.size)) : null,
    el('span', {}, categoryName(app.category)));

  const tabNames = { about: t('about'), notes: t('notes', { v: app.version }), versions: t('versions') };
  const tabs = el('div', { class: 'tabs' }, Object.entries(tabNames).map(([tab, label]) => el('button', {
    class: state.tab === tab ? 'active' : '',
    onclick: () => {
      if (state.tab === tab) return;
      state.tab = tab;
      renderDetail();
      fadeIn($('detail').querySelector('.tab-body'));
    },
  }, label)));

  let body;
  if (state.tab === 'versions') {
    body = versionsView(app);
  } else {
    body = el('div', { class: 'markdown' });
    if (state.tab === 'notes') {
      body.innerHTML = app.notesHtml ? sanitize(app.notesHtml, app) : `<p class="loading">${t('noNotes')}</p>`;
    } else if (state.readmes[app.repo]) {
      body.innerHTML = state.readmes[app.repo];
    } else {
      body.append(el('p', { class: 'loading' }, t('loading')));
      loadReadme(app);
    }
  }
  body.classList.add('tab-body');
  body.querySelectorAll('a[href]').forEach((link) => link.addEventListener('click', (event) => {
    event.preventDefault();
    send({ type: 'openUrl', url: link.href });
  }));

  detail.replaceChildren(...[
    el('div', { class: 'detail-head' }, appIcon(app, true),
      el('div', { class: 'info' }, el('h1', {}, app.name), el('p', { class: 'tagline' }, app.tagline), meta,
        el('p', { class: 'detail-links' },
          el('a', { href: '#', class: 'button', onclick: (event) => { event.preventDefault(); send({ type: 'openUrl', url: app.homepage }); } }, t('viewOnGitHub')),
          el('a', { href: '#', class: 'button', onclick: (event) => { event.preventDefault(); openFeedback(app.name); } }, t('reportProblem')))),
      detailActions(app)),
    app.screenshots.length ? el('div', { class: 'screenshots' }, app.screenshots.map((src) => {
      const image = el('img', { src, alt: t('screenshot', { name: app.name }), loading: 'lazy', onclick: () => openLightbox(src) });
      image.addEventListener('error', () => image.remove());
      return image;
    })) : null,
    tabs,
    body].filter(Boolean));
}

function renderBusy(appId) {
  // Progress updates touch only the progress bars, not the whole page.
  document.querySelectorAll(`[data-busy="${CSS.escape(appId)}"]`).forEach((node) => {
    const app = appById(appId);
    if (app) node.replaceWith(busyView(app));
  });
}

function noticeText() {
  if (!state.noticeUntil || state.noticeUntil < Date.now()) return t(state.notice);
  const time = new Date(state.noticeUntil).toLocaleTimeString(currentLanguage, { hour: '2-digit', minute: '2-digit' });
  return `${t(state.notice)} ${t('tryAfter', { time })}`;
}

function renderBanner() {
  const banner = $('banner');
  if (state.notice) {
    banner.className = 'banner warning';
    banner.replaceChildren(el('span', {}, noticeText()));
    banner.hidden = false;
  } else if (hubHasUpdate()) {
    const busy = state.busy.hub;
    banner.className = 'banner';
    let action;
    if (busy) {
      const percent = busy.percent;
      action = el('div', { class: 'busy banner-busy' },
        el('span', { class: 'busy-label' }, percent >= 0 && percent < 100 ? t('downloading', { p: percent }) : t('restarting')),
        el('div', { class: `progress${percent < 0 || percent >= 100 ? ' indeterminate' : ''}` }, el('div', { style: `width:${Math.max(percent, 0)}%` })));
    } else if (state.hubLatest.asset) {
      const appsBusy = Object.keys(state.busy).length > 0 || state.queue.length > 0;
      action = el('button', {
        class: 'button primary',
        disabled: appsBusy,
        title: appsBusy ? t('waitInstalls') : null,
        onclick: selfUpdate,
      }, t('updateRestart'));
    } else {
      action = el('button', { class: 'button primary', onclick: () => send({ type: 'openUrl', url: state.hubLatest.url }) }, t('download'));
    }
    banner.replaceChildren(
      el('span', {}, t('hubAvailable', { v: state.hubLatest.version }), state.errors.hub ? el('span', { class: 'error' }, ` ${errorText(state.errors.hub)}`) : null),
      action);
    banner.hidden = false;
  } else {
    banner.hidden = true;
  }
}

function settingSwitch(id, on, disabled, onchange) {
  return el('button', { class: 'switch', role: 'switch', id, 'aria-checked': String(on), disabled, onclick: onchange });
}

function settingRow(id, title, text, control) {
  return el('div', { class: 'setting' }, el('label', { for: id }, el('strong', {}, title), el('span', {}, text)), control);
}

// "name\tappId\texe\tentry…" lines: the apps whose settings can be backed up.
function backupSpec() {
  return state.apps.filter((app) => app.settings.length)
    .map((app) => [app.repo, app.appId, app.exe || '', ...app.settings].join('\t')).join('\n');
}

function renderSettings() {
  const language = el('select', {
    class: 'select', id: 'language',
    onchange: (event) => {
      state.settings.language = event.target.value;
      writeSettings();
      applyLanguage();
    },
  }, el('option', { value: 'auto' }, t('languageAuto')),
  Object.entries(LANGUAGES).map(([code, name]) => el('option', { value: code }, name)));
  language.value = state.settings.language;

  const backupBusy = state.backupMessage && state.backupMessage.working;
  const message = state.backupMessage && !backupBusy
    ? el('p', { class: state.backupMessage.error ? 'error' : 'note' }, state.backupMessage.text) : null;

  $('settings').replaceChildren(
    el('h1', { class: 'page-title' }, t('settings')),
    el('h2', { class: 'section-title' }, t('general')),
    settingRow('language', t('language'), '', language),
    settingRow('startup', t('startup'), t('startupText'), settingSwitch('startup', state.startup, false, () => {
      send({ type: 'startup', enabled: state.startup ? '0' : '1' });
    })),
    settingRow('notify', t('notify'), t('notifyText'), settingSwitch('notify', state.settings.notify && state.startup, !state.startup, () => {
      state.settings.notify = !state.settings.notify;
      writeSettings();
      renderSettings();
    })),
    settingRow('auto-update', t('autoUpdate'), t('autoUpdateText'), settingSwitch('auto-update', state.settings.autoUpdate, false, () => {
      state.settings.autoUpdate = !state.settings.autoUpdate;
      writeSettings();
      renderSettings();
    })),
    el('h2', { class: 'section-title' }, t('backupSection')),
    el('div', { class: 'setting' },
      el('div', {}, el('span', {}, t('backupText')), message),
      el('div', { class: 'setting-buttons' },
        el('button', { class: 'button', disabled: backupBusy, onclick: startBackup }, backupBusy ? t('working') : t('backupButton')),
        el('button', { class: 'button', disabled: backupBusy, onclick: startRestore }, t('restoreButton')))),
    el('h2', { class: 'section-title' }, t('about')),
    el('div', { class: 'setting' },
      el('div', {}, el('strong', {}, `Vitool Box ${state.hubVersion || ''}`), el('span', {}, t('aboutText'))),
      el('div', { class: 'setting-buttons' },
        el('button', { class: 'button', onclick: () => openFeedback('Vitool Box') }, t('reportProblem')),
        el('button', { class: 'button', onclick: () => send({ type: 'openUrl', url: `https://github.com/${OWNER}/${HUB_REPO}` }) }, t('viewOnGitHub')))));
}

function openFeedback(appName) {
  const url = appName ? `${FEEDBACK_URL}?usp=pp_url&${FEEDBACK_APP_FIELD}=${encodeURIComponent(appName)}` : FEEDBACK_URL;
  send({ type: 'openUrl', url });
}

function startBackup() {
  state.backupMessage = { working: true };
  renderSettings();
  send({ type: 'backup', spec: backupSpec() });
}

function startRestore() {
  confirmDialog(t('restoreTitle'), t('restoreText'), t('restore'), () => {
    state.backupMessage = { working: true };
    renderSettings();
    send({ type: 'restore', spec: backupSpec() });
  }, false);
}

function render() {
  renderBanner();
  if (state.view === 'detail') renderDetail();
  else if (state.view === 'settings') renderSettings();
  else renderGrid();
}

// Replays the fade-in on a view or a part of it.
function fadeIn(node) {
  node.classList.remove('fade-in');
  void node.offsetWidth;
  node.classList.add('fade-in');
}

function setView(view) {
  state.view = view;
  $('home').hidden = view !== 'home';
  $('detail').hidden = view !== 'detail';
  $('settings').hidden = view !== 'settings';
  $('back').hidden = view === 'home';
  $('search').hidden = view !== 'home';
  $('open-settings').hidden = view === 'settings';
  document.querySelector('main').scrollTop = 0;
  render();
  fadeIn($(view));
}

function showDetail(repo) {
  state.current = repo;
  state.tab = 'about';
  setView('detail');
}

function showHome() {
  state.current = null;
  setView('home');
}

function openLightbox(src) {
  const lightbox = $('lightbox');
  lightbox.querySelector('img').src = src;
  lightbox.hidden = false;
}

function applyLanguage() {
  setLanguage(state.settings.language);
  $('credits').textContent = `${t('credits')} · github.com/${OWNER}`;
  $('contact').textContent = t('contact');
  renderCategories();
  render();
  sendHostStrings();
}

// --- Start ---------------------------------------------------------------------

state.settings = readSettings();
setLanguage(state.settings.language);
$('sort').value = state.settings.sort;
$('credits').textContent = `${t('credits')} · github.com/${OWNER}`;
$('contact').textContent = t('contact');
$('contact').addEventListener('click', (event) => { event.preventDefault(); openFeedback(); });
$('credits').addEventListener('click', (event) => { event.preventDefault(); send({ type: 'openUrl', url: `https://github.com/${OWNER}` }); });
$('back').addEventListener('click', showHome);
$('open-settings').addEventListener('click', () => setView('settings'));
$('update-all').addEventListener('click', updateAll);
$('select-mode').addEventListener('click', () => setSelecting(true));
$('selection-cancel').addEventListener('click', () => setSelecting(false));
$('selection-install').addEventListener('click', installSelected);
$('refresh').addEventListener('click', () => loadCatalog());
$('search').addEventListener('input', (event) => { state.query = event.target.value; renderGrid(); });
$('category').addEventListener('change', (event) => { state.category = event.target.value; renderGrid(); fadeIn($('grid')); });
$('sort').addEventListener('change', (event) => { state.settings.sort = event.target.value; writeSettings(); renderGrid(); fadeIn($('grid')); });
$('lightbox').addEventListener('click', () => { $('lightbox').hidden = true; });
document.querySelectorAll('.chip').forEach((chip) => chip.addEventListener('click', () => {
  document.querySelectorAll('.chip').forEach((other) => other.classList.toggle('active', other === chip));
  state.filter = chip.dataset.filter;
  renderGrid();
  fadeIn($('grid'));
}));
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  if (!$('lightbox').hidden) $('lightbox').hidden = true;
  else if (!$('dialog').hidden) $('dialog-cancel').click();
  else if (state.selecting) setSelecting(false);
  else if (state.view !== 'home') showHome();
});
// The installed list can change outside the hub (e.g. Settings > Apps).
window.addEventListener('focus', () => { if (state.apps.length) refreshInstalled(); });
// Shown for the first time after a hidden start: run what was held back.
document.addEventListener('visibilitychange', () => {
  if (document.hidden || !state.hidden) return;
  state.hidden = false;
  maybeAutoUpdate();
});

send({ type: 'hello' });
sendHostStrings();
loadCatalog();
