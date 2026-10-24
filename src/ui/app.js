// Vitool Box web UI. Finds the apps on GitHub (repositories of OWNER with the
// TOPIC topic, a vitool.json manifest and at least one release) and asks the
// host (VitoolBox.exe) to install, remove and start them.
'use strict';

const OWNER = 'VitomDev';
const TOPIC = 'vitool-box';
const HUB_REPO = 'Vitool-Box';
const CACHE_KEY = 'vitool.catalog.v1';
const CACHE_HOURS = 3;
const API = 'https://api.github.com';
const RAW = 'https://raw.githubusercontent.com';

const state = {
  apps: [],
  installed: {},   // appId -> { version }
  busy: {},        // appId -> { action, percent }
  errors: {},      // appId -> message
  filter: 'all',
  query: '',
  current: null,   // repo shown in the detail view
  tab: 'about',
  readmes: {},     // repo -> html
  hubVersion: null,
  hubLatest: null,
};

const $ = (id) => document.getElementById(id);

// --- Host bridge ---------------------------------------------------------------

const host = window.chrome && window.chrome.webview;

function send(message) {
  if (host) host.postMessage(message);
}

if (host) {
  host.addEventListener('message', (event) => {
    const message = event.data;
    switch (message.type) {
      case 'hello':
        state.hubVersion = message.version;
        renderBanner();
        break;
      case 'state':
        state.installed = message.apps;
        render();
        break;
      case 'progress':
        if (state.busy[message.appId]) state.busy[message.appId].percent = Number(message.percent);
        renderBusy(message.appId);
        break;
      case 'done':
        delete state.busy[message.appId];
        if (message.error) state.errors[message.appId] = message.error;
        refreshInstalled();
        break;
    }
  });
}

function refreshInstalled() {
  send({ type: 'state', ids: state.apps.map((app) => app.appId).join(',') });
}

// --- Catalog -------------------------------------------------------------------

async function getJson(url, accept) {
  const response = await fetch(url, { headers: { Accept: accept || 'application/vnd.github+json' } });
  if (!response.ok) {
    const error = new Error(`HTTP ${response.status}`);
    error.rateLimited = response.status === 403 || response.status === 429;
    error.status = response.status;
    throw error;
  }
  return response.json();
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

async function loadApp(repo) {
  const branch = repo.default_branch;
  let manifest;
  try {
    const response = await fetch(`${RAW}/${OWNER}/${repo.name}/${branch}/vitool.json`, { cache: 'no-cache' });
    if (!response.ok) return null;
    manifest = await response.json();
  } catch {
    return null;
  }
  if (!manifest || !/^\{[0-9A-Fa-f-]{36}\}$/.test(manifest.appId || '')) return null;

  let release;
  try {
    release = await getJson(`${API}/repos/${OWNER}/${repo.name}/releases/latest`, 'application/vnd.github.html+json');
  } catch (error) {
    if (error.status === 404) return null;
    throw error;
  }
  const asset = pickInstaller(release.assets || [], manifest.installer);
  const sha256 = asset && /^sha256:[0-9a-f]{64}$/.test(asset.digest || '') ? asset.digest.slice(7) : null;

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
    homepage: repo.html_url,
    updatedAt: release.published_at,
    version: (release.tag_name || '').replace(/^v/i, ''),
    notesHtml: release.body_html || '',
    releaseUrl: release.html_url,
    asset: asset && sha256 ? { name: asset.name, url: asset.browser_download_url, sha256, size: asset.size } : null,
  };
}

async function fetchCatalog() {
  const search = await getJson(`${API}/search/repositories?q=topic:${TOPIC}+user:${OWNER}&per_page=100`);
  const results = await Promise.all(search.items.map((repo) => loadApp(repo)));
  const apps = results.filter(Boolean).sort((a, b) => a.name.localeCompare(b.name));
  let hubLatest = null;
  try {
    const release = await getJson(`${API}/repos/${OWNER}/${HUB_REPO}/releases/latest`);
    hubLatest = { version: release.tag_name.replace(/^v/i, ''), url: release.html_url };
  } catch { /* No release of the hub yet. */ }
  return { apps, hubLatest };
}

function readCache() {
  try {
    return JSON.parse(localStorage.getItem(CACHE_KEY));
  } catch {
    return null;
  }
}

function writeCache(catalog) {
  try {
    localStorage.setItem(CACHE_KEY, JSON.stringify({ time: Date.now(), ...catalog }));
  } catch { /* Storage unavailable: the catalog is fetched again next time. */ }
}

function applyCatalog(catalog) {
  state.apps = catalog.apps;
  state.hubLatest = catalog.hubLatest;
  refreshInstalled();
  render();
  renderBanner();
}

async function loadCatalog(force) {
  const cached = readCache();
  if (cached && !force && Date.now() - cached.time < CACHE_HOURS * 3600 * 1000) {
    applyCatalog(cached);
    return;
  }
  if (cached) applyCatalog(cached);

  $('refresh').classList.add('spinning');
  try {
    const catalog = await fetchCatalog();
    writeCache(catalog);
    state.notice = null;
    applyCatalog(catalog);
  } catch (error) {
    state.notice = error.rateLimited
      ? 'GitHub is limiting requests for now. Showing the last list; try again in a while.'
      : 'Could not reach GitHub. Check your internet connection.';
    renderBanner();
    if (!cached) render();
  } finally {
    $('refresh').classList.remove('spinning');
  }
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
  return iso ? new Date(iso).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }) : '';
}

// --- Actions -------------------------------------------------------------------

function install(app) {
  if (!app.asset || state.busy[app.appId]) return;
  delete state.errors[app.appId];
  state.busy[app.appId] = { action: 'install', percent: 0 };
  send({ type: 'install', appId: app.appId, url: app.asset.url, sha256: app.asset.sha256, fileName: app.asset.name });
  render();
}

function uninstall(app) {
  if (state.busy[app.appId]) return;
  confirmDialog(`Uninstall ${app.name}?`, 'The app and its settings will be removed from this PC. You can turn it on again at any time.', 'Uninstall', () => {
    delete state.errors[app.appId];
    state.busy[app.appId] = { action: 'uninstall', percent: -1 };
    send({ type: 'uninstall', appId: app.appId });
    render();
  });
}

function toggle(app) {
  if (state.installed[app.appId]) uninstall(app);
  else install(app);
}

function confirmDialog(title, text, okLabel, onOk) {
  $('dialog-title').textContent = title;
  $('dialog-text').textContent = text;
  $('dialog-ok').textContent = okLabel;
  $('dialog').hidden = false;
  $('dialog-ok').focus();
  const close = () => { $('dialog').hidden = true; };
  $('dialog-ok').onclick = () => { close(); onOk(); };
  $('dialog-cancel').onclick = close;
}

// --- Rendering -----------------------------------------------------------------

function busyView(app) {
  const busy = state.busy[app.appId];
  if (!busy) return null;
  const indeterminate = busy.action === 'uninstall' || busy.percent < 0 || busy.percent >= 100;
  const label = busy.action === 'uninstall' ? 'Uninstalling…' : busy.percent >= 100 ? 'Installing…' : busy.percent >= 0 ? `Downloading ${busy.percent}%` : 'Downloading…';
  return el('div', { class: 'busy', 'data-busy': app.appId },
    el('span', { class: 'busy-label' }, label),
    el('div', { class: `progress${indeterminate ? ' indeterminate' : ''}` }, el('div', { style: `width:${Math.max(busy.percent, 0)}%` })));
}

function statusPill(app) {
  const status = statusOf(app);
  const text = { available: app.asset ? 'Available' : 'No installer', installed: 'Installed', update: 'Update available' }[status];
  return el('span', { class: `status ${status}` }, text);
}

function switchButton(app) {
  const on = Boolean(state.installed[app.appId]);
  return el('button', {
    class: 'switch',
    role: 'switch',
    'aria-checked': String(on),
    title: on ? 'Turn off (uninstall)' : 'Turn on (install)',
    disabled: Boolean(state.busy[app.appId]) || (!on && !app.asset),
    onclick: (event) => { event.stopPropagation(); toggle(app); },
  });
}

function visibleApps() {
  const query = state.query.trim().toLowerCase();
  return state.apps.filter((app) => {
    if (state.filter === 'installed' && !state.installed[app.appId]) return false;
    if (state.filter === 'updates' && statusOf(app) !== 'update') return false;
    if (!query) return true;
    return [app.name, app.tagline, app.description, app.category].some((text) => text.toLowerCase().includes(query));
  });
}

function renderGrid() {
  const grid = $('grid');
  grid.replaceChildren(...visibleApps().map((app) => el('article', { class: 'card', onclick: () => showDetail(app.repo) },
    el('div', { class: 'card-head' }, appIcon(app),
      el('div', {}, el('h3', {}, app.name), el('div', { class: 'category' }, app.category))),
    el('p', {}, app.tagline),
    el('div', { class: 'card-foot' }, busyView(app) || statusPill(app), switchButton(app)))));

  const empty = $('empty');
  empty.hidden = grid.children.length > 0;
  if (!state.apps.length) empty.textContent = state.notice ? 'No apps to show yet.' : 'Looking for apps…';
  else if (state.filter === 'installed' && !state.query) empty.textContent = 'You have no apps turned on yet. Pick one from All apps.';
  else if (state.filter === 'updates' && !state.query) empty.textContent = 'Everything is up to date. ✨';
  else empty.textContent = 'No apps match your search.';
}

function detailActions(app) {
  const installed = state.installed[app.appId];
  const busy = busyView(app);
  const actions = el('div', { class: 'actions' });
  if (busy) {
    actions.append(busy);
  } else if (!installed) {
    actions.append(el('button', { class: 'button primary', disabled: !app.asset, onclick: () => install(app) }, 'Install'));
  } else {
    if (statusOf(app) === 'update') actions.append(el('button', { class: 'button primary', onclick: () => install(app) }, `Update to ${app.version}`));
    if (app.exe) actions.append(el('button', { class: statusOf(app) === 'update' ? 'button' : 'button primary', onclick: () => send({ type: 'launch', appId: app.appId, exe: app.exe }) }, 'Open'));
    actions.append(el('button', { class: 'button danger', onclick: () => uninstall(app) }, 'Uninstall'));
  }
  if (state.errors[app.appId]) actions.append(el('div', { class: 'error' }, state.errors[app.appId]));
  if (!app.asset) actions.append(el('div', { class: 'error' }, 'This release has no verified installer.'));
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
  try {
    const response = await fetch(`${API}/repos/${OWNER}/${app.repo}/readme`, { headers: { Accept: 'application/vnd.github.html' } });
    if (!response.ok) throw new Error();
    state.readmes[app.repo] = sanitize(await response.text(), app);
  } catch {
    state.readmes[app.repo] = `<p class="loading">${app.description ? '' : 'Could not load the description.'}</p>`;
  }
  if (state.current === app.repo) renderDetail();
}

function renderDetail() {
  const app = state.apps.find((item) => item.repo === state.current);
  const detail = $('detail');
  if (!app) {
    showHome();
    return;
  }
  const installed = state.installed[app.appId];
  const meta = el('div', { class: 'meta' },
    el('span', {}, `Version ${app.version}`),
    installed && installed.version !== app.version ? el('span', {}, `Installed ${installed.version}`) : null,
    el('span', {}, `Updated ${formatDate(app.updatedAt)}`),
    app.asset ? el('span', {}, formatSize(app.asset.size)) : null,
    el('span', {}, app.category));

  const tabs = el('div', { class: 'tabs' },
    ['about', 'notes'].map((tab) => el('button', {
      class: state.tab === tab ? 'active' : '',
      onclick: () => { state.tab = tab; renderDetail(); },
    }, tab === 'about' ? 'About' : `What's new in ${app.version}`)));

  const body = el('div', { class: 'markdown' });
  if (state.tab === 'notes') {
    body.innerHTML = app.notesHtml ? sanitize(app.notesHtml, app) : '<p class="loading">No release notes.</p>';
  } else if (state.readmes[app.repo]) {
    body.innerHTML = state.readmes[app.repo];
  } else {
    body.append(el('p', { class: 'loading' }, 'Loading…'));
    loadReadme(app);
  }
  body.querySelectorAll('a[href]').forEach((link) => link.addEventListener('click', (event) => {
    event.preventDefault();
    send({ type: 'openUrl', url: link.href });
  }));

  detail.replaceChildren(...[
    el('div', { class: 'detail-head' }, appIcon(app, true),
      el('div', { class: 'info' }, el('h1', {}, app.name), el('p', { class: 'tagline' }, app.tagline), meta,
        el('p', {}, el('a', { href: '#', class: 'button', onclick: (event) => { event.preventDefault(); send({ type: 'openUrl', url: app.homepage }); } }, 'View on GitHub'))),
      detailActions(app)),
    app.screenshots.length ? el('div', { class: 'screenshots' }, app.screenshots.map((src) => {
      const image = el('img', { src, alt: `${app.name} screenshot`, loading: 'lazy', onclick: () => openLightbox(src) });
      image.addEventListener('error', () => image.remove());
      return image;
    })) : null,
    tabs,
    body].filter(Boolean));
}

function renderBusy(appId) {
  // Progress updates touch only the progress bars, not the whole page.
  document.querySelectorAll(`[data-busy="${CSS.escape(appId)}"]`).forEach((node) => {
    const app = state.apps.find((item) => item.appId === appId);
    if (app) node.replaceWith(busyView(app));
  });
}

function renderBanner() {
  const banner = $('banner');
  if (state.notice) {
    banner.className = 'banner warning';
    banner.replaceChildren(el('span', {}, state.notice));
    banner.hidden = false;
  } else if (state.hubVersion && state.hubLatest && compareVersions(state.hubLatest.version, state.hubVersion) > 0) {
    banner.className = 'banner';
    banner.replaceChildren(el('span', {}, `Vitool Box ${state.hubLatest.version} is available.`),
      el('button', { class: 'button primary', onclick: () => send({ type: 'openUrl', url: state.hubLatest.url }) }, 'Download'));
    banner.hidden = false;
  } else {
    banner.hidden = true;
  }
}

function render() {
  if (state.current) renderDetail();
  else renderGrid();
}

function showDetail(repo) {
  state.current = repo;
  state.tab = 'about';
  $('home').hidden = true;
  $('detail').hidden = false;
  $('back').hidden = false;
  $('search').hidden = true;
  document.querySelector('main').scrollTop = 0;
  renderDetail();
}

function showHome() {
  state.current = null;
  $('home').hidden = false;
  $('detail').hidden = true;
  $('back').hidden = true;
  $('search').hidden = false;
  renderGrid();
}

function openLightbox(src) {
  const lightbox = $('lightbox');
  lightbox.querySelector('img').src = src;
  lightbox.hidden = false;
}

// --- Start ---------------------------------------------------------------------

$('back').addEventListener('click', showHome);
$('refresh').addEventListener('click', () => loadCatalog(true));
$('search').addEventListener('input', (event) => { state.query = event.target.value; renderGrid(); });
$('lightbox').addEventListener('click', () => { $('lightbox').hidden = true; });
document.querySelectorAll('.chip').forEach((chip) => chip.addEventListener('click', () => {
  document.querySelectorAll('.chip').forEach((other) => other.classList.toggle('active', other === chip));
  state.filter = chip.dataset.filter;
  renderGrid();
}));
document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  if (!$('lightbox').hidden) $('lightbox').hidden = true;
  else if (!$('dialog').hidden) $('dialog').hidden = true;
  else if (state.current) showHome();
});
// The installed list can change outside the hub (e.g. Settings > Apps).
window.addEventListener('focus', () => { if (state.apps.length) refreshInstalled(); });

send({ type: 'hello' });
loadCatalog(false);
