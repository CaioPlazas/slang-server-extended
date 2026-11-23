import tape from 'tape'
import { canRunBundledServer } from '../../src/lib/platform'

tape('bundled server only runs on linux x64', (t) => {
  t.true(canRunBundledServer('linux', 'x64'))
  t.false(canRunBundledServer('linux', 'arm64'))
  t.false(canRunBundledServer('darwin', 'x64'))
  t.false(canRunBundledServer('darwin', 'arm64'))
  t.false(canRunBundledServer('win32', 'x64'))
  t.end()
})
