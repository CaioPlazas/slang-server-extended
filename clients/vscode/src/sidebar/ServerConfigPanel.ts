import * as vscode from 'vscode'
import { Config } from '../config.gen'
import { getServerJsonPath, readServerConfig, writeServerConfig } from './ServerConfigManager'
import { getWorkspaceFolder } from '../utils'

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
      case 'browseWorkDir':
        await this.browseWorkDir()
        break
      case 'browseUvmDir':
        await this.browseUvmDir(message.rowId)
        break
      case 'browseUvmFile':
        await this.browseUvmFile()
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

  private async browseWorkDir(): Promise<void> {
    const wsFolder = getWorkspaceFolder()
    const uris = await vscode.window.showOpenDialog({
      canSelectFiles: false,
      canSelectFolders: true,
      canSelectMany: false,
      openLabel: 'Select Work Directory',
      defaultUri: wsFolder ? vscode.Uri.file(wsFolder) : undefined,
    })
    if (!uris || uris.length === 0) {
      return
    }
    void this.panel.webview.postMessage({
      command: 'workDirSelected',
      path: vscode.workspace.asRelativePath(uris[0]),
    } satisfies WebviewInMessage)
  }

  private async browseUvmDir(rowId: number): Promise<void> {
    const wsFolder = getWorkspaceFolder()
    const uris = await vscode.window.showOpenDialog({
      canSelectFiles: false,
      canSelectFolders: true,
      canSelectMany: false,
      openLabel: 'Select UVM Include Folder',
      defaultUri: wsFolder ? vscode.Uri.file(wsFolder) : undefined,
    })
    if (!uris || uris.length === 0) {
      return
    }
    void this.panel.webview.postMessage({
      command: 'uvmDirSelected',
      rowId,
      path: vscode.workspace.asRelativePath(uris[0]),
    } satisfies WebviewInMessage)
  }

  private async browseUvmFile(): Promise<void> {
    const wsFolder = getWorkspaceFolder()
    const uris = await vscode.window.showOpenDialog({
      canSelectFiles: true,
      canSelectFolders: false,
      canSelectMany: false,
      openLabel: 'Select uvm.sv',
      filters: { SystemVerilog: ['sv', 'svh'] },
      defaultUri: wsFolder ? vscode.Uri.file(wsFolder) : undefined,
    })
    if (!uris || uris.length === 0) {
      return
    }
    void this.panel.webview.postMessage({
      command: 'uvmFileSelected',
      path: vscode.workspace.asRelativePath(uris[0]),
    } satisfies WebviewInMessage)
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
  | { command: 'browseWorkDir' }
  | { command: 'browseUvmDir'; rowId: number }
  | { command: 'browseUvmFile' }

type WebviewInMessage =
  | { command: 'init'; config: Config; exists: boolean; path: string }
  | { command: 'saved'; path: string }
  | { command: 'error'; message: string }
  | { command: 'workDirSelected'; path: string }
  | { command: 'uvmDirSelected'; rowId: number; path: string }
  | { command: 'uvmFileSelected'; path: string }

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
      <div class="tip">
        For the best experience, consider passing an flist (<code>-f</code> or
        <code>-F</code>) together with your include directories (<code>-I</code>). This
        lets the server build your project's full file hierarchy and index every header
        it can reach, which enables proper cross-file linting instead of best-effort
        single-file checks. Both <code>-f</code> and <code>-F</code> accept a command
        file - which one to use just depends on your project's workflow: with
        <code>-f</code> paths inside the file are relative to the current working
        directory, while with <code>-F</code> they're relative to the file itself.
        Example: <code>-F project.flist -I rtl/...</code> (the trailing <code>...</code>
        searches a directory recursively).
      </div>
      <div class="field">
        <span>Flags</span>
        <small class="muted">
          Flags to pass to slang. Pick a flag from the list (or type your own) on the
          left, its value on the right; leave the flag blank for a bare argument like a
          source file. Append <code>...</code> after a directory to search it
          recursively, e.g. <code>rtl/...</code>.
        </small>
        <div id="flags-rows" class="rows"></div>
        <button type="button" class="secondary" id="add-flag">+ Add flag</button>
        <datalist id="known-flags"></datalist>
      </div>
      <label class="field">
        <span>Work directory</span>
        <small class="muted">Effective working directory for resolving relative paths in Flags</small>
        <div class="input-row">
          <input id="workDir" type="text" placeholder="(workspace root)" />
          <button type="button" class="secondary" id="browse-workdir" disabled>Browse…</button>
        </div>
      </label>
    </section>

    <section>
      <h2 class="checkbox-header">
        <label>
          <input id="uvmVerificationLinting" type="checkbox" />
          (Experimental) UVM &amp; Verification Linting
        </label>
      </h2>
      <div class="tip">
        Improves analysis of UVM-style verification code: binds class-only <code>.svh</code>
        files <code>\`include</code>d inside a package/module to that owner's context instead
        of parsing them standalone, and follows virtual-interface member types and class
        <code>extends</code> base-class names across files when resolving a shallow
        compilation's dependencies. Off by default; behavior may change while this is
        validated.
      </div>
      <div id="uvm-fields">
        <div class="field">
          <span>UVM include folders</span>
          <small class="muted">
            Folders to search for UVM includes, applied as <code>-I</code> flags. Append
            <code>...</code> after a folder to search it recursively, e.g.
            <code>uvm/src/...</code>.
          </small>
          <div id="uvm-dir-rows" class="rows"></div>
          <button type="button" class="secondary" id="add-uvm-dir">+ Add folder</button>
        </div>
        <label class="field">
          <span>uvm.sv file</span>
          <small class="muted">Path to the UVM library's top-level uvm_pkg.sv/uvm.sv file</small>
          <div class="input-row">
            <input id="uvmFile" type="text" placeholder="(none)" />
            <button type="button" class="secondary" id="browse-uvm-file">Browse…</button>
          </div>
        </label>
      </div>
    </section>

    <section>
      <h2 class="checkbox-header">
        <label>
          <input id="singleUnitMacros" type="checkbox" />
          (Experimental) Single-Unit Macro Inheritance
        </label>
      </h2>
      <div class="tip">
        Approximates <code>--single-unit</code> preprocessor behavior: predefines the union of
        all <code>\`define</code>s found in the build's files when parsing each file, so macros
        defined in one file (e.g. <code>uvm_macros.svh</code> pulled in by a package) resolve in
        files that don't <code>\`include</code> them. Symbol scoping is unchanged. Off by
        default.
      </div>
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

<template id="flag-row-template">
  <div class="row flag-row">
    <button type="button" class="remove" title="Remove">&times;</button>
    <div class="input-row">
      <input class="flag" type="text" list="known-flags" placeholder="flag, e.g. -I" />
      <input class="value" type="text" placeholder="value, e.g. rtl/..." />
    </div>
  </div>
</template>

<template id="uvm-dir-row-template">
  <div class="row uvm-dir-row">
    <button type="button" class="remove" title="Remove">&times;</button>
    <div class="input-row">
      <input class="path" type="text" placeholder="e.g. uvm/src or uvm/src/..." />
      <button type="button" class="secondary browse">Browse…</button>
    </div>
  </div>
</template>

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
  .input-row { display: flex; gap: 6px; }
  .input-row input { flex: 1; }
  .flag-row .flag { flex: 0 0 200px; }
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
  .tip {
    background: var(--vscode-textBlockQuote-background);
    border-left: 3px solid var(--vscode-textBlockQuote-border, var(--vscode-focusBorder));
    padding: 8px 12px;
    margin: 8px 0 14px;
    font-size: 0.95em;
    color: var(--vscode-descriptionForeground);
  }
  .hidden { display: none; }
  h2.checkbox-header { border-bottom: none; }
  h2.checkbox-header label { display: flex; align-items: center; gap: 8px; cursor: pointer; }
  #uvm-fields.disabled { opacity: 0.5; pointer-events: none; }
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
  const browseWorkDirBtn = $('browse-workdir');

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

  // Flags slang's driver actually accepts (see external/slang/source/driver/Driver.cpp
  // addStandardArgs()), used both to populate the flag dropdown and to know, when
  // reloading a saved "flags" string, which tokens are followed by a value that must
  // stay paired with them in one row. Not exhaustive -- anything not listed here (a
  // custom/unlisted flag, or its value) round-trips as its own bare-argument row.
  const VALUE_FLAGS = [
    ['-I', '-I,--include-directory,+incdir', '<dir>'],
    ['-D', '-D,--define-macro,+define', '<macro>=<value>'],
    ['-U', '-U,--undefine-macro', '<macro>'],
    ['-f', '-f', '<file>'],
    ['-F', '-F', '<file>'],
    ['-y', '-y,--libdir', '<dir>'],
    ['-Y', '-Y,--libext,+libext', '<ext>'],
    ['-v', '-v,--libfile', '<file>'],
    ['-G', '-G', '<name>=<value>'],
    ['-L', '-L', '<library>'],
    ['-W', '-W', '<warning>'],
    ['-T', '-T,--timing', 'min|typ|max'],
    ['-C', '-C', '<file>'],
    ['--top', '--top', '<module>'],
    ['--timescale', '--timescale', '<base>/<precision>'],
    ['--defaultLibName', '--defaultLibName', '<name>'],
    ['--libmap', '--libmap', '<file>'],
    ['--waiver-file', '--waiver-file', '<file>'],
    ['--error-limit', '--error-limit', '<n>'],
    ['--compat', '--compat', 'vcs|mentor|...'],
    ['--suppress-warnings', '--suppress-warnings', '<file-pattern>'],
    ['--suppress-macro-warnings', '--suppress-macro-warnings', '<file-pattern>'],
    ['--max-hierarchy-depth', '--max-hierarchy-depth', '<n>'],
    ['--max-include-depth', '--max-include-depth', '<n>'],
    ['--define-system-task', '--define-system-task', '<prototype>'],
  ];
  const BOOL_FLAGS = [
    '--single-unit',
    '--lint-only',
    '--ignore-unknown-modules',
    '--allow-use-before-declare',
    '--relax-enum-conversions',
    '--relax-string-conversions',
    '--allow-hierarchical-const',
    '--allow-toplevel-iface-ports',
    '--disable-instance-caching',
    '--incdir-first',
    '--disable-local-includes',
  ];
  const INCLUDE_FLAG_ALIASES = ['-I', '--include-directory', '+incdir'];

  // alias -> true (takes a value) / false (bare switch), covering every alias of every
  // entry above so e.g. "--include-directory foo" round-trips the same as "-I foo".
  const FLAG_TAKES_VALUE = {};
  VALUE_FLAGS.forEach((f) => f[1].split(',').forEach((alias) => (FLAG_TAKES_VALUE[alias] = true)));
  BOOL_FLAGS.forEach((name) => (FLAG_TAKES_VALUE[name] = false));

  const knownFlagsDatalist = $('known-flags');
  VALUE_FLAGS.map((f) => f[0])
    .concat(BOOL_FLAGS)
    .forEach((name) => {
      const opt = document.createElement('option');
      opt.value = name;
      knownFlagsDatalist.appendChild(opt);
    });

  function isIncludeFlag(tok) {
    return INCLUDE_FLAG_ALIASES.indexOf(tok) >= 0;
  }

  function addFlagRow(flag, value) {
    const node = addRow('flag-row-template', 'flags-rows');
    node.querySelector('.flag').value = flag || '';
    node.querySelector('.value').value = value || '';
  }

  // UVM include-folder rows are a friendlier view over the same "flags" string as the
  // generic Flags list above (never a separate config field) -- each row is addressed by a
  // stable rowId (not DOM index, which shifts across add/remove) so its Browse button's
  // reply can be routed back to the right row.
  let uvmDirRowSeq = 0;
  const uvmDirRowsById = new Map();

  function addUvmDirRow(path) {
    const rowId = ++uvmDirRowSeq;
    const node = addRow('uvm-dir-row-template', 'uvm-dir-rows');
    node.dataset.rowId = String(rowId);
    node.querySelector('.path').value = path || '';
    node.querySelector('.browse').addEventListener('click', () => {
      vscode.postMessage({ command: 'browseUvmDir', rowId });
    });
    node.querySelector('.remove').addEventListener('click', () => uvmDirRowsById.delete(rowId));
    uvmDirRowsById.set(rowId, node);
    return node;
  }

  function updateUvmFieldsEnabled() {
    $('uvm-fields').classList.toggle('disabled', !$('uvmVerificationLinting').checked);
  }

  $('add-index').addEventListener('click', () => addIndexRow());
  $('add-build').addEventListener('click', () => addBuildRow());
  $('add-flag').addEventListener('click', () => addFlagRow());
  $('add-uvm-dir').addEventListener('click', () => addUvmDirRow());
  $('uvmVerificationLinting').addEventListener('change', updateUvmFieldsEnabled);
  $('browse-uvm-file').addEventListener('click', () => {
    vscode.postMessage({ command: 'browseUvmFile' });
  });

  // Pulls "-I <dir>" pairs (only when <dir> itself looks UVM-related -- otherwise it's a
  // general include and stays in the generic Flags list; recursion is just whatever the
  // user typed, e.g. a trailing "/...", left untouched) and a bare uvm.sv/uvm_pkg.sv-ish
  // token out of a flags string, for the friendlier UVM-specific rows above. Everything
  // else becomes a generic Flags row: a token from FLAG_TAKES_VALUE is paired with the
  // token right after it (so e.g. "-f /rtl/top.flist" stays one row), a bare boolean flag
  // gets its own row with no value, and anything unrecognized is its own bare-value row.
  function looksLikeUvmFile(tok) {
    if (tok.startsWith('-') || tok.startsWith('+')) return false;
    const base = tok.split('/').pop().toLowerCase();
    return base.includes('uvm') && (base.endsWith('.sv') || base.endsWith('.svh'));
  }

  function isUvmPath(p) {
    return p.toLowerCase().includes('uvm');
  }

  function parseFlagRows(flagsStr) {
    const tokens = strOrEmpty(flagsStr)
      .split(/\\s+/)
      .map((t) => t.trim())
      .filter((t) => t.length > 0);
    const dirs = [];
    let uvmFile;
    const rows = [];
    for (let i = 0; i < tokens.length; i++) {
      const tok = tokens[i];
      const takesValue = FLAG_TAKES_VALUE[tok];
      if (takesValue === true && i + 1 < tokens.length) {
        const val = tokens[i + 1];
        if (isIncludeFlag(tok) && isUvmPath(val)) {
          dirs.push(val);
        } else {
          rows.push({ flag: tok, value: val });
        }
        i++;
        continue;
      }
      if (takesValue === false) {
        rows.push({ flag: tok, value: '' });
        continue;
      }
      if (uvmFile === undefined && looksLikeUvmFile(tok)) {
        uvmFile = tok;
        continue;
      }
      rows.push({ flag: '', value: tok });
    }
    return { dirs: dirs, uvmFile: uvmFile, rows: rows };
  }

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

    $('uvmVerificationLinting').checked = !!(config.experimental && config.experimental.uvmVerificationLinting);
    updateUvmFieldsEnabled();
    $('singleUnitMacros').checked = !!(config.experimental && config.experimental.singleUnitMacros);

    const parsed = parseFlagRows(config.flags);
    $('uvm-dir-rows').innerHTML = '';
    uvmDirRowsById.clear();
    parsed.dirs.forEach(addUvmDirRow);
    $('uvmFile').value = parsed.uvmFile || '';

    $('flags-rows').innerHTML = '';
    parsed.rows.forEach((r) => addFlagRow(r.flag, r.value));

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

    const flagValues = [];
    document.querySelectorAll('#flags-rows .flag-row').forEach((row) => {
      const flag = row.querySelector('.flag').value.trim();
      const value = row.querySelector('.value').value.trim();
      if (flag.length === 0 && value.length === 0) return;
      flagValues.push(flag.length > 0 ? (value.length > 0 ? flag + ' ' + value : flag) : value);
    });
    // UVM rows are just a friendlier view over the same "flags" string -- their tokens are
    // appended after the generic Flags list's tokens, not stored as separate config fields.
    // Saving normalizes/reorders these to the end of "flags"; that's expected for a
    // generated section, not data loss. Only contribute anything when the experimental
    // toggle is on -- otherwise these fields are inert and must not leak into "flags".
    const uvmChecked = $('uvmVerificationLinting').checked;
    if (uvmChecked) {
      document.querySelectorAll('#uvm-dir-rows .uvm-dir-row').forEach((row) => {
        const p = row.querySelector('.path').value.trim();
        if (p.length === 0) return;
        flagValues.push('-I ' + p);
      });
      const uvmFileVal = optStr('uvmFile');
      if (uvmFileVal !== undefined) flagValues.push(uvmFileVal);
    }
    if (flagValues.length > 0) config.flags = flagValues.join(' ');

    const singleUnitMacrosChecked = $('singleUnitMacros').checked;
    if (uvmChecked || singleUnitMacrosChecked) {
      config.experimental = {};
      if (uvmChecked) config.experimental.uvmVerificationLinting = true;
      if (singleUnitMacrosChecked) config.experimental.singleUnitMacros = true;
    }

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

  browseWorkDirBtn.addEventListener('click', () => {
    vscode.postMessage({ command: 'browseWorkDir' });
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
        browseWorkDirBtn.disabled = false;
        break;
      case 'saved':
        flashSaved();
        pathLine.textContent = 'Editing ' + message.path;
        break;
      case 'workDirSelected':
        $('workDir').value = message.path;
        break;
      case 'uvmDirSelected': {
        const node = uvmDirRowsById.get(message.rowId);
        if (node) node.querySelector('.path').value = message.path;
        break;
      }
      case 'uvmFileSelected':
        $('uvmFile').value = message.path;
        break;
      case 'error':
        showError(message.message);
        break;
    }
  });

  vscode.postMessage({ command: 'ready' });
})();
`
