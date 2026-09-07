specific_ss.add(when: 'CONFIG_SYSTEM_ONLY', if_true: [files(
  'arch_init.c',
  'ioport.c',
  'globals-target.c',
  'memory.c',
  'physmem.c',
)])

system_ss.add(files(
  'balloon.c',
  'bootdevice.c',
  'cpus.c',
  'cpu-timers.c',
  'datadir.c',
  'dirtylimit.c',
  'dma-helpers.c',
  'globals.c',
  'memory_mapping.c',
  'qdev-monitor.c',
  'qtest.c',
  'rtc.c',
  'runstate-action.c',
  'runstate-hmp-cmds.c',
  'runstate.c',
  'tpm-hmp-cmds.c',
  'vl.c',
  'watchpoint.c',
), sdl, libpmem, libdaxctl)

if have_tpm
  system_ss.add(files('tpm.c'))
endif

system_ss.add(when: seccomp, if_true: files('qemu-seccomp.c'))
system_ss.add(when: 'CONFIG_DEVICE_TREE',
              if_true: [fdt, files('device_tree.c')],
              if_false: files('device_tree-stub.c'))
if host_os == 'linux'
  system_ss.add(files('async-teardown.c'))
endif

# Export symbols for shared lib build
if get_option('shared_lib')
  if host_os == 'darwin'
    configure_file(
      input: files('qemu.symbols'),
      output: 'qemu-ld64.symbols',
      capture: true,
      command: ['sed', '-ne', 's/^[[:space:]]*\\([a-zA-Z_$0-9]*\\);/_\\1/p', '@INPUT@'])
    emulator_link_args += ['-Wl,-exported_symbols_list,system/qemu-ld64.symbols']
  else
    emulator_link_args += ['-Xlinker', '--dynamic-list=' + (meson.project_source_root() / 'system/qemu.symbols')]
  endif
endif
