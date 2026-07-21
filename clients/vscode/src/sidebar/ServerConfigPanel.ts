import * as vscode from 'vscode'
import { Config } from '../config.gen'
import { getServerJsonPath, readServerConfig, writeServerConfig } from './ServerConfigManager'

// GUI editor for the workspace `.slang/server.json`. Singleton webview panel:
// reopening while it's already open just reveals + reloads it from disk.
export class ServerConfigPanel {
  private static current: ServerConfigPanel | undefined

  private readonly panel: vscode.WebviewPanel
  private disposables: vscode.Disposable[] = []

  static async createOrShow(): Promise<void> {
    if (ServerConfigPanel.current) {
      ServerConfigPanel.current.panel.reveal(vscode.ViewColumn.Active)
      await ServerConfigPanel.current.load()
      return
    }

    const panel = vscode.window.createWebviewPanel(
      'slangServerConfig',
      'Slang Server Config',
      vscode.ViewColumn.Active,
      { enableScripts: true, retainContextWhenHidden: true }
    )
    ServerConfigPanel.current = new ServerConfigPanel(panel)
  }

  private constructor(panel: vscode.WebviewPanel) {
    this.panel = panel
    this.panel.webview.html = getHtml(this.panel.webview)

    this.panel.onDidDispose(() => this.dispose(), null, this.disposables)

    this.panel.webview.onDidReceiveMessage(
      (message: WebviewMessage) => void this.handleMessage(message),
      null,
      this.disposables
    )
  }

  private async handleMessage(message: WebviewMessage): Promise<void> {
    switch (message.command) {
      case 'ready':
        await this.load()
        break
      case 'save':
        await this.save(message.config)
        break
      case 'openFile':
        await this.openFile()
        break
    }
  }

  private async load(): Promise<void> {
    const serverPath = getServerJsonPath()
    if (!serverPath) {
      void this.panel.webview.postMessage({
        command: 'error',
        message: 'No workspace folder is open, so there is nowhere to save server.json.',
      } satisfies WebviewInMessage)
      return
    }

    try {
      const { config, exists } = await readServerConfig()
      void this.panel.webview.postMessage({
        command: 'init',
        config,
        exists,
        path: vscode.workspace.asRelativePath(serverPath),
      } satisfies WebviewInMessage)
    } catch (err) {
      void this.panel.webview.postMessage({
        command: 'error',
        message: err instanceof Error ? err.message : String(err),
      } satisfies WebviewInMessage)
    }
  }

  private async save(config: Config): Promise<void> {
    try {
      const savedPath = await writeServerConfig(config)
      const relPath = vscode.workspace.asRelativePath(savedPath)
      void this.panel.webview.postMessage({
        command: 'saved',
        path: relPath,
      } satisfies WebviewInMessage)
      vscode.window.setStatusBarMessage(`Saved ${relPath}`, 3000)
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      void this.panel.webview.postMessage({
        command: 'error',
        message: `Failed to save: ${msg}`,
      } satisfies WebviewInMessage)
      vscode.window.showErrorMessage(`Failed to save server.json: ${msg}`)
    }
  }

  private async openFile(): Promise<void> {
    const serverPath = getServerJsonPath()
    if (!serverPath) {
      return
    }
    try {
      // writeServerConfig creates the file (and .slang dir) if missing, so this
      // always has something to open, matching the "create on save" behavior.
      const { exists } = await readServerConfig()
      if (!exists) {
        await writeServerConfig({})
      }
      const doc = await vscode.workspace.openTextDocument(serverPath)
      await vscode.window.showTextDocument(doc, { viewColumn: vscode.ViewColumn.Beside })
    } catch (err) {
      vscode.window.showErrorMessage(
        `Failed to open server.json: ${err instanceof Error ? err.message : String(err)}`
      )
    }
  }

  private dispose(): void {
    ServerConfigPanel.current = undefined
    this.panel.dispose()
    while (this.disposables.length) {
      this.disposables.pop()?.dispose()
    }
  }
}

////////////////////////////////////////////////////
// Webview <-> extension messages
////////////////////////////////////////////////////

type WebviewMessage =
  | { command: 'ready' }
  | { command: 'save'; config: Config }
  | { command: 'openFile' }

type WebviewInMessage =
  | { command: 'init'; config: Config; exists: boolean; path: string }
  | { command: 'saved'; path: string }
  | { command: 'error'; message: string }

function getNonce(): string {
  let text = ''
  const possible = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'
  for (let i = 0; i < 32; i++) {
    text += possible.charAt(Math.floor(Math.random() * possible.length))
  }
  return text
}

function getHtml(webview: vscode.Webview): string {
  const nonce = getNonce()
  const csp = [
    `default-src 'none'`,
    `style-src ${webview.cspSource} 'unsafe-inline'`,
    `script-src 'nonce-${nonce}'`,
  ].join('; ')

  return /* html */ `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="${csp}" />
<title>Slang Server Config</title>
<style>
${STYLE}
</style>
</head>
<body>
<div id="app">
  <header>
    <h1>Slang Server Config</h1>
    <div id="path-line" class="muted"></div>
  </header>

  <div id="error-banner" class="banner error hidden"></div>
  <div id="saved-banner" class="banner saved hidden">Saved.</div>

  <form id="form">
    <section>
      <h2>General</h2>
      <label class="field">
        <span>Flags</span>
        <small class="muted">Flags to pass to slang, e.g. <code>-f path/to/slang.f</code></small>
        <textarea id="flags" rows="3" placeholder="(none)"></textarea>
      </label>
      <label class="field">
        <span>Work directory</span>
        <small class="muted">Effective working directory for resolving relative paths in Flags</small>
        <input id="workDir" type="text" placeholder="(workspace root)" />
      </label>
    </section>

    <section>
      <h2>Indexing</h2>
      <label class="field inline">
        <span>Indexing threads</span>
        <input id="indexingThreads" type="number" min="0" step="1" placeholder="(auto)" />
      </label>
      <div class="field">
        <span>Index directories</span>
        <small class="muted">Which directories to index. Leave empty to index the whole workspace.</small>
        <div id="index-rows" class="rows"></div>
        <button type="button" class="secondary" id="add-index">+ Add index entry</button>
      </div>
    </section>

    <section>
      <h2>Build</h2>
      <label class="field">
        <span>Build file</span>
        <small class="muted">Build (.f) file to automatically open on start</small>
        <input id="build" type="text" placeholder="(none)" />
      </label>
      <label class="field">
        <span>Build pattern</span>
        <small class="muted">Glob pattern for selecting build files, e.g. <code>builds/{}.f</code></small>
        <input id="buildPattern" type="text" placeholder="(match all .f files)" />
      </label>
      <label class="field inline">
        <span>Build paths are relative to the build file</span>
        <select id="buildRelativePaths">
          <option value="">(default)</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      </label>
      <div class="field">
        <span>Builds</span>
        <small class="muted">Additional build-file sources: direct .f selection, or command-based .f generation</small>
        <div id="builds-rows" class="rows"></div>
        <button type="button" class="secondary" id="add-build">+ Add build entry</button>
      </div>
    </section>

    <section>
      <h2>Waveforms</h2>
      <label class="field">
        <span>Waveform pattern</span>
        <small class="muted">Glob to open a waveform for a build. <code>{name}</code> / <code>{top}</code> are substituted.</small>
        <input id="wavesPattern" type="text" placeholder="(none)" />
      </label>
      <label class="field">
        <span>WCP command</span>
        <small class="muted">Waveform viewer command; <code>{}</code> is replaced with the WCP port</small>
        <input id="wcpCommand" type="text" placeholder="(none)" />
      </label>
    </section>

    <section>
      <h2>Hovers</h2>
      <label class="field inline">
        <span>Doc comment format</span>
        <select id="docCommentFormat">
          <option value="">(default: markdown)</option>
          <option value="markdown">markdown</option>
          <option value="plaintext">plaintext</option>
          <option value="raw">raw</option>
        </select>
      </label>
    </section>

    <section>
      <h2>Inlay hints</h2>
      <label class="field inline">
        <span>Port types</span>
        <select id="portTypes">
          <option value="">(default: false)</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      </label>
      <label class="field inline">
        <span>Ordered instance names</span>
        <select id="orderedInstanceNames">
          <option value="">(default: true)</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      </label>
      <label class="field inline">
        <span>Wildcard port names</span>
        <select id="wildcardNames">
          <option value="">(default: true)</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      </label>
      <label class="field inline">
        <span>Function arg names (min args, 0=off)</span>
        <input id="funcArgNames" type="number" min="0" step="1" placeholder="(default: 2)" />
      </label>
      <label class="field inline">
        <span>Macro arg names (min args, 0=off)</span>
        <input id="macroArgNames" type="number" min="0" step="1" placeholder="(default: 2)" />
      </label>
    </section>

    <section>
      <h2>Advanced</h2>
      <label class="field inline">
        <span>Resolve include fragments</span>
        <select id="resolveIncludeFragments">
          <option value="">(default: true)</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      </label>
    </section>

    <details id="deprecated-section">
      <summary>Deprecated fields</summary>
      <section>
        <small class="muted">Kept for round-tripping existing config; prefer "Index directories" above.</small>
        <label class="field">
          <span>Index globs <em>(deprecated, use Index directories)</em></span>
          <textarea id="indexGlobs" rows="2" placeholder="(none) - one glob per line"></textarea>
        </label>
        <label class="field">
          <span>Exclude dirs <em>(deprecated, use Index directories)</em></span>
          <textarea id="excludeDirsTop" rows="2" placeholder="(none) - one directory per line"></textarea>
        </label>
      </section>
    </details>

    <footer>
      <button type="submit" id="save-btn" disabled>Save</button>
      <button type="button" id="open-file-btn" class="secondary" disabled>Open server.json</button>
      <button type="button" id="reload-btn" class="secondary" disabled>Reload</button>
    </footer>
  </form>
</div>

<template id="index-row-template">
  <div class="row index-row">
    <button type="button" class="remove" title="Remove">&times;</button>
    <label class="field">
      <span>Directories</span>
      <textarea class="dirs" rows="2" placeholder="(whole workspace) - one directory per line"></textarea>
    </label>
    <label class="field">
      <span>Exclude directories</span>
      <textarea class="excludeDirs" rows="2" placeholder="(none) - one directory name per line"></textarea>
    </label>
  </div>
</template>

<template id="build-row-template">
  <div class="row build-row">
    <button type="button" class="remove" title="Remove">&times;</button>
    <label class="field inline">
      <span>Name</span>
      <input class="name" type="text" placeholder="(optional)" />
    </label>
    <label class="field inline">
      <span>Glob</span>
      <input class="glob" type="text" placeholder="e.g. build/**/*.f" />
    </label>
    <label class="field inline">
      <span>Command</span>
      <input class="command" type="text" placeholder="(optional) command producing .f content on stdout" />
    </label>
  </div>
</template>

<script nonce="${nonce}">
${SCRIPT}
</script>
</body>
</html>`
}

const STYLE = `
  :root { color-scheme: light dark; }
  * { box-sizing: border-box; }
  body {
    font-family: var(--vscode-font-family);
    color: var(--vscode-editor-foreground);
    background: var(--vscode-editor-background);
    padding: 0 16px 32px;
  }
  h1 { font-size: 1.3em; margin-bottom: 2px; }
  h2 { font-size: 1.05em; margin: 0 0 8px; border-bottom: 1px solid var(--vscode-panel-border); padding-bottom: 4px; }
  header { position: sticky; top: 0; background: var(--vscode-editor-background); padding-top: 12px; z-index: 1; }
  .muted { color: var(--vscode-descriptionForeground); }
  code { font-family: var(--vscode-editor-font-family); }
  section { margin: 20px 0; padding: 12px; border: 1px solid var(--vscode-panel-border); border-radius: 4px; }
  .field { display: flex; flex-direction: column; gap: 4px; margin: 10px 0; }
  .field.inline { flex-direction: row; align-items: center; justify-content: space-between; gap: 12px; }
  .field.inline > span { flex: 1; }
  .field > span { font-weight: 600; }
  input[type="text"], input[type="number"], textarea, select {
    font-family: var(--vscode-font-family);
    background: var(--vscode-input-background);
    color: var(--vscode-input-foreground);
    border: 1px solid var(--vscode-input-border, transparent);
    border-radius: 2px;
    padding: 4px 6px;
  }
  .field.inline input, .field.inline select { max-width: 260px; flex: none; width: 220px; }
  textarea { font-family: var(--vscode-editor-font-family); resize: vertical; }
  input:focus, textarea:focus, select:focus { outline: 1px solid var(--vscode-focusBorder); }
  .rows { display: flex; flex-direction: column; gap: 8px; margin: 8px 0; }
  .row {
    position: relative;
    border: 1px dashed var(--vscode-panel-border);
    border-radius: 4px;
    padding: 10px 32px 4px 10px;
  }
  .row .remove {
    position: absolute; top: 6px; right: 6px;
    background: transparent; border: none; color: var(--vscode-descriptionForeground);
    cursor: pointer; font-size: 16px; line-height: 1; padding: 2px 6px;
  }
  .row .remove:hover { color: var(--vscode-errorForeground); }
  button {
    background: var(--vscode-button-background);
    color: var(--vscode-button-foreground);
    border: none; border-radius: 2px; padding: 6px 14px; cursor: pointer;
  }
  button:hover:not(:disabled) { background: var(--vscode-button-hoverBackground); }
  button:disabled { opacity: 0.5; cursor: default; }
  button.secondary {
    background: var(--vscode-button-secondaryBackground, transparent);
    color: var(--vscode-button-secondaryForeground, var(--vscode-editor-foreground));
    border: 1px solid var(--vscode-panel-border);
  }
  button.secondary:hover:not(:disabled) { background: var(--vscode-button-secondaryHoverBackground, var(--vscode-list-hoverBackground)); }
  footer { display: flex; gap: 8px; margin-top: 20px; position: sticky; bottom: 0; background: var(--vscode-editor-background); padding: 12px 0; }
  .banner { padding: 8px 12px; border-radius: 4px; margin: 8px 0; }
  .banner.error { background: var(--vscode-inputValidation-errorBackground, #5a1d1d); border: 1px solid var(--vscode-inputValidation-errorBorder, transparent); }
  .banner.saved { background: var(--vscode-inputValidation-infoBackground, #1d3d5a); border: 1px solid var(--vscode-inputValidation-infoBorder, transparent); }
  .hidden { display: none; }
  details#deprecated-section { margin: 20px 0; }
  details#deprecated-section summary { cursor: pointer; color: var(--vscode-descriptionForeground); }
`

// Plain DOM/JS, no framework: this is a small, self-contained form, and the
// whole panel already lives inline in the extension bundle.
const SCRIPT = `
(function () {
  const vscode = acquireVsCodeApi();

  const $ = (id) => document.getElementById(id);
  const form = $('form');
  const errorBanner = $('error-banner');
  const savedBanner = $('saved-banner');
  const pathLine = $('path-line');
  const saveBtn = $('save-btn');
  const openFileBtn = $('open-file-btn');
  const reloadBtn = $('reload-btn');

  let savedTimeout;

  function showError(msg) {
    errorBanner.textContent = msg;
    errorBanner.classList.remove('hidden');
  }
  function clearError() {
    errorBanner.classList.add('hidden');
  }
  function flashSaved() {
    savedBanner.classList.remove('hidden');
    clearTimeout(savedTimeout);
    savedTimeout = setTimeout(() => savedBanner.classList.add('hidden'), 2500);
  }

  //////////////////////////////////////////////////////////////////
  // Repeatable row helpers (index[] / builds[])
  //////////////////////////////////////////////////////////////////

  function addRow(templateId, containerId) {
    const tpl = $(templateId);
    const container = $(containerId);
    const node = tpl.content.firstElementChild.cloneNode(true);
    node.querySelector('.remove').addEventListener('click', () => node.remove());
    container.appendChild(node);
    return node;
  }

  function linesToArray(text) {
    const lines = text.split('\\n').map((l) => l.trim()).filter((l) => l.length > 0);
    return lines.length > 0 ? lines : undefined;
  }

  function arrayToLines(arr) {
    return Array.isArray(arr) ? arr.join('\\n') : '';
  }

  function addIndexRow(entry) {
    entry = entry || {};
    const node = addRow('index-row-template', 'index-rows');
    node.querySelector('.dirs').value = arrayToLines(entry.dirs);
    node.querySelector('.excludeDirs').value = arrayToLines(entry.excludeDirs);
  }

  function addBuildRow(entry) {
    entry = entry || {};
    const node = addRow('build-row-template', 'builds-rows');
    node.querySelector('.name').value = entry.name || '';
    node.querySelector('.glob').value = entry.glob || '';
    node.querySelector('.command').value = entry.command || '';
  }

  $('add-index').addEventListener('click', () => addIndexRow());
  $('add-build').addEventListener('click', () => addBuildRow());

  //////////////////////////////////////////////////////////////////
  // Load config into the form
  //////////////////////////////////////////////////////////////////

  function strOrEmpty(v) {
    return v === undefined || v === null ? '' : String(v);
  }

  function boolSelectValue(v) {
    return v === true ? 'true' : v === false ? 'false' : '';
  }

  function numOrEmpty(v) {
    return v === undefined || v === null ? '' : String(v);
  }

  function loadConfig(config) {
    $('flags').value = strOrEmpty(config.flags);
    $('workDir').value = strOrEmpty(config.workDir);
    $('indexingThreads').value = numOrEmpty(config.indexingThreads);
    $('build').value = strOrEmpty(config.build);
    $('buildPattern').value = strOrEmpty(config.buildPattern);
    $('buildRelativePaths').value = boolSelectValue(config.buildRelativePaths);
    $('wavesPattern').value = strOrEmpty(config.wavesPattern);
    $('wcpCommand').value = strOrEmpty(config.wcpCommand);
    $('docCommentFormat').value = strOrEmpty(config.hovers && config.hovers.docCommentFormat);
    const inlay = config.inlayHints || {};
    $('portTypes').value = boolSelectValue(inlay.portTypes);
    $('orderedInstanceNames').value = boolSelectValue(inlay.orderedInstanceNames);
    $('wildcardNames').value = boolSelectValue(inlay.wildcardNames);
    $('funcArgNames').value = numOrEmpty(inlay.funcArgNames);
    $('macroArgNames').value = numOrEmpty(inlay.macroArgNames);
    $('resolveIncludeFragments').value = boolSelectValue(config.resolveIncludeFragments);
    $('indexGlobs').value = arrayToLines(config.indexGlobs);
    $('excludeDirsTop').value = arrayToLines(config.excludeDirs);

    $('index-rows').innerHTML = '';
    (config.index || []).forEach(addIndexRow);
    $('builds-rows').innerHTML = '';
    (config.builds || []).forEach(addBuildRow);

    if (config.indexGlobs || config.excludeDirs) {
      $('deprecated-section').setAttribute('open', '');
    }
  }

  //////////////////////////////////////////////////////////////////
  // Read the form back into a Config object
  //////////////////////////////////////////////////////////////////

  function optStr(id) {
    const v = $(id).value.trim();
    return v.length > 0 ? v : undefined;
  }

  function optNum(id) {
    const v = $(id).value.trim();
    if (v.length === 0) return undefined;
    const n = Number(v);
    return Number.isNaN(n) ? undefined : n;
  }

  function optBool(id) {
    const v = $(id).value;
    return v === 'true' ? true : v === 'false' ? false : undefined;
  }

  function optLines(id) {
    return linesToArray($(id).value);
  }

  function readConfig() {
    const config = {};

    const flags = optStr('flags');
    if (flags !== undefined) config.flags = flags;
    const workDir = optStr('workDir');
    if (workDir !== undefined) config.workDir = workDir;
    const indexingThreads = optNum('indexingThreads');
    if (indexingThreads !== undefined) config.indexingThreads = indexingThreads;
    const build = optStr('build');
    if (build !== undefined) config.build = build;
    const buildPattern = optStr('buildPattern');
    if (buildPattern !== undefined) config.buildPattern = buildPattern;
    const buildRelativePaths = optBool('buildRelativePaths');
    if (buildRelativePaths !== undefined) config.buildRelativePaths = buildRelativePaths;
    const wavesPattern = optStr('wavesPattern');
    if (wavesPattern !== undefined) config.wavesPattern = wavesPattern;
    const wcpCommand = optStr('wcpCommand');
    if (wcpCommand !== undefined) config.wcpCommand = wcpCommand;
    const resolveIncludeFragments = optBool('resolveIncludeFragments');
    if (resolveIncludeFragments !== undefined) config.resolveIncludeFragments = resolveIncludeFragments;

    const docCommentFormat = optStr('docCommentFormat');
    if (docCommentFormat !== undefined) config.hovers = { docCommentFormat };

    const inlay = {};
    const portTypes = optBool('portTypes');
    if (portTypes !== undefined) inlay.portTypes = portTypes;
    const orderedInstanceNames = optBool('orderedInstanceNames');
    if (orderedInstanceNames !== undefined) inlay.orderedInstanceNames = orderedInstanceNames;
    const wildcardNames = optBool('wildcardNames');
    if (wildcardNames !== undefined) inlay.wildcardNames = wildcardNames;
    const funcArgNames = optNum('funcArgNames');
    if (funcArgNames !== undefined) inlay.funcArgNames = funcArgNames;
    const macroArgNames = optNum('macroArgNames');
    if (macroArgNames !== undefined) inlay.macroArgNames = macroArgNames;
    if (Object.keys(inlay).length > 0) config.inlayHints = inlay;

    const indexGlobs = optLines('indexGlobs');
    if (indexGlobs !== undefined) config.indexGlobs = indexGlobs;
    const excludeDirsTop = optLines('excludeDirsTop');
    if (excludeDirsTop !== undefined) config.excludeDirs = excludeDirsTop;

    const index = [];
    document.querySelectorAll('#index-rows .index-row').forEach((row) => {
      const dirs = linesToArray(row.querySelector('.dirs').value);
      const excludeDirs = linesToArray(row.querySelector('.excludeDirs').value);
      if (dirs !== undefined || excludeDirs !== undefined) {
        const entry = {};
        if (dirs !== undefined) entry.dirs = dirs;
        if (excludeDirs !== undefined) entry.excludeDirs = excludeDirs;
        index.push(entry);
      }
    });
    if (index.length > 0) config.index = index;

    const builds = [];
    document.querySelectorAll('#builds-rows .build-row').forEach((row) => {
      const name = row.querySelector('.name').value.trim();
      const glob = row.querySelector('.glob').value.trim();
      const command = row.querySelector('.command').value.trim();
      if (name || glob || command) {
        const entry = {};
        if (name) entry.name = name;
        if (glob) entry.glob = glob;
        if (command) entry.command = command;
        builds.push(entry);
      }
    });
    if (builds.length > 0) config.builds = builds;

    return config;
  }

  //////////////////////////////////////////////////////////////////
  // Wiring
  //////////////////////////////////////////////////////////////////

  form.addEventListener('submit', (e) => {
    e.preventDefault();
    clearError();
    vscode.postMessage({ command: 'save', config: readConfig() });
  });

  openFileBtn.addEventListener('click', () => {
    vscode.postMessage({ command: 'openFile' });
  });

  reloadBtn.addEventListener('click', () => {
    clearError();
    vscode.postMessage({ command: 'ready' });
  });

  window.addEventListener('message', (event) => {
    const message = event.data;
    switch (message.command) {
      case 'init':
        clearError();
        loadConfig(message.config);
        pathLine.textContent = message.exists
          ? 'Editing ' + message.path
          : message.path + ' does not exist yet - Save will create it';
        saveBtn.disabled = false;
        openFileBtn.disabled = false;
        reloadBtn.disabled = false;
        break;
      case 'saved':
        flashSaved();
        pathLine.textContent = 'Editing ' + message.path;
        break;
      case 'error':
        showError(message.message);
        break;
    }
  });

  vscode.postMessage({ command: 'ready' });
})();
`
