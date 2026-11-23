import tape from 'tape'
import { Config } from '../../src/config.gen'
import { mergeServerConfig } from '../../src/sidebar/mergeServerConfig'

tape('save keeps keys the form does not edit', (t) => {
  const existing = {
    incdirs: ['rtl/include'],
    flags: '-DOLD',
    inlayHints: { portTypes: true, activeParameterValues: true },
  } as unknown as Config
  const form = { flags: '-DNEW', inlayHints: { funcArgNames: 2 } } as unknown as Config
  t.deepEqual(mergeServerConfig(existing, form), {
    incdirs: ['rtl/include'],
    flags: '-DNEW',
    inlayHints: { activeParameterValues: true, funcArgNames: 2 },
  })
  t.end()
})

tape('save removes form keys the user cleared', (t) => {
  const existing = { workDir: 'sim', hovers: { docCommentFormat: 'raw' } } as unknown as Config
  t.deepEqual(mergeServerConfig(existing, {} as Config), {})
  t.end()
})
