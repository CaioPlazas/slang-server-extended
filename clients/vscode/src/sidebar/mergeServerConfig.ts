import { Config } from '../config.gen'

// Keys the config panel's form edits. Must match `readConfig()` in ServerConfigPanel.ts.
// Anything else in server.json (incdirs, newer upstream keys, ...) is kept untouched on save.
const FORM_KEYS = [
  'flags',
  'workDir',
  'indexingThreads',
  'build',
  'buildPattern',
  'buildRelativePaths',
  'wavesPattern',
  'wcpCommand',
  'resolveIncludeFragments',
  'indexGlobs',
  'excludeDirs',
  'index',
  'builds',
] as const

const FORM_NESTED_KEYS: Record<string, readonly string[]> = {
  hovers: ['docCommentFormat'],
  inlayHints: [
    'portTypes',
    'orderedInstanceNames',
    'wildcardNames',
    'funcArgNames',
    'macroArgNames',
  ],
}

type Obj = Record<string, unknown>

/** Overlay the form's values on the file's: a form key left empty is removed, others survive. */
export function mergeServerConfig(existing: Config, form: Config): Config {
  const src = form as Obj
  const out: Obj = { ...(existing as Obj) }
  for (const key of FORM_KEYS) {
    if (key in src) {
      out[key] = src[key]
    } else {
      delete out[key]
    }
  }
  for (const [parent, keys] of Object.entries(FORM_NESTED_KEYS)) {
    const fromForm = (src[parent] ?? {}) as Obj
    const merged: Obj = { ...((out[parent] ?? {}) as Obj) }
    for (const key of keys) {
      if (key in fromForm) {
        merged[key] = fromForm[key]
      } else {
        delete merged[key]
      }
    }
    if (Object.keys(merged).length > 0) {
      out[parent] = merged
    } else {
      delete out[parent]
    }
  }
  return out as Config
}
