import * as process from 'process'

export type Platform = 'windows' | 'linux' | 'mac'

export type PlatformMap = { [key in Platform]: string }

/** The bundled `bin/slang-server` is a static Linux x86-64 ELF; anywhere else it cannot run. */
export function canRunBundledServer(
  platform: string = process.platform,
  arch: string = process.arch
): boolean {
  return platform === 'linux' && arch === 'x64'
}

export function getPlatform(): Platform {
  switch (process.platform) {
    case 'win32':
      return 'windows'
    case 'darwin':
      return 'mac'
    default:
      // includes WSL
      return 'linux'
  }
}
