import * as fs from 'fs/promises'
import * as path from 'path'
import { Config } from '../config.gen'
import { getWorkspaceFolder } from '../utils'

const SERVER_DIR = '.slang'
const SERVER_FILE = 'server.json'

export function getServerJsonPath(): string | undefined {
  const ws = getWorkspaceFolder()
  if (!ws) {
    return undefined
  }
  return path.join(ws, SERVER_DIR, SERVER_FILE)
}

export interface ServerConfigLoadResult {
  config: Config
  // Whether .slang/server.json already existed on disk (false => saving will create it)
  exists: boolean
}

// Throws if the file exists but isn't valid JSON, so callers don't silently
// present (and risk saving) an empty config over one the user is mid-edit on.
export async function readServerConfig(): Promise<ServerConfigLoadResult> {
  const serverPath = getServerJsonPath()
  if (!serverPath) {
    return { config: {}, exists: false }
  }

  let content: string
  try {
    content = await fs.readFile(serverPath, 'utf-8')
  } catch (err) {
    if ((err as { code?: string })?.code === 'ENOENT') {
      return { config: {}, exists: false }
    }
    throw err
  }

  try {
    return { config: JSON.parse(content) as Config, exists: true }
  } catch (err) {
    throw new Error(
      `Failed to parse ${serverPath}: ${err instanceof Error ? err.message : String(err)}`
    )
  }
}

export async function writeServerConfig(config: Config): Promise<string> {
  const serverPath = getServerJsonPath()
  if (!serverPath) {
    throw new Error('No workspace folder found')
  }

  const dir = path.dirname(serverPath)
  await fs.mkdir(dir, { recursive: true })
  await fs.writeFile(serverPath, JSON.stringify(config, null, 2) + '\n', 'utf-8')
  return serverPath
}
