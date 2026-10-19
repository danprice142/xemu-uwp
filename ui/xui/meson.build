xemu_ss.add(files(
  'actions.cc',
  'animation.cc',
  'compat.cc',
  'debug.cc',
  'font-manager.cc',
  'game-library.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'notifications.cc',
  'popup-menu.cc',
  'reporting.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'snapshot-manager.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
))

if opengl.found()
  xemu_ss.add(files('gl-helpers.cc'))
else
  xemu_ss.add(files('gl-helpers-stub.cc'))
endif

if host_os == 'windows'
  xemu_ss.add(files('update.cc'))
endif
