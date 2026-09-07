#!/usr/bin/env node
import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = fileURLToPath(new URL('../../', import.meta.url))
export const usage =
  'Usage: node tooling/native/build.mjs <sol|terra> <configure|build|test> <debug|release>'

/** Validate the public CLI before inspecting or invoking native tools. */
export function parseArgs(args) {
  const [app, operation, config] = args
  if (
    args.length !== 3 ||
    !['sol', 'terra'].includes(app) ||
    !['configure', 'build', 'test'].includes(operation) ||
    !['debug', 'release'].includes(config)
  ) {
    throw new Error(usage)
  }
  return { app, operation, config }
}

/** Keep upstream source roots and all generated outputs config-specific. */
export function commandsFor({ app, operation, config }, platform = process.platform) {
  const appRoot = path.join(root, 'apps', app)
  const source = app === 'terra' ? path.join(appRoot, 'native') : appRoot
  const build = path.join(appRoot, `cmake-build-${platform}-${config}`)
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
    if (platform === 'win32') {
      command.push('-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++')
    }
    command.push(
      ...(app === 'sol'
        ? ['-DSOL_BUILD_WEB_UI=OFF', '-DBUILD_DOCS=OFF', '-DBUILD_TESTS=ON']
        : ['-DBUILD_TESTING=ON']),
    )
    return [command]
  }
  if (operation === 'build') {
    const commands = [['cmake', '--build', build, '--parallel']]
    if (app === 'terra') commands.push(['cmake', '--build', build, '--target', 'stage-extension'])
    return commands
  }
  return app === 'sol'
    ? [[path.join(build, 'tests', `test_sol${platform === 'win32' ? '.exe' : ''}`)]]
    : [['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error']]
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
  const commands = commandsFor(options)
  const commandDirectory =
    options.app === 'sol' && options.operation === 'test' ? path.dirname(commands[0][0]) : root
  const run = (command, argv, extra = {}) => {
    const result = spawnSync(command, argv, { cwd: root, stdio: 'inherit', ...extra })
    if (result.error) throw new Error(`Cannot run ${command}: ${result.error.message}`)
    if (result.status !== 0)
      throw new Error(`${command} failed (${result.signal ?? result.status}).`)
  }
  if (process.platform === 'win32') {
    const shell = process.env.SUPERNOVA_MSYS2_SHELL || 'C:\\msys64\\msys2_shell.cmd'
    if (!path.isAbsolute(shell) || !existsSync(shell) || /["%\r\n]/.test(shell)) {
      throw new Error(
        `MSYS2 shell missing or invalid: ${shell}. Set SUPERNOVA_MSYS2_SHELL to an absolute msys2_shell.cmd path.`,
      )
    }
    // Tool names must resolve inside UCRT64, never another MinGW/MSVC installation.
    const tools = ['cmake', 'ninja', 'gcc', 'g++', 'ctest']
    const checks = tools.map(
      (tool) =>
        `[ -x /ucrt64/bin/${tool}.exe ] || { echo 'Missing UCRT64 tool: ${tool}' >&2; exit 1; }`,
    )
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
      { windowsVerbatimArguments: true, env: { ...process.env, SUPERNOVA_NATIVE_COMMAND: script } },
    )
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
    for (const [command, ...argv] of commands) run(command, argv, { cwd: commandDirectory })
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    main(process.argv.slice(2))
  } catch (error) {
    console.error(`Native build: ${error.message}`)
    process.exitCode = 1
  }
}
