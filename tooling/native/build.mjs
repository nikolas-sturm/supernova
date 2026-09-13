#!/usr/bin/env node
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import {
  accessSync,
  constants,
  existsSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs'
import { availableParallelism } from 'node:os'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = fileURLToPath(new URL('../../', import.meta.url))
export const usage =
  'Usage: node tooling/native/build.mjs <sol|terra|vdd> <configure|build|test> <debug|release> [--dev] [--tests] [--fresh]'

/** Validate the public CLI before inspecting or invoking native tools. */
export function parseArgs(args) {
  const [app, operation, config, ...flags] = args
  if (
    new Set(flags).size !== flags.length ||
    flags.some((flag) => !['--dev', '--tests', '--fresh'].includes(flag)) ||
    (flags.includes('--dev') && (app !== 'sol' || config !== 'debug')) ||
    (flags.includes('--tests') && (app !== 'sol' || operation !== 'build')) ||
    (flags.includes('--fresh') && operation !== 'configure') ||
    !['sol', 'terra', 'vdd'].includes(app) ||
    !['configure', 'build', 'test'].includes(operation) ||
    !['debug', 'release'].includes(config)
  ) {
    throw new Error(usage)
  }
  return {
    app,
    operation,
    config,
    ...(flags.includes('--dev') ? { dev: true } : {}),
    ...(flags.includes('--tests') ? { tests: true } : {}),
    ...(flags.includes('--fresh') ? { fresh: true } : {}),
  }
}

/** Keep upstream source roots and all generated outputs config-specific. */
export function commandsFor(
  { app, operation, config, dev = false, tests = false, ccache, jobs },
  platform = process.platform,
) {
  if (app === 'vdd' && platform !== 'win32') {
    throw new Error('SolVDD supports Windows only.')
  }
  const appRoot = path.join(root, 'apps', app === 'vdd' ? 'sol' : app)
  const source =
    app === 'terra'
      ? path.join(appRoot, 'native')
      : app === 'vdd'
        ? path.join(appRoot, 'third-party', 'solvdd')
        : appRoot
  const build = path.join(
    appRoot,
    `cmake-build-${platform}-${app === 'vdd' ? 'vdd-' : ''}${dev ? 'dev-' : ''}${config}`,
  )
  const configuration = config === 'debug' ? 'Debug' : 'Release'
  if (operation === 'configure') {
    const command = [
      'cmake',
      '-S',
      source,
      '-B',
      build,
      '-G',
      'Ninja',
      `-DCMAKE_BUILD_TYPE=${configuration}`,
    ]
    if (platform === 'win32' && app !== 'vdd') {
      command.push('-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++')
    }
    if (app !== 'vdd' && ccache !== undefined) {
      for (const language of ['C', 'CXX'])
        command.push(`-DCMAKE_${language}_COMPILER_LAUNCHER=${ccache.replaceAll('\\', '/')}`)
    }
    command.push(
      ...(app === 'vdd'
        ? [`-DVDD_CONFIGURATION=${configuration}`, '-DVDD_PLATFORM=x64']
        : app === 'sol'
          ? ['-DSOL_BUILD_WEB_UI=OFF', '-DBUILD_DOCS=OFF', '-DBUILD_TESTS=ON']
          : ['-DBUILD_TESTING=ON']),
    )
    if (dev)
      command.push(`-DSOL_ASSETS_DIR_DEF=${path.join(build, 'assets').replaceAll('\\', '/')}`)
    return [command]
  }
  if (operation === 'build') {
    const commands = [['cmake', '--build', build, '--parallel']]
    if (jobs !== undefined) commands[0].push(String(jobs))
    if (app === 'sol') commands[0].push('--target', tests ? 'test_sol' : 'sol')
    if (app === 'terra') commands.push(['cmake', '--build', build, '--target', 'stage-extension'])
    return commands
  }
  return app === 'sol'
    ? [[path.join(build, 'tests', `test_sol${platform === 'win32' ? '.exe' : ''}`)]]
    : [['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error']]
}

/** Resolve optional ccache without installing tools or changing compiler selection. */
export function findCcache(env = process.env, platform = process.platform, windowsPaths) {
  if (env.SUPERNOVA_CCACHE === 'off') return ''
  const paths = platform === 'win32' ? path.win32 : path.posix
  const candidates = env.SUPERNOVA_CCACHE
    ? [env.SUPERNOVA_CCACHE]
    : [
        ...(platform === 'win32'
          ? (windowsPaths ?? [
              'C:\\Tools\\ccache.exe',
              paths.join(
                paths.dirname(env.SUPERNOVA_MSYS2_SHELL || 'C:\\msys64\\msys2_shell.cmd'),
                'ucrt64/bin/ccache.exe',
              ),
            ])
          : []),
        ...(env.PATH || env.Path || '')
          .split(paths.delimiter)
          .filter(Boolean)
          .map((directory) =>
            paths.join(directory, platform === 'win32' ? 'ccache.exe' : 'ccache'),
          ),
      ]
  for (const candidate of candidates) {
    if (!paths.isAbsolute(candidate)) continue
    try {
      accessSync(candidate, constants.X_OK)
      if (statSync(candidate).isFile()) return candidate
    } catch {}
  }
  if (env.SUPERNOVA_CCACHE)
    throw new Error('SUPERNOVA_CCACHE must name an existing absolute ccache executable, or off.')
  return ''
}

/** Bound native jobs independently of Nx task concurrency. */
export function buildJobs(
  value = process.env.CMAKE_BUILD_PARALLEL_LEVEL,
  fallback = availableParallelism(),
) {
  if (value === undefined || value === '') return fallback
  if (!/^[1-9]\d*$/.test(value) || !Number.isSafeInteger(Number(value)))
    throw new Error('CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer.')
  return Number(value)
}

const hash = (value) => createHash('sha256').update(value).digest('hex')

/** Skip explicit configure only after a successful run with identical inputs and cache. */
export function configureIfNeeded(command, { fingerprint, fresh = false, run }) {
  const directory = command[command.indexOf('-B') + 1]
  const cache = path.join(directory, 'CMakeCache.txt')
  const stamp = path.join(directory, '.supernova-configure.json')
  let previous
  try {
    previous = JSON.parse(readFileSync(stamp, 'utf8'))
  } catch {}
  if (
    !fresh &&
    existsSync(cache) &&
    existsSync(path.join(directory, 'build.ninja')) &&
    previous?.fingerprint === fingerprint &&
    previous.cache === hash(readFileSync(cache))
  ) {
    console.log(`[native] Configure unchanged: ${directory}`)
    return false
  }
  rmSync(stamp, { force: true })
  run(command)
  writeFileSync(stamp, JSON.stringify({ fingerprint, cache: hash(readFileSync(cache)) }))
  return true
}

/** Quote shell data, including workspace paths with spaces or apostrophes. */
export function shellQuote(value) {
  return `'${value.replaceAll("'", "'\\''")}'`
}

/** Run synchronously so failures propagate to the invoking build orchestrator. */
export function main(args) {
  const options = parseArgs(args)
  if (!['win32', 'linux'].includes(process.platform)) {
    throw new Error(
      `Unsupported platform: ${process.platform}. Native builds support Windows UCRT64 and Linux only.`,
    )
  }
  if (options.operation === 'configure' && options.app !== 'vdd') options.ccache = findCcache()
  if (options.operation === 'build') options.jobs = buildJobs()
  const commands = commandsFor(options)
  const commandDirectory =
    options.app === 'sol' && options.operation === 'test' ? path.dirname(commands[0][0]) : root
  const run = (command, argv, extra = {}) => {
    const result = spawnSync(command, argv, { cwd: root, stdio: 'inherit', ...extra })
    if (result.error) throw new Error(`Cannot run ${command}: ${result.error.message}`)
    if (result.status !== 0)
      throw new Error(`${command} failed (${result.signal ?? result.status}).`)
  }
  let execute
  let toolDirectory
  if (process.platform === 'win32') {
    const shell = process.env.SUPERNOVA_MSYS2_SHELL || 'C:\\msys64\\msys2_shell.cmd'
    if (!path.isAbsolute(shell) || !existsSync(shell) || /["%\r\n]/.test(shell)) {
      throw new Error(
        `MSYS2 shell missing or invalid: ${shell}. Set SUPERNOVA_MSYS2_SHELL to an absolute msys2_shell.cmd path.`,
      )
    }
    toolDirectory = path.join(path.dirname(shell), 'ucrt64/bin')
    // Tool names must resolve inside UCRT64, never another MinGW/MSVC installation.
    const tools = ['cmake', 'ninja', 'ctest', ...(options.app === 'vdd' ? [] : ['gcc', 'g++'])]
    const checks = tools.map(
      (tool) =>
        `[ -x /ucrt64/bin/${tool}.exe ] || { echo 'Missing UCRT64 tool: ${tool}' >&2; exit 1; }`,
    )
    execute = (commands) => {
      const script = [
        'set -eu',
        '[ "$MSYSTEM" = UCRT64 ] || { echo "Expected MSYS2 UCRT64" >&2; exit 1; }',
        'export PATH="/ucrt64/bin:/usr/bin:$PATH"',
        ...checks,
        `cd ${shellQuote(commandDirectory.replaceAll('\\', '/'))}`,
        ...commands.map((command) =>
          command.map((value) => shellQuote(value.replaceAll('\\', '/'))).join(' '),
        ),
      ].join('\n')
      // Carry shell program through the environment, not cmd.exe argument parsing.
      run(
        process.env.ComSpec || 'C:\\Windows\\System32\\cmd.exe',
        [
          '/d',
          '/s',
          '/c',
          `""${shell}" -defterm -here -no-start -ucrt64 -c "source tooling/native/windows.sh""`,
        ],
        {
          windowsVerbatimArguments: true,
          env: { ...process.env, SUPERNOVA_NATIVE_COMMAND: script },
        },
      )
    }
  } else {
    for (const tool of [
      'cmake',
      'ninja',
      'ctest',
      process.env.CC || 'cc',
      process.env.CXX || 'c++',
    ]) {
      run(tool, ['--version'])
    }
    execute = (commands) => {
      for (const [command, ...argv] of commands) run(command, argv, { cwd: commandDirectory })
    }
  }
  if (options.operation !== 'configure') {
    if (options.operation === 'build')
      console.log(
        `[native] Building ${options.app} with ${options.jobs} parallel jobs${options.tests ? ' (tests)' : ''}`,
      )
    execute(commands)
    return
  }
  const capture = (command, argv, allowDirty = false) => {
    const result = spawnSync(command, argv, { cwd: root, encoding: 'utf8' })
    if (result.error || (result.status !== 0 && !(allowDirty && result.status === 1)))
      throw new Error(
        `Cannot inspect configure input ${command}: ${result.error?.message || result.stderr}`,
      )
    return [result.status, result.stdout]
  }
  const tools = ['cmake', 'ninja', ...(options.app === 'vdd' ? [] : ['gcc', 'g++'])].map((tool) => {
    const executable = toolDirectory
      ? path.join(toolDirectory, `${tool}.exe`)
      : tool === 'gcc'
        ? process.env.CC || 'cc'
        : tool === 'g++'
          ? process.env.CXX || 'c++'
          : tool
    return [executable, capture(executable, ['--version'])]
  })
  if (options.ccache) tools.push([options.ccache, capture(options.ccache, ['--version'])])
  const environment = Object.fromEntries(
    Object.entries(process.env)
      .filter(
        ([key]) =>
          /^(PATH|CC|CXX|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS|CMAKE_.*|CPATH|CPLUS_INCLUDE_PATH|C_INCLUDE_PATH|LIBRARY_PATH|PKG_CONFIG.*|SUPERNOVA_.*|BRANCH|BUILD_VERSION|CLONE_URL|COMMIT|TAG|CPM_.*|FETCHCONTENT_.*)$/i.test(
            key,
          ) && key !== 'CMAKE_BUILD_PARALLEL_LEVEL',
      )
      .sort(([a], [b]) => a.localeCompare(b)),
  )
  const version = [
    capture('git', ['rev-parse', 'HEAD']),
    capture('git', ['rev-parse', '--abbrev-ref', 'HEAD']),
    capture('git', ['diff', '--quiet', '--exit-code'], true),
  ]
  if (options.app === 'terra') {
    for (const directory of [
      'apps/terra/refs/moonlight-qt',
      'apps/terra/refs/moonlight-qt/moonlight-common-c/moonlight-common-c',
    ])
      version.push(capture('git', ['-C', directory, 'rev-parse', 'HEAD']))
  }
  console.log(
    `[native] Compiler cache: ${options.ccache || 'disabled'}; Ninja tracks source/CMake changes`,
  )
  configureIfNeeded(commands[0], {
    fingerprint: hash(
      JSON.stringify({
        commands,
        environment,
        tools,
        version,
        wrapper: hash(readFileSync(fileURLToPath(import.meta.url))),
        shell: hash(readFileSync(new URL('./windows.sh', import.meta.url))),
      }),
    ),
    fresh: options.fresh,
    run: (command) => execute([command]),
  })
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    main(process.argv.slice(2))
  } catch (error) {
    console.error(`Native build: ${error.message}`)
    process.exitCode = 1
  }
}
